// ScriptSystem.h

#pragma once

#include <cstddef>
#include <deque>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include <myengine/ecs/Entity.h>
#include <myengine/ecs/System.h>

namespace myengine::core
{
    class Logger;
}

namespace myengine::input
{
    class InputManager;
}

namespace myengine::scene
{
    class PrefabLibrary;
}

namespace myengine::scripting
{
    class ScriptRuntime;

    // Recent script errors (for the log, the console and the editor)
    struct ScriptError
    {
        std::string file;
        int line = 0; // line of the last traceback frame inside a script file
        std::string message; // "AttributeError: ..." + full traceback
        double time = 0.0;
    };

    // Runs script behaviours: instances, physics event queue, deferred destroy, hot reload.
    // Registered after PhysicsSystem. Python objects live here, not in components.
    class ScriptSystem final : public ecs::IUpdateSystem
    {
    public:
        ScriptSystem(ScriptRuntime& runtime, input::InputManager& input, scene::PrefabLibrary& prefabs, core::Logger& logger);
        ~ScriptSystem() override;

        void Update(ecs::World& world, float deltaTime) override;

        void RequestReloadAll(); // explicit command (F5 / editor button)
        void ResetInstances(); // on SceneLoadedEvent and on Stop: drop every instance
        void Shutdown(); // release every py::object, called before ScriptRuntime::Shutdown

        const std::deque<ScriptError>& GetRecentErrors() const; // last 32
        const std::map<std::string, std::string>& GetHudLines() const; // hud.set(...)
        nlohmann::json GetLiveFields(ecs::EntityId entity, std::size_t scriptIndex) const; // current values in Play

    private:
        ScriptRuntime& runtime_;
        input::InputManager& input_;
        scene::PrefabLibrary& prefabs_;
        core::Logger& logger_;

        std::deque<ScriptError> recentErrors_;
        std::map<std::string, std::string> hudLines_;
    };
}