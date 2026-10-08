// ScriptSystem.cpp

#include <myengine/scripting/ScriptSystem.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <pybind11/eval.h>
#include <pybind11/pybind11.h>

#include <myengine/core/Logger.h>
#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/HierarchyComponent.h>
#include <myengine/ecs/components/ScriptComponent.h>
#include <myengine/events/EventBus.h>
#include <myengine/physics/PhysicsEvents.h>
#include <myengine/scene/SceneEvents.h>
#include <myengine/scripting/ScriptRuntime.h>

#include <tracy/Tracy.hpp>

#include "Behaviour.h"
#include "ScriptContext.h"
#include "ScriptModules.h"

namespace py = pybind11;

namespace myengine::scripting
{
    namespace
    {
        constexpr std::size_t kMaxRecentErrors = 32;
        constexpr int kMaxDestroyRounds = 16; // OnDestroy may destroy more entities; the rest waits for the next frame

        // Helpers that are simpler in Python: props -> fields of a new instance, exception -> file:line + traceback
        constexpr const char* kHelpersCode = R"PY(
import gc
import inspect
import json
import os
import sys
import traceback


def _declared_fields(cls):
    """Fields of a behaviour = class attributes with a type annotation, over the whole MRO."""
    fields = {}
    for klass in reversed(cls.__mro__):
        try:
            fields.update(inspect.get_annotations(klass))
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
    summary = "".join(traceback.format_exception_only(exc_type, exc_value)).strip()
    full_text = "".join(traceback.format_exception(exc_type, exc_value, exc_tb)).rstrip()
    return file, line, summary, full_text


def stats(scripts_dir):
    """(number of modules loaded from assets/scripts, number of objects tracked by the garbage collector)"""
    scripts_dir = os.path.normcase(os.path.abspath(scripts_dir))
    modules = sum(1 for module in list(sys.modules.values())
                  if _is_script_file(getattr(module, "__file__", None) or "", scripts_dir))
    return modules, len(gc.get_objects())
)PY";

        enum class InstanceState
        {
            PendingStart,
            Active,
            Faulted,
        };

        struct Instance
        {
            py::object object; // owns the Python object; nullptr if creation failed
            detail::Behaviour* native = nullptr; // the same object seen from C++, valid while `object` is alive
            std::string name; // "coin.Coin", for messages
            InstanceState state = InstanceState::PendingStart;
        };

        struct QueuedEvent
        {
            enum class Kind
            {
                Collision,
                Trigger,
            };

            Kind kind = Kind::Collision;
            ecs::EntityId first = ecs::kInvalidEntity; // CollisionEvent::entityA / TriggerEvent::triggerEntity
            ecs::EntityId second = ecs::kInvalidEntity; // CollisionEvent::entityB / TriggerEvent::otherEntity
            ecs::components::Vec3 point{};
            ecs::components::Vec3 normal{}; // from first to second
            float impulse = 0.0f;
        };

