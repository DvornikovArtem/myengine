// ScriptRuntime.cpp

#include <myengine/scripting/ScriptRuntime.h>

#include <system_error>
#include <utility>

#include <pybind11/embed.h>

#include <myengine/core/Logger.h>

#include "ScriptContext.h"
#include "ScriptHelpers.h"
#include "ScriptModules.h"

#include <tracy/Tracy.hpp>

namespace py = pybind11;

namespace myengine::scripting
{
    namespace detail
    {
        ScriptContext& GetScriptContext()
        {
            static ScriptContext context;
            return context;
        }

        py::module_ GetModule(const std::string& name)
        {
            MYENGINE_ASSERT_SCRIPT_THREAD();
            return py::module_::import(name.c_str());
        }
    }

    namespace
    {
        constexpr std::size_t kMaxConsoleOutput = 64 * 1024;

        class ScopedConsoleCapture
        {
        public:
            explicit ScopedConsoleCapture(std::string& output)
                : previous_(std::move(detail::GetScriptContext().consoleOutput))
            {
                detail::GetScriptContext().consoleOutput = [&output, truncated = false](const char* level, const std::string& text) mutable
                {
                    if (truncated)
                    {
                        return;
                    }
                    const std::string line = (std::string(level) == "INFO" ? std::string() : "[" + std::string(level) + "] ") + text + '\n';
                    if (output.size() + line.size() > kMaxConsoleOutput)
                    {
                        output.append(line, 0, kMaxConsoleOutput - output.size());
                        output += "\n... console output truncated ...\n";
                        truncated = true;
                    }
                    else
                    {
                        output += line;
                    }
                };
            }

            ~ScopedConsoleCapture() { detail::GetScriptContext().consoleOutput = std::move(previous_); }
            ScopedConsoleCapture(const ScopedConsoleCapture&) = delete;
            ScopedConsoleCapture& operator=(const ScopedConsoleCapture&) = delete;

        private:
            std::function<void(const char*, const std::string&)> previous_;
        };

        // Runs once after the interpreter starts. The exe is a WIN32 app without a console,
        // so print() would print nowhere: stdout/stderr are redirected into the engine log.
        constexpr const char* kBootstrapCode = R"PY(
import sys
import myengine


class _LogStream:
    """Replacement for sys.stdout / sys.stderr: every finished line goes to the engine log."""

    encoding = "utf-8"

    def __init__(self, write_line):
        self._write_line = write_line
        self._buffer = ""

    def write(self, text):
        self._buffer += text
        while "\n" in self._buffer:
            line, self._buffer = self._buffer.split("\n", 1)
            self._write_line(line)
        return len(text)

    def flush(self):
        if self._buffer:
            self._write_line(self._buffer)
            self._buffer = ""

    def isatty(self):
        return False


sys.stdout = _LogStream(myengine.log.info)
sys.stderr = _LogStream(myengine.log.error)

print(f"Python {sys.version.split()[0]} is ready, Vec3 check: {myengine.Vec3(1, 2, 3) + myengine.Vec3(1, 1, 1)}")
)PY";

        // Turns a failed PyStatus into a log line. Returns true if the status is OK.
        bool CheckStatus(const PyStatus& status, const char* what, core::Logger* logger)
        {
            if (!PyStatus_Exception(status))
            {
                return true;
            }

            if (logger != nullptr)
            {
                const std::string details = status.err_msg != nullptr ? status.err_msg : "unknown error";
                logger->Error(std::string("ScriptRuntime: ") + what + " failed: " + details);
            }
            return false;
        }

