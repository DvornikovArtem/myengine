#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <windows.h>
#include <pybind11/embed.h>
#include <tracy/Tracy.hpp>

#include <myengine/core/Logger.h>
#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/CameraControllerComponent.h>
#include <myengine/ecs/components/ColliderComponent.h>
#include <myengine/ecs/components/HierarchyComponent.h>
#include <myengine/ecs/components/MeshRendererComponent.h>
#include <myengine/ecs/components/RigidbodyComponent.h>
#include <myengine/ecs/components/ScriptComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/events/EventBus.h>
#include <myengine/input/InputManager.h>
#include <myengine/jobs/JobSystem.h>
#include <myengine/physics/PhysicsEvents.h>
#include <myengine/scene/SceneEvents.h>
#include <myengine/scripting/PrefabLibrary.h>
#include <myengine/scripting/ScriptRuntime.h>
#include <myengine/scripting/ScriptSystem.h>

namespace py = pybind11;

namespace
{
    namespace ecs = myengine::ecs;
    namespace components = ecs::components;

    void Check(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    class Fixture
    {
    public:
        Fixture()
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            directory = std::filesystem::temp_directory_path() / std::filesystem::u8path("myengine T2 скрипты " + std::to_string(stamp));
            Check(std::filesystem::create_directory(directory), "Could not create a unique temporary directory");
            const auto source = std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "tests/scripting/api_behaviours.py";
            std::filesystem::copy_file(source, directory / "api_behaviours.py");
            logger = std::make_unique<myengine::core::Logger>();
            Check(logger->Initialize(directory / "test.log"), "Logger initialization failed");
            const auto prefabsDirectory = directory / "prefabs";
            std::filesystem::create_directory(prefabsDirectory);
            const nlohmann::json prefab = {{"entities", nlohmann::json::array({
                {{"id", 10}, {"Tag", {{"name", "Prefabricated"}}}, {"Transform", {{"position", {1, 0, 0}}}},
                    {"Script", {{"scripts", nlohmann::json::array({{{"module", "api_behaviours"}, {"class", "Counter"}, {"props", {{"count", 20}}}}})}}}},
                {{"id", 30}, {"Tag", {{"name", "PrefabChild"}}}, {"Hierarchy", {{"parent", 10}}},
                    {"Script", {{"scripts", nlohmann::json::array({{{"module", "api_behaviours"}, {"class", "Counter"}, {"props", {{"count", 30}}}}})}}}}
            })}};
            std::ofstream prefabFile(prefabsDirectory / "test.prefab.json", std::ios::binary);
            prefabFile << prefab.dump();
            prefabFile.close();
            Check(static_cast<bool>(prefabFile), "Could not write the test prefab");
            Check(prefabs.Initialize(prefabsDirectory, logger.get()), "Test prefabs initialization failed");
            wchar_t executable[32768]{};
            Check(GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))) != 0, "Executable path unavailable");
            const auto executableDirectory = std::filesystem::path(executable).parent_path();
            Check(runtime.Initialize({executableDirectory, directory, logger.get()}), "Embedded Python initialization failed");
            system = std::make_unique<myengine::scripting::ScriptSystem>(runtime, input, prefabs, *logger);
            myengine::core::ServiceLocator::GetEditorRuntimeState().mode = myengine::editor::RuntimeMode::Play;
        }

        ~Fixture()
        {
            system.reset(); // subscriptions and Python objects go away before the interpreter
            prefabs.Shutdown();
            runtime.Shutdown();
            logger.reset(); // close the log before deleting the owned temporary directory
            if (!keepArtifacts)
            {
                std::error_code error;
                std::filesystem::remove_all(directory, error);
            }
        }

        ecs::EntityId Entity(const std::string& name)
        {
            const auto id = world.CreateEntity();
            world.Emplace<components::TagComponent>(id).name = name;
            return id;
        }

        void Script(const ecs::EntityId entity, const char* className, nlohmann::json props = nlohmann::json::object())
        {
            auto* scripts = world.TryGet<components::ScriptComponent>(entity);
            if (scripts == nullptr)
            {
                scripts = &world.Emplace<components::ScriptComponent>(entity);
            }
            scripts->scripts.push_back({"api_behaviours", className, std::move(props)});
        }

        void Tick(const float deltaTime = 0.016f)
        {
            system->Update(world, deltaTime);
        }

        template <class Predicate>
        void WaitFor(Predicate&& ready)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!ready())
            {
                Check(std::chrono::steady_clock::now() < deadline, "Timed out waiting for the Streaming file watcher / hot reload");
                Tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        std::filesystem::path directory;
        std::unique_ptr<myengine::core::Logger> logger;
        myengine::input::InputManager input;
        myengine::scene::PrefabLibrary prefabs;
        myengine::scripting::ScriptRuntime runtime;
        ecs::World world;
        std::unique_ptr<myengine::scripting::ScriptSystem> system;
        bool keepArtifacts = false;
    };

    void WriteModule(const Fixture& fixture, const std::string& source)
    {
        std::ofstream file(fixture.directory / "api_behaviours.py", std::ios::binary | std::ios::trunc);
        file << source;
        Check(static_cast<bool>(file), "Could not write a temporary test module");
    }

    void TestApiAndLifecycle(Fixture& fixture)
    {
        const auto player = fixture.Entity("Player");
        fixture.world.Emplace<components::TransformComponent>(player);
        fixture.world.Emplace<components::RigidbodyComponent>(player);
        fixture.world.Emplace<components::ColliderComponent>(player);
        fixture.world.Emplace<components::MeshRendererComponent>(player);
        const auto manager = fixture.Entity("Manager");
        fixture.Script(manager, "Counter", {{"count", 10}, {"enabled", true}, {"label", "from props"}});
        for (const auto& item : {std::pair{"QueryFar", 3.0f}, std::pair{"QueryNear", 1.0f}, std::pair{"QueryEdge", 2.0f}})
        {
            fixture.world.Emplace<components::TransformComponent>(fixture.Entity(item.first)).position = {item.second, 0, 0};
        }
        const auto parent = fixture.Entity("Parent");
        fixture.world.Emplace<components::TransformComponent>(parent).position = {100, 0, 0};
        const auto child = fixture.Entity("QueryChild");
        fixture.world.Emplace<components::TransformComponent>(child).position = {2, 0, 0};
        Check(fixture.world.SetParent(child, parent), "Could not create a parent/child test hierarchy");
        fixture.input.BindAction("test_action", 'R');
        fixture.input.OnKeyDown('R');
        fixture.input.OnKeyDown(VK_SPACE);
        fixture.input.OnKeyDown(VK_F1);
        fixture.Tick(0.25f);

        py::dict globals;
        py::eval_file((std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "tests/scripting/api_checks.py").u8string(), globals);
        globals["components"]();
        globals["queries"]();
        globals["input_and_time"]();
        Check(fixture.world.Get<components::TransformComponent>(player).position.x == 2, "Python did not change the C++ transform");
        Check(fixture.world.Get<components::RigidbodyComponent>(player).velocity.x == 3, "Python did not change the C++ rigidbody");
        Check(!fixture.world.Get<components::MeshRendererComponent>(player).visible, "Python did not change the C++ mesh visibility");
        py::exec("saved_mesh = me.world.find('Player').mesh", globals);
        fixture.world.Remove<components::MeshRendererComponent>(player);
        py::exec(R"PY(
try:
    saved_mesh.visible
    assert False, "A removed component was accessed through a cached proxy"
except AttributeError:
    pass
)PY", globals);
        fixture.world.Emplace<components::MeshRendererComponent>(player).visible = true;
        py::exec("assert saved_mesh.visible", globals);
        py::exec(R"PY(
import api_behaviours as behaviours
manager = me.world.find("Manager")
counter = manager.get_script(behaviours.Counter)
assert counter.count == 11 and counter.label == "from props"
assert manager.get_script(behaviours.Listener) is None
assert me.send("Manager", "add", 5)
assert counter.count == 16
assert not me.send("missing", "add", 1)
assert not me.send("Manager", "missing_method")
try:
    manager.get_script(int)
    assert False
except TypeError:
    pass
me.hud.set("test", "Python HUD")
)PY", globals);
        Check(fixture.system->GetHudLines().at("test") == "Python HUD", "HUD text was not stored");
        py::exec("me.hud.clear('test')", globals);
        Check(fixture.system->GetHudLines().count("test") == 0, "HUD text was not cleared");
        fixture.input.BeginFrame();
        py::exec("assert me.input.is_down('test_action') and not me.input.was_pressed('test_action')", globals);
        fixture.input.ReleaseAllInputs();
        py::exec("assert not me.input.is_key_down('R')", globals);

        const auto listenerA = fixture.Entity("ListenerA");
        const auto listenerB = fixture.Entity("ListenerB");
        fixture.Script(listenerA, "Listener");
        fixture.Script(listenerB, "Listener");
        fixture.Tick();
        auto& bus = myengine::core::ServiceLocator::GetEventBus();
        bus.Publish(myengine::physics::CollisionEvent{listenerA, listenerB, {}, {1, 0, 0}, 2});
        bus.Publish(myengine::physics::TriggerEvent{listenerA, listenerB, {}});
        bus.Publish(myengine::physics::TriggerEvent{listenerB, listenerA, {}});
        fixture.Tick();
        py::exec(R"PY(
a = me.world.find("ListenerA").get_script(behaviours.Listener)
b = me.world.find("ListenerB").get_script(behaviours.Listener)
assert a.collisions == b.collisions == 1
assert a.triggers == b.triggers == 1
assert a.normal == me.Vec3(1, 0, 0) and b.normal == me.Vec3(-1, 0, 0)
)PY", globals);

        const auto broken = fixture.Entity("Broken");
        fixture.Script(broken, "Broken");
        fixture.Tick();
        Check(fixture.system->GetInstanceStatus(broken, 0) == "Faulted", "A failed OnUpdate did not fault its instance");
        const auto errorCount = fixture.system->GetRecentErrors().size();
        fixture.Tick();
        Check(fixture.system->GetRecentErrors().size() == errorCount, "A faulted instance was called again");
        Check(fixture.system->GetInstanceStatus(manager, 0) == "Active", "A failed script stopped another instance");
        py::exec("assert not me.send('Manager', 'fail')", globals);
        Check(fixture.system->GetInstanceStatus(manager, 0) == "Faulted", "A failed message did not fault the recipient");
        const auto& messageError = fixture.system->GetRecentErrors().back();
        Check(messageError.file == "api_behaviours.py" && messageError.line > 0, "Python error has no file:line");

        // Deferred deletion uses Artem's T3 queue, including recursive removal and repeated requests.
        const auto doomed = fixture.Entity("Doomed");
        fixture.world.Emplace<components::TransformComponent>(doomed);
        fixture.Script(doomed, "Counter");
        const auto doomedChild = fixture.Entity("DoomedChild");
        fixture.Script(doomedChild, "Counter");
        Check(fixture.world.SetParent(doomedChild, doomed), "Could not create a destroy subtree");
        fixture.Tick();
        py::exec(R"PY(
doomed = me.world.find("Doomed")
saved_transform = doomed.transform
doomed.destroy()
doomed.destroy()
assert doomed.alive and me.world.find("Doomed") is None
)PY", globals);
        fixture.Tick();
        Check(!fixture.world.IsAlive(doomed) && !fixture.world.IsAlive(doomedChild), "Destroy did not remove the whole subtree");
        py::exec(R"PY(
assert not doomed.alive
doomed.destroy()  # remains idempotent after the flush too
try:
    saved_transform.position
    assert False, "A dead component reference was accepted"
except me.EntityDeadError:
    pass
try:
    saved_transform.position = me.Vec3(float("nan"), 0, 0)
    assert False, "A dead proxy must be rejected before validating component values"
except me.EntityDeadError:
    pass
assert behaviours.events.count(("destroy", "Doomed")) == 1
assert behaviours.events.count(("destroy", "DoomedChild")) == 1
saved_player = me.world.find("Player")
saved_body = saved_player.rigidbody
)PY", globals);

        fixture.world.ClearEntities();
        const auto newPlayer = fixture.Entity("NewPlayer");
        Check(newPlayer == player, "Test must reuse the old entity id after scene replacement");
        fixture.world.Emplace<components::RigidbodyComponent>(newPlayer);
        bus.Publish(myengine::scene::SceneLoadedEvent{&fixture.world});
        fixture.Tick();
        py::exec(R"PY(
assert not saved_player.alive
assert saved_player != me.world.find("NewPlayer")
try:
    saved_body.velocity
    assert False, "A proxy from the previous scene accessed a new entity with the same id"
except me.EntityDeadError:
    pass
try:
    saved_body.mass = 0
    assert False, "A component setter from the previous scene was accepted"
except me.EntityDeadError:
    pass
)PY", globals);
        Check(fixture.system->GetHudLines().empty(), "Stop / scene reset did not clear the HUD");
    }

    // me.camera (the primary scene camera) and me.debug (collider wireframes), RL1
    void TestCameraAndDebug(Fixture& fixture)
    {
        fixture.Tick(); // the script context holds the world after an update
        py::dict globals;
        py::exec("import myengine as me", globals);
        py::exec(R"PY(
assert me.camera.set(me.Vec3(0, 0, 0), me.Vec3(0, 0, 0)) is False
assert me.camera.get() is None and me.camera.get_fov() is None
)PY", globals);

        const auto cameraEntity = fixture.Entity("Camera");
        auto& camera = fixture.world.Emplace<components::CameraComponent>(cameraEntity);
        camera.isPrimary = false;
        auto& controller = fixture.world.Emplace<components::CameraControllerComponent>(cameraEntity);
        py::exec("assert me.camera.set(me.Vec3(1, 2, 3), me.Vec3(0, 0, 0)) is False", globals);
        Check(!controller.scriptControlled, "A camera that is not primary must stay under the free-fly controller");

        camera.isPrimary = true;
        py::exec(R"PY(
assert me.camera.set(me.Vec3(1, 2, 3), me.Vec3(10, 20, 30), 80.0)
position, rotation = me.camera.get()
assert (position.x, position.y, position.z) == (1, 2, 3)
assert (rotation.x, rotation.y, rotation.z) == (10, 20, 30)
assert me.camera.get_fov() == 80.0
assert me.camera.set(me.Vec3(4, 5, 6), me.Vec3(0, 90, 0))   # no fov: the old one stays
assert me.camera.get_fov() == 80.0
for bad in (0.0, 180.0, float("nan")):
    try:
        me.camera.set(me.Vec3(0, 0, 0), me.Vec3(0, 0, 0), bad)
        assert False, "fov out of range was accepted"
    except ValueError:
        pass
try:
    me.camera.set(me.Vec3(float("nan"), 0, 0), me.Vec3(0, 0, 0))
    assert False, "a non-finite position was accepted"
except ValueError:
    pass
)PY", globals);
        Check(camera.position.x == 4 && camera.position.y == 5 && camera.position.z == 6, "Python did not move the C++ camera");
        Check(camera.rotationDeg.y == 90 && camera.fovYDeg == 80.0f, "Python did not rotate the C++ camera");
        Check(controller.scriptControlled, "me.camera.set did not hand the camera to the script");

        auto& physics = myengine::core::ServiceLocator::GetPhysicsWorldState();
        const bool wireframes = physics.debugDrawEnabled;
        py::exec("me.debug.show_colliders(False); assert me.debug.colliders_visible() is False", globals);
        Check(!physics.debugDrawEnabled, "me.debug.show_colliders(False) did not reach the physics state");
        py::exec("me.debug.show_colliders(True); assert me.debug.colliders_visible() is True", globals);
        Check(physics.debugDrawEnabled, "me.debug.show_colliders(True) did not reach the physics state");
        physics.debugDrawEnabled = wireframes;

        fixture.world.DestroyEntity(cameraEntity);
    }

    void TestSpawn(Fixture& fixture)
    {
        const auto spawner = fixture.Entity("Spawner");
        fixture.Script(spawner, "PrefabSpawner");
        fixture.Tick();
        py::dict globals;
        py::exec(R"PY(
import myengine as me
import api_behaviours as behaviours
spawner = me.world.find("Spawner").get_script(behaviours.PrefabSpawner)
spawned = spawner.spawned
assert spawned.alive and spawner.waiting_for_start
assert spawned.transform.position == me.Vec3(4, 2, 3)
assert spawned.get_script(behaviours.Counter) is None
assert ("start", "Prefabricated") not in behaviours.events
saved_player = me.world.find("NewPlayer")
)PY", globals);
        const auto root = globals["spawned"].attr("id").cast<ecs::EntityId>();
        const auto child = fixture.world.Get<components::HierarchyComponent>(root).children.at(0);
        Check(fixture.world.Get<components::HierarchyComponent>(child).parent == root, "Python spawn did not remap the child parent");
        Check(fixture.system->GetInstanceStatus(root, 0).empty(), "Spawned behaviour started in the same frame");
        fixture.Tick();
        py::exec(R"PY(
assert spawned.get_script(behaviours.Counter).count == 21
assert me.world.find("PrefabChild").get_script(behaviours.Counter).count == 31
assert behaviours.events.count(("start", "Prefabricated")) == 1
assert saved_player.alive  # spawn does not publish SceneLoadedEvent or invalidate handles
second = me.world.spawn("test")
third = me.world.spawn("test", position=None)
assert second != third and second != spawned
assert second.transform.position == me.Vec3(1, 0, 0)
assert third.get_script(behaviours.Counter) is None
)PY", globals);
        const auto count = fixture.world.GetEntities().size();
        py::exec(R"PY(
for name in ("missing", "../test"):
    try:
        me.world.spawn(name)
        assert False, "Invalid prefab was spawned"
    except RuntimeError:
        pass
try:
    me.world.spawn("test", me.Vec3(float("nan"), 0, 0))
    assert False, "An invalid spawn position was accepted"
except ValueError:
    pass
try:
    me.world.spawn("test", (1, 2, 3))
    assert False, "Spawn position must be Vec3 or None"
except TypeError:
    pass
spawned.destroy()
)PY", globals);
        Check(fixture.world.GetEntities().size() == count, "Failed Python spawn changed the world");
        fixture.Tick();
        Check(!fixture.world.IsAlive(root) && !fixture.world.IsAlive(child), "Destroy did not remove a spawned prefab subtree");
        py::exec("assert not spawned.alive and saved_player.alive\nassert behaviours.events.count(('destroy', 'PrefabChild')) == 1", globals);
    }

    void TestHotReloadAndFields(Fixture& fixture)
    {
        const auto probe = fixture.Entity("Reload");
        fixture.Script(probe, "ReloadProbe");
        fixture.Tick();
        const auto fields = fixture.runtime.DescribeFields("api_behaviours", "ReloadProbe");
        Check(fields.size() == 3, "T7 field descriptions are incomplete");
        py::dict globals;
        py::exec("import myengine as me\nimport api_behaviours\nold = me.world.find('Reload').get_script(api_behaviours.ReloadProbe)\nold.state = 123", globals);
        const auto path = std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "tests/scripting/api_behaviours.py";
        std::ifstream originalFile(path, std::ios::binary);
        std::string source{std::istreambuf_iterator<char>(originalFile), std::istreambuf_iterator<char>()};
        const auto speedPosition = source.find("speed: float = 3.0");
        Check(speedPosition != std::string::npos, "Reload test source marker missing");
        source.replace(speedPosition, std::string("speed: float = 3.0").size(), "speed: float = 8.0");
        myengine::core::ServiceLocator::GetEditorRuntimeState().scriptReloadKeepsState = true;
        WriteModule(fixture, source);
        fixture.system->RequestReloadAll();
        fixture.WaitFor([&]() { return fixture.system->GetLiveFields(probe, 0).value("speed", 0.0) == 8.0; });
        py::exec(R"PY(
import api_behaviours
fresh = me.world.find("Reload").get_script(api_behaviours.ReloadProbe)
assert fresh is not old and fresh.state == 123
assert fresh.speed == 8 and fresh.reloads == 1 and fresh.ticks >= old.ticks
assert fresh.entity.alive  # hot reload does not invalidate entity handles
saved_module = api_behaviours
)PY", globals);
        const auto newFields = fixture.runtime.DescribeFields("api_behaviours", "ReloadProbe");
        Check(newFields.front().defaultValue == 8.0, "Hot reload did not invalidate the inspector field cache");

        const auto errorsBefore = fixture.system->GetRecentErrors().size();
        WriteModule(fixture, "def broken(:\n");
        fixture.system->RequestReloadAll();
        fixture.WaitFor([&]() { return fixture.system->GetRecentErrors().size() > errorsBefore; });
        Check(fixture.system->GetInstanceStatus(probe, 0) == "Active", "A syntax error stopped the previously loaded script");
        py::exec("import sys\nassert sys.modules['api_behaviours'] is saved_module", globals);
        Check(fixture.system->GetRecentErrors().back().file == "api_behaviours.py", "A reload error has no filename");

        myengine::core::ServiceLocator::GetEditorRuntimeState().scriptReloadKeepsState = false;
        WriteModule(fixture, source);
        fixture.system->RequestReloadAll();
        fixture.WaitFor([&]() { return fixture.system->GetLiveFields(probe, 0).value("reloads", -1) == 0; });
        py::exec("import api_behaviours\nassert me.world.find('Reload').get_script(api_behaviours.ReloadProbe).state == 42", globals);
        myengine::core::ServiceLocator::GetEditorRuntimeState().mode = myengine::editor::RuntimeMode::Edit;
        fixture.Tick();
        Check(fixture.system->GetInstanceStatus(probe, 0).empty(), "Stop did not release script instances");
        Check(myengine::core::ServiceLocator::GetEditorRuntimeState().scriptStats.instances == 0, "Stop did not clear the instance counter");
    }

    void TestScriptProfiling(Fixture& fixture)
    {
        fixture.system->ResetInstances();
        fixture.world.ClearEntities();
        auto& editorState = myengine::core::ServiceLocator::GetEditorRuntimeState();
        auto& stats = editorState.scriptStats;
        Check(stats.updateMs == 0.0, "Reset did not clear the script timing");
        editorState.mode = myengine::editor::RuntimeMode::Play;

        const auto receiver = fixture.Entity("TimingReceiver");
        fixture.Script(receiver, "ProfileProbe");
        const auto sender = fixture.Entity("TimingSender");
        fixture.Script(sender, "ProfileProbe", {{"delay", 0.01}, {"receiver", "TimingReceiver"}, {"message_delay", 0.01}});

        const auto timedTick = [&](const double minimumMs)
        {
            stats.updateMs = -1.0; // every path must publish a fresh sample
            const auto start = std::chrono::steady_clock::now();
            fixture.Tick();
            const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            Check(std::isfinite(stats.updateMs) && stats.updateMs >= minimumMs, "Script work or an early-return path was not timed");
            Check(stats.updateMs <= elapsedMs, "Nested send calls were counted twice in the script timing");
        };
        timedTick(20.0);
        py::dict globals;
        py::exec(R"PY(
import myengine as me
import api_behaviours as behaviours
sender = me.world.find("TimingSender").get_script(behaviours.ProfileProbe)
receiver = me.world.find("TimingReceiver").get_script(behaviours.ProfileProbe)
assert receiver.messages == 1
sender.receiver = ""
sender.fail = True
)PY", globals);
        const auto errors = stats.errors;
        timedTick(10.0);
        Check(fixture.system->GetInstanceStatus(sender, 0) == "Faulted", "Profiling changed exception handling");
        Check(stats.errors == errors + 1, "A profiled exception was not reported exactly once");
        timedTick(0.0); // a faulted object is skipped, but the frame still gets a new measurement
        Check(stats.errors == errors + 1, "Profiling retried a faulted callback");

        editorState.mode = myengine::editor::RuntimeMode::Edit;
        timedTick(0.0); // the Edit-mode early return still includes watcher and statistics work
        Check(stats.instances == 0, "Profiling prevented Stop from releasing instances");
        fixture.system->ResetInstances();
        Check(stats.updateMs == 0.0, "Stop left a stale script timing");
        fixture.system->Shutdown();
        Check(stats.updateMs == 0.0, "Shutdown left a stale script timing");

        myengine::scripting::ScriptRuntime disabledRuntime;
        myengine::scene::PrefabLibrary disabledPrefabs;
        myengine::scripting::ScriptSystem disabledSystem(disabledRuntime, fixture.input, disabledPrefabs, *fixture.logger);
        stats.updateMs = -1.0;
        disabledSystem.Update(fixture.world, 0.016f);
        Check(std::isfinite(stats.updateMs) && stats.updateMs >= 0.0, "Disabled scripting left a stale timing");
    }

    int RunTests(Fixture& fixture)
    {
        try
        {
            TestApiAndLifecycle(fixture);
            TestCameraAndDebug(fixture);
            TestSpawn(fixture);
            TestHotReloadAndFields(fixture);
            TestScriptProfiling(fixture);
            std::cout << "OK: script API, prefab spawn, safe handles, lifecycle, messages, events, hot reload, inspector fields and profiling\n";
            return 0;
        }
        catch (const std::exception& error)
        {
            // The exception (and its Python references) dies while the interpreter is still alive.
            std::cerr << "FAILED: " << error.what() << '\n';
            std::cerr << "Log: " << (fixture.directory / "test.log").u8string() << '\n';
            fixture.keepArtifacts = true;
            return 1;
        }
    }
}

int main(const int argc, char** argv)
{
    if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--wait-for-tracy"))
    {
        std::cerr << "Usage: myengine_script_api_tests [--wait-for-tracy]\n";
        return 1;
    }
    if (argc == 2)
    {
#if defined(TRACY_ENABLE)
        // The headless test can finish before capture retries its connection. Only a manual trace run waits.
        std::cout << "Waiting for Tracy capture (up to 10 seconds)...\n" << std::flush;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!TracyIsConnected && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!TracyIsConnected)
        {
            std::cerr << "FAILED: start Tracy capture first, then retry\n";
            return 1;
        }
#else
        std::cerr << "FAILED: Tracy capture requires RelWithDebInfo\n";
        return 1;
#endif
    }

    myengine::jobs::Initialize(2);
    int result = 1;
    try
    {
        Fixture fixture;
        result = RunTests(fixture);
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    myengine::jobs::Shutdown();
    return result;
}
