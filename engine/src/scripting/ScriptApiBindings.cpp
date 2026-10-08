// ScriptApiBindings.cpp
// Script -> engine API. Proxies keep a handle, never a pointer into component storage.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <pybind11/stl.h>

#include <myengine/ecs/World.h>
#include <myengine/ecs/components/ColliderComponent.h>
#include <myengine/ecs/components/MeshRendererComponent.h>
#include <myengine/ecs/components/RigidbodyComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/input/InputManager.h>
#include <myengine/scene/TransformUtils.h>
#include <myengine/scripting/ScriptSystem.h>

#include "ScriptApi.h"

namespace py = pybind11;

namespace myengine::scripting::detail
{
    namespace
    {
        using ecs::components::Vec3;
        using ecs::components::TransformComponent;
        using ecs::components::RigidbodyComponent;
        using ecs::components::ColliderComponent;
        using ecs::components::MeshRendererComponent;
        using ecs::components::TagComponent;

        struct TransformRef
        {
            EntityRef entity;
        };
        struct RigidbodyRef
        {
            EntityRef entity;
        };
        struct ColliderRef
        {
            EntityRef entity;
        };
        struct MeshRendererRef
        {
            EntityRef entity;
        };
        struct TimeRef {};

        ecs::World& RequireWorld()
        {
            MYENGINE_ASSERT_SCRIPT_THREAD();
            auto* world = GetScriptContext().world;
            if (world == nullptr)
            {
                throw std::runtime_error("The script world is not available before the first frame or after shutdown");
            }
            return *world;
        }

        bool IsAlive(const EntityRef& entity)
        {
            MYENGINE_ASSERT_SCRIPT_THREAD();
            const auto& context = GetScriptContext();
            return context.world != nullptr && entity.sceneVersion == context.sceneVersion && context.world->IsAlive(entity.id);
        }

        ecs::World& RequireAlive(const EntityRef& entity)
        {
            if (!IsAlive(entity))
            {
                throw EntityDeadError("Entity " + std::to_string(entity.id) + " is dead or belongs to an unloaded scene");
            }
            return RequireWorld();
        }

        template <class Component>
        Component& RequireComponent(const EntityRef& entity, const char* name)
        {
            auto* component = RequireAlive(entity).TryGet<Component>(entity.id);
            if (component == nullptr)
            {
                throw py::attribute_error("Entity " + std::to_string(entity.id) + " has no " + name + " component");
            }
            return *component;
        }

        ScriptSystem& RequireSystem()
        {
            MYENGINE_ASSERT_SCRIPT_THREAD();
            auto* system = GetScriptContext().system;
            if (system == nullptr)
            {
                throw std::runtime_error("The script system is not available");
            }
            return *system;
        }

        input::InputManager& RequireInput()
        {
            MYENGINE_ASSERT_SCRIPT_THREAD();
            auto* input = GetScriptContext().input;
            if (input == nullptr)
            {
                throw std::runtime_error("Script input is not available before the first frame or after shutdown");
            }
            return *input;
        }

        void CheckVector(const Vec3& value)
        {
            if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
            {
                throw py::value_error("A component vector must contain finite numbers");
            }
        }

        void CheckPositive(const float value, const char* name)
        {
            if (!std::isfinite(value) || value <= 0.0f)
            {
                throw py::value_error(std::string(name) + " must be finite and positive");
            }
        }

