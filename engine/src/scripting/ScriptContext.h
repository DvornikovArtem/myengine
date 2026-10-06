// ScriptContext.h
// Private header of the scripting subsystem: engine pointers that the bindings (ScriptBindings.cpp) need.

#pragma once

#include <cassert>
#include <thread>

namespace myengine::core
{
    class Logger;
}

namespace myengine::ecs
{
    class World;
}

namespace myengine::scripting::detail
{
    struct ScriptContext
    {
        core::Logger* logger = nullptr;
        ecs::World* world = nullptr; // set by ScriptSystem::Update, nullptr before the first frame
        std::thread::id mainThreadId;
    };

    // One context per process: the embedded interpreter is global too
    ScriptContext& GetScriptContext();

    // Defined in ScriptBindings.cpp. The engine is a static library: without a reference to that object file
    // the linker drops it together with PYBIND11_EMBEDDED_MODULE, and "import myengine" fails
    void EnsureBindingsLinked();

    inline bool IsScriptThread()
    {
        return std::this_thread::get_id() == GetScriptContext().mainThreadId;
    }
}

// Python is called only from the main thread (see docs/scripting/architecture.md, "Потоки")
#define MYENGINE_ASSERT_SCRIPT_THREAD() assert(::myengine::scripting::detail::IsScriptThread() && "Python must be used only on the main thread")