// ScriptBindings.cpp
// The "myengine" Python module: everything a script can see from the engine.

#include <cstdio>
#include <functional>
#include <string>

#include <pybind11/embed.h>
#include <pybind11/operators.h>

#include <myengine/core/Logger.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/Vector3.h>

#include <myengine/scripting/ScriptSystem.h>

#include "Behaviour.h"
#include "EntityRef.h"
#include "ScriptContext.h"

namespace py = pybind11;

namespace
{
    using myengine::core::LogLevel;
    using myengine::ecs::components::Vec3;
    using myengine::scripting::detail::EntityRef;
    using myengine::scripting::detail::GetScriptContext;

    void WriteLog(const LogLevel level, const std::string& message)
    {
        MYENGINE_ASSERT_SCRIPT_THREAD();

        if (auto* logger = GetScriptContext().logger)
        {
            logger->Log(level, "[script] " + message);
        }
    }

    std::string FormatFloat(const float value)
    {
        char buffer[32]{};
        // + 0.0f turns -0 into 0, so a flipped normal prints as Vec3(0, -1, 0), not Vec3(-0, -1, -0)
        std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value + 0.0f));
        return buffer;
    }

    bool IsEntityAlive(const EntityRef& entity)
    {
        MYENGINE_ASSERT_SCRIPT_THREAD();

        const auto* world = GetScriptContext().world;
        return world != nullptr && world->IsAlive(entity.id);
    }

    // Deferred: the entity is destroyed at the end of the script step, a second call does nothing
    void DestroyEntity(const EntityRef& entity)
    {
        MYENGINE_ASSERT_SCRIPT_THREAD();

        if (auto* system = GetScriptContext().system)
        {
            system->RequestDestroy(entity.id);
        }
    }
}

void myengine::scripting::detail::EnsureBindingsLinked()
{
}

PYBIND11_EMBEDDED_MODULE(myengine, m)
{
    m.doc() = "myengine scripting API";

    // myengine.log.info / warn / error / debug; print() is redirected to log.info as well
    auto log = m.def_submodule("log", "Engine log (logs/myengine.log)");
    log.def("debug", [](const std::string& message) { WriteLog(LogLevel::Debug, message); }, py::arg("message"));
    log.def("info", [](const std::string& message) { WriteLog(LogLevel::Info, message); }, py::arg("message"));
    log.def("warn", [](const std::string& message) { WriteLog(LogLevel::Warning, message); }, py::arg("message"));
    log.def("error", [](const std::string& message) { WriteLog(LogLevel::Error, message); }, py::arg("message"));

    // Value type: Python gets a copy. t.position.x += 1 changes the copy only, assign the whole vector
    py::class_<Vec3>(m, "Vec3", "3D vector (a copy, not a reference into the engine)")
        .def(py::init<>())
        .def(py::init([](const float x, const float y, const float z) { return Vec3{x, y, z}; }), py::arg("x"), py::arg("y"), py::arg("z"))
        .def_readwrite("x", &Vec3::x)
        .def_readwrite("y", &Vec3::y)
        .def_readwrite("z", &Vec3::z)
        .def(py::self + py::self)
        .def(py::self - py::self)
        .def(-py::self)
        .def(py::self * float())
        .def(float() * py::self)
        .def(py::self / float())
        .def("__eq__", [](const Vec3& lhs, const Vec3& rhs) { return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z; })
        .def("length", [](const Vec3& value) { return myengine::ecs::components::Length(value); })
        .def("normalized", [](const Vec3& value) { return myengine::ecs::components::Normalize(value); })
        .def("dot", [](const Vec3& lhs, const Vec3& rhs) { return myengine::ecs::components::Dot(lhs, rhs); }, py::arg("other"))
        .def("cross", [](const Vec3& lhs, const Vec3& rhs) { return myengine::ecs::components::Cross(lhs, rhs); }, py::arg("other"))
        .def("__repr__", [](const Vec3& value)
            {
                return "Vec3(" + FormatFloat(value.x) + ", " + FormatFloat(value.y) + ", " + FormatFloat(value.z) + ")";
            });

    // Entity handle. Components, name and the rest come in T2.
    // No Python constructor: entities come from the engine (self.entity, world.spawn, world.find)
    py::class_<EntityRef>(m, "Entity", "Handle to an engine entity (id + liveness check, never a raw pointer)")
        .def_property_readonly("id", [](const EntityRef& entity) { return entity.id; })
        .def_property_readonly("alive", &IsEntityAlive)
        .def("destroy", &DestroyEntity, "Destroy the entity and its children at the end of the script step (deferred)")
        .def("__eq__", [](const EntityRef& lhs, const EntityRef& rhs) { return lhs.id == rhs.id; })
        .def("__hash__", [](const EntityRef& entity) { return std::hash<myengine::ecs::EntityId>{}(entity.id); })
        .def("__repr__", [](const EntityRef& entity) { return "Entity(" + std::to_string(entity.id) + ")"; });

    myengine::scripting::detail::BindBehaviour(m);
}