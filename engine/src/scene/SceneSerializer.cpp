// SceneSerializer.cpp

#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <myengine/core/Logger.h>
#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/CameraControllerComponent.h>
#include <myengine/ecs/components/ColliderComponent.h>
#include <myengine/ecs/components/HierarchyComponent.h>
#include <myengine/ecs/components/MeshRendererComponent.h>
#include <myengine/ecs/components/MotionComponent.h>
#include <myengine/ecs/components/PlayerControllerComponent.h>
#include <myengine/ecs/components/RigidbodyComponent.h>
#include <myengine/ecs/components/ScriptComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/components/WindowBindingComponent.h>
#include <myengine/scene/SceneEvents.h>
#include <myengine/scene/SceneSerializer.h>

namespace myengine::scene
{
    namespace
    {
        using json = nlohmann::json;

        void Log(core::Logger* logger, const std::string& message)
        {
            if (logger != nullptr)
            {
                logger->Info(message);
            }
        }

        void LogWarning(core::Logger* logger, const std::string& message)
        {
            if (logger != nullptr)
            {
                logger->Warning(message);
            }
        }

        json VecToJson(const ecs::components::Vec3& value)
        {
            return json::array({value.x, value.y, value.z});
        }

        ecs::components::Vec3 VecFromJson(const json& value, const ecs::components::Vec3& fallback = {})
        {
            if (!value.is_array() || value.size() != 3)
            {
                return fallback;
            }

            ecs::components::Vec3 result = fallback;
            result.x = value[0].get<float>();
            result.y = value[1].get<float>();
            result.z = value[2].get<float>();
            return result;
        }

        json SerializeWorld(const ecs::World& world)
        {
            json root;
            root["entities"] = json::array();

            for (const ecs::EntityId entity : world.GetEntities())
            {
                root["entities"].push_back(SerializeEntity(world, entity));
            }

            return root;
        }

        bool DeserializeWorld(ecs::World& world, const json& root, core::Logger* logger)
        {
            if (!root.contains("entities") || !root["entities"].is_array())
            {
                LogWarning(logger, "SceneSerializer: invalid scene JSON (entities array is missing)");
                return false;
            }

            // Invalid component data must not leave the live world half-loaded (or save it on exit).
            ecs::World loadedWorld;

            std::vector<std::pair<ecs::EntityId, ecs::EntityId>> hierarchyLinks;

            for (const auto& entityJson : root["entities"])
            {
                if (!entityJson.contains("id"))
                {
                    LogWarning(logger, "SceneSerializer: entity id is missing");
                    return false;
                }

                const auto& idJson = entityJson["id"];
                if (!idJson.is_number_integer())
                {
                    LogWarning(logger, "SceneSerializer: entity id must be an integer");
                    return false;
                }
                const auto id = idJson.get<std::uint64_t>();
                if (id == ecs::kInvalidEntity || id >= std::numeric_limits<ecs::EntityId>::max())
                {
                    LogWarning(logger, "SceneSerializer: entity id is out of range");
                    return false;
                }
                const auto entity = static_cast<ecs::EntityId>(id);
                if (loadedWorld.IsAlive(entity))
                {
                    LogWarning(logger, "SceneSerializer: invalid or duplicate entity id");
                    return false;
                }
                loadedWorld.CreateEntityWithId(entity);
            }

            for (const auto& entityJson : root["entities"])
            {
                const auto entity = static_cast<ecs::EntityId>(entityJson["id"].get<std::uint32_t>());

                DeserializeEntity(loadedWorld, entity, entityJson);

                if (entityJson.contains("Hierarchy"))
                {
                    const auto parent = static_cast<ecs::EntityId>(entityJson["Hierarchy"].value("parent", 0u));
                    if (parent != ecs::kInvalidEntity)
                    {
                        hierarchyLinks.emplace_back(entity, parent);
                    }
                }
            }

            for (const auto& [child, parent] : hierarchyLinks)
            {
                loadedWorld.SetParent(child, parent);
            }

            world.SwapEntities(loadedWorld);
            return true;
        }
    }

