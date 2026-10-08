// ScriptModules.h
// Private header of the scripting subsystem: the single point where script modules are imported.

#pragma once

#include <string>

#include <pybind11/pybind11.h>

namespace myengine::scripting::detail
{
    // Imports assets/scripts/<name>.py (cached by Python in sys.modules). Throws pybind11::error_already_set.
    // Hot reload (T5) replaces modules here, so every import goes through this function
    pybind11::module_ GetModule(const std::string& name);
}