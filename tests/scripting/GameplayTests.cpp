#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <windows.h>
#include <pybind11/embed.h>

#include <myengine/core/Logger.h>
#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/systems/PhysicsSystem.h>
#include <myengine/input/InputManager.h>
#include <myengine/jobs/JobSystem.h>
#include <myengine/physics/PhysicsEvents.h>
#include <myengine/scene/SceneSerializer.h>
#include <myengine/scripting/PrefabLibrary.h>
#include <myengine/scripting/ScriptRuntime.h>
#include <myengine/scripting/ScriptSystem.h>

namespace py = pybind11;

namespace
{
    void Check(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::string ReadFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        Check(static_cast<bool>(file), "Could not read the gameplay test asset");
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    class Fixture
    {
    public:
        Fixture()
        {
            const auto source = std::filesystem::u8path(MYENGINE_SOURCE_DIR);
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            directory = std::filesystem::temp_directory_path() / std::filesystem::u8path("myengine T6 игра " + std::to_string(stamp));
            Check(std::filesystem::create_directory(directory), "Could not create a unique temporary directory");
            for (const auto* name : {"game_manager", "coin_spawner", "coin", "enemy_spawner", "chaser", "firework"})
            {
                std::filesystem::copy_file(source / "assets/scripts" / (std::string(name) + ".py"), directory / (std::string(name) + ".py"));
            }
            const auto prefabsDirectory = directory / "prefabs";
            std::filesystem::create_directory(prefabsDirectory);
            for (const auto* name : {"coin", "chaser", "firework"})
            {
                std::filesystem::copy_file(source / "assets/prefabs" / (std::string(name) + ".prefab.json"),
                    prefabsDirectory / (std::string(name) + ".prefab.json"));
            }
            baseScene = ReadFile(source / "assets/scenes/coin_guard_demo.json");
            originalCoin = ReadFile(directory / "coin.py");
            logger = std::make_unique<myengine::core::Logger>();
            Check(logger->Initialize(directory / "test.log"), "Logger initialization failed");
            Check(prefabs.Initialize(prefabsDirectory, logger.get()), "Prefabs initialization failed");
            wchar_t executable[32768]{};
            Check(GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))) != 0, "Executable path unavailable");
            Check(runtime.Initialize({std::filesystem::path(executable).parent_path(), directory, logger.get()}), "Embedded Python initialization failed");
            system = std::make_unique<myengine::scripting::ScriptSystem>(runtime, input, prefabs, *logger);
        }

        ~Fixture()
        {
            system.reset(); // release Python instances before stopping the interpreter
            prefabs.Shutdown();
            runtime.Shutdown();
            logger.reset();
            if (!keepArtifacts)
            {
                std::error_code error;
                std::filesystem::remove_all(directory, error);
            }
        }

        void LoadScene(const std::string& text)
        {
            auto& editor = myengine::core::ServiceLocator::GetEditorRuntimeState();
            editor.mode = myengine::editor::RuntimeMode::Play;
            editor.scriptReloadKeepsState = true;
            myengine::core::ServiceLocator::GetPhysicsWorldState() = {};
            physics = myengine::ecs::systems::PhysicsSystem{};
            input.ReleaseAllInputs();
            Check(myengine::scene::LoadWorldFromString(world, text, logger.get()), "Gameplay scene load failed");
            snapshot = text;
        }

        void Tick(const float deltaTime, const bool simulatePhysics)
        {
            if (simulatePhysics)
            {
                physics.Update(world, deltaTime);
            }
            system->Update(world, deltaTime);
            input.BeginFrame();
            Check(system->GetRecentErrors().empty(), "A gameplay script reported an error; see the retained test.log");
            Check(myengine::core::ServiceLocator::GetEditorRuntimeState().scriptStats.faultedInstances == 0, "A gameplay instance became Faulted");
        }

        void ChangeCoinRule(const int multiplier)
        {
            auto source = originalCoin;
            const std::string marker = "\"add_score\", self.score_value)";
            const auto position = source.find(marker);
            Check(position != std::string::npos, "Coin rule marker missing");
            source.replace(position, marker.size(), "\"add_score\", self.score_value * " + std::to_string(multiplier) + ")");
            source += "\nTEST_RULE_VERSION = " + std::to_string(multiplier) + "\n";
            std::ofstream file(directory / "coin.py", std::ios::binary | std::ios::trunc);
            file << source;
            file.close();
            Check(static_cast<bool>(file), "Could not change the temporary coin rule");
            system->RequestReloadAll();
        }

        template <class Predicate>
        void WaitFor(Predicate&& ready)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!ready())
            {
                Check(std::chrono::steady_clock::now() < deadline, "Timed out waiting for gameplay hot reload");
                Tick(0.0f, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        std::filesystem::path directory;
        std::string baseScene;
        std::string originalCoin;
        std::string snapshot;
        std::unique_ptr<myengine::core::Logger> logger;
        myengine::input::InputManager input;
        myengine::scene::PrefabLibrary prefabs;
        myengine::scripting::ScriptRuntime runtime;
        myengine::ecs::World world;
        myengine::ecs::systems::PhysicsSystem physics;
        std::unique_ptr<myengine::scripting::ScriptSystem> system;
        bool keepArtifacts = false;
    };

    int RunTests(Fixture& fixture)
    {
        try
        {
            py::dict globals;
            globals["base_scene"] = fixture.baseScene;
            globals["load_scene"] = py::cpp_function([&](const std::string& text) { fixture.LoadScene(text); });
            globals["tick"] = py::cpp_function([&](const float dt, const bool physics) { fixture.Tick(dt, physics); }, py::arg("dt") = 0.016f, py::arg("physics") = false);
            globals["hud_line"] = py::cpp_function([&](const std::string& key) { return fixture.system->GetHudLines().at(key); });
            globals["entity_count"] = py::cpp_function([&]() { return fixture.world.GetEntities().size(); });
            globals["instance_count"] = py::cpp_function([]() { return myengine::core::ServiceLocator::GetEditorRuntimeState().scriptStats.instances; });
            globals["trigger"] = py::cpp_function([](const myengine::ecs::EntityId coin, const myengine::ecs::EntityId player)
                {
                    myengine::core::ServiceLocator::GetEventBus().Publish(myengine::physics::TriggerEvent{coin, player, {}});
                });
            globals["collision"] = py::cpp_function([](const myengine::ecs::EntityId enemy, const myengine::ecs::EntityId player)
                {
                    myengine::core::ServiceLocator::GetEventBus().Publish(myengine::physics::CollisionEvent{enemy, player, {}, {}, 1.0f});
                });
            globals["press_r"] = py::cpp_function([&]()
                {
                    fixture.input.OnKeyDown('R');
                    fixture.Tick(0.0f, false);
                    fixture.input.OnKeyUp('R');
                });
            globals["change_coin_rule"] = py::cpp_function([&](const int multiplier) { fixture.ChangeCoinRule(multiplier); });
            globals["wait_for"] = py::cpp_function([&](const py::function& ready) { fixture.WaitFor([&]() { return ready().cast<bool>(); }); });
            globals["keep_reload_state"] = py::cpp_function([](const bool keep) { myengine::core::ServiceLocator::GetEditorRuntimeState().scriptReloadKeepsState = keep; });
            globals["stop"] = py::cpp_function([&]()
                {
                    myengine::core::ServiceLocator::GetEditorRuntimeState().mode = myengine::editor::RuntimeMode::Edit;
                    fixture.Tick(0.0f, false);
                    Check(myengine::scene::LoadWorldFromString(fixture.world, fixture.snapshot, fixture.logger.get()), "Stop snapshot restore failed");
                    Check(fixture.system->GetHudLines().empty(), "Stop did not clear the gameplay HUD");
                });
            globals["play"] = py::cpp_function([&]()
                {
                    myengine::core::ServiceLocator::GetEditorRuntimeState().mode = myengine::editor::RuntimeMode::Play;
                    fixture.Tick(0.0f, false);
                });
            py::eval_file((std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "tests/scripting/gameplay_checks.py").u8string(), globals);
            globals["run_checks"]();
            Check(fixture.runtime.DescribeFields("game_manager", "GameManager").size() >= 10, "Game balance fields are not exposed to the inspector");
            std::cout << "OK: T6 gameplay, prefabs, physics pickup, waves, chase, hits, win/loss, restart, lifetime, L1/L2 reload and Play/Stop\n";
            return 0;
        }
        catch (const std::exception& error)
        {
            // Python exceptions and test globals must die while the interpreter is alive.
            fixture.keepArtifacts = true;
            std::cerr << "FAILED: " << error.what() << "\nArtifacts: " << fixture.directory.u8string() << '\n';
            return 1;
        }
    }
}

int main()
{
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
