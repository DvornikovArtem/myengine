// AssistantTools.h

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <myengine/ecs/Entity.h>
#include <myengine/scripting/ScriptRuntime.h>
#include <myengine/scripting/ScriptSystem.h>

namespace myengine::ecs
{
    class World;
}

namespace myengine::scene
{
    class PrefabLibrary;
}

namespace myengine::assistant
{
    // What the tools may touch. The editor fills it; tests use a headless world.
    struct AssistantToolContext
    {
        ecs::World* world = nullptr;
        scene::PrefabLibrary* prefabs = nullptr;
        std::filesystem::path scriptsDirectory; // assets/scripts
        std::function<std::string()> captureSnapshot; // whole-scene snapshot (same as the editor's undo uses)
        std::function<bool(std::string_view)> restoreSnapshot;
        // Puts "before -> current scene" on the editor's undo stack
        std::function<void(const std::string& label, const std::string& before)> recordUndo;
        std::function<bool()> saveScene;
        std::function<std::vector<scripting::ScriptFieldInfo>(const std::string& module, const std::string& className)> describeScriptFields;
        std::function<std::vector<scripting::ScriptError>()> recentErrors;
        std::function<std::string(ecs::EntityId entity, std::size_t scriptIndex)> scriptStatus;
        std::function<nlohmann::json(ecs::EntityId entity, std::size_t scriptIndex)> liveScriptFields;
    };

    struct AssistantToolInfo
    {
        std::string name;
        std::string description;
        nlohmann::json inputSchema;
        bool mutatesWorld = false; // needs a confirmation in the panel before it runs
    };

    struct AssistantToolResult
    {
        bool ok = true;
        nlohmann::json value = nlohmann::json::object();
        std::string message; // the reason when !ok
    };

    // Registry of the engine tools the assistant can call. Main thread only.
    // Read tools never change anything. Mutating tools work in Edit mode only, change the scene through
    // snapshots and leave one undo step (Ctrl+Z) per call.
    class AssistantTools
    {
    public:
        explicit AssistantTools(AssistantToolContext context);

        std::vector<AssistantToolInfo> List() const;
        const AssistantToolInfo* Find(std::string_view name) const;
        AssistantToolResult Call(std::string_view name, const nlohmann::json& arguments);
        // One line for a confirmation card
        std::string Describe(std::string_view name, const nlohmann::json& arguments) const;

    private:
        using Handler = AssistantToolResult (AssistantTools::*)(const nlohmann::json&);

        struct Entry
        {
            AssistantToolInfo info;
            Handler handler = nullptr;
        };

        void Register(std::string name, std::string description, nlohmann::json schema, bool mutates, Handler handler);

        AssistantToolResult GetMode(const nlohmann::json& arguments);
        AssistantToolResult ListEntities(const nlohmann::json& arguments);
        AssistantToolResult GetEntity(const nlohmann::json& arguments);
        AssistantToolResult ListPrefabs(const nlohmann::json& arguments);
        AssistantToolResult GetPrefab(const nlohmann::json& arguments);
        AssistantToolResult ListScripts(const nlohmann::json& arguments);
        AssistantToolResult DescribeScriptFields(const nlohmann::json& arguments);
        AssistantToolResult GetRecentErrors(const nlohmann::json& arguments);
        AssistantToolResult SpawnPrefab(const nlohmann::json& arguments);
        AssistantToolResult CreateEntity(const nlohmann::json& arguments);
        AssistantToolResult DeleteEntity(const nlohmann::json& arguments);
        AssistantToolResult SetScriptProps(const nlohmann::json& arguments);
        AssistantToolResult SetComponent(const nlohmann::json& arguments);
        AssistantToolResult Play(const nlohmann::json& arguments);
        AssistantToolResult Stop(const nlohmann::json& arguments);
        AssistantToolResult SaveScene(const nlohmann::json& arguments);

        ecs::EntityId ResolveEntity(const nlohmann::json& arguments, const char* idKey, std::string& error) const;
        bool RequireEditMode(std::string& error) const;

        AssistantToolContext context_;
        std::vector<Entry> entries_;
    };
}