        bool FileExists(const std::filesystem::path& path)
        {
            std::error_code error;
            return std::filesystem::is_regular_file(path, error);
        }
    }

    ScriptRuntime::~ScriptRuntime()
    {
        Shutdown();
    }

    bool ScriptRuntime::Initialize(const ScriptRuntimeDesc& desc)
    {
        if (initialized_)
        {
            return true;
        }

        detail::EnsureBindingsLinked();

        desc_ = desc;
        mainThreadId_ = std::this_thread::get_id();

        auto& context = detail::GetScriptContext();
        context.logger = desc_.logger;
        context.mainThreadId = mainThreadId_;
        ++context.sceneVersion;
        context.deltaTime = 0.0f;
        context.totalTime = 0.0;
        context.frame = 0;

        // Without the standard library the interpreter can not start: disable scripting instead of failing later.
        // (python314.dll itself is a load-time dependency of the exe, Windows reports it before main)
        const auto stdlibPath = desc_.exeDir / L"python314.zip";
        if (!FileExists(stdlibPath))
        {
            if (desc_.logger != nullptr)
            {
                desc_.logger->Error(
                    "ScriptRuntime: python314.zip not found next to the exe "
                    "(external/runtime is copied on build; rerun setup.bat if it is missing). Scripting is disabled");
            }
            return false;
        }

        // UTF-8 mode: open() in scripts reads UTF-8 by default
        PyPreConfig preConfig;
        PyPreConfig_InitIsolatedConfig(&preConfig);
        preConfig.utf8_mode = 1;
        if (!CheckStatus(Py_PreInitialize(&preConfig), "Py_PreInitialize", desc_.logger))
        {
            return false;
        }

        // Isolated: ignore PYTHONHOME, PYTHONPATH, user site-packages and the installed Python.
        // All paths are wide strings, so the Cyrillic user folder does not break them.
        PyConfig config;
        PyConfig_InitIsolatedConfig(&config);
        config.write_bytecode = 0; // no __pycache__ inside assets/scripts
        config.install_signal_handlers = 0;

        bool configOk = CheckStatus(PyConfig_SetString(&config, &config.home, desc_.exeDir.c_str()), "PyConfig home", desc_.logger);
        config.module_search_paths_set = 1;
        configOk = configOk && CheckStatus(PyWideStringList_Append(&config.module_search_paths, stdlibPath.c_str()), "sys.path (stdlib)", desc_.logger);
        configOk = configOk && CheckStatus(PyWideStringList_Append(&config.module_search_paths, desc_.exeDir.c_str()), "sys.path (exe dir)", desc_.logger);
        configOk = configOk && CheckStatus(PyWideStringList_Append(&config.module_search_paths, desc_.scriptsDir.c_str()), "sys.path (scripts)", desc_.logger);
        if (!configOk)
        {
            PyConfig_Clear(&config);
            return false;
        }

        try
        {
            // Clears the config itself. add_program_dir_to_path = false: sys.path is fully set above
            py::initialize_interpreter(&config, 0, nullptr, false);
        }
        catch (const std::exception& exception)
        {
            if (desc_.logger != nullptr)
            {
                desc_.logger->Error(std::string("ScriptRuntime: interpreter start failed: ") + exception.what());
            }
            return false;
        }

        initialized_ = true;

        try
        {
            py::exec(kBootstrapCode);
            detail::InstallHelpers();

            if (desc_.logger != nullptr)
            {
                const auto sysPath = py::str(py::module_::import("sys").attr("path")).cast<std::string>();
                desc_.logger->Info("ScriptRuntime: initialized, sys.path = " + sysPath);
            }
        }
        catch (const py::error_already_set& error)
        {
            if (desc_.logger != nullptr)
            {
                desc_.logger->Error(std::string("ScriptRuntime: bootstrap failed: ") + error.what());
            }
            Shutdown();
            return false;
        }

        return true;
    }

    void ScriptRuntime::Shutdown()
    {
        if (!initialized_)
        {
            return;
        }

        MYENGINE_ASSERT_SCRIPT_THREAD();

        ResetConsole();
        try
        {
            // Flush a print() without a trailing newline and detach the log streams before finalization
            py::exec("import sys\nsys.stdout.flush()\nsys.stderr.flush()\nsys.stdout = sys.__stdout__\nsys.stderr = sys.__stderr__\n");
        }
        catch (const py::error_already_set&)
        {
        }

        py::finalize_interpreter();
        initialized_ = false;
        fieldCache_.clear();

        auto& context = detail::GetScriptContext();
        context.world = nullptr;
        context.system = nullptr;
        context.input = nullptr;
        context.consoleOutput = {};

        if (desc_.logger != nullptr)
        {
            desc_.logger->Info("ScriptRuntime: shut down");
        }
        context.logger = nullptr;
    }

    bool ScriptRuntime::IsInitialized() const
    {
        return initialized_;
    }

    bool ScriptRuntime::IsMainThread() const
    {
        return std::this_thread::get_id() == mainThreadId_;
    }

    const std::filesystem::path& ScriptRuntime::GetScriptsDir() const
    {
        return desc_.scriptsDir;
    }

    std::vector<ScriptFieldInfo> ScriptRuntime::DescribeFields(const std::string& module, const std::string& className)
    {
        const std::string key = module + "." + className;
        if (const auto cached = fieldCache_.find(key); cached != fieldCache_.end())
        {
            return cached->second;
        }

        std::vector<ScriptFieldInfo> fields;
        if (initialized_)
        {
            MYENGINE_ASSERT_SCRIPT_THREAD();
            try
            {
                const py::object cls = detail::GetModule(module).attr(className.c_str());
                for (const auto& item : detail::Helper("describe_fields")(cls))
                {
                    const auto entry = item.cast<py::tuple>();
                    ScriptFieldInfo field;
                    field.name = entry[0].cast<std::string>();
                    const auto kind = entry[1].cast<std::string>();
                    if (kind == "bool")
                    {
                        field.type = ScriptFieldInfo::Type::Bool;
                        field.defaultValue = entry[2].cast<bool>();
                    }
                    else if (kind == "int")
                    {
                        field.type = ScriptFieldInfo::Type::Int;
                        field.defaultValue = entry[2].cast<long long>();
                    }
                    else if (kind == "float")
                    {
                        field.type = ScriptFieldInfo::Type::Float;
                        field.defaultValue = entry[2].cast<double>();
                    }
                    else
                    {
                        field.type = ScriptFieldInfo::Type::String;
                        field.defaultValue = entry[2].cast<std::string>();
                    }
                    fields.push_back(std::move(field));
                }
            }
            catch (const std::exception& exception)
            {
                if (desc_.logger != nullptr)
                {
                    desc_.logger->Warning("ScriptRuntime: can not read fields of " + key + ": " + exception.what());
                }
            }
        }

        fieldCache_[key] = fields;
        return fields;
    }

    void ScriptRuntime::ClearFieldCache()
    {
        fieldCache_.clear();
    }

    ScriptConsoleResult ScriptRuntime::ExecuteConsole(const std::string& source)
    {
        ZoneScopedN("Scripts::Console");
        ScriptConsoleResult result;
        if (!initialized_)
        {
            result.error = "Python runtime is not initialized.";
            return result;
        }
        MYENGINE_ASSERT_SCRIPT_THREAD();
        if (source.size() > 16 * 1024 || source.find('\0') != std::string::npos)
        {
            try { detail::Helper("cancel_console_input")(); } catch (const std::exception&) {}
            result.error = "Console input must be at most 16 KiB per line and contain no NUL bytes.";
            return result;
        }

        // Do not attribute an unfinished print from a game script to this console command.
        auto flushStreams = []()
        {
            const auto sys = py::module_::import("sys");
            sys.attr("stdout").attr("flush")();
            sys.attr("stderr").attr("flush")();
        };
        try
        {
            flushStreams();
        }
        catch (const std::exception& error)
        {
            result.error = error.what();
            return result;
        }
        ScopedConsoleCapture capture(result.output);
        try
        {
            result.incomplete = detail::Helper("console_push")(source).cast<bool>();
            result.success = true;
            flushStreams(); // print(end="") is visible immediately too
        }
        catch (py::error_already_set& error)
        {
            result.success = false;
            result.file = "<console>";
            try
            {
                const auto orNone = [](const py::object& value) { return value ? value : py::none(); };
                const auto described = detail::Helper("describe_error")(
                    orNone(error.type()), orNone(error.value()), orNone(error.trace()), desc_.scriptsDir.u8string()).cast<py::tuple>();
                const auto file = described[0].cast<std::string>();
                if (!file.empty())
                {
                    result.file = file;
                }
                result.line = described[1].cast<int>();
                result.error = result.file + ":" + std::to_string(result.line) + ": " + described[2].cast<std::string>() + "\n" + described[3].cast<std::string>();
            }
            catch (const std::exception&)
            {
                result.error = error.what();
            }
            try { flushStreams(); } catch (const std::exception&) {}
        }
        catch (const std::exception& error)
        {
            result.success = false;
            result.error = error.what();
        }
        if (result.error.size() > kMaxConsoleOutput)
        {
            result.error.resize(kMaxConsoleOutput);
            result.error += "\n... traceback truncated ...";
        }
        return result;
    }

    void ScriptRuntime::ResetConsole()
    {
        if (!initialized_)
        {
            return;
        }
        MYENGINE_ASSERT_SCRIPT_THREAD();
        try
        {
            detail::Helper("reset_console")();
            ++consoleGeneration_;
        }
        catch (const std::exception& error)
        {
            if (desc_.logger != nullptr)
            {
                desc_.logger->Warning(std::string("ScriptRuntime: console reset failed: ") + error.what());
            }
        }
    }

    std::uint64_t ScriptRuntime::GetConsoleGeneration() const { return consoleGeneration_; }
}