    json SerializeEntity(const ecs::World& world, const ecs::EntityId entity)
    {
        if (!world.IsAlive(entity))
        {
            throw std::invalid_argument("Cannot serialize a dead entity");
        }

        json entityJson;
        entityJson["id"] = entity;

        if (const auto* tag = world.TryGet<ecs::components::TagComponent>(entity); tag != nullptr)
        {
            entityJson["Tag"] = {{"name", tag->name}};
        }

        if (const auto* transform = world.TryGet<ecs::components::TransformComponent>(entity); transform != nullptr)
        {
            entityJson["Transform"] =
            {
                {"position", VecToJson(transform->position)},
                {"rotationDeg", VecToJson(transform->rotationDeg)},
                {"scale", VecToJson(transform->scale)},
            };
        }

        if (const auto* renderer = world.TryGet<ecs::components::MeshRendererComponent>(entity); renderer != nullptr)
        {
            entityJson["MeshRenderer"] =
            {
                {"meshPath", renderer->meshPath},
                {"materialPath", renderer->materialPath},
                {"visible", renderer->visible},
            };
        }

        if (const auto* hierarchy = world.TryGet<ecs::components::HierarchyComponent>(entity); hierarchy != nullptr)
        {
            entityJson["Hierarchy"] =
            {
                {"parent", hierarchy->parent},
                {"children", hierarchy->children},
            };
        }

        if (const auto* motion = world.TryGet<ecs::components::MotionComponent>(entity); motion != nullptr)
        {
            entityJson["Motion"] =
            {
                {"linearVelocity", VecToJson(motion->linearVelocity)},
                {"angularVelocityDeg", VecToJson(motion->angularVelocityDeg)},
            };
        }

        if (const auto* rigidbody = world.TryGet<ecs::components::RigidbodyComponent>(entity); rigidbody != nullptr)
        {
            entityJson["Rigidbody"] =
            {
                {"velocity", VecToJson(rigidbody->velocity)},
                {"acceleration", VecToJson(rigidbody->acceleration)},
                {"mass", rigidbody->mass},
                {"gravityScale", rigidbody->gravityScale},
                {"linearDamping", rigidbody->linearDamping},
                {"sleepLinearSpeed", rigidbody->sleepLinearSpeed},
                {"useGravity", rigidbody->useGravity},
                {"isKinematic", rigidbody->isKinematic},
            };
        }

        if (const auto* collider = world.TryGet<ecs::components::ColliderComponent>(entity); collider != nullptr)
        {
            entityJson["Collider"] =
            {
                {"type", collider->type == ecs::components::ColliderType::Sphere ? "sphere" : "box"},
                {"halfExtents", VecToJson(collider->halfExtents)},
                {"radius", collider->radius},
                {"offset", VecToJson(collider->offset)},
                {"friction", collider->friction},
                {"bounciness", collider->bounciness},
                {"isTrigger", collider->isTrigger},
            };
        }

        if (const auto* controller = world.TryGet<ecs::components::PlayerControllerComponent>(entity); controller != nullptr)
        {
            entityJson["PlayerController"] =
            {
                {"windowId", controller->windowId},
                {"moveSpeed", controller->moveSpeed},
                {"jumpSpeed", controller->jumpSpeed},
                {"airControl", controller->airControl},
            };
        }

        if (const auto* binding = world.TryGet<ecs::components::WindowBindingComponent>(entity); binding != nullptr)
        {
            entityJson["WindowBinding"] = {{"windowId", binding->windowId}};
        }

        if (const auto* camera = world.TryGet<ecs::components::CameraComponent>(entity); camera != nullptr)
        {
            entityJson["Camera"] =
            {
                {"position", VecToJson(camera->position)},
                {"rotationDeg", VecToJson(camera->rotationDeg)},
                {"fovYDeg", camera->fovYDeg},
                {"orthographicHalfHeight", camera->orthographicHalfHeight},
                {"nearPlane", camera->nearPlane},
                {"farPlane", camera->farPlane},
                {"isPrimary", camera->isPrimary},
            };
        }

        if (const auto* controller = world.TryGet<ecs::components::CameraControllerComponent>(entity); controller != nullptr)
        {
            entityJson["CameraController"] =
            {
                {"moveSpeed", controller->moveSpeed},
                {"zoomSpeed", controller->zoomSpeed},
                {"rotateSpeedDeg", controller->rotateSpeedDeg},
                {"mouseSensitivityDeg", controller->mouseSensitivityDeg},
            };
        }

        if (const auto* script = world.TryGet<ecs::components::ScriptComponent>(entity); script != nullptr)
        {
            json scripts = json::array();
            for (const auto& entry : script->scripts)
            {
                scripts.push_back({
                    {"module", entry.module},
                    {"class", entry.className},
                    {"props", entry.props},
                });
            }
            entityJson["Script"] = {{"scripts", std::move(scripts)}};
        }

        return entityJson;
    }