        double SecondsSince(const std::chrono::steady_clock::time_point start)
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }
    }

    struct ScriptSystem::Impl
    {
        std::unordered_map<ecs::EntityId, std::vector<Instance>> instances;
        std::vector<QueuedEvent> events; // filled by EventBus listeners during physics, delivered in Update
        std::vector<ecs::EntityId> destroyQueue;
        std::unordered_set<ecs::EntityId> destroyPending;

        py::object helpers; // dict with the functions from kHelpersCode, created on first use

        events::SubscriptionId collisionSubscription = 0;
        events::SubscriptionId triggerSubscription = 0;
        events::SubscriptionId sceneLoadedSubscription = 0;

        std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
        float statsTimer = 1.0f; // refresh slow stats on the first frame
        std::uint32_t totalErrors = 0;

        // Drops every Python object. Without an interpreter the references are leaked on purpose:
        // releasing them after Py_Finalize would crash
        void ReleasePythonObjects(const bool interpreterAlive)
        {
            if (!interpreterAlive)
            {
                for (auto& [entity, list] : instances)
                {
                    for (auto& instance : list)
                    {
                        instance.object.release();
                    }
                }
                helpers.release();
            }

            instances.clear();
            helpers = py::object();
        }
    };

    namespace
    {
        // Every call from C++ into a script goes through here. A Python exception (or a C++ exception thrown by a
        // binding) is logged with file:line and disables only this instance: the engine and other scripts go on
        template <class Report, class Call>
        bool SafeCall(Instance& instance, Report&& report, const char* method, Call&& call)
        {
            try
            {
                call();
                return true;
            }
            catch (py::error_already_set& error)
            {
                report(instance.name, method, &error, nullptr);
            }
            catch (const std::exception& exception)
            {
                report(instance.name, method, nullptr, exception.what());
            }

            instance.state = InstanceState::Faulted;
            return false;
        }
    }

    ScriptSystem::ScriptSystem(ScriptRuntime& runtime, input::InputManager& input, scene::PrefabLibrary& prefabs, core::Logger& logger)
        : runtime_(runtime),
          input_(input),
          prefabs_(prefabs),
          logger_(logger),
          impl_(std::make_unique<Impl>())
    {
        auto& bus = core::ServiceLocator::GetEventBus();

        // Listeners only copy the event. Calling a script here would run it inside the physics step:
        // creating or destroying a body there breaks the solver, and EventBus is not reentrant
        impl_->collisionSubscription = bus.Subscribe<physics::CollisionEvent>(
            [this](const physics::CollisionEvent& event)
            {
                if (runtime_.IsInitialized())
                {
                    impl_->events.push_back({QueuedEvent::Kind::Collision, event.entityA, event.entityB, event.point, event.normal, event.impulse});
                }
            });
        impl_->triggerSubscription = bus.Subscribe<physics::TriggerEvent>(
            [this](const physics::TriggerEvent& event)
            {
                if (runtime_.IsInitialized())
                {
                    impl_->events.push_back({QueuedEvent::Kind::Trigger, event.triggerEntity, event.otherEntity, event.point, {}, 0.0f});
                }
            });
        // Stop restores the Play snapshot: components are recreated at new addresses, script objects start over
        impl_->sceneLoadedSubscription = bus.Subscribe<scene::SceneLoadedEvent>(
            [this](const scene::SceneLoadedEvent&) { ResetInstances(); });
    }

    ScriptSystem::~ScriptSystem()
    {
        Shutdown();

        auto& bus = core::ServiceLocator::GetEventBus();
        bus.Unsubscribe(impl_->collisionSubscription);
        bus.Unsubscribe(impl_->triggerSubscription);
        bus.Unsubscribe(impl_->sceneLoadedSubscription);
    }

    void ScriptSystem::Update(ecs::World& world, const float deltaTime)
    {
        ZoneScopedN("Scripts::Update");

        if (!runtime_.IsInitialized())
        {
            return;
        }

        MYENGINE_ASSERT_SCRIPT_THREAD();
        auto& context = detail::GetScriptContext();
        context.world = &world;
        context.system = this;

        if (!EnsureHelpers())
        {
            return;
        }

        // Hot reload (T5) goes here: it works in Edit too

        if (core::ServiceLocator::GetEditorRuntimeState().mode != editor::RuntimeMode::Play)
        {
            // Scripts run only in Play. Normally Stop already reset everything through SceneLoadedEvent
            if (!impl_->instances.empty() || !impl_->events.empty() || !impl_->destroyQueue.empty())
            {
                ResetInstances();
            }
            UpdateStats(deltaTime);
            return;
        }

        // Script errors are handled per call (SafeCall). This is the last line: nothing from the scripting layer
        // may leave Update and stop the engine loop
        try
        {
            CreateInstances(world);
            StartInstances();
            DispatchEvents(world);
            UpdateInstances(world, deltaTime);
            FlushDestroyed(world);
            UpdateStats(deltaTime);
        }
        catch (const std::exception& exception)
        {
            logger_.Error(std::string("ScriptSystem: unexpected error, the frame's script step was cut short: ") + exception.what());
        }
    }

    bool ScriptSystem::EnsureHelpers()
    {
        if (impl_->helpers)
        {
            return true;
        }

        try
        {
            py::dict helpers;
            py::exec(kHelpersCode, helpers);
            impl_->helpers = std::move(helpers);
            return true;
        }
        catch (py::error_already_set& error)
        {
            logger_.Error(std::string("ScriptSystem: helper code failed, scripts are not run: ") + error.what());
            return false;
        }
    }

    void ScriptSystem::CreateInstances(ecs::World& world)
    {
        ZoneScopedN("Scripts::Start");

        // Entities removed not by a script (should not happen in Play): drop their objects without OnDestroy
        for (auto it = impl_->instances.begin(); it != impl_->instances.end();)
        {
            it = world.IsAlive(it->first) ? std::next(it) : impl_->instances.erase(it);
        }

        // Collect first: creating an instance runs Python, which must not run inside a registry walk
        std::vector<ecs::EntityId> newEntities;
        world.ForEach<ecs::components::ScriptComponent>(
            [this, &newEntities](const ecs::EntityId entity, ecs::components::ScriptComponent&)
            {
                if (impl_->instances.find(entity) == impl_->instances.end())
                {
                    newEntities.push_back(entity);
                }
            });

        if (newEntities.empty())
        {
            return;
        }

        const py::object behaviourType = py::type::of<detail::Behaviour>();
        const auto report = [this](const std::string& name, const char* method, py::error_already_set* error, const char* message)
        {
            ReportError(name, method, error, message);
        };

        for (const ecs::EntityId entity : newEntities)
        {
            const auto* component = world.TryGet<ecs::components::ScriptComponent>(entity);
            if (component == nullptr)
            {
                continue;
            }

            // A copy: Python code below may change the world
            const auto entries = component->scripts;
            std::vector<Instance> created;
            created.reserve(entries.size());

            for (const auto& entry : entries)
            {
                Instance instance;
                instance.name = entry.module + "." + entry.className;

                const bool ok = SafeCall(instance, report, "create", [&]()
                    {
                        py::module_ module = detail::GetModule(entry.module);
                        py::object cls = module.attr(entry.className.c_str());

                        const int isBehaviour = PyType_Check(cls.ptr()) ? PyObject_IsSubclass(cls.ptr(), behaviourType.ptr()) : 0;
                        if (isBehaviour < 0)
                        {
                            throw py::error_already_set();
                        }
                        if (isBehaviour == 0)
                        {
                            throw std::runtime_error(instance.name + " is not a subclass of myengine.Behaviour");
                        }

                        py::object object = cls();
                        auto* native = object.cast<detail::Behaviour*>();
                        native->entity = detail::EntityRef{entity};

                        const auto warnings = impl_->helpers["apply_props"](object, cls, entry.props.dump()).cast<py::list>();
                        for (const auto& warning : warnings)
                        {
                            logger_.Warning("ScriptSystem: " + instance.name + " (entity " + std::to_string(entity) + "): " +
                                warning.cast<std::string>());
                        }

                        instance.object = std::move(object);
                        instance.native = native;
                    });

                instance.state = ok ? InstanceState::PendingStart : InstanceState::Faulted;
                created.push_back(std::move(instance));
            }

            // Failed instances stay as Faulted placeholders, so creation is not retried (and logged) every frame
            impl_->instances[entity] = std::move(created);
        }
    }

    void ScriptSystem::StartInstances()
    {
        const auto report = [this](const std::string& name, const char* method, py::error_already_set* error, const char* message)
        {
            ReportError(name, method, error, message);
        };

        std::vector<ecs::EntityId> entities;
        entities.reserve(impl_->instances.size());
        for (const auto& [entity, list] : impl_->instances)
        {
            entities.push_back(entity);
        }

        for (const ecs::EntityId entity : entities)
        {
            for (std::size_t index = 0;; ++index)
            {
                // Looked up again after every call: the map is never changed by a script call, but stay defensive
                const auto it = impl_->instances.find(entity);
                if (it == impl_->instances.end() || index >= it->second.size())
                {
                    break;
                }

                auto& instance = it->second[index];
                if (instance.state != InstanceState::PendingStart)
                {
                    continue;
                }

                auto* native = instance.native;
                if (SafeCall(instance, report, "OnStart", [native]() { native->OnStart(); }))
                {
                    instance.state = InstanceState::Active;
                }
            }
        }
    }

    void ScriptSystem::DispatchEvents(ecs::World& world)
    {
        ZoneScopedN("Scripts::Events");

        if (impl_->events.empty())
        {
            return;
        }

        const auto report = [this](const std::string& name, const char* method, py::error_already_set* error, const char* message)
        {
            ReportError(name, method, error, message);
        };

        // Calls `call(native)` for every active instance on the entity
        const auto deliver = [&](const ecs::EntityId entity, const char* method, const auto& call)
        {
            for (std::size_t index = 0;; ++index)
            {
                // Destroyed entities and entities waiting for destroy get no more events
                if (!world.IsAlive(entity) || IsDestroyPending(entity))
                {
                    return;
                }

                const auto it = impl_->instances.find(entity);
                if (it == impl_->instances.end() || index >= it->second.size())
                {
                    return;
                }

                auto& instance = it->second[index];
                if (instance.state == InstanceState::Active)
                {
                    auto* native = instance.native;
                    SafeCall(instance, report, method, [&]() { call(native); });
                }
            }
        };

        // Events published while scripts run (none today) go to the next frame
        const auto events = std::move(impl_->events);
        impl_->events.clear();

        // Physics sends a trigger once per trigger collider: with two triggers the same pair comes twice
        std::set<std::tuple<int, ecs::EntityId, ecs::EntityId>> delivered;

        for (const auto& event : events)
        {
            const auto low = std::min(event.first, event.second);
            const auto high = std::max(event.first, event.second);
            if (!delivered.insert({static_cast<int>(event.kind), low, high}).second)
            {
                continue;
            }

            const detail::EntityRef first{event.first};
            const detail::EntityRef second{event.second};

            if (event.kind == QueuedEvent::Kind::Collision)
            {
                // The normal always points from the receiving entity to the other one
                deliver(event.first, "OnCollision", [&](detail::Behaviour* native) { native->OnCollision(second, event.point, event.normal, event.impulse); });
                deliver(event.second, "OnCollision", [&](detail::Behaviour* native) { native->OnCollision(first, event.point, -event.normal, event.impulse); });
            }
            else
            {
                deliver(event.first, "OnTrigger", [&](detail::Behaviour* native) { native->OnTrigger(second); });
                deliver(event.second, "OnTrigger", [&](detail::Behaviour* native) { native->OnTrigger(first); });
            }
        }
    }

    void ScriptSystem::UpdateInstances(ecs::World& world, const float deltaTime)
    {
        ZoneScopedN("Scripts::OnUpdate");

        const auto report = [this](const std::string& name, const char* method, py::error_already_set* error, const char* message)
        {
            ReportError(name, method, error, message);
        };

        // A copy of the ids, not a walk over the live registry: scripts may spawn entities during OnUpdate
        // (their scripts start next frame)
        std::vector<ecs::EntityId> entities;
        entities.reserve(impl_->instances.size());
        for (const auto& [entity, list] : impl_->instances)
        {
            entities.push_back(entity);
        }

        for (const ecs::EntityId entity : entities)
        {
            for (std::size_t index = 0;; ++index)
            {
                if (!world.IsAlive(entity) || IsDestroyPending(entity))
                {
                    break;
                }

                const auto it = impl_->instances.find(entity);
                if (it == impl_->instances.end() || index >= it->second.size())
                {
                    break;
                }

                auto& instance = it->second[index];
                if (instance.state == InstanceState::Active)
                {
                    auto* native = instance.native;
                    SafeCall(instance, report, "OnUpdate", [native, deltaTime]() { native->OnUpdate(deltaTime); });
                }
            }
        }
    }

    void ScriptSystem::FlushDestroyed(ecs::World& world)
    {
        ZoneScopedN("Scripts::Flush");

        const auto report = [this](const std::string& name, const char* method, py::error_already_set* error, const char* message)
        {
            ReportError(name, method, error, message);
        };

        for (int round = 0; round < kMaxDestroyRounds && !impl_->destroyQueue.empty(); ++round)
        {
            const auto batch = std::move(impl_->destroyQueue);
            impl_->destroyQueue.clear();

            for (const ecs::EntityId root : batch)
            {
                if (!world.IsAlive(root))
                {
                    continue;
                }

                // The entity and all its children, parents first
                std::vector<ecs::EntityId> subtree{root};
                for (std::size_t index = 0; index < subtree.size(); ++index)
                {
                    if (const auto* hierarchy = world.TryGet<ecs::components::HierarchyComponent>(subtree[index]))
                    {
                        subtree.insert(subtree.end(), hierarchy->children.begin(), hierarchy->children.end());
                    }
                }

                // OnDestroy while every entity of the subtree is still alive
                for (const ecs::EntityId entity : subtree)
                {
                    const auto it = impl_->instances.find(entity);
                    if (it == impl_->instances.end())
                    {
                        continue;
                    }

                    for (std::size_t index = 0; index < it->second.size(); ++index)
                    {
                        auto& instance = it->second[index];
                        if (instance.state == InstanceState::Active)
                        {
                            auto* native = instance.native;
                            SafeCall(instance, report, "OnDestroy", [native]() { native->OnDestroy(); });
                        }
                    }
                }

                // Children first, then the parent
                for (auto it = subtree.rbegin(); it != subtree.rend(); ++it)
                {
                    impl_->instances.erase(*it);
                    world.DestroyEntity(*it);
                }
            }
        }

        // Requests made by OnDestroy after the last round are dropped together with the pending marks
        if (!impl_->destroyQueue.empty())
        {
            logger_.Warning("ScriptSystem: too many chained destroy requests, " + std::to_string(impl_->destroyQueue.size()) + " left for the next frame");
            impl_->destroyPending = std::unordered_set<ecs::EntityId>(impl_->destroyQueue.begin(), impl_->destroyQueue.end());
            return;
        }
        impl_->destroyPending.clear();
    }

    void ScriptSystem::UpdateStats(const float deltaTime)
    {
        auto& stats = core::ServiceLocator::GetEditorRuntimeState().scriptStats;
        stats.instances = 0;
        stats.activeInstances = 0;
        stats.faultedInstances = 0;
        for (const auto& [entity, list] : impl_->instances)
        {
            for (const auto& instance : list)
            {
                ++stats.instances;
                stats.activeInstances += instance.state == InstanceState::Active ? 1u : 0u;
                stats.faultedInstances += instance.state == InstanceState::Faulted ? 1u : 0u;
            }
        }
        stats.errors = impl_->totalErrors;

        // gc.get_objects() walks every object: once per second is enough for a leak check
        impl_->statsTimer += deltaTime;
        if (impl_->statsTimer < 1.0f)
        {
            return;
        }
        impl_->statsTimer = 0.0f;

        try
        {
            const auto result = impl_->helpers["stats"](runtime_.GetScriptsDir().u8string()).cast<py::tuple>();
            stats.scriptModules = result[0].cast<std::uint32_t>();
            stats.pythonObjects = result[1].cast<std::uint32_t>();
        }
        catch (py::error_already_set& error)
        {
            logger_.Warning(std::string("ScriptSystem: stats failed: ") + error.what());
        }
    }

    void ScriptSystem::ReportError(const std::string& name, const char* method, void* pythonError, const char* message)
    {
        ScriptError scriptError;
        scriptError.time = SecondsSince(impl_->startTime);
        std::string summary = message != nullptr ? message : "unknown error";
        std::string fullText;

        auto* error = static_cast<py::error_already_set*>(pythonError);
        if (error != nullptr && impl_->helpers)
        {
            // An exception raised by pybind11 itself (e.g. a missing super().__init__()) has no traceback:
            // a null handle can not be passed to Python, None can
            const auto orNone = [](const py::object& value) { return value ? value : py::none(); };
            try
            {
                const auto described = impl_->helpers["describe_error"](
                    orNone(error->type()), orNone(error->value()), orNone(error->trace()), runtime_.GetScriptsDir().u8string()).cast<py::tuple>();
                scriptError.file = described[0].cast<std::string>();
                scriptError.line = described[1].cast<int>();
                summary = described[2].cast<std::string>();
                fullText = described[3].cast<std::string>();
            }
            catch (const std::exception&)
            {
                // error_already_set is a std::exception too
                summary = error->what();
            }
        }
        else if (error != nullptr)
        {
            summary = error->what();
        }

        // First line: file:line, then the full traceback
        std::string firstLine = scriptError.file.empty()
            ? std::string()
            : scriptError.file + ":" + std::to_string(scriptError.line) + ": ";
        firstLine += summary + " [" + name + "." + method + "]";

        scriptError.message = fullText.empty() ? firstLine : firstLine + "\n" + fullText;
        logger_.Error("Script error: " + scriptError.message);

        ++impl_->totalErrors;
        recentErrors_.push_back(std::move(scriptError));
        while (recentErrors_.size() > kMaxRecentErrors)
        {
            recentErrors_.pop_front();
        }
    }

    void ScriptSystem::RequestReloadAll()
    {
        // T5
    }

    void ScriptSystem::ResetInstances()
    {
        impl_->ReleasePythonObjects(runtime_.IsInitialized());
        impl_->events.clear();
        impl_->destroyQueue.clear();
        impl_->destroyPending.clear();
    }

    void ScriptSystem::Shutdown()
    {
        ResetInstances();

        auto& context = detail::GetScriptContext();
        if (context.system == this)
        {
            context.system = nullptr;
            context.world = nullptr;
        }
    }

    void ScriptSystem::RequestDestroy(const ecs::EntityId entity)
    {
        const auto* world = detail::GetScriptContext().world;
        if (world == nullptr || !world->IsAlive(entity))
        {
            return;
        }

        if (impl_->destroyPending.insert(entity).second)
        {
            impl_->destroyQueue.push_back(entity);
        }
    }

    bool ScriptSystem::IsDestroyPending(const ecs::EntityId entity) const
    {
        return impl_->destroyPending.find(entity) != impl_->destroyPending.end();
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