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

#include <pybind11/pybind11.h>

#include <myengine/core/FileWatcher.h>
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
#include "ScriptApi.h"
#include "ScriptContext.h"
#include "ScriptHelpers.h"
#include "ScriptModules.h"

namespace py = pybind11;

namespace myengine::scripting
{
    namespace
    {
        constexpr std::size_t kMaxRecentErrors = 32;
        constexpr int kMaxDestroyRounds = 16; // OnDestroy may destroy more entities; the rest waits for the next frame


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
            std::string module; // "coin"
            std::string className; // "Coin"
            std::string name; // "coin.Coin", for messages
            std::size_t scriptIndex = 0; // index in ScriptComponent::scripts, for props on hot reload
            InstanceState state = InstanceState::PendingStart;
            bool started = false; // OnStart has finished: hot reload may keep the state
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

        // Hot reload: the scan of assets/scripts runs in a Streaming job, changes arrive here on the main thread
        core::FileWatcher watcher;
        std::vector<core::FileChange> fileChanges;
        bool expectFullBatch = false; // the next batch is a "reload all" (F5 or a changed helper module)

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
            }

            instances.clear();
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

        // Creates the Python object of a behaviour: module -> class -> cls() -> entity -> props.
        // Used when an entity appears and when hot reload replaces an object
        template <class Report>
        bool MakeObject(Instance& instance, const ecs::EntityId entity, const nlohmann::json& props, core::Logger& logger, Report&& report)
        {
            return SafeCall(instance, report, "create", [&]()
                {
                    const py::object behaviourType = py::type::of<detail::Behaviour>();
                    py::module_ module = detail::GetModule(instance.module);
                    py::object cls = module.attr(instance.className.c_str());

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

                    const auto warnings = detail::Helper("apply_props")(object, cls, props.dump()).cast<py::list>();
                    for (const auto& warning : warnings)
                    {
                        logger.Warning("ScriptSystem: " + instance.name + " (entity " + std::to_string(entity) + "): " +
                            warning.cast<std::string>());
                    }

                    instance.object = std::move(object);
                    instance.native = native;
                });
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

        // Every *.py in assets/scripts (the source tree), new files included. Changes are applied in Update
        if (runtime_.IsInitialized())
        {
            impl_->watcher.WatchDirectory(runtime_.GetScriptsDir(), ".py",
                [this](const core::FileChange& change) { impl_->fileChanges.push_back(change); });
        }
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
        if (context.system == this && context.world != nullptr && context.world != &world)
        {
            ResetInstances();
        }
        if (context.system != this || context.world != &world)
        {
            ++context.sceneVersion;
        }
        context.world = &world;
        context.system = this;
        context.input = &input_;
        context.deltaTime = deltaTime;
        context.totalTime += deltaTime;
        ++context.frame;

        // Script errors are handled per call (SafeCall). This is the last line: nothing from the scripting layer
        // may leave Update and stop the engine loop
        try
        {
            // 0. Hot reload, in Edit too: the scan runs in a Streaming job, swapping modules happens here
            {
                ZoneScopedN("Scripts::HotReload");
                impl_->watcher.Poll();
                ProcessFileChanges();
            }

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

    void ScriptSystem::ProcessFileChanges()
    {
        if (impl_->fileChanges.empty())
        {
            return;
        }

        const auto changes = std::move(impl_->fileChanges);
        impl_->fileChanges.clear();
        const bool fullBatch = impl_->expectFullBatch;
        impl_->expectFullBatch = false;

        struct ModuleChange
        {
            std::string name; // "coin" or "enemies.chaser"
            std::string fileName; // "coin.py", for messages
            const core::FileChange* change = nullptr;
            py::object loaded; // the module currently in sys.modules, or None
            bool helper = false; // loaded and defines no Behaviour: other modules may import from it
        };

        const py::object modules = py::module_::import("sys").attr("modules");
        const py::object behaviourType = py::type::of<detail::Behaviour>();
        const auto& scriptsDir = runtime_.GetScriptsDir();

        std::vector<ModuleChange> moduleChanges;
        for (const auto& change : changes)
        {
            // assets/scripts/enemies/chaser.py -> "enemies.chaser"
            auto relative = change.path.lexically_relative(scriptsDir).replace_extension();
            if (relative.empty() || *relative.begin() == "..")
            {
                continue;
            }
            std::string name;
            for (const auto& part : relative)
            {
                name += (name.empty() ? "" : ".") + part.u8string();
            }

            ModuleChange moduleChange;
            moduleChange.name = name;
            moduleChange.fileName = change.path.filename().u8string();
            moduleChange.change = &change;

            if (change.removed)
            {
                logger_.Warning("ScriptSystem: " + moduleChange.fileName + " was removed; the loaded code stays until the next start");
                continue;
            }
            if (change.contents.empty())
            {
                // An editor may truncate the file first and write it a moment later: the next scan brings the text
                continue;
            }

            moduleChange.loaded = modules.attr("get")(name);
            moduleChange.helper = !moduleChange.loaded.is_none() &&
                !detail::Helper("defines_behaviour")(moduleChange.loaded, behaviourType).cast<bool>();
            moduleChanges.push_back(std::move(moduleChange));
        }

        // Helper modules first: the others import from them while their own code runs
        std::stable_partition(moduleChanges.begin(), moduleChanges.end(), [](const ModuleChange& item) { return item.helper; });

        const auto report = [this](const std::string& name, const char* method, py::error_already_set* error, const char* message)
        {
            ReportError(name, method, error, message);
        };

        bool anyReloaded = false;
        bool helperReloaded = false;
        std::vector<std::string> reloadedModules; // swapped in sys.modules, or new files that compile
        for (const auto& item : moduleChanges)
        {
            const auto path = item.change->path.u8string();
            const py::bytes source(item.change->contents);

            Instance probe; // SafeCall reports through an instance name; nothing else of it is used
            probe.name = item.name;

            if (item.loaded.is_none())
            {
                // Not imported yet: Python reads it from disk when a script needs it. Report syntax errors right now.
                // A script that failed with "No module named ..." comes alive once its file appears
                if (SafeCall(probe, report, "reload", [&]() { detail::Helper("check_syntax")(path, source); }))
                {
                    reloadedModules.push_back(item.name);
                }
                continue;
            }

            const bool ok = SafeCall(probe, report, "reload", [&]() { detail::Helper("reload_module")(item.name, path, source); });
            if (ok)
            {
                logger_.Info("ScriptSystem: hot reload " + item.fileName);
                reloadedModules.push_back(item.name);
                anyReloaded = true;
                helperReloaded = helperReloaded || item.helper;
            }
            else
            {
                logger_.Warning("ScriptSystem: " + item.fileName + " was not reloaded, the previous version keeps running");
            }
        }

        if (reloadedModules.empty())
        {
            return;
        }

        // The inspector reads the field lists again (a new file may also make a failed module importable)
        runtime_.ClearFieldCache();

        if (core::ServiceLocator::GetEditorRuntimeState().mode == editor::RuntimeMode::Play)
        {
            ReloadInstances(reloadedModules);
        }

        if (!anyReloaded)
        {
            return;
        }

        // "from utils import f" in other modules still points to the old function: run all scripts again
        if (helperReloaded && !fullBatch)
        {
            logger_.Info("ScriptSystem: a helper module changed, reloading all scripts");
            RequestReloadAll();
        }
    }

    void ScriptSystem::ReloadInstances(const std::vector<std::string>& modules)
    {
        auto* world = detail::GetScriptContext().world;
        if (world == nullptr)
        {
            return;
        }

        const bool keepState = core::ServiceLocator::GetEditorRuntimeState().scriptReloadKeepsState;
        const auto report = [this](const std::string& name, const char* method, py::error_already_set* error, const char* message)
        {
            ReportError(name, method, error, message);
        };

        unsigned kept = 0;
        unsigned restarted = 0;
        unsigned failed = 0;
        std::set<std::string> changedDefaults;

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
                const auto it = impl_->instances.find(entity);
                if (it == impl_->instances.end() || index >= it->second.size())
                {
                    break;
                }
                if (std::find(modules.begin(), modules.end(), it->second[index].module) == modules.end())
                {
                    continue;
                }

                // Props as they are in the scene / prefab now
                const auto* component = world->TryGet<ecs::components::ScriptComponent>(entity);
                const std::size_t scriptIndex = it->second[index].scriptIndex;
                const nlohmann::json props = component != nullptr && scriptIndex < component->scripts.size()
                    ? component->scripts[scriptIndex].props
                    : nlohmann::json::object();

                Instance fresh;
                fresh.module = it->second[index].module;
                fresh.className = it->second[index].className;
                fresh.name = it->second[index].name;
                fresh.scriptIndex = scriptIndex;

                // The class may be gone or broken in the new code: then the old object keeps running
                if (!MakeObject(fresh, entity, props, logger_, report))
                {
                    ++failed;
                    continue;
                }

                // Looked up again: creating the object ran Python code
                auto& instance = impl_->instances.find(entity)->second[index];
                const bool transfer = keepState && instance.object && instance.started;
                if (transfer)
                {
                    // L2: the state moves to the new object, fields with a changed default take the new value
                    const bool ok = SafeCall(fresh, report, "reload", [&]()
                        {
                            for (const auto& name : detail::Helper("transfer_state")(instance.object, fresh.object))
                            {
                                changedDefaults.insert(name.cast<std::string>());
                            }
                        });
                    if (!ok)
                    {
                        ++failed;
                        continue;
                    }

                    fresh.state = InstanceState::Active;
                    fresh.started = true;
                    instance = std::move(fresh); // the old Python object is released here, without OnDestroy

                    auto* native = instance.native;
                    SafeCall(instance, report, "OnReload", [native]() { native->OnReload(); });
                    ++kept;
                }
                else
                {
                    // L1 (or the old object never started / never existed): start over, OnStart runs this frame
                    fresh.state = InstanceState::PendingStart;
                    instance = std::move(fresh);
                    ++restarted;
                }
            }
        }

        if (kept + restarted + failed == 0)
        {
            return;
        }

        std::string message = "ScriptSystem: hot reload in Play: " + std::to_string(kept) + " object(s) kept their state, " +
            std::to_string(restarted) + " restarted, " + std::to_string(failed) + " kept the old code";
        if (!changedDefaults.empty())
        {
            message += "; fields with a new default:";
            for (const auto& name : changedDefaults)
            {
                message += " " + name;
            }
        }
        logger_.Info(message);
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

            for (std::size_t scriptIndex = 0; scriptIndex < entries.size(); ++scriptIndex)
            {
                const auto& entry = entries[scriptIndex];
                Instance instance;
                instance.module = entry.module;
                instance.className = entry.className;
                instance.name = entry.module + "." + entry.className;
                instance.scriptIndex = scriptIndex;

                const bool ok = MakeObject(instance, entity, entry.props, logger_, report);
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
                    instance.started = true;
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
            const auto result = detail::Helper("stats")(runtime_.GetScriptsDir().u8string()).cast<py::tuple>();
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
        if (error != nullptr)
        {
            // An exception raised by pybind11 itself (e.g. a missing super().__init__()) has no traceback:
            // a null handle can not be passed to Python, None can
            const auto orNone = [](const py::object& value) { return value ? value : py::none(); };
            try
            {
                const auto described = detail::Helper("describe_error")(
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
        // The next scan (a Streaming job, like every scan) reads every script, and all loaded modules are run again
        impl_->watcher.RequestFullRescan();
        impl_->expectFullBatch = true;
    }

    void ScriptSystem::ResetInstances()
    {
        // Invalidate saved handles before releasing old objects: __del__ must not touch the new scene.
        if (detail::GetScriptContext().system == this)
        {
            ++detail::GetScriptContext().sceneVersion;
        }
        impl_->ReleasePythonObjects(runtime_.IsInitialized());
        impl_->events.clear();
        impl_->destroyQueue.clear();
        impl_->destroyPending.clear();
        hudLines_.clear();
    }

    void ScriptSystem::Shutdown()
    {
        // Before jobs::Shutdown: wait for a running file scan
        impl_->watcher.Stop();
        impl_->fileChanges.clear();
        ResetInstances();

        auto& context = detail::GetScriptContext();
        if (context.system == this)
        {
            context.system = nullptr;
            context.world = nullptr;
            context.input = nullptr;
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

    nlohmann::json ScriptSystem::GetLiveFields(const ecs::EntityId entity, const std::size_t scriptIndex) const
    {
        const auto it = impl_->instances.find(entity);
        if (!runtime_.IsInitialized() || it == impl_->instances.end() || scriptIndex >= it->second.size() || !it->second[scriptIndex].object)
        {
            return nlohmann::json::object();
        }

        MYENGINE_ASSERT_SCRIPT_THREAD();
        try
        {
            const auto text = detail::Helper("live_fields")(it->second[scriptIndex].object).cast<std::string>();
            return nlohmann::json::parse(text);
        }
        catch (const std::exception&)
        {
            // A broken property in the script: the inspector shows nothing rather than failing every frame
            return nlohmann::json::object();
        }
    }

    std::string ScriptSystem::GetInstanceStatus(const ecs::EntityId entity, const std::size_t scriptIndex) const
    {
        const auto it = impl_->instances.find(entity);
        if (it == impl_->instances.end() || scriptIndex >= it->second.size())
        {
            return std::string();
        }

        switch (it->second[scriptIndex].state)
        {
        case InstanceState::PendingStart:
            return "Starting";
        case InstanceState::Active:
            return "Active";
        case InstanceState::Faulted:
            return "Faulted";
        }
        return std::string();
    }

    py::object detail::ScriptApi::GetScript(ScriptSystem& system, const ecs::EntityId entity, const py::handle scriptClass)
    {
        MYENGINE_ASSERT_SCRIPT_THREAD();
        if (!PyType_Check(scriptClass.ptr()))
        {
            throw py::type_error("get_script expects a Behaviour class");
        }
        const int subclass = PyObject_IsSubclass(scriptClass.ptr(), py::type::of<detail::Behaviour>().ptr());
        if (subclass < 0)
        {
            throw py::error_already_set();
        }
        if (subclass == 0)
        {
            throw py::type_error("get_script expects a Behaviour class");
        }
        const auto it = system.impl_->instances.find(entity);
        if (it != system.impl_->instances.end() && !system.IsDestroyPending(entity))
        {
            for (const auto& instance : it->second)
            {
                if (instance.object && instance.state != InstanceState::Faulted && py::isinstance(instance.object, scriptClass))
                {
                    return instance.object;
                }
            }
        }
        return py::none();
    }

    bool detail::ScriptApi::Send(ScriptSystem& system, const ecs::EntityId entity, const std::string& method, const py::args& args)
    {
        MYENGINE_ASSERT_SCRIPT_THREAD();
        const auto report = [&system](const std::string& name, const char* operation, py::error_already_set* error, const char* message)
        {
            system.ReportError(name, operation, error, message);
        };
        bool delivered = false;
        for (std::size_t index = 0;; ++index)
        {
            const auto it = system.impl_->instances.find(entity);
            if (it == system.impl_->instances.end() || index >= it->second.size() || system.IsDestroyPending(entity))
            {
                break;
            }
            auto& instance = it->second[index];
            if (!instance.object || instance.state == InstanceState::Faulted)
            {
                continue;
            }
            SafeCall(instance, report, method.c_str(), [&]()
                {
                    const py::object object = instance.object;
                    if (py::hasattr(object, method.c_str()))
                    {
                        object.attr(method.c_str())(*args);
                        delivered = true;
                    }
                });
        }
        return delivered;
    }

    void detail::ScriptApi::SetHudLine(ScriptSystem& system, const std::string& key, const std::string& text)
    {
        MYENGINE_ASSERT_SCRIPT_THREAD();
        system.hudLines_[key] = text;
    }

    void detail::ScriptApi::ClearHudLine(ScriptSystem& system, const std::string& key)
    {
        MYENGINE_ASSERT_SCRIPT_THREAD();
        system.hudLines_.erase(key);
    }
}
