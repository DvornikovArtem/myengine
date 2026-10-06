// ScriptSystem.cpp

#include <myengine/scripting/ScriptSystem.h>

#include <myengine/core/Logger.h>
#include <myengine/scripting/ScriptRuntime.h>

#include <tracy/Tracy.hpp>

#include "ScriptContext.h"

namespace myengine::scripting
{
    ScriptSystem::ScriptSystem(ScriptRuntime& runtime, input::InputManager& input, scene::PrefabLibrary& prefabs, core::Logger& logger)
        : runtime_(runtime),
          input_(input),
          prefabs_(prefabs),
          logger_(logger)
    {
    }

    ScriptSystem::~ScriptSystem()
    {
        Shutdown();
    }

    void ScriptSystem::Update(ecs::World& world, const float /*deltaTime*/)
    {
        ZoneScopedN("Scripts::Update");

        if (!runtime_.IsInitialized())
        {
            return;
        }

        MYENGINE_ASSERT_SCRIPT_THREAD();
        detail::GetScriptContext().world = &world;

        // T3: create instances, OnStart, physics events, OnUpdate, deferred destroy
        // T5: hot reload
    }

    void ScriptSystem::RequestReloadAll()
    {
        // T5
    }

    void ScriptSystem::ResetInstances()
    {
        // T3
    }

    void ScriptSystem::Shutdown()
    {
        // T3: release every py::object here, before Py_Finalize
        if (runtime_.IsInitialized())
        {
            detail::GetScriptContext().world = nullptr;
        }
    }

    const std::deque<ScriptError>& ScriptSystem::GetRecentErrors() const
    {
        return recentErrors_;
    }

    const std::map<std::string, std::string>& ScriptSystem::GetHudLines() const
    {
        return hudLines_;
    }

    nlohmann::json ScriptSystem::GetLiveFields(const ecs::EntityId /*entity*/, const std::size_t /*scriptIndex*/) const
    {
        // T7
        return nlohmann::json::object();
    }
}