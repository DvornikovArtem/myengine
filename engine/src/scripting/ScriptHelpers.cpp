// ScriptHelpers.cpp

#include "ScriptHelpers.h"

#include <pybind11/eval.h>

#include "ScriptContext.h"

namespace py = pybind11;

namespace myengine::scripting::detail
{
    namespace
    {
        constexpr const char* kHelpersModule = "_myengine_helpers";

        constexpr const char* kHelpersCode = R"PY(
import gc
import importlib.util
import inspect
import json
import os
import sys
import traceback

_TYPES_BY_NAME = {"bool": bool, "int": int, "float": float, "str": str}


def _normalize(annotation):
    # "from __future__ import annotations" turns annotations into strings
    if isinstance(annotation, str):
        return _TYPES_BY_NAME.get(annotation, annotation)
    return annotation


def _declared_fields(cls):
    """Fields of a behaviour = class attributes with a type annotation, over the whole MRO."""
    fields = {}
    for klass in reversed(cls.__mro__):
        try:
            for name, annotation in inspect.get_annotations(klass).items():
                fields[name] = _normalize(annotation)
        except Exception:
            pass
    return fields


def _coerce(annotation, value):
    if annotation is bool:
        return isinstance(value, bool), value
    if annotation is int:
        return isinstance(value, int) and not isinstance(value, bool), value
    if annotation is float:
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            return True, float(value)
        return False, value
    if annotation is str:
        return isinstance(value, str), value
    return True, value


def apply_props(obj, cls, props_json):
    """Sets scene/prefab values on a new instance. Returns warnings for unknown fields and wrong types."""
    warnings = []
    fields = _declared_fields(cls)
    for name, value in json.loads(props_json).items():
        if name not in fields:
            warnings.append(f"unknown field '{name}' (declare it in the class: {name}: <type> = <default>)")
            continue
        ok, value = _coerce(fields[name], value)
        if not ok:
            expected = getattr(fields[name], "__name__", str(fields[name]))
            warnings.append(f"field '{name}' expects {expected}, got {type(value).__name__}; the default is kept")
            continue
        setattr(obj, name, value)
    return warnings


def describe_fields(cls):
    """[(name, kind, default)] for the inspector; kind is "float", "int", "bool" or "str", other types are skipped."""
    result = []
    for name, annotation in _declared_fields(cls).items():
        kind = next((key for key, value in _TYPES_BY_NAME.items() if value is annotation), None)
        if kind is None or name.startswith("_"):
            continue
        ok, default = _coerce(annotation, getattr(cls, name, None))
        if not ok:
            default = {"bool": False, "int": 0, "float": 0.0, "str": ""}[kind]
        result.append((name, kind, default))
    return result


def _is_script_file(path, scripts_dir):
    try:
        return os.path.normcase(os.path.abspath(path)).startswith(scripts_dir)
    except Exception:
        return False


def describe_error(exc_type, exc_value, exc_tb, scripts_dir):
    """Returns (file, line, summary, full_text). file:line is the last traceback frame inside assets/scripts."""
    scripts_dir = os.path.normcase(os.path.abspath(scripts_dir))
    file, line = "", 0
    for frame in traceback.extract_tb(exc_tb):
        if _is_script_file(frame.filename, scripts_dir):
            file, line = os.path.basename(frame.filename), frame.lineno or 0
    if isinstance(exc_value, SyntaxError) and exc_value.filename and _is_script_file(exc_value.filename, scripts_dir):
        file, line = os.path.basename(exc_value.filename), exc_value.lineno or 0
    # The last line is the error itself ("SyntaxError: expected ':'"); for a SyntaxError the lines above it
    # repeat the source line, and they stay in full_text
    summary_lines = [line.strip() for line in traceback.format_exception_only(exc_type, exc_value) if line.strip()]
    summary = summary_lines[-1] if summary_lines else str(exc_value)
    full_text = "".join(traceback.format_exception(exc_type, exc_value, exc_tb)).rstrip()
    return file, line, summary, full_text


def stats(scripts_dir):
    """(number of modules loaded from assets/scripts, number of objects tracked by the garbage collector)"""
    scripts_dir = os.path.normcase(os.path.abspath(scripts_dir))
    modules = sum(1 for module in list(sys.modules.values())
                  if _is_script_file(getattr(module, "__file__", None) or "", scripts_dir))
    return modules, len(gc.get_objects())


def check_syntax(path, source):
    """Raises SyntaxError with the file and line if the code does not compile."""
    compile(source, path, "exec")


def reload_module(name, path, source):
    """Hot reload. The code runs in a NEW module object and sys.modules is switched only on success:
    a SyntaxError or an error at module level leaves the old module (and the running game) untouched.
    importlib.reload would run the code inside the old module and leave it half-updated on an error."""
    code = compile(source, path, "exec")
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    exec(code, module.__dict__)
    sys.modules[name] = module
    return module


def defines_behaviour(module, base):
    """True if the module defines a subclass of myengine.Behaviour (not only imports one)."""
    return any(isinstance(value, type) and issubclass(value, base) and value.__module__ == module.__name__
               for value in list(vars(module).values()))
)PY";
    }

    void InstallHelpers()
    {
        MYENGINE_ASSERT_SCRIPT_THREAD();

        const py::module_ types = py::module_::import("types");
        py::object module = types.attr("ModuleType")(kHelpersModule);
        py::exec(kHelpersCode, module.attr("__dict__"));
        py::module_::import("sys").attr("modules")[kHelpersModule] = module;
    }

    py::object Helper(const char* name)
    {
        MYENGINE_ASSERT_SCRIPT_THREAD();
        return py::module_::import(kHelpersModule).attr(name);
    }
}