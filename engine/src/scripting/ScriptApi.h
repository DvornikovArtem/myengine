// ScriptApi.h
// Private bridge between the bindings and ScriptSystem's Python objects.

#pragma once

#include <stdexcept>
#include <string>

#include <pybind11/pybind11.h>

#include "EntityRef.h"

namespace myengine::scripting
{
    class ScriptSystem;
}

namespace myengine::scripting::detail
{
    class EntityDeadError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    // A friend of ScriptSystem: pybind11 remains out of the public engine headers.
    struct ScriptApi
    {
        static pybind11::object GetScript(ScriptSystem& system, ecs::EntityId entity, pybind11::handle scriptClass);
        static bool Send(ScriptSystem& system, ecs::EntityId entity, const std::string& method, const pybind11::args& args);
        static void SetHudLine(ScriptSystem& system, const std::string& key, const std::string& text);
        static void ClearHudLine(ScriptSystem& system, const std::string& key);
    };

    void BindScriptApi(pybind11::module_& module);
}
