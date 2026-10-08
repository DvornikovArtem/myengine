// ScriptRuntime.h

#pragma once

#include <filesystem>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace myengine::core
{
    class Logger;
}

namespace myengine::scripting
{
    // Plain data for the editor; Python objects never leave the scripting subsystem.
    struct ScriptConsoleResult
    {
        bool success = false;
        bool incomplete = false; // a compound statement needs another line (the prompt becomes ...)
        std::string output; // print, expression values and myengine.log messages
        std::string error; // file:line and full traceback
        std::string file;
        int line = 0;
    };

    struct ScriptRuntimeDesc
    {
        std::filesystem::path exeDir; // where python314.dll / python314.zip are
        std::filesystem::path scriptsDir; // assets/scripts (source tree, so hot reload sees edits)
        core::Logger* logger = nullptr;
    };

    // Script field description for the inspector (built from the class annotations)
    struct ScriptFieldInfo
    {
        enum class Type
        {
            Float,
            Int,
            Bool,
            String,
        };

        std::string name;
        Type type = Type::Float;
        nlohmann::json defaultValue; // default value from the class code
    };

    // Owns the embedded Python interpreter. One per application, lives in Application.
    // Everything Python-related goes through this class; pybind11 is not visible in this header.
    class ScriptRuntime
    {
    public:
        ScriptRuntime() = default;
        ~ScriptRuntime();

        ScriptRuntime(const ScriptRuntime&) = delete;
        ScriptRuntime& operator=(const ScriptRuntime&) = delete;

        // PyConfig (isolated, wide-char paths), sys.path, stdout/stderr -> Logger, import myengine.
        // Must be called on the main thread; Python is used only from that thread afterwards.
        bool Initialize(const ScriptRuntimeDesc& desc);

        // Py_Finalize. Call only after every py::object was released (ScriptSystem::Shutdown). Safe to call twice.
        void Shutdown();

        bool IsInitialized() const;
        bool IsMainThread() const;

        const std::filesystem::path& GetScriptsDir() const;

        // Fields of module.className for the inspector, works in Edit mode without instances.
        // Cached; an import error is logged once and gives an empty list until the cache is cleared
        std::vector<ScriptFieldInfo> DescribeFields(const std::string& module, const std::string& className);
        // Called after hot reload: field lists are built again from the new code
        void ClearFieldCache();

        ScriptConsoleResult ExecuteConsole(const std::string& source); // one REPL line, main thread only
        void ResetConsole(); // release saved variables / pending lines before Stop or interpreter shutdown
        std::uint64_t GetConsoleGeneration() const; // UI notices an automatic namespace reset

    private:
        ScriptRuntimeDesc desc_;
        std::map<std::string, std::vector<ScriptFieldInfo>> fieldCache_; // "module.Class" -> fields
        std::thread::id mainThreadId_;
        bool initialized_ = false;
        std::uint64_t consoleGeneration_ = 0;
    };
}