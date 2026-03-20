// SceneSerializer.cpp

#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <myengine/core/Logger.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/CameraControllerComponent.h>
#include <myengine/ecs/components/HierarchyComponent.h>
#include <myengine/ecs/components/MeshRendererComponent.h>
#include <myengine/ecs/components/MotionComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/components/WindowBindingComponent.h>
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
    }

    bool SaveWorldToJson(const ecs::World& world, const std::filesystem::path& path, core::Logger* logger)
    {
        json root;
        root["entities"] = json::array();

        for (const ecs::EntityId entity : world.GetEntities())
        {
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

            root["entities"].push_back(std::move(entityJson));
        }

        try
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file(path);
            if (!file.is_open())
            {
                LogWarning(logger, "SceneSerializer: failed to open file for save: " + path.string());
                return false;
            }

            file << root.dump(2);
            Log(logger, "SceneSerializer: scene saved to " + path.string());
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
            LogWarning(logger, "SceneSerializer: failed to open file for load: " + path.string());
            return false;
        }

        try
        {
            json root;
            file >> root;

            if (!root.contains("entities") || !root["entities"].is_array())
            {
                LogWarning(logger, "SceneSerializer: invalid scene JSON (entities array is missing)");
                return false;
            }

            world.ClearEntities();

            std::vector<std::pair<ecs::EntityId, ecs::EntityId>> hierarchyLinks;

            for (const auto& entityJson : root["entities"])
            {
                if (!entityJson.contains("id"))
                {
                    continue;
                }

                const auto entity = static_cast<ecs::EntityId>(entityJson["id"].get<std::uint32_t>());
                world.CreateEntityWithId(entity);
            }

            for (const auto& entityJson : root["entities"])
            {
                if (!entityJson.contains("id"))
                {
                    continue;
                }

                const auto entity = static_cast<ecs::EntityId>(entityJson["id"].get<std::uint32_t>());

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
                world.SetParent(child, parent);
            }

            Log(logger, "SceneSerializer: scene loaded from " + path.string());
            return true;
        }
        catch (const std::exception& ex)
        {
            LogWarning(logger, "SceneSerializer: load failed: " + std::string(ex.what()));
            return false;
        }
    }
}