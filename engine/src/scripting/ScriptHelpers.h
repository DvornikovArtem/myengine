// ScriptHelpers.h
// Private header of the scripting subsystem: helper functions that are simpler to write in Python.

#pragma once

#include <pybind11/pybind11.h>

namespace myengine::scripting::detail
{
    // Creates the internal module "_myengine_helpers" in sys.modules. Called once by ScriptRuntime::Initialize.
    // The module lives inside the interpreter, so no C++ object has to be released before Py_Finalize
    void InstallHelpers();

    // A function of the helper module: apply_props, describe_error, stats, reload_module, check_syntax,
    // defines_behaviour, describe_fields, transfer_state, live_fields. Throws pybind11::error_already_set
    pybind11::object Helper(const char* name);
}