// ScriptSystem.h

#pragma once

#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

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

        // entity.destroy() from a script: the entity and its children are destroyed at the end of the script step.
        // World::DestroyEntity is immediate, and destroying in the middle of a registry walk breaks iterators
        void RequestDestroy(ecs::EntityId entity);
        bool IsDestroyPending(ecs::EntityId entity) const;

        const std::deque<ScriptError>& GetRecentErrors() const; // last 32
        const std::map<std::string, std::string>& GetHudLines() const; // hud.set(...)
        nlohmann::json GetLiveFields(ecs::EntityId entity, std::size_t scriptIndex) const; // current values in Play
        // "Active", "Starting", "Faulted"; empty if the script has no object (Edit mode)
        std::string GetInstanceStatus(ecs::EntityId entity, std::size_t scriptIndex) const;

    private:
        // Python objects (pybind11 types) live in Impl, so this header does not include pybind11
        struct Impl;

        // Hot reload: compile -> new module object -> swap in sys.modules; on any error the old module stays
        void ProcessFileChanges();
        // Hot reload in Play: objects of these modules are replaced by objects of the new classes (L2 keeps state, L1 restarts)
        void ReloadInstances(const std::vector<std::string>& modules);
        // pythonError is a pybind11::error_already_set* (void* keeps pybind11 out of this header)
        void ReportError(const std::string& name, const char* method, void* pythonError, const char* message);

        void CreateInstances(ecs::World& world);
        void StartInstances();
        void DispatchEvents(ecs::World& world);
        void UpdateInstances(ecs::World& world, float deltaTime);
        void FlushDestroyed(ecs::World& world);
        void UpdateStats(float deltaTime);

        ScriptRuntime& runtime_;
        input::InputManager& input_;
        scene::PrefabLibrary& prefabs_;
        core::Logger& logger_;

        std::unique_ptr<Impl> impl_;
        std::deque<ScriptError> recentErrors_;
        std::map<std::string, std::string> hudLines_;
    };
}