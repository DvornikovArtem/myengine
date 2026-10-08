#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

#include <windows.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <myengine/core/Logger.h>
#include <myengine/core/ServiceLocator.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/ScriptComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/input/InputManager.h>
#include <myengine/jobs/JobSystem.h>
#include <myengine/scene/SceneEvents.h>
#include <myengine/scene/SceneSerializer.h>
#include <myengine/scripting/PrefabLibrary.h>
#include <myengine/scripting/ScriptRuntime.h>
#include <myengine/scripting/ScriptSystem.h>
#include <myengine/ui/ScriptConsole.h>

namespace
{
    namespace ecs = myengine::ecs;
    namespace components = ecs::components;
    namespace scripting = myengine::scripting;
    namespace ui = myengine::ui;
    using json = nlohmann::json;

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
            directory = std::filesystem::temp_directory_path() / std::filesystem::u8path("myengine D6 консоль " + std::to_string(stamp));
            Check(std::filesystem::create_directory(directory), "Could not create a unique test directory");
            std::filesystem::copy_file(std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "tests/scripting/console_behaviours.py", directory / "console_behaviours.py");
            const auto prefabsDirectory = directory / "prefabs";
            std::filesystem::create_directory(prefabsDirectory);
            const json prefab = {{"entities", json::array({
                {{"id", 1}, {"Tag", {{"name", "ConsoleSpawn"}}}, {"Transform", {{"position", {0, 0, 0}}}},
                    {"Script", {{"scripts", json::array({{{"module", "console_behaviours"}, {"class", "ConsoleCounter"}, {"props", {{"count", 20}}}}})}}}},
                {{"id", 2}, {"Tag", {{"name", "ConsoleChild"}}}, {"Hierarchy", {{"parent", 1}}}}
            })}};
            std::ofstream file(prefabsDirectory / "test.prefab.json", std::ios::binary);
            file << prefab.dump();
            file.close();
            Check(static_cast<bool>(file), "Could not create the test prefab");
            logger = std::make_unique<myengine::core::Logger>();
            Check(logger->Initialize(directory / "test.log"), "Logger initialization failed");
            Check(prefabs.Initialize(prefabsDirectory, logger.get()), "Prefab library initialization failed");
            wchar_t executable[32768]{};
            Check(GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))) != 0, "Executable path unavailable");
            Check(runtime.Initialize({std::filesystem::path(executable).parent_path(), directory, logger.get()}), "Embedded Python initialization failed");
            system = std::make_unique<scripting::ScriptSystem>(runtime, input, prefabs, *logger);
            services.execute = [this](const std::string& source) { return system->ExecuteConsole(source); };
            services.reset = [this]() { runtime.ResetConsole(); };
            services.generation = [this]() { return runtime.GetConsoleGeneration(); };
            services.errors = [this]()
            {
                const auto& errors = system->GetRecentErrors();
                return std::vector<scripting::ScriptError>(errors.begin(), errors.end());
            };
            services.clearErrors = [this]() { system->ClearRecentErrors(); };

            IMGUI_CHECKVERSION();
            context = ImGui::CreateContext();
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.DisplaySize = ImVec2(1200.0f, 900.0f);
            io.DeltaTime = 1.0f / 60.0f;
            unsigned char* pixels = nullptr;
            int width = 0, height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        }

        ~Fixture()
        {
            ImGui::DestroyContext(context);
            system.reset();
            prefabs.Shutdown();
            runtime.Shutdown();
            logger.reset();
            if (!keepArtifacts)
            {
                std::error_code error;
                std::filesystem::remove_all(directory, error);
            }
        }

        void Tick()
        {
            system->Update(world, 0.0f);
        }

        scripting::ScriptConsoleResult Run(const std::string& source)
        {
            auto result = system->ExecuteConsole(source);
            if (!result.success)
            {
                throw std::runtime_error("Console failed: " + source + "\n" + result.error);
            }
            return result;
        }

        void Frame(ui::ScriptConsole& console, bool enabled = true)
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(1000, 600), ImGuiCond_Always);
            ImGui::Begin("D6 test", nullptr, ImGuiWindowFlags_NoSavedSettings);
            console.Draw(services, enabled);
            ImGui::End();
            ImGui::Render();
            Check(ImGui::GetDrawData()->TotalVtxCount > 0, "The console did not draw any UI");
        }

        std::filesystem::path directory;
        std::unique_ptr<myengine::core::Logger> logger;
        myengine::input::InputManager input;
        myengine::scene::PrefabLibrary prefabs;
        scripting::ScriptRuntime runtime;
        ecs::World world;
        std::unique_ptr<scripting::ScriptSystem> system;
        ui::ScriptConsoleServices services;
        ImGuiContext* context = nullptr;
        ecs::EntityId player = 0;
        ecs::EntityId counter = 0;
        bool keepArtifacts = false;
    };

    void TestGatesAndRepl(Fixture& fixture)
    {
        auto& editor = myengine::core::ServiceLocator::GetEditorRuntimeState();
        editor.mode = myengine::editor::RuntimeMode::Play;
        Check(!fixture.system->ExecuteConsole("1 + 2").success, "Console ran before the world was attached");
        fixture.player = fixture.world.CreateEntity();
        fixture.world.Emplace<components::TagComponent>(fixture.player).name = "Player";
        fixture.world.Emplace<components::TransformComponent>(fixture.player).position = {1, 2, 3};
        fixture.counter = fixture.world.CreateEntity();
        fixture.world.Emplace<components::TagComponent>(fixture.counter).name = "Counter";
        fixture.world.Emplace<components::ScriptComponent>(fixture.counter).scripts.push_back({"console_behaviours", "ConsoleCounter", json::object()});
        fixture.Tick();
        editor.mode = myengine::editor::RuntimeMode::Edit;
        Check(!fixture.system->ExecuteConsole("me.world.find('Player').name = 'Changed'").success, "An Edit-mode console command changed the scene");
        Check(fixture.world.Get<components::TagComponent>(fixture.player).name == "Player", "Edit gate changed the entity");
        editor.mode = myengine::editor::RuntimeMode::Play;

        Check(fixture.Run("1 + 2").output == "3\n", "Expression result was not displayed");
        Check(fixture.Run("_ + 39").output == "42\n", "The last-result variable did not work");
        Check(fixture.Run("number = 10").output.empty(), "Assignment printed an expression result");
        Check(fixture.Run("number + 5").output == "15\n", "Console variables did not persist");
        Check(fixture.Run("print('Привет из консоли', end='')").output == "Привет из консоли\n", "UTF-8 / partial print was not captured");
        Check(fixture.Run("import sys; print('stderr', file=sys.stderr)").output.find("[ERROR] stderr") != std::string::npos, "stderr was not captured");
        Check(fixture.Run("me.log.warn('warning')").output.find("[WARN] warning") != std::string::npos, "Engine log output was not captured");
        Check(fixture.Run("for index in range(3):").incomplete, "The loop did not request another line");
        Check(fixture.Run("    print(index)").incomplete, "An indented loop body did not request the terminating blank line");
        Check(fixture.Run("").output == "0\n1\n2\n", "The multiline loop did not execute");
        Check(fixture.Run("def total(value):").incomplete && fixture.Run("    return value + number").incomplete, "A function definition did not collect its lines");
        Check(!fixture.Run("").incomplete && fixture.Run("total(2)").output == "12\n", "A function lost its console globals");
    }

    void TestWorldAndLifetime(Fixture& fixture)
    {
        fixture.Run("player = me.world.find('Player')");
        fixture.Run("player.transform.position = me.Vec3(4, 5, 6)");
        Check(fixture.world.Get<components::TransformComponent>(fixture.player).position.x == 4, "Console transform write did not reach the world");
        fixture.Run("from console_behaviours import ConsoleCounter");
        fixture.Run("behaviour = me.world.find('Counter').get_script(ConsoleCounter)");
        fixture.Run("behaviour.count = 100");
        fixture.Tick();
        Check(fixture.system->GetLiveFields(fixture.counter, 0).at("count") == 101, "Console did not reach the live behaviour");
        fixture.Run("spawned = me.world.spawn('test', me.Vec3(2, 3, 4))");
        const auto spawned = fixture.world.GetEntities().size();
        Check(spawned == 4, "Console spawn did not create the prefab root and child");
        Check(fixture.Run("spawned.get_script(ConsoleCounter) is None").output == "True\n", "A console-spawned behaviour started inside the UI call");
        fixture.Tick();
        Check(fixture.Run("spawned.get_script(ConsoleCounter).count").output == "21\n", "Spawned props did not start on the next script frame");
        fixture.Run("child = me.world.find('ConsoleChild'); spawned.destroy(); spawned.destroy()");
        Check(fixture.world.GetEntities().size() == spawned, "Console destroy was not deferred");
        fixture.Tick();
        Check(fixture.world.GetEntities().size() == 2 && fixture.Run("spawned.alive or child.alive").output == "False\n", "Deferred recursive destruction failed");
        const auto dead = fixture.system->ExecuteConsole("spawned.transform.position");
        Check(!dead.success && dead.error.find("EntityDeadError") != std::string::npos, "A stale console handle read deleted memory");

        fixture.Run("import console_behaviours, weakref; console_behaviours.saved = weakref.ref(behaviour)");
        fixture.Run("behaviour"); // the displayed object is retained only by the console's _
        const auto snapshot = myengine::scene::SerializeWorldToString(fixture.world);
        const auto generation = fixture.runtime.GetConsoleGeneration();
        Check(myengine::scene::LoadWorldFromString(fixture.world, snapshot, fixture.logger.get()), "Could not restore the Play scene");
        Check(fixture.runtime.GetConsoleGeneration() > generation, "Scene load did not reset the console namespace");
        fixture.Tick();
        Check(fixture.Run("import console_behaviours; console_behaviours.saved() is None").output == "True\n", "The console kept an old behaviour alive after scene load");
        Check(!fixture.system->ExecuteConsole("number").success, "A previous scene's console variables survived ResetInstances");
        fixture.Run("40 + 2");
    }

    void TestErrorsAndLimits(Fixture& fixture)
    {
        for (const auto* source : {"1 / 0", "if:", "raise SystemExit(2)", "raise KeyboardInterrupt()"})
        {
            const auto result = fixture.system->ExecuteConsole(source);
            Check(!result.success && !result.incomplete && result.file == "<console>" && result.line == 1, "A Python error escaped or lost its console location");
            Check(!result.error.empty(), "Console error has no traceback");
            Check(fixture.Run("6 * 7").output == "42\n", "Console did not recover after an exception");
        }
        fixture.Run("import console_behaviours");
        const auto moduleError = fixture.system->ExecuteConsole("console_behaviours.fail_from_module()");
        Check(!moduleError.success && moduleError.file == "console_behaviours.py" && moduleError.line > 0, "An imported script error lost its file:line");
        fixture.Run("print('x' * 100000)");
        const auto hugeOutput = fixture.Run("print('x' * 100000)");
        Check(hugeOutput.output.size() < 66000 && hugeOutput.output.find("truncated") != std::string::npos, "Console output is unbounded");
        fixture.Run("value = (");
        Check(!fixture.system->ExecuteConsole(std::string(17000, 'x')).success, "An oversized console line was executed");
        Check(fixture.Run("42").output == "42\n", "Oversized input did not cancel the pending block");
        Check(!fixture.system->ExecuteConsole(std::string("x\0y", 3)).success, "A NUL-containing console line was executed");

        fixture.system->ClearRecentErrors();
        const auto before = myengine::core::ServiceLocator::GetEditorRuntimeState().scriptStats.errors;
        for (int index = 0; index < 40; ++index)
        {
            Check(!fixture.system->ExecuteConsole("1 / 0").success, "A repeated console error was swallowed");
        }
        Check(fixture.system->GetRecentErrors().size() == 32, "The error list is not bounded to 32 entries");
        Check(myengine::core::ServiceLocator::GetEditorRuntimeState().scriptStats.errors == before + 40, "Console errors were not counted");
        fixture.Tick();
        Check(fixture.system->GetInstanceStatus(fixture.counter, 0) == "Active", "A console typo faulted a game behaviour");

        const auto faulty = fixture.world.CreateEntity();
        fixture.world.Emplace<components::ScriptComponent>(faulty).scripts.push_back({"console_behaviours", "ConsoleFault", json::object()});
        fixture.Tick();
        const auto& error = fixture.system->GetRecentErrors().back();
        Check(error.file == "console_behaviours.py" && error.line > 0 && error.message.find("OnUpdate") != std::string::npos,
            "A real game-script fault was absent from the console's error list");
        Check(fixture.system->GetInstanceStatus(faulty, 0) == "Faulted", "The faulty behaviour was not disabled");
        const auto total = myengine::core::ServiceLocator::GetEditorRuntimeState().scriptStats.errors;
        fixture.services.clearErrors();
        Check(fixture.services.errors().empty() && myengine::core::ServiceLocator::GetEditorRuntimeState().scriptStats.errors == total, "Clear errors reset the total error counter");
        Check(fixture.system->GetInstanceStatus(faulty, 0) == "Faulted", "Clear errors revived a faulty behaviour");
        fixture.world.DestroyEntity(faulty);
        fixture.Tick();
    }

    void TestPanel(Fixture& fixture)
    {
        ui::ScriptConsole console;
        Check(console.Submit(fixture.services, "for item in range(2):") && console.IsIncomplete(), "The panel did not show a continuation prompt");
        Check(console.Submit(fixture.services, "    print(item)"), "The panel did not submit an indented line");
        Check(console.Submit(fixture.services, "") && !console.IsIncomplete(), "The panel did not finish the block on a blank line");
        Check(console.PreviousCommand() == "    print(item)", "Up history did not recall the previous command");
        Check(console.PreviousCommand() == "for item in range(2):", "Up history did not recall an older command");
        Check(console.NextCommand() == "    print(item)" && console.NextCommand().empty(), "Down history did not return to empty input");
        for (int index = 0; index < 300; ++index)
        {
            Check(console.Submit(fixture.services, "value = " + std::to_string(index)), "The panel failed during a long command history");
        }
        Check(console.GetHistoryCount() == 64 && console.GetOutputCount() <= 256, "Panel history / transcript grew without a bound");
        fixture.Frame(console);
        console.ClearOutput();
        Check(console.GetOutputCount() == 0 && console.GetHistoryCount() == 64, "Clear output erased command history");
        Check(fixture.Run("value").output == "299\n", "Clear output reset Python variables");
        console.Reset(fixture.services);
        Check(!fixture.system->ExecuteConsole("value").success && !console.IsIncomplete(), "Reset Python kept the namespace or pending input");

        // InputText -> Enter -> the real system, not just a direct call to Submit.
        ui::ScriptConsole inputConsole;
        auto execute = fixture.services.execute;
        std::string received;
        fixture.services.execute = [&](const std::string& source) { received = source; return execute(source); };
        fixture.Frame(inputConsole);
        fixture.Frame(inputConsole);
        const auto* window = ImGui::FindWindowByName("D6 test");
        const auto position = ImVec2(window->WorkRect.Min.x + ImGui::CalcTextSize(">>>").x + ImGui::GetStyle().ItemSpacing.x + 30.0f,
            window->WorkRect.Max.y - ImGui::GetFrameHeight() + 6.0f);
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y);
        fixture.Frame(inputConsole);
        io.AddMouseButtonEvent(0, true);
        fixture.Frame(inputConsole);
        io.AddMouseButtonEvent(0, false);
        fixture.Frame(inputConsole);
        io.AddInputCharactersUTF8("21 * 2");
        fixture.Frame(inputConsole);
        io.AddKeyEvent(ImGuiKey_Enter, true);
        fixture.Frame(inputConsole);
        io.AddKeyEvent(ImGuiKey_Enter, false);
        fixture.Frame(inputConsole);
        Check(received == "21 * 2" && inputConsole.GetOutputCount() >= 2, "Enter in the console input did not execute Python");
        fixture.services.execute = execute;

        Check(inputConsole.Submit(fixture.services, "if True:") && inputConsole.IsIncomplete(), "Could not prepare the reset-generation test");
        fixture.system->ResetInstances();
        fixture.Tick();
        fixture.Frame(inputConsole);
        Check(!inputConsole.IsIncomplete(), "The UI kept a continuation prompt after scene reset");
        fixture.Frame(inputConsole, false); // error view still draws while commands are disabled
    }
}

int main()
{
    myengine::jobs::Initialize(2);
    int result = 1;
    try
    {
        {
            Fixture fixture;
            try
            {
                TestGatesAndRepl(fixture);
                TestWorldAndLifetime(fixture);
                TestErrorsAndLimits(fixture);
                TestPanel(fixture);
            }
            catch (...)
            {
                fixture.keepArtifacts = true;
                std::cerr << "Log: " << (fixture.directory / "test.log").u8string() << '\n';
                throw;
            }
        }
        {
            Fixture restarted; // no static Python objects may survive interpreter finalization
            Check(restarted.runtime.ExecuteConsole("'number' in globals()").output == "False\n", "A restarted interpreter retained console variables");
            restarted.system->Shutdown();
            restarted.runtime.Shutdown();
            Check(!restarted.runtime.ExecuteConsole("1 + 2").success && !restarted.system->ExecuteConsole("1 + 2").success,
                "Console ran after interpreter shutdown");
        }
        std::cout << "OK: D6 REPL, UTF-8 output, multiline, history, engine API, safe errors, bounds, scene reset and interpreter restart\n";
        result = 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    myengine::jobs::Shutdown();
    return result;
}