    void DeserializeEntity(ecs::World& world, const ecs::EntityId entity, const json& entityJson)
    {
        if (!world.IsAlive(entity) || !entityJson.is_object())
        {
            throw std::invalid_argument("DeserializeEntity requires a live entity and a JSON object");
        }

        if (entityJson.contains("Tag"))
        {
            auto& tag = world.Emplace<ecs::components::TagComponent>(entity);
            tag.name = entityJson["Tag"].value("name", std::string());
        }

        if (entityJson.contains("Transform"))
        {
            auto& transform = world.Emplace<ecs::components::TransformComponent>(entity);
            const auto& transformJson = entityJson["Transform"];
            transform.position = VecFromJson(transformJson.value("position", json::array()));
            transform.rotationDeg = VecFromJson(transformJson.value("rotationDeg", json::array()));
            transform.scale = VecFromJson(transformJson.value("scale", json::array()), ecs::components::Vec3{1.0f, 1.0f, 1.0f});
        }

        if (entityJson.contains("MeshRenderer"))
        {
            auto& renderer = world.Emplace<ecs::components::MeshRendererComponent>(entity);
            const auto& rendererJson = entityJson["MeshRenderer"];
            renderer.meshPath = rendererJson.value("meshPath", std::string());
            renderer.materialPath = rendererJson.value("materialPath", std::string());
            renderer.visible = rendererJson.value("visible", true);
        }

        if (entityJson.contains("Motion"))
        {
            auto& motion = world.Emplace<ecs::components::MotionComponent>(entity);
            const auto& motionJson = entityJson["Motion"];
            motion.linearVelocity = VecFromJson(motionJson.value("linearVelocity", json::array()));
            motion.angularVelocityDeg = VecFromJson(motionJson.value("angularVelocityDeg", json::array()));
        }

        if (entityJson.contains("Rigidbody"))
        {
            auto& rigidbody = world.Emplace<ecs::components::RigidbodyComponent>(entity);
            const auto& rigidbodyJson = entityJson["Rigidbody"];
            rigidbody.velocity = VecFromJson(rigidbodyJson.value("velocity", json::array()));
            rigidbody.acceleration = VecFromJson(rigidbodyJson.value("acceleration", json::array()));
            rigidbody.mass = rigidbodyJson.value("mass", 1.0f);
            rigidbody.gravityScale = rigidbodyJson.value("gravityScale", 1.0f);
            rigidbody.linearDamping = rigidbodyJson.value("linearDamping", 0.14f);
            rigidbody.sleepLinearSpeed = rigidbodyJson.value("sleepLinearSpeed", 0.08f);
            rigidbody.useGravity = rigidbodyJson.value("useGravity", true);
            rigidbody.isKinematic = rigidbodyJson.value("isKinematic", false);
        }

        if (entityJson.contains("Collider"))
        {
            auto& collider = world.Emplace<ecs::components::ColliderComponent>(entity);
            const auto& colliderJson = entityJson["Collider"];
            const std::string colliderType = colliderJson.value("type", std::string("box"));
            collider.type = colliderType == "sphere" ? ecs::components::ColliderType::Sphere : ecs::components::ColliderType::Box;
            collider.halfExtents = VecFromJson(colliderJson.value("halfExtents", json::array()), ecs::components::Vec3{0.5f, 0.5f, 0.5f});
            collider.radius = colliderJson.value("radius", 0.5f);
            collider.offset = VecFromJson(colliderJson.value("offset", json::array()));
            collider.friction = colliderJson.value("friction", 0.65f);
            collider.bounciness = colliderJson.value("bounciness", 0.12f);
            collider.isTrigger = colliderJson.value("isTrigger", false);
        }

        if (entityJson.contains("PlayerController"))
        {
            auto& controller = world.Emplace<ecs::components::PlayerControllerComponent>(entity);
            const auto& controllerJson = entityJson["PlayerController"];
            controller.windowId = controllerJson.value("windowId", 0u);
            controller.moveSpeed = controllerJson.value("moveSpeed", 4.5f);
            controller.jumpSpeed = controllerJson.value("jumpSpeed", 6.25f);
            controller.airControl = controllerJson.value("airControl", 0.45f);
        }

        if (entityJson.contains("WindowBinding"))
        {
            auto& binding = world.Emplace<ecs::components::WindowBindingComponent>(entity);
            binding.windowId = entityJson["WindowBinding"].value("windowId", 0u);
        }

        if (entityJson.contains("Camera"))
        {
            auto& camera = world.Emplace<ecs::components::CameraComponent>(entity);
            const auto& cameraJson = entityJson["Camera"];
            camera.position = VecFromJson(cameraJson.value("position", json::array()));
            camera.rotationDeg = VecFromJson(cameraJson.value("rotationDeg", json::array()));
            camera.fovYDeg = cameraJson.value("fovYDeg", 60.0f);
            camera.orthographicHalfHeight = cameraJson.value("orthographicHalfHeight", 1.0f);
            camera.nearPlane = cameraJson.value("nearPlane", 0.01f);
            camera.farPlane = cameraJson.value("farPlane", 200.0f);
            camera.isPrimary = cameraJson.value("isPrimary", true);
        }

        if (entityJson.contains("CameraController"))
        {
            auto& controller = world.Emplace<ecs::components::CameraControllerComponent>(entity);
            const auto& controllerJson = entityJson["CameraController"];
            controller.moveSpeed = controllerJson.value("moveSpeed", 1.4f);
            controller.zoomSpeed = controllerJson.value("zoomSpeed", 1.0f);
            controller.rotateSpeedDeg = controllerJson.value("rotateSpeedDeg", 80.0f);
            controller.mouseSensitivityDeg = controllerJson.value("mouseSensitivityDeg", 0.12f);
        }

        if (entityJson.contains("Script"))
        {
            auto& script = world.Emplace<ecs::components::ScriptComponent>(entity);
            const auto& scriptJson = entityJson["Script"];
            const auto& scripts = scriptJson.at("scripts");
            if (!scripts.is_array())
            {
                throw std::invalid_argument("Script.scripts must be an array");
            }

            for (const auto& entryJson : scripts)
            {
                ecs::components::ScriptComponent::Entry entry;
                entry.module = entryJson.value("module", std::string());
                entry.className = entryJson.value("class", std::string());
                entry.props = entryJson.value("props", json::object());
                if (!entry.props.is_object())
                {
                    throw std::invalid_argument("Script.props must be an object");
                }
                script.scripts.push_back(std::move(entry));
            }
        }

        // Parent ids may be local to a prefab. The caller resolves links after creating all entities.
        if (entityJson.contains("Hierarchy") && !world.Has<ecs::components::HierarchyComponent>(entity))
        {
            world.Emplace<ecs::components::HierarchyComponent>(entity);
        }
    }

