// AssistantTools.cpp

#include <myengine/assistant/AssistantTools.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/HierarchyComponent.h>
#include <myengine/ecs/components/ScriptComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/editor/EditorState.h>
#include <myengine/physics/PhysicsWorldState.h>
#include <myengine/scene/SceneSerializer.h>
#include <myengine/scripting/PrefabLibrary.h>

namespace myengine::assistant
{
    namespace
    {
        using json = nlohmann::json;

        constexpr std::size_t kMaxListed = 500;
        constexpr std::size_t kDefaultListed = 100;
        constexpr std::size_t kMaxSpawn = 50;
        constexpr std::size_t kMaxErrorChars = 700;

        AssistantToolResult Fail(std::string message)
        {
            AssistantToolResult result;
            result.ok = false;
            result.message = std::move(message);
            return result;
        }

        AssistantToolResult Done(json value)
        {
            AssistantToolResult result;
            result.value = std::move(value);
            return result;
        }

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        bool ParseVec3(const json& value, ecs::components::Vec3& out)
        {
            if (!value.is_array() || value.size() != 3)
            {
                return false;
            }
            float parts[3]{};
            for (std::size_t index = 0; index < 3; ++index)
            {
                if (!value[index].is_number())
                {
                    return false;
                }
                parts[index] = value[index].get<float>();
                if (!std::isfinite(parts[index]))
                {
                    return false;
                }
            }
            out = {parts[0], parts[1], parts[2]};
            return true;
        }

        json Vec3ToJson(const ecs::components::Vec3& value)
        {
            return json::array({value.x, value.y, value.z});
        }

        std::string EntityName(const ecs::World& world, const ecs::EntityId entity)
        {
            const auto* tag = world.TryGet<ecs::components::TagComponent>(entity);
            return tag != nullptr ? tag->name : std::string();
        }

        std::string Shorten(std::string text, const std::size_t limit)
        {
            if (text.size() > limit)
            {
                std::size_t cut = limit;
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
                {
                    --cut;
                }
                text.resize(cut);
                text += "...";
            }
            return text;
        }