        std::uint32_t KeyCode(std::string key)
        {
            std::transform(key.begin(), key.end(), key.begin(), [](const unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            if (key.size() == 1 && ((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= '0' && key[0] <= '9')))
            {
                return static_cast<std::uint32_t>(key[0]);
            }
            static const std::unordered_map<std::string, std::uint32_t> namedKeys{
                {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"ESC", VK_ESCAPE}, {"ESCAPE", VK_ESCAPE},
                {"TAB", VK_TAB}, {"SHIFT", VK_SHIFT}, {"CTRL", VK_CONTROL}, {"CONTROL", VK_CONTROL},
                {"ALT", VK_MENU}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"UP", VK_UP}, {"DOWN", VK_DOWN},
                {"BACKSPACE", VK_BACK}, {"DELETE", VK_DELETE}, {"HOME", VK_HOME}, {"END", VK_END},
                {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT}
            };
            if (const auto found = namedKeys.find(key); found != namedKeys.end())
            {
                return found->second;
            }
            if (key.size() >= 2 && key.size() <= 3 && key[0] == 'F')
            {
                unsigned number = 0;
                for (std::size_t index = 1; index < key.size(); ++index)
                {
                    if (key[index] < '0' || key[index] > '9')
                    {
                        throw py::value_error("Unknown key: " + key);
                    }
                    number = number * 10 + static_cast<unsigned>(key[index] - '0');
                }
                if (number >= 1 && number <= 24)
                {
                    return VK_F1 + number - 1;
                }
            }
            throw py::value_error("Unknown key: " + key);
        }

        std::vector<ecs::EntityId> QueryEntities()
        {
            auto ids = RequireWorld().GetEntities();
            // Requests waiting for the end of the frame are not new query results.
            const auto* system = GetScriptContext().system;
            ids.erase(std::remove_if(ids.begin(), ids.end(), [system](const ecs::EntityId id)
                { return system != nullptr && system->IsDestroyPending(id); }), ids.end());
            std::sort(ids.begin(), ids.end());
            return ids;
        }

        py::object Find(const std::string& name)
        {
            auto& world = RequireWorld();
            for (const auto id : QueryEntities())
            {
                const auto* tag = world.TryGet<TagComponent>(id);
                if (tag != nullptr && tag->name == name)
                {
                    return py::cast(EntityRef{id});
                }
            }
            return py::none();
        }

        bool MatchesPrefix(ecs::World& world, const ecs::EntityId id, const std::string& prefix)
        {
            const auto* tag = world.TryGet<TagComponent>(id);
            return prefix.empty() || (tag != nullptr && tag->name.compare(0, prefix.size(), prefix) == 0);
        }
    }