    bool SaveWorldToJson(const ecs::World& world, const std::filesystem::path& path, core::Logger* logger)
    {
        const json root = SerializeWorld(world);

        try
        {
            if (!path.parent_path().empty())
            {
                std::filesystem::create_directories(path.parent_path());
            }
            std::ofstream file(path);
            if (!file.is_open())
            {
                LogWarning(logger, "SceneSerializer: failed to open file for save: " + path.u8string());
                return false;
            }

            file << root.dump(2);
            Log(logger, "SceneSerializer: scene saved to " + path.u8string());
            return true;
        }
        catch (const std::exception& ex)
        {
            LogWarning(logger, "SceneSerializer: save failed: " + std::string(ex.what()));
            return false;
        }
    }

    bool LoadWorldFromJson(ecs::World& world, const std::filesystem::path& path, core::Logger* logger)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            LogWarning(logger, "SceneSerializer: failed to open file for load: " + path.u8string());
            return false;
        }

        try
        {
            json root;
            file >> root;
            if (!DeserializeWorld(world, root, logger))
            {
                return false;
            }

            Log(logger, "SceneSerializer: scene loaded from " + path.u8string());
            core::ServiceLocator::GetEventBus().Publish(SceneLoadedEvent{&world});
            return true;
        }
        catch (const std::exception& ex)
        {
            LogWarning(logger, "SceneSerializer: load failed: " + std::string(ex.what()));
            return false;
        }
    }

    std::string SerializeWorldToString(const ecs::World& world)
    {
        return SerializeWorld(world).dump(2);
    }

    bool LoadWorldFromString(ecs::World& world, const std::string_view jsonText, core::Logger* logger)
    {
        try
        {
            std::istringstream stream{std::string(jsonText)};
            json root;
            stream >> root;

            if (!DeserializeWorld(world, root, logger))
            {
                return false;
            }

            Log(logger, "SceneSerializer: scene restored from in-memory snapshot");
            core::ServiceLocator::GetEventBus().Publish(SceneLoadedEvent{&world});
            return true;
        }
        catch (const std::exception& ex)
        {
            LogWarning(logger, "SceneSerializer: snapshot restore failed: " + std::string(ex.what()));
            return false;
        }
    }
}
