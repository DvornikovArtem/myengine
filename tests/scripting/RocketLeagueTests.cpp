// Headless Rocket League match: the real scene, script, prefabs and physics, no window.
// The checks themselves are in rocket_league_checks.py.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

#include <windows.h>
#include <pybind11/embed.h>

#include <myengine/core/Logger.h>
#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/systems/PhysicsSystem.h>
#include <myengine/input/InputManager.h>
#include <myengine/jobs/JobSystem.h>
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
        Check(static_cast<bool>(file), "Could not read the Rocket League test asset");
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    class Fixture
    {
    public:
        Fixture()
        {
            const auto source = std::filesystem::u8path(MYENGINE_SOURCE_DIR);
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            directory = std::filesystem::temp_directory_path() / std::filesystem::u8path("myengine RL матч " + std::to_string(stamp));
            Check(std::filesystem::create_directory(directory), "Could not create a unique temporary directory");
            std::filesystem::copy_file(source / "assets/scripts/rocket_league.py", directory / "rocket_league.py");
            const auto prefabsDirectory = directory / "prefabs";
            std::filesystem::create_directory(prefabsDirectory);
            for (const auto* name : {"rl_car", "rl_ball", "rl_block", "rl_deco", "rl_orb"})
            {
                std::filesystem::copy_file(source / "assets/prefabs" / (std::string(name) + ".prefab.json"),
                    prefabsDirectory / (std::string(name) + ".prefab.json"));
            }
            baseScene = ReadFile(source / "assets/scenes/rocket_league.json"); // read only: the test never saves a scene
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
            myengine::core::ServiceLocator::GetPhysicsWorldState() = {};
            physics = myengine::ecs::systems::PhysicsSystem{};
            input.ReleaseAllInputs();
            Check(myengine::scene::LoadWorldFromString(world, text, logger.get()), "Rocket League scene load failed");
        }

        void Tick(const float deltaTime)
        {
            physics.Update(world, deltaTime);
            system->Update(world, deltaTime);
            input.BeginFrame();
            Check(system->GetRecentErrors().empty(), "The Rocket League script reported an error; see the retained test.log");
            Check(myengine::core::ServiceLocator::GetEditorRuntimeState().scriptStats.faultedInstances == 0, "A Rocket League instance became Faulted");
        }

        std::filesystem::path directory;
        std::string baseScene;
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
            globals["tick"] = py::cpp_function([&](const float dt) { fixture.Tick(dt); }, py::arg("dt") = 1.0f / 60.0f);
            globals["hud_line"] = py::cpp_function([&](const std::string& name)
                {
                    const auto& lines = fixture.system->GetHudLines();
                    const auto found = lines.find(name);
                    return found == lines.end() ? std::string() : found->second;
                });
            globals["key"] = py::cpp_function([&](const unsigned code, const bool down)
                {
                    if (down)
                    {
                        fixture.input.OnKeyDown(code);
                    }
                    else
                    {
                        fixture.input.OnKeyUp(code);
                    }
                });
            py::eval_file((std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "tests/scripting/rocket_league_checks.py").u8string(), globals);
            globals["run_checks"]();
            std::cout << "OK: Rocket League match, car hierarchy, wheels roll and steer\n";
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