    void BindScriptApi(py::module_& module)
    {
        py::register_exception<EntityDeadError>(module, "EntityDeadError", PyExc_RuntimeError);

        py::class_<TransformRef>(module, "Transform")
            .def_property("position", [](const TransformRef& ref) { return RequireComponent<TransformComponent>(ref.entity, "Transform").position; },
                [](const TransformRef& ref, const Vec3& value)
                {
                    auto& transform = RequireComponent<TransformComponent>(ref.entity, "Transform");
                    CheckVector(value);
                    transform.position = value;
                },
                "A Vec3 copy: assign the whole vector; position.x += 1 changes only the copy")
            .def_property("rotation", [](const TransformRef& ref) { return RequireComponent<TransformComponent>(ref.entity, "Transform").rotationDeg; },
                [](const TransformRef& ref, const Vec3& value)
                {
                    auto& transform = RequireComponent<TransformComponent>(ref.entity, "Transform");
                    CheckVector(value);
                    transform.rotationDeg = value;
                }, "Euler angles in degrees, returned as a copy")
            .def_property("scale", [](const TransformRef& ref) { return RequireComponent<TransformComponent>(ref.entity, "Transform").scale; },
                [](const TransformRef& ref, const Vec3& value)
                {
                    auto& transform = RequireComponent<TransformComponent>(ref.entity, "Transform");
                    CheckVector(value);
                    transform.scale = value;
                }, "A Vec3 copy: assign the whole vector");

        py::class_<RigidbodyRef>(module, "Rigidbody")
            .def_property("velocity", [](const RigidbodyRef& ref) { return RequireComponent<RigidbodyComponent>(ref.entity, "Rigidbody").velocity; },
                [](const RigidbodyRef& ref, const Vec3& value)
                {
                    auto& body = RequireComponent<RigidbodyComponent>(ref.entity, "Rigidbody");
                    CheckVector(value);
                    body.velocity = value;
                }, "A Vec3 copy: assign the whole vector")
            .def_property("mass", [](const RigidbodyRef& ref) { return RequireComponent<RigidbodyComponent>(ref.entity, "Rigidbody").mass; },
                [](const RigidbodyRef& ref, const float value)
                {
                    auto& body = RequireComponent<RigidbodyComponent>(ref.entity, "Rigidbody");
                    CheckPositive(value, "mass");
                    body.mass = value;
                })
            .def_property("use_gravity", [](const RigidbodyRef& ref) { return RequireComponent<RigidbodyComponent>(ref.entity, "Rigidbody").useGravity; },
                [](const RigidbodyRef& ref, const bool value) { RequireComponent<RigidbodyComponent>(ref.entity, "Rigidbody").useGravity = value; })
            .def_property("is_kinematic", [](const RigidbodyRef& ref) { return RequireComponent<RigidbodyComponent>(ref.entity, "Rigidbody").isKinematic; },
                [](const RigidbodyRef& ref, const bool value) { RequireComponent<RigidbodyComponent>(ref.entity, "Rigidbody").isKinematic = value; })
            .def_property_readonly("is_grounded", [](const RigidbodyRef& ref) { return RequireComponent<RigidbodyComponent>(ref.entity, "Rigidbody").isGrounded; })
            .def("add_impulse", [](const RigidbodyRef& ref, const Vec3& impulse)
                {
                    auto& body = RequireComponent<RigidbodyComponent>(ref.entity, "Rigidbody");
                    CheckVector(impulse);
                    CheckPositive(body.mass, "mass");
                    body.velocity += impulse / body.mass;
                }, py::arg("impulse"));

        py::class_<ColliderRef>(module, "Collider")
            .def_property("is_trigger", [](const ColliderRef& ref) { return RequireComponent<ColliderComponent>(ref.entity, "Collider").isTrigger; },
                [](const ColliderRef& ref, const bool value) { RequireComponent<ColliderComponent>(ref.entity, "Collider").isTrigger = value; })
            .def_property("radius", [](const ColliderRef& ref) { return RequireComponent<ColliderComponent>(ref.entity, "Collider").radius; },
                [](const ColliderRef& ref, const float value)
                {
                    auto& collider = RequireComponent<ColliderComponent>(ref.entity, "Collider");
                    CheckPositive(value, "radius");
                    collider.radius = value;
                })
            .def_property("half_extents", [](const ColliderRef& ref) { return RequireComponent<ColliderComponent>(ref.entity, "Collider").halfExtents; },
                [](const ColliderRef& ref, const Vec3& value)
                {
                    auto& collider = RequireComponent<ColliderComponent>(ref.entity, "Collider");
                    CheckPositive(value.x, "half_extents.x");
                    CheckPositive(value.y, "half_extents.y");
                    CheckPositive(value.z, "half_extents.z");
                    collider.halfExtents = value;
                }, "A Vec3 copy: assign the whole vector");

        py::class_<MeshRendererRef>(module, "MeshRenderer")
            .def_property("mesh", [](const MeshRendererRef& ref) { return RequireComponent<MeshRendererComponent>(ref.entity, "MeshRenderer").meshPath; },
                [](const MeshRendererRef& ref, const std::string& value) { RequireComponent<MeshRendererComponent>(ref.entity, "MeshRenderer").meshPath = value; })
            .def_property("material", [](const MeshRendererRef& ref) { return RequireComponent<MeshRendererComponent>(ref.entity, "MeshRenderer").materialPath; },
                [](const MeshRendererRef& ref, const std::string& value) { RequireComponent<MeshRendererComponent>(ref.entity, "MeshRenderer").materialPath = value; })
            .def_property("visible", [](const MeshRendererRef& ref) { return RequireComponent<MeshRendererComponent>(ref.entity, "MeshRenderer").visible; },
                [](const MeshRendererRef& ref, const bool value) { RequireComponent<MeshRendererComponent>(ref.entity, "MeshRenderer").visible = value; });

        // No public constructor: handles come from self.entity or world queries.
        py::class_<EntityRef>(module, "Entity", "Entity handle (id + scene version), never a raw pointer")
            .def_property_readonly("id", [](const EntityRef& entity) { return entity.id; })
            .def_property_readonly("alive", &IsAlive)
            .def_property("name", [](const EntityRef& entity)
                {
                    const auto* tag = RequireAlive(entity).TryGet<TagComponent>(entity.id);
                    return tag != nullptr ? tag->name : std::string();
                }, [](const EntityRef& entity, const std::string& name)
                {
                    auto& world = RequireAlive(entity);
                    auto* tag = world.TryGet<TagComponent>(entity.id);
                    if (tag == nullptr)
                    {
                        tag = &world.Emplace<TagComponent>(entity.id);
                    }
                    tag->name = name;
                })
            .def_property_readonly("transform", [](const EntityRef& entity) { RequireComponent<TransformComponent>(entity, "Transform"); return TransformRef{entity}; })
            .def_property_readonly("rigidbody", [](const EntityRef& entity) { RequireComponent<RigidbodyComponent>(entity, "Rigidbody"); return RigidbodyRef{entity}; })
            .def_property_readonly("collider", [](const EntityRef& entity) { RequireComponent<ColliderComponent>(entity, "Collider"); return ColliderRef{entity}; })
            .def_property_readonly("mesh", [](const EntityRef& entity) { RequireComponent<MeshRendererComponent>(entity, "MeshRenderer"); return MeshRendererRef{entity}; })
            .def("get_script", [](const EntityRef& entity, const py::handle scriptClass)
                {
                    RequireAlive(entity);
                    return ScriptApi::GetScript(RequireSystem(), entity.id, scriptClass);
                }, py::arg("cls"))
            .def("destroy", [](const EntityRef& entity)
                {
                    if (IsAlive(entity))
                    {
                        RequireSystem().RequestDestroy(entity.id);
                    }
                }, "Deferred and idempotent: removed with its children at the end of the script step")
            .def("__eq__", [](const EntityRef& lhs, const EntityRef& rhs) { return lhs.id == rhs.id && lhs.sceneVersion == rhs.sceneVersion; }, py::is_operator())
            .def("__hash__", [](const EntityRef& entity)
                { return std::hash<ecs::EntityId>{}(entity.id) ^ (std::hash<std::uint64_t>{}(entity.sceneVersion) << 1); })
            .def("__repr__", [](const EntityRef& entity) { return "Entity(" + std::to_string(entity.id) + ")"; });

        auto worldModule = module.def_submodule("world", "Queries return handles; pending destroy requests are excluded");
        worldModule.def("find", &Find, py::arg("name"));
        worldModule.def("find_all", [](const std::string& prefix)
            {
                auto& world = RequireWorld();
                std::vector<EntityRef> result;
                for (const auto id : QueryEntities())
                {
                    if (MatchesPrefix(world, id, prefix))
                    {
                        result.push_back(EntityRef{id});
                    }
                }
                return result;
            }, py::arg("prefix") = "");
        worldModule.def("find_in_radius", [](const Vec3& center, const float radius, const std::string& prefix)
            {
                CheckVector(center);
                if (!std::isfinite(radius) || radius < 0.0f)
                {
                    throw py::value_error("radius must be finite and non-negative");
                }
                auto& world = RequireWorld();
                std::unordered_map<ecs::EntityId, DirectX::XMFLOAT4X4> cache;
                std::unordered_set<ecs::EntityId> visiting;
                std::vector<std::pair<double, ecs::EntityId>> hits;
                const double radiusSquared = static_cast<double>(radius) * radius;
                for (const auto id : QueryEntities())
                {
                    if (!world.Has<TransformComponent>(id) || !MatchesPrefix(world, id, prefix))
                    {
                        continue;
                    }
                    DirectX::XMFLOAT4X4 matrix;
                    DirectX::XMStoreFloat4x4(&matrix, scene::ResolveWorldMatrix(world, id, cache, visiting));
                    const double x = static_cast<double>(matrix._41) - center.x;
                    const double y = static_cast<double>(matrix._42) - center.y;
                    const double z = static_cast<double>(matrix._43) - center.z;
                    const double distance = x * x + y * y + z * z;
                    if (distance <= radiusSquared)
                    {
                        hits.emplace_back(distance, id);
                    }
                }
                std::sort(hits.begin(), hits.end());
                std::vector<EntityRef> result;
                for (const auto& hit : hits)
                {
                    result.push_back(EntityRef{hit.second});
                }
                return result;
            }, py::arg("center"), py::arg("radius"), py::arg("prefix") = "");

        auto inputModule = module.def_submodule("input");
        inputModule.def("is_down", [](const std::string& action) { return RequireInput().IsActionDown(action); }, py::arg("action"));
        inputModule.def("was_pressed", [](const std::string& action) { return RequireInput().WasActionPressed(action); }, py::arg("action"));
        inputModule.def("is_key_down", [](const std::string& key) { return RequireInput().IsKeyDown(KeyCode(key)); }, py::arg("key"));
        inputModule.def("was_key_pressed", [](const std::string& key) { return RequireInput().WasKeyPressed(KeyCode(key)); }, py::arg("key"));

        py::class_<TimeRef>(module, "_Time")
            .def_property_readonly("dt", [](const TimeRef&) { MYENGINE_ASSERT_SCRIPT_THREAD(); return GetScriptContext().deltaTime; })
            .def_property_readonly("total", [](const TimeRef&) { MYENGINE_ASSERT_SCRIPT_THREAD(); return GetScriptContext().totalTime; })
            .def_property_readonly("frame", [](const TimeRef&) { MYENGINE_ASSERT_SCRIPT_THREAD(); return GetScriptContext().frame; });
        module.attr("time") = py::cast(TimeRef{});

        auto hudModule = module.def_submodule("hud");
        hudModule.def("set", [](const std::string& key, const std::string& text) { ScriptApi::SetHudLine(RequireSystem(), key, text); }, py::arg("key"), py::arg("text"));
        hudModule.def("clear", [](const std::string& key) { ScriptApi::ClearHudLine(RequireSystem(), key); }, py::arg("key"));
        module.def("send", [](const std::string& name, const std::string& method, const py::args& args)
            {
                const auto recipient = Find(name);
                return !recipient.is_none() && ScriptApi::Send(RequireSystem(), recipient.cast<EntityRef>().id, method, args);
            }, py::arg("name"), py::arg("method"), "Call matching behaviours safely; returns false if no method was delivered");
    }
}