        json Schema(json properties, json required = json::array())
        {
            return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}};
        }

        json EntityIdProperties()
        {
            return {
                {"id", {{"type", "integer"}, {"description", "Entity id (from list_entities)"}}},
                {"name", {{"type", "string"}, {"description", "Entity name (Tag), used when id is not given; the first match is taken"}}},
            };
        }

        const json kVec3Schema = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}, {"description", "[x, y, z]"}};
    }

    AssistantTools::AssistantTools(AssistantToolContext context) : context_(std::move(context))
    {
        Register("get_mode", "Editor state: Edit or Play, selected entity, unsaved changes.", Schema(json::object()), false, &AssistantTools::GetMode);
        Register("list_entities",
            "List scene entities (id, name, position, components, parent). Use filter to search by name.",
            Schema({
                {"filter", {{"type", "string"}, {"description", "Case-insensitive part of the entity name"}}},
                {"limit", {{"type", "integer"}, {"description", "Maximum number of entities (default 100, at most 500)"}}},
            }),
            false, &AssistantTools::ListEntities);
        Register("get_entity",
            "Full data of one entity as in the scene file; in Play also the live values and status of its scripts.",
            Schema(EntityIdProperties()), false, &AssistantTools::GetEntity);
        Register("list_prefabs", "Names of the prefab templates in assets/prefabs.", Schema(json::object()), false, &AssistantTools::ListPrefabs);
        Register("get_prefab", "JSON of one prefab template.",
            Schema({{"name", {{"type", "string"}, {"description", "Prefab name without path and extension"}}}}, json::array({"name"})),
            false, &AssistantTools::GetPrefab);
        Register("list_scripts", "Script modules in assets/scripts with their Behaviour classes.", Schema(json::object()), false, &AssistantTools::ListScripts);
        Register("describe_script_fields", "Editable fields (name, type, default) of a script class.",
            Schema({
                {"module", {{"type", "string"}, {"description", "Module name, e.g. coin"}}},
                {"class", {{"type", "string"}, {"description", "Class name, e.g. Coin"}}},
            }, json::array({"module", "class"})),
            false, &AssistantTools::DescribeScriptFields);
        Register("get_recent_errors", "Last script errors (file, line, message). Check it after changing a script.", Schema(json::object()), false,
            &AssistantTools::GetRecentErrors);

        Register("spawn_prefab",
            "Create entities from a prefab in the scene (Edit mode). Give position for one copy or positions for several (at most 50).",
            Schema({
                {"name", {{"type", "string"}, {"description", "Prefab name, e.g. coin"}}},
                {"position", kVec3Schema},
                {"positions", {{"type", "array"}, {"items", kVec3Schema}, {"description", "One copy per position"}}},
            }, json::array({"name"})),
            true, &AssistantTools::SpawnPrefab);
        Register("create_entity",
            "Create an entity from scene-file components (Edit mode), e.g. {\"Transform\": {\"position\": [0,1,0]}, \"MeshRenderer\": {...}}.",
            Schema({
                {"name", {{"type", "string"}}},
                {"position", kVec3Schema},
                {"components", {{"type", "object"}, {"description", "Component name -> JSON in the scene file format (not Hierarchy)"}}},
            }),
            true, &AssistantTools::CreateEntity);
        Register("delete_entity", "Delete an entity with its children (Edit mode).", Schema(EntityIdProperties()), true, &AssistantTools::DeleteEntity);
        Register("set_script_props",
            "Change props of a script on an entity (Edit mode). Props are merged into the existing ones; unknown fields are rejected.",
            [] {
                auto properties = EntityIdProperties();
                properties["index"] = {{"type", "integer"}, {"description", "Script index in the entity's Script component (default 0)"}};
                properties["props"] = {{"type", "object"}, {"description", "Field name -> new value"}};
                return Schema(std::move(properties), json::array({"props"}));
            }(),
            true, &AssistantTools::SetScriptProps);
        Register("set_component",
            "Change fields of a component on an entity (Edit mode). The value is merged into the component JSON from the scene file.",
            [] {
                auto properties = EntityIdProperties();
                properties["component"] = {{"type", "string"}, {"description", "Transform, Tag, MeshRenderer, Rigidbody, Collider, Camera, CameraController, Motion, PlayerController or WindowBinding"}};
                properties["value"] = {{"type", "object"}, {"description", "Fields to change, e.g. {\"position\": [1,0,2]}"}};
                return Schema(std::move(properties), json::array({"component", "value"}));
            }(),
            true, &AssistantTools::SetComponent);
        Register("play", "Start the game (Edit -> Play).", Schema(json::object()), true, &AssistantTools::Play);
        Register("stop", "Stop the game and restore the scene (Play -> Edit).", Schema(json::object()), true, &AssistantTools::Stop);
        Register("save_scene", "Save the scene to its file (Edit mode).", Schema(json::object()), true, &AssistantTools::SaveScene);
    }

    void AssistantTools::Register(std::string name, std::string description, json schema, const bool mutates, const Handler handler)
    {
        Entry entry;
        entry.info.name = std::move(name);
        entry.info.description = std::move(description);
        entry.info.inputSchema = std::move(schema);
        entry.info.mutatesWorld = mutates;
        entry.handler = handler;
        entries_.push_back(std::move(entry));
    }

    std::vector<AssistantToolInfo> AssistantTools::List() const
    {
        std::vector<AssistantToolInfo> infos;
        infos.reserve(entries_.size());
        for (const auto& entry : entries_)
        {
            infos.push_back(entry.info);
        }
        return infos;
    }

    const AssistantToolInfo* AssistantTools::Find(const std::string_view name) const
    {
        for (const auto& entry : entries_)
        {
            if (entry.info.name == name)
            {
                return &entry.info;
            }
        }
        return nullptr;
    }

    AssistantToolResult AssistantTools::Call(const std::string_view name, const json& arguments)
    {
        for (const auto& entry : entries_)
        {
            if (entry.info.name != name)
            {
                continue;
            }
            if (context_.world == nullptr)
            {
                return Fail("The scene is not available.");
            }
            const json args = arguments.is_object() ? arguments : json::object();
            try
            {
                return (this->*entry.handler)(args);
            }
            catch (const json::exception& error)
            {
                return Fail(std::string("Invalid arguments: ") + error.what());
            }
            catch (const std::exception& error)
            {
                return Fail(std::string("The tool failed: ") + error.what());
            }
        }
        return Fail("Unknown tool: " + std::string(name));
    }

    bool AssistantTools::RequireEditMode(std::string& error) const
    {
        if (core::ServiceLocator::GetEditorRuntimeState().mode != editor::RuntimeMode::Edit)
        {
            error = "The scene can be changed only in Edit mode. Call stop first (the game is running).";
            return false;
        }
        return true;
    }

    ecs::EntityId AssistantTools::ResolveEntity(const json& arguments, const char* idKey, std::string& error) const
    {
        auto& world = *context_.world;
        if (arguments.contains(idKey) && arguments[idKey].is_number_integer())
        {
            const auto id = arguments[idKey].get<long long>();
            if (id > 0 && world.IsAlive(static_cast<ecs::EntityId>(id)))
            {
                return static_cast<ecs::EntityId>(id);
            }
            error = "No entity with id " + std::to_string(id) + ". Use list_entities to see the current ids.";
            return ecs::kInvalidEntity;
        }
        if (arguments.contains("name") && arguments["name"].is_string())
        {
            const auto name = arguments["name"].get<std::string>();
            auto entities = world.GetEntities();
            std::sort(entities.begin(), entities.end());
            for (const auto entity : entities)
            {
                if (EntityName(world, entity) == name)
                {
                    return entity;
                }
            }
            error = "No entity named '" + name + "'.";
            return ecs::kInvalidEntity;
        }
        error = "Give the entity id or name.";
        return ecs::kInvalidEntity;
    }

    std::string AssistantTools::Describe(const std::string_view name, const json& arguments) const
    {
        const json args = arguments.is_object() ? arguments : json::object();
        const auto entityLabel = [&]() -> std::string
        {
            if (args.contains("id") && args["id"].is_number_integer() && context_.world != nullptr)
            {
                const auto id = static_cast<ecs::EntityId>(args["id"].get<long long>());
                if (context_.world->IsAlive(id))
                {
                    return "#" + std::to_string(id) + " '" + EntityName(*context_.world, id) + "'";
                }
                return "#" + std::to_string(id);
            }
            if (args.contains("name") && args["name"].is_string())
            {
                return "'" + args["name"].get<std::string>() + "'";
            }
            return "an entity";
        };
        const auto positionText = [](const json& value) -> std::string
        {
            ecs::components::Vec3 position;
            if (!ParseVec3(value, position))
            {
                return {};
            }
            std::ostringstream text;
            text << "(" << position.x << ", " << position.y << ", " << position.z << ")";
            return text.str();
        };

        try
        {
            if (name == "spawn_prefab")
            {
                const auto prefab = args.value("name", std::string("?"));
                if (args.contains("positions") && args["positions"].is_array())
                {
                    std::string text = "Spawn prefab '" + prefab + "' x" + std::to_string(args["positions"].size()) + " at ";
                    std::size_t shown = 0;
                    for (const auto& position : args["positions"])
                    {
                        if (shown == 4)
                        {
                            text += ", ...";
                            break;
                        }
                        text += (shown++ == 0 ? "" : ", ") + positionText(position);
                    }
                    return text;
                }
                const auto at = args.contains("position") ? positionText(args["position"]) : std::string();
                return "Spawn prefab '" + prefab + "'" + (at.empty() ? std::string(" at its template position") : " at " + at);
            }
            if (name == "create_entity")
            {
                return "Create entity '" + args.value("name", std::string("Entity")) + "'";
            }
            if (name == "delete_entity")
            {
                return "Delete entity " + entityLabel() + " with its children";
            }
            if (name == "set_script_props")
            {
                return "Set script props on " + entityLabel();
            }
            if (name == "set_component")
            {
                return "Change component " + args.value("component", std::string("?")) + " of " + entityLabel();
            }
            if (name == "play")
            {
                return "Start the game (Play)";
            }
            if (name == "stop")
            {
                return "Stop the game and restore the scene";
            }
            if (name == "save_scene")
            {
                return "Save the scene to its file";
            }
        }
        catch (const json::exception&)
        {
            // fall through to the generic text
        }
        return std::string(name);
    }

    AssistantToolResult AssistantTools::GetMode(const json&)
    {
        const auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        json result = {
            {"mode", editorState.mode == editor::RuntimeMode::Edit ? "Edit" : "Play"},
            {"sceneDirty", editorState.sceneDirty},
            {"entityCount", context_.world->GetEntities().size()},
        };
        const auto selected = editorState.selectedEntity;
        if (selected != ecs::kInvalidEntity && context_.world->IsAlive(selected))
        {
            result["selected"] = {{"id", selected}, {"name", EntityName(*context_.world, selected)}};
        }
        return Done(std::move(result));
    }

    AssistantToolResult AssistantTools::ListEntities(const json& arguments)
    {
        auto& world = *context_.world;
        const auto filter = Lower(arguments.value("filter", std::string()));
        const auto limit = std::min<std::size_t>(
            arguments.contains("limit") && arguments["limit"].is_number_integer()
                ? static_cast<std::size_t>(std::max<long long>(1, arguments["limit"].get<long long>()))
                : kDefaultListed,
            kMaxListed);

        auto entities = world.GetEntities();
        std::sort(entities.begin(), entities.end());
        json list = json::array();
        std::size_t matched = 0;
        for (const auto entity : entities)
        {
            const auto name = EntityName(world, entity);
            if (!filter.empty() && Lower(name).find(filter) == std::string::npos)
            {
                continue;
            }
            ++matched;
            if (list.size() >= limit)
            {
                continue;
            }
            const auto serialized = scene::SerializeEntity(world, entity);
            json item = {{"id", entity}, {"name", name}};
            if (serialized.contains("Transform"))
            {
                item["position"] = serialized["Transform"].value("position", json::array());
            }
            json components = json::array();
            for (const auto& [key, value] : serialized.items())
            {
                if (key != "id" && key != "Tag")
                {
                    components.push_back(key);
                }
            }
            item["components"] = std::move(components);
            if (serialized.contains("Hierarchy") && serialized["Hierarchy"].value("parent", 0u) != 0u)
            {
                item["parent"] = serialized["Hierarchy"]["parent"];
            }
            list.push_back(std::move(item));
        }
        return Done({{"total", entities.size()}, {"matched", matched}, {"returned", list.size()}, {"entities", std::move(list)}});
    }

    AssistantToolResult AssistantTools::GetEntity(const json& arguments)
    {
        std::string error;
        const auto entity = ResolveEntity(arguments, "id", error);
        if (entity == ecs::kInvalidEntity)
        {
            return Fail(error);
        }
        json result = scene::SerializeEntity(*context_.world, entity);
        if (core::ServiceLocator::GetEditorRuntimeState().mode == editor::RuntimeMode::Play && result.contains("Script") &&
            result["Script"].contains("scripts") && result["Script"]["scripts"].is_array())
        {
            json live = json::array();
            for (std::size_t index = 0; index < result["Script"]["scripts"].size(); ++index)
            {
                json item = {{"index", index}};
                if (context_.scriptStatus)
                {
                    item["status"] = context_.scriptStatus(entity, index);
                }
                if (context_.liveScriptFields)
                {
                    item["fields"] = context_.liveScriptFields(entity, index);
                }
                live.push_back(std::move(item));
            }
            result["liveScripts"] = std::move(live);
        }
        return Done(std::move(result));
    }

    AssistantToolResult AssistantTools::ListPrefabs(const json&)
    {
        if (context_.prefabs == nullptr)
        {
            return Fail("The prefab library is not available.");
        }
        auto names = context_.prefabs->ListPrefabs();
        std::sort(names.begin(), names.end());
        return Done({{"prefabs", names}});
    }

    AssistantToolResult AssistantTools::GetPrefab(const json& arguments)
    {
        if (context_.prefabs == nullptr)
        {
            return Fail("The prefab library is not available.");
        }
        const auto name = arguments.at("name").get<std::string>();
        const auto* prefab = context_.prefabs->GetPrefabJson(name);
        if (prefab == nullptr)
        {
            return Fail("Prefab '" + name + "' was not found or is invalid: " + context_.prefabs->GetLastError());
        }
        return Done({{"name", name}, {"prefab", *prefab}});
    }

    AssistantToolResult AssistantTools::ListScripts(const json&)
    {
        json modules = json::array();
        std::error_code error;
        if (!context_.scriptsDirectory.empty() && std::filesystem::is_directory(context_.scriptsDirectory, error))
        {
            std::vector<std::filesystem::path> files;
            for (const auto& entry : std::filesystem::directory_iterator(context_.scriptsDirectory, error))
            {
                if (entry.is_regular_file(error) && entry.path().extension() == ".py" && entry.path().stem().string().rfind('_', 0) != 0)
                {
                    files.push_back(entry.path());
                }
            }
            std::sort(files.begin(), files.end());
            for (const auto& file : files)
            {
                json classes = json::array();
                std::ifstream stream(file, std::ios::binary);
                std::string line;
                while (std::getline(stream, line))
                {
                    if (line.rfind("class ", 0) == 0)
                    {
                        const auto end = line.find_first_of("(:", 6);
                        classes.push_back(line.substr(6, end == std::string::npos ? std::string::npos : end - 6));
                    }
                }
                modules.push_back({{"module", file.stem().string()}, {"file", "assets/scripts/" + file.filename().string()}, {"classes", std::move(classes)}});
            }
        }
        return Done({{"scripts", std::move(modules)}});
    }

    AssistantToolResult AssistantTools::DescribeScriptFields(const json& arguments)
    {
        if (!context_.describeScriptFields)
        {
            return Fail("Script fields are not available.");
        }
        const auto module = arguments.at("module").get<std::string>();
        const auto className = arguments.at("class").get<std::string>();
        const auto fields = context_.describeScriptFields(module, className);
        json list = json::array();
        for (const auto& field : fields)
        {
            const char* type = "float";
            switch (field.type)
            {
            case scripting::ScriptFieldInfo::Type::Float: type = "float"; break;
            case scripting::ScriptFieldInfo::Type::Int: type = "int"; break;
            case scripting::ScriptFieldInfo::Type::Bool: type = "bool"; break;
            case scripting::ScriptFieldInfo::Type::String: type = "string"; break;
            }
            list.push_back({{"name", field.name}, {"type", type}, {"default", field.defaultValue}});
        }
        if (list.empty())
        {
            return Fail("No fields found for " + module + "." + className + " (wrong module or class, or the script has a syntax error).");
        }
        return Done({{"module", module}, {"class", className}, {"fields", std::move(list)}});
    }

    AssistantToolResult AssistantTools::GetRecentErrors(const json&)
    {
        json list = json::array();
        if (context_.recentErrors)
        {
            const auto errors = context_.recentErrors();
            const std::size_t first = errors.size() > 10 ? errors.size() - 10 : 0;
            for (std::size_t index = errors.size(); index > first; --index)
            {
                const auto& error = errors[index - 1];
                list.push_back({{"file", error.file}, {"line", error.line}, {"time", error.time}, {"message", Shorten(error.message, kMaxErrorChars)}});
            }
        }
        return Done({{"errors", std::move(list)}});
    }

    AssistantToolResult AssistantTools::SpawnPrefab(const json& arguments)
    {
        std::string error;
        if (!RequireEditMode(error))
        {
            return Fail(error);
        }
        if (context_.prefabs == nullptr)
        {
            return Fail("The prefab library is not available.");
        }
        const auto name = arguments.at("name").get<std::string>();

        std::vector<ecs::components::Vec3> positions;
        bool hasPositions = false;
        if (arguments.contains("positions"))
        {
            if (!arguments["positions"].is_array() || arguments["positions"].empty() || arguments["positions"].size() > kMaxSpawn)
            {
                return Fail("positions must be a list of 1 to " + std::to_string(kMaxSpawn) + " [x, y, z] items.");
            }
            for (const auto& item : arguments["positions"])
            {
                ecs::components::Vec3 position;
                if (!ParseVec3(item, position))
                {
                    return Fail("Every position must be [x, y, z] with finite numbers.");
                }
                positions.push_back(position);
            }
            hasPositions = true;
        }
        else if (arguments.contains("position"))
        {
            ecs::components::Vec3 position;
            if (!ParseVec3(arguments["position"], position))
            {
                return Fail("position must be [x, y, z] with finite numbers.");
            }
            positions.push_back(position);
            hasPositions = true;
        }

        const std::string before = context_.captureSnapshot ? context_.captureSnapshot() : std::string();
        json spawned = json::array();
        const std::size_t count = hasPositions ? positions.size() : 1;
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto* position = hasPositions ? &positions[index] : nullptr;
            const auto entity = context_.prefabs->Instantiate(*context_.world, name, position);
            if (entity == ecs::kInvalidEntity)
            {
                const auto reason = context_.prefabs->GetLastError();
                if (context_.restoreSnapshot && !before.empty())
                {
                    context_.restoreSnapshot(before); // all or nothing
                }
                return Fail("Could not spawn prefab '" + name + "': " + (reason.empty() ? std::string("unknown prefab") : reason));
            }
            spawned.push_back({{"id", entity}, {"name", EntityName(*context_.world, entity)}});
        }
        if (context_.recordUndo && !before.empty())
        {
            context_.recordUndo("Assistant: Spawn " + name, before);
        }
        return Done({{"spawned", std::move(spawned)}});
    }

    AssistantToolResult AssistantTools::CreateEntity(const json& arguments)
    {
        std::string error;
        if (!RequireEditMode(error))
        {
            return Fail(error);
        }
        json data = json::object();
        if (arguments.contains("components"))
        {
            if (!arguments["components"].is_object())
            {
                return Fail("components must be an object: component name -> JSON.");
            }
            data = arguments["components"];
        }
        static const std::set<std::string> kAllowed = {
            "Tag", "Transform", "MeshRenderer", "Rigidbody", "Collider", "Camera", "CameraController", "Motion", "PlayerController", "WindowBinding", "Script"};
        for (const auto& [key, value] : data.items())
        {
            if (kAllowed.count(key) == 0 || !value.is_object())
            {
                return Fail("Unsupported component '" + key + "'. Allowed: Tag, Transform, MeshRenderer, Rigidbody, Collider, Camera, CameraController, Motion, PlayerController, WindowBinding, Script.");
            }
        }
        if (arguments.contains("name") && arguments["name"].is_string())
        {
            data["Tag"] = {{"name", arguments["name"].get<std::string>()}};
        }
        else if (!data.contains("Tag"))
        {
            data["Tag"] = {{"name", "Entity"}};
        }
        if (arguments.contains("position"))
        {
            ecs::components::Vec3 position;
            if (!ParseVec3(arguments["position"], position))
            {
                return Fail("position must be [x, y, z] with finite numbers.");
            }
            if (!data.contains("Transform"))
            {
                data["Transform"] = json::object();
            }
            data["Transform"]["position"] = Vec3ToJson(position);
        }

        const std::string before = context_.captureSnapshot ? context_.captureSnapshot() : std::string();
        auto& world = *context_.world;
        const auto entity = world.CreateEntity();
        try
        {
            scene::DeserializeEntity(world, entity, data);
        }
        catch (const std::exception& failure)
        {
            world.DestroyEntity(entity);
            return Fail(std::string("The entity data is invalid: ") + failure.what());
        }
        if (context_.recordUndo && !before.empty())
        {
            context_.recordUndo("Assistant: Create Entity", before);
        }
        return Done({{"created", {{"id", entity}, {"name", EntityName(world, entity)}}}});
    }

    AssistantToolResult AssistantTools::DeleteEntity(const json& arguments)
    {
        std::string error;
        if (!RequireEditMode(error))
        {
            return Fail(error);
        }
        const auto entity = ResolveEntity(arguments, "id", error);
        if (entity == ecs::kInvalidEntity)
        {
            return Fail(error);
        }
        const auto name = EntityName(*context_.world, entity);
        const std::string before = context_.captureSnapshot ? context_.captureSnapshot() : std::string();
        if (!context_.world->DestroyEntity(entity))
        {
            return Fail("The entity could not be deleted.");
        }
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (editorState.selectedEntity == entity || !context_.world->IsAlive(editorState.selectedEntity))
        {
            editorState.selectedEntity = ecs::kInvalidEntity;
        }
        if (context_.recordUndo && !before.empty())
        {
            context_.recordUndo("Assistant: Delete Entity", before);
        }
        return Done({{"deleted", {{"id", entity}, {"name", name}}}});
    }

    AssistantToolResult AssistantTools::SetScriptProps(const json& arguments)
    {
        std::string error;
        if (!RequireEditMode(error))
        {
            return Fail(error);
        }
        const auto entity = ResolveEntity(arguments, "id", error);
        if (entity == ecs::kInvalidEntity)
        {
            return Fail(error);
        }
        if (!arguments.contains("props") || !arguments["props"].is_object() || arguments["props"].empty())
        {
            return Fail("props must be a non-empty object: field name -> value.");
        }
        auto* script = context_.world->TryGet<ecs::components::ScriptComponent>(entity);
        if (script == nullptr || script->scripts.empty())
        {
            return Fail("The entity has no script.");
        }
        const long long requested = arguments.contains("index") && arguments["index"].is_number_integer() ? arguments["index"].get<long long>() : 0;
        if (requested < 0 || static_cast<std::size_t>(requested) >= script->scripts.size())
        {
            return Fail("Script index " + std::to_string(requested) + " is out of range (the entity has " + std::to_string(script->scripts.size()) + ").");
        }
        auto& entry = script->scripts[static_cast<std::size_t>(requested)];

        // Fields are checked against the class so a typo does not silently become a dead prop
        if (context_.describeScriptFields)
        {
            const auto fields = context_.describeScriptFields(entry.module, entry.className);
            if (!fields.empty())
            {
                for (const auto& [key, value] : arguments["props"].items())
                {
                    const auto field = std::find_if(fields.begin(), fields.end(), [&](const scripting::ScriptFieldInfo& item) { return item.name == key; });
                    if (field == fields.end())
                    {
                        std::string known;
                        for (const auto& item : fields)
                        {
                            known += (known.empty() ? "" : ", ") + item.name;
                        }
                        return Fail("Script " + entry.module + "." + entry.className + " has no field '" + key + "'. Fields: " + known + ".");
                    }
                    const bool numeric = field->type == scripting::ScriptFieldInfo::Type::Float || field->type == scripting::ScriptFieldInfo::Type::Int;
                    const bool matches =
                        (numeric && value.is_number()) ||
                        (field->type == scripting::ScriptFieldInfo::Type::Bool && value.is_boolean()) ||
                        (field->type == scripting::ScriptFieldInfo::Type::String && value.is_string());
                    if (!matches)
                    {
                        return Fail("Field '" + key + "' has a different type (expected " + (numeric ? "a number" : field->type == scripting::ScriptFieldInfo::Type::Bool ? "true/false" : "a string") + ").");
                    }
                }
            }
        }

        const std::string before = context_.captureSnapshot ? context_.captureSnapshot() : std::string();
        if (!entry.props.is_object())
        {
            entry.props = json::object();
        }
        for (const auto& [key, value] : arguments["props"].items())
        {
            entry.props[key] = value;
        }
        if (context_.recordUndo && !before.empty())
        {
            context_.recordUndo("Assistant: Edit Script Props", before);
        }
        return Done({{"id", entity}, {"index", requested}, {"props", entry.props}});
    }

    AssistantToolResult AssistantTools::SetComponent(const json& arguments)
    {
        std::string error;
        if (!RequireEditMode(error))
        {
            return Fail(error);
        }
        const auto entity = ResolveEntity(arguments, "id", error);
        if (entity == ecs::kInvalidEntity)
        {
            return Fail(error);
        }
        const auto component = arguments.at("component").get<std::string>();
        static const std::set<std::string> kAllowed = {
            "Tag", "Transform", "MeshRenderer", "Rigidbody", "Collider", "Camera", "CameraController", "Motion", "PlayerController", "WindowBinding"};
        if (kAllowed.count(component) == 0)
        {
            return Fail("Unsupported component '" + component + "'. Hierarchy and Script have their own tools (set_script_props).");
        }
        if (!arguments.contains("value") || !arguments["value"].is_object() || arguments["value"].empty())
        {
            return Fail("value must be a non-empty object with the fields to change.");
        }

        const auto current = scene::SerializeEntity(*context_.world, entity);
        json merged = current.contains(component) ? current[component] : json::object();
        for (const auto& [key, value] : arguments["value"].items())
        {
            merged[key] = value;
        }

        const std::string before = context_.captureSnapshot ? context_.captureSnapshot() : std::string();
        try
        {
            scene::DeserializeEntity(*context_.world, entity, json{{component, merged}});
        }
        catch (const std::exception& failure)
        {
            if (context_.restoreSnapshot && !before.empty())
            {
                context_.restoreSnapshot(before);
            }
            return Fail(std::string("The component data is invalid: ") + failure.what());
        }
        if (context_.recordUndo && !before.empty())
        {
            context_.recordUndo("Assistant: Edit " + component, before);
        }
        const auto after = scene::SerializeEntity(*context_.world, entity);
        return Done({{"id", entity}, {"component", component}, {"value", after.contains(component) ? after[component] : json::object()}});
    }

    AssistantToolResult AssistantTools::Play(const json&)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (editorState.mode != editor::RuntimeMode::Edit)
        {
            return Fail("The game is already running.");
        }
        if (!context_.captureSnapshot)
        {
            return Fail("Play is not available.");
        }
        editorState.playModeSnapshot = context_.captureSnapshot();
        editorState.mode = editor::RuntimeMode::Play;
        core::ServiceLocator::GetPhysicsWorldState().physicsPaused = false;
        return Done({{"mode", "Play"}});
    }

    AssistantToolResult AssistantTools::Stop(const json&)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (editorState.mode != editor::RuntimeMode::Play)
        {
            return Fail("The game is not running.");
        }
        const bool restored = editorState.playModeSnapshot.empty() || (context_.restoreSnapshot && context_.restoreSnapshot(editorState.playModeSnapshot));
        if (!restored)
        {
            return Fail("The scene could not be restored.");
        }
        editorState.mode = editor::RuntimeMode::Edit;
        core::ServiceLocator::GetPhysicsWorldState().physicsPaused = true;
        editorState.playModeSnapshot.clear();
        return Done({{"mode", "Edit"}});
    }

    AssistantToolResult AssistantTools::SaveScene(const json&)
    {
        std::string error;
        if (!RequireEditMode(error))
        {
            return Fail(error);
        }
        if (!context_.saveScene || !context_.saveScene())
        {
            return Fail("The scene could not be saved.");
        }
        core::ServiceLocator::GetEditorRuntimeState().sceneDirty = false;
        return Done({{"saved", true}});
    }
}
