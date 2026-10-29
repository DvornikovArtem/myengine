// The model of the Build window (docs/editor/build-panel.md), without a window and without a real build: the manifests of the games (and the readable errors of a
// broken one), the lines "##progress" / "##result" / "##zip" and the warnings and errors among the output of the build, the options and the command line of the
// script, the free place, the state of a run (Idle -> Running -> Done / Failed / Cancelled), and the runner on a short child process (output, exit code, Cancel
// that stops the whole tree).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <myengine/editor/BuildPanelModel.h>
#include <myengine/editor/BuildRunner.h>

namespace
{
    namespace fs = std::filesystem;
    using namespace myengine::editor;
    using namespace std::chrono_literals;

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool Has(const std::string& text, const std::string& part)
    {
        return text.find(part) != std::string::npos;
    }

    void WriteFile(const fs::path& path, const std::string& text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << text;
    }

    // a folder of the test that goes away at the end
    struct TempRepo
    {
        fs::path root;
        TempRepo()
        {
            root = fs::temp_directory_path() / ("build_panel_model_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::create_directories(root);
        }
        ~TempRepo()
        {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    };

    const char* kGoodManifest = R"({
        "id": "demo", "displayName": "Demo Game", "name": "DemoGame", "exe": "DemoGame.exe",
        "build": { "cmakeTarget": "demo_target", "exePath": "app/Release/DemoGame.exe", "userDataFolder": "DemoGame", "icon": "app/demo.ico", "checkScript": "tools/check_package.ps1" },
        "options": { "steam": "optional", "wwise": "no", "zip": false },
        "externalData": [ { "name": "Freedoom", "check": "external/freedoom/freedoom1.wad", "fetch": "tools/fetch_freedoom.ps1" } ]
    })";

    // ---- the manifests ----

    void TestManifestParsing()
    {
        GameManifest manifest;
        std::string error;
        Check(ParseGameManifest(kGoodManifest, "demo", manifest, error), "a good manifest is read: " + error);
        Check(manifest.id == "demo" && manifest.displayName == "Demo Game" && manifest.name == "DemoGame" && manifest.exe == "DemoGame.exe", "the names");
        Check(manifest.cmakeTarget == "demo_target" && manifest.exePath == "app/Release/DemoGame.exe" && manifest.userDataFolder == "DemoGame" && manifest.icon == "app/demo.ico",
              "the build part");
        Check(manifest.checkScript == "tools/check_package.ps1", "the check script");
        Check(manifest.steam == GameFeature::Optional && manifest.wwise == GameFeature::No && !manifest.zipByDefault, "the options");
        Check(manifest.externalData.size() == 1 && manifest.externalData[0].check == "external/freedoom/freedoom1.wad" && manifest.externalData[0].fetch == "tools/fetch_freedoom.ps1", "the external data");

        // what is left out gets its default
        GameManifest minimal;
        Check(ParseGameManifest(R"({"id": "m", "displayName": "M", "name": "M", "exe": "M.exe"})", "m", minimal, error), "a manifest of four lines: " + error);
        Check(minimal.exePath == "app/Release/M.exe" && minimal.userDataFolder == "M" && minimal.cmakeTarget.empty() && minimal.zipByDefault &&
                  minimal.steam == GameFeature::Optional && minimal.wwise == GameFeature::Optional && minimal.externalData.empty(),
              "the defaults of a minimal manifest");

        // a BOM and comments of the real ones do not matter: the keys "comment" are just keys
        GameManifest withComment;
        Check(ParseGameManifest(R"({"id": "c", "displayName": "C", "name": "C", "exe": "C.exe", "comment": "text", "assets": ["a"]})", "c", withComment, error), "unknown keys are allowed");
    }

    void TestManifestErrors()
    {
        struct Case
        {
            const char* text;
            const char* folder;
            const char* part;   // what the message must say
        };
        const Case cases[] = {
            {"{ not json", "x", "not valid JSON"},
            {"[1, 2]", "x", "must be a JSON object"},
            {R"({"displayName": "X", "name": "X", "exe": "X.exe"})", "x", "\"id\" is missing"},
            {R"({"id": "x", "displayName": "  ", "name": "X", "exe": "X.exe"})", "x", "\"displayName\" is empty"},
            {R"({"id": "x", "displayName": "X", "name": 5, "exe": "X.exe"})", "x", "\"name\" must be a string"},
            {R"({"id": "other", "displayName": "X", "name": "X", "exe": "X.exe"})", "x", "games/x"},
            {R"({"id": "Bad Id", "displayName": "X", "name": "X", "exe": "X.exe"})", "Bad Id", "only a-z"},
            {R"({"id": "x", "displayName": "X", "name": "X", "exe": "X.bin"})", "x", "ends with .exe"},
            {R"({"id": "x", "displayName": "X", "name": "X", "exe": "X.exe", "build": 3})", "x", "\"build\" must be an object"},
            {R"({"id": "x", "displayName": "X", "name": "X", "exe": "X.exe", "options": {"steam": "maybe"}})", "x", "\"steam\" is \"maybe\""},
            {R"({"id": "x", "displayName": "X", "name": "X", "exe": "X.exe", "options": {"zip": "yes"}})", "x", "\"zip\" must be true or false"},
            {R"({"id": "x", "displayName": "X", "name": "X", "exe": "X.exe", "externalData": {}})", "x", "\"externalData\" must be an array"},
            {R"({"id": "x", "displayName": "X", "name": "X", "exe": "X.exe", "externalData": [{"name": "D"}]})", "x", "externalData[0]: \"check\" is missing"},
        };
        for (const Case& test : cases)
        {
            GameManifest manifest;
            std::string error;
            Check(!ParseGameManifest(test.text, test.folder, manifest, error), std::string("a broken manifest is refused: ") + test.text);
            Check(Has(error, test.part), std::string("the message \"") + error + "\" must say \"" + test.part + "\"");
        }
    }

    void TestLoadGames()
    {
        TempRepo repo;
        WriteFile(repo.root / "games" / "demo" / "game.json", kGoodManifest);
        WriteFile(repo.root / "games" / "broken" / "game.json", "{ oops");
        WriteFile(repo.root / "games" / "empty" / "readme.txt", "no manifest here");
        WriteFile(repo.root / "games" / "alpha" / "game.json", R"({"id": "alpha", "displayName": "Alpha", "name": "Alpha", "exe": "Alpha.exe"})");

        const std::vector<GameEntry> games = LoadGameManifests(repo.root);
        Check(games.size() == 3, "three folders with game.json: " + std::to_string(games.size()));
        Check(games[0].folder == "alpha" && games[1].folder == "broken" && games[2].folder == "demo", "sorted by the folder");
        Check(games[0].manifest.has_value() && games[0].error.empty(), "alpha is fine");
        Check(!games[1].manifest.has_value() && Has(games[1].error, "not valid JSON"), "a broken manifest is listed with its reason");
        Check(games[2].manifest.has_value() && games[2].manifest->displayName == "Demo Game", "demo is fine");
        Check(LoadGameManifests(repo.root / "nothing").empty(), "no games folder: no games");
    }

    // the repository above the exe of the test (build/tests/<config>/...): the folder that has games and tools; empty when the build folder is somewhere else
    fs::path FindRepositoryRoot()
    {
        wchar_t buffer[MAX_PATH * 4] = {};
        GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
        std::error_code ec;
        for (fs::path folder = fs::path(buffer).parent_path(); !folder.empty() && folder != folder.parent_path(); folder = folder.parent_path())
        {
            if (fs::is_directory(folder / "games", ec) && fs::is_directory(folder / "tools", ec) && fs::exists(folder / "CMakeLists.txt", ec))
            {
                return folder;
            }
        }
        return {};
    }

    void TestTheGamesOfTheRepository()
    {
        const fs::path root = FindRepositoryRoot();
        if (root.empty())
        {
            std::cout << "  (the repository is not above the build folder: this case is skipped)" << std::endl;
            return;
        }
        const std::vector<GameEntry> games = LoadGameManifests(root);
        bool rocket = false;
        bool doom = false;
        for (const GameEntry& game : games)
        {
            Check(game.error.empty() && game.manifest.has_value(), "games/" + game.folder + "/game.json: " + game.error);
            rocket = rocket || game.folder == "rocketarena";
            doom = doom || game.folder == "flydoom";
        }
        Check(rocket && doom, "games/rocketarena and games/flydoom are in the repository");
        for (const GameEntry& game : games)
        {
            if (game.folder == "rocketarena")
            {
                const GameManifest& manifest = *game.manifest;
                Check(manifest.name == "RocketArena" && manifest.exe == "RocketArena.exe" && manifest.cmakeTarget == "myengine" && manifest.exePath == "app/Release/RocketArena.exe",
                      "Rocket Arena is built as before");
                Check(CmakeTargetExists(root, manifest.cmakeTarget), "the target of Rocket Arena is in the CMake files");
                Check(fs::exists(root / "tools" / "package_game.ps1") && !manifest.checkScript.empty() && fs::exists(root / manifest.checkScript), "the scripts are there");
            }
        }
    }

    void TestCmakeTargets()
    {
        TempRepo repo;
        WriteFile(repo.root / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(x)\nadd_subdirectory(app)\n");
        WriteFile(repo.root / "app" / "CMakeLists.txt", "ADD_EXECUTABLE ( myengine\n  src/main.cpp)\nadd_library(game_lib STATIC a.cpp)\n");
        WriteFile(repo.root / "external" / "dep" / "CMakeLists.txt", "add_executable(hidden main.cpp)\n");
        WriteFile(repo.root / "build" / "CMakeLists.txt", "add_executable(from_build main.cpp)\n");
        WriteFile(repo.root / "game" / "doom" / "CMakeLists.txt", "# add_executable(commented_out x.cpp)\nadd_executable(flydoom\n    x.cpp)\n");

        Check(CmakeTargetExists(repo.root, "myengine"), "a target of add_executable, in any case and with the name on the next line");
        Check(CmakeTargetExists(repo.root, "game_lib"), "a target of add_library");
        Check(CmakeTargetExists(repo.root, "flydoom"), "a target in a deeper folder");
        Check(!CmakeTargetExists(repo.root, "hidden"), "the folder external is not read");
        Check(!CmakeTargetExists(repo.root, "from_build"), "a build folder is not read");
        Check(!CmakeTargetExists(repo.root, "missing_target") && !CmakeTargetExists(repo.root, ""), "a target that is not there");
        Check(!CmakeTargetExists(repo.root, "myeng"), "a part of a name is not the target");

        ExternalDataItem item;
        item.name = "Data";
        item.check = "external/freedoom/freedoom1.wad";
        Check(!ExternalDataPresent(repo.root, item), "the data is not there");
        WriteFile(repo.root / "external" / "freedoom" / "freedoom1.wad", "x");
        Check(ExternalDataPresent(repo.root, item), "the data is there");
    }

    // ---- the lines ----

    void TestLines()
    {
        BuildLine line = ParseBuildLine("##progress 42 build");
        Check(line.kind == BuildLineKind::Progress && line.percent == 42 && line.stage == "build", "a progress line");
        line = ParseBuildLine("##progress 150 over\r");
        Check(line.kind == BuildLineKind::Progress && line.percent == 100 && line.stage == "over", "a number over 100 is 100, the line break is cut");
        line = ParseBuildLine("##progress -5 under");
        Check(line.kind == BuildLineKind::Progress && line.percent == 0, "a number under 0 is 0");
        line = ParseBuildLine("##progress 7");
        Check(line.kind == BuildLineKind::Progress && line.percent == 7 && line.stage.empty(), "a progress line without a stage");
        Check(ParseBuildLine("##progress many words").kind == BuildLineKind::Plain, "a progress line without a number is only text");

        line = ParseBuildLine("##result ok C:\\My Games\\dist\\RocketArena");
        Check(line.kind == BuildLineKind::Result && line.ok && line.text == "C:\\My Games\\dist\\RocketArena", "a result with a path that has a space");
        line = ParseBuildLine("##result fail Unknown game 'nope'");
        Check(line.kind == BuildLineKind::Result && !line.ok && line.text == "Unknown game 'nope'", "a failed result");
        Check(ParseBuildLine("##result maybe").kind == BuildLineKind::Plain, "an unknown result is only text");
        line = ParseBuildLine("##zip C:\\dist\\RocketArena-1.0.0-win64.zip");
        Check(line.kind == BuildLineKind::Zip && line.text == "C:\\dist\\RocketArena-1.0.0-win64.zip", "the zip line");

        const char* errors[] = {
            "C:\\src\\a.cpp(12,5): error C2065: 'x': undeclared identifier [C:\\b\\x.vcxproj]",
            "LINK : fatal error LNK1104: cannot open file 'x.lib'",
            "CMake Error at CMakeLists.txt:3 (project):",
            "myengine.vcxproj : error MSB8020: the build tools cannot be found",
            "[PACKAGE] cmake failed with exit code 1",
            "ERROR: something",
            "C:\\src\\a.cpp(3): \xd0\xbe\xd1\x88\xd0\xb8\xd0\xb1\xd0\xba\xd0\xb0 C2065: \"x\"",   // "ошибка C2065"
        };
        for (const char* text : errors)
        {
            Check(ParseBuildLine(text).kind == BuildLineKind::Error, std::string("an error line: ") + text);
        }
        const char* warnings[] = {
            "WARNING: The Steamworks SDK was not found",
            "C:\\src\\a.cpp(3): warning C4100: 't': unreferenced parameter",
            "CMake Warning at CMakeLists.txt:5:",
            "C:\\src\\a.cpp(3): \xd0\xbf\xd1\x80\xd0\xb5\xd0\xb4\xd1\x83\xd0\xbf\xd1\x80\xd0\xb5\xd0\xb6\xd0\xb4\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5 C4100: x",   // "предупреждение C4100"
        };
        for (const char* text : warnings)
        {
            Check(ParseBuildLine(text).kind == BuildLineKind::Warning, std::string("a warning line: ") + text);
        }
        const char* plain[] = {
            "    0 Warning(s)",
            "    0 Error(s)",
            "    \xd0\x9f\xd1\x80\xd0\xb5\xd0\xb4\xd1\x83\xd0\xbf\xd1\x80\xd0\xb5\xd0\xb6\xd0\xb4\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb9: 0",   // "Предупреждений: 0"
            "    \xd0\x9e\xd1\x88\xd0\xb8\xd0\xb1\xd0\xbe\xd0\xba: 0",                                                                             // "Ошибок: 0"
            "[PACKAGE] Building the myengine target (Release)",
            "  myengine.vcxproj -> C:\\b\\app\\Release\\RocketArena.exe",
            "",
        };
        for (const char* text : plain)
        {
            Check(ParseBuildLine(text).kind == BuildLineKind::Plain, std::string("a plain line: ") + text);
        }
    }

    // ---- the options ----

    void TestOptionsAndCommandLine()
    {
        GameManifest manifest;
        std::string error;
        Check(ParseGameManifest(kGoodManifest, "demo", manifest, error), error);
        manifest.wwise = GameFeature::Optional;
        manifest.zipByDefault = true;

        BuildOptions options;
        options.gameId = "demo";
        std::vector<std::string> arguments = MakeScriptArguments(options, manifest);
        const std::vector<std::string> expected = {"-Game", "demo", "-Config", "Shipping", "-Out", "dist"};
        Check(arguments == expected, "the default arguments");

        options.config = BuildConfig::RelWithDebInfo;
        options.steam = SdkChoice::On;
        options.wwise = SdkChoice::Off;
        options.zip = false;
        options.outputFolder = "C:\\My Builds";
        options.buildFolder = "D:\\flydoom-build\\b0\\build-shipping";
        arguments = MakeScriptArguments(options, manifest);
        const std::vector<std::string> expectedAll = {"-Game", "demo", "-Config", "RelWithDebInfo", "-WithSteam", "-NoWwise", "-NoZip", "-Out", "C:\\My Builds", "-BuildDir", "D:\\flydoom-build\\b0\\build-shipping"};
        Check(arguments == expectedAll, "every switch");
        options.steam = SdkChoice::Off;
        arguments = MakeScriptArguments(options, manifest);
        Check(std::find(arguments.begin(), arguments.end(), "-NoSteam") != arguments.end() && std::find(arguments.begin(), arguments.end(), "-WithSteam") == arguments.end(), "Steam off");

        manifest.steam = GameFeature::No;
        manifest.wwise = GameFeature::No;
        arguments = MakeScriptArguments(options, manifest);
        Check(std::find(arguments.begin(), arguments.end(), "-NoSteam") == arguments.end() && std::find(arguments.begin(), arguments.end(), "-NoWwise") == arguments.end(),
              "a switch that the game does not have is left out");

        manifest.steam = GameFeature::Optional;
        const std::string line = MakeBuildCommandLine("C:\\Repo Dir", options, manifest);
        Check(line.rfind("powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"C:\\Repo Dir\\tools\\package_game.ps1\" -Game demo", 0) == 0, "the start of the command line: " + line);
        Check(Has(line, "-Out \"C:\\My Builds\"") && Has(line, "-BuildDir D:\\flydoom-build\\b0\\build-shipping"), "a value with a space is quoted, the build folder on another drive is passed: " + line);

        const std::string check = MakeCheckCommandLine("C:\\Repo", manifest, "C:\\Repo\\dist\\DemoGame");
        Check(check == "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\\Repo\\tools\\check_package.ps1 -Dist C:\\Repo\\dist\\DemoGame", "the check: " + check);

        BuildOptions relative;
        relative.gameId = "demo";
        relative.outputFolder = "dist";
        const fs::path folder = PackageFolder("C:\\Repo", relative, manifest);
        Check(folder == fs::path("C:\\Repo\\dist\\DemoGame"), "the package folder, relative: " + folder.string());
        relative.outputFolder = "D:\\out";
        Check(PackageFolder("C:\\Repo", relative, manifest) == fs::path("D:\\out\\DemoGame"), "the package folder, full path");
        relative.outputFolder = "  ";
        Check(PackageFolder("C:\\Repo", relative, manifest) == fs::path("C:\\Repo\\dist\\DemoGame"), "an empty output folder is dist");
    }

    void TestDiskSpace()
    {
        Check(IsDiskSpaceLow(0) && IsDiskSpaceLow(kLowDiskBytes - 1), "below 3 GB is low");
        Check(!IsDiskSpaceLow(kLowDiskBytes) && !IsDiskSpaceLow(50ull * 1024 * 1024 * 1024), "3 GB and more is not");
        Check(FormatSize(2ull * 1024 * 1024 * 1024 + 512ull * 1024 * 1024) == "2.5 GB", "the size in GB: " + FormatSize(2ull * 1024 * 1024 * 1024 + 512ull * 1024 * 1024));
        Check(FormatSize(300ull * 1024 * 1024) == "300 MB", "the size in MB");
    }

    // ---- the state ----

    void TestSession()
    {
        BuildSession session;
        Check(session.State() == BuildState::Idle && !session.IsRunning(), "Idle at the start");
        session.AddLine("nobody listens");
        Check(session.Log().empty(), "a line in Idle is dropped");

        Check(session.Begin(BuildJob::Package, "Build"), "Idle -> Running");
        Check(session.State() == BuildState::Running && session.IsRunning(), "Running");
        Check(!session.Begin(BuildJob::Package, "second"), "a second run while one goes is refused");
        Check(session.Title() == "Build", "the first run keeps its title");

        session.AddLine("##progress 10 configure");
        session.AddLine("C:\\a.cpp(1): warning C4100: x");
        session.AddLine("C:\\a.cpp(2): error C2065: y");
        session.AddLine("##progress 70 built");
        Check(session.Percent() == 70 && session.Stage() == "built", "the bar follows the progress lines");
        Check(session.WarningCount() == 1 && session.ErrorCount() == 1 && session.Log().size() == 4, "the counts and the log");
        session.AddLine("##zip C:\\dist\\x.zip");
        session.AddLine("##result ok C:\\dist\\RocketArena");
        session.Finish(0);
        Check(session.State() == BuildState::Done && session.Percent() == 100, "Done: the result and the code 0");
        Check(session.ResultPath() == "C:\\dist\\RocketArena" && session.ZipPath() == "C:\\dist\\x.zip", "the result and the zip");
        session.AddLine("after the end");
        Check(session.Log().size() == 6, "nothing is added after the end");

        // the next run starts clean
        Check(session.Begin(BuildJob::Check, "Check"), "Done -> Running again");
        Check(session.Log().empty() && session.Percent() == 0 && session.ResultPath().empty() && session.ErrorCount() == 0 && session.Job() == BuildJob::Check, "a new run is clean");
        session.AddLine("##result fail the check failed");
        session.Finish(1);
        Check(session.State() == BuildState::Failed && session.FailureText() == "the check failed" && session.ErrorCount() == 1, "Failed by the result line");

        Check(session.Begin(BuildJob::Package, "Build"), "Failed -> Running");
        session.AddLine("##progress 30 build");
        session.Finish(2);
        Check(session.State() == BuildState::Failed && Has(session.FailureText(), "code 2") && session.Percent() == 30, "Failed by the exit code without a result line");

        Check(session.Begin(BuildJob::Package, "Build"), "Failed -> Running");
        session.AddLine("##result ok C:\\x");
        session.Finish(1);
        Check(session.State() == BuildState::Failed, "a result ok with an exit code 1 is a failure");

        Check(session.Begin(BuildJob::Package, "Build"), "Failed -> Running");
        session.Finish(0);
        Check(session.State() == BuildState::Done && session.ResultPath().empty(), "an exit code 0 without a result line is Done");

        Check(session.Begin(BuildJob::Package, "Build"), "Done -> Running");
        session.AddLine("##progress 20 build");
        session.RequestCancel();
        Check(session.IsRunning() && session.CancelRequested(), "Cancel does not end the run: the process is still being stopped");
        session.AddLine("the last words of the process");
        session.Finish(1);
        Check(session.State() == BuildState::Cancelled, "Cancelled: the run ends by the code, but the person asked for it");

        Check(session.Begin(BuildJob::Package, "Build"), "Cancelled -> Running");
        session.FailToStart("Cannot start the process (Windows error 2)");
        Check(session.State() == BuildState::Failed && Has(session.FailureText(), "Windows error 2") && session.ErrorCount() == 1, "the process did not start");

        session.Clear();
        Check(session.State() == BuildState::Idle && session.Log().empty(), "Clear");
        Check(session.Begin(BuildJob::Package, "Build"), "Idle -> Running");
        session.Clear();
        Check(session.IsRunning(), "Clear does nothing while a run goes");
        session.Finish(0);

        // the log is bounded
        Check(session.Begin(BuildJob::Package, "Build"), "Done -> Running");
        const std::uint64_t before = session.LogVersion();
        for (std::size_t i = 0; i < BuildSession::kMaxLines + 500; ++i)
        {
            session.AddLine("line " + std::to_string(i));
        }
        Check(session.Log().size() <= BuildSession::kMaxLines && session.Log().size() > BuildSession::kMaxLines / 2, "the oldest lines go when the log is full");
        Check(session.Log().back().text == "line " + std::to_string(BuildSession::kMaxLines + 499), "the newest line stays");
        Check(session.LogVersion() > before, "the version of the log grows");
    }

    // ---- the runner ----

    std::vector<std::string> RunToEnd(BuildRunner& runner, int& exitCode, std::chrono::milliseconds limit)
    {
        std::vector<std::string> lines;
        const auto deadline = std::chrono::steady_clock::now() + limit;
        bool finished = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            for (std::string& line : runner.TakeLines())
            {
                lines.push_back(std::move(line));
            }
            if (runner.TakeFinished(exitCode))
            {
                finished = true;
                break;
            }
            std::this_thread::sleep_for(10ms);
        }
        Check(finished, "the process ended in time");
        return lines;
    }

    void TestRunner()
    {
        {
            BuildRunner runner;
            std::string error;
            Check(runner.Start("cmd.exe /c \"echo ##progress 50 half& echo second line& echo ##result ok C:\\x& exit 3\"", fs::temp_directory_path(), error), "the process starts: " + error);
            int code = -1;
            const std::vector<std::string> lines = RunToEnd(runner, code, 20s);
            Check(lines.size() == 3 && lines[0] == "##progress 50 half" && lines[1] == "second line" && lines[2] == "##result ok C:\\x", "the lines of the output, one by one: " + std::to_string(lines.size()));
            Check(code == 3, "the exit code of the process: " + std::to_string(code));
            Check(!runner.IsRunning(), "not running after the end");
            int again = 0;
            Check(!runner.TakeFinished(again), "the end is given out once");

            // the runner runs again
            Check(runner.Start("cmd.exe /c \"echo again\"", fs::temp_directory_path(), error), "a second run on the same runner: " + error);
            const std::vector<std::string> second = RunToEnd(runner, code, 20s);
            Check(second.size() == 1 && second[0] == "again" && code == 0, "the second run is clean");
        }
        {
            BuildRunner runner;
            std::string error;
            Check(!runner.Start("C:\\no\\such\\program.exe --x", fs::temp_directory_path(), error) && !error.empty(), "a program that is not there is an error, not a hang");
        }
        {
            // Cancel stops the process and the one it started (cmd starts ping): the run ends at once, not in half a minute
            BuildRunner runner;
            std::string error;
            Check(runner.Start("cmd.exe /c \"echo started& ping -n 40 127.0.0.1 > nul& echo never\"", fs::temp_directory_path(), error), "a long process starts: " + error);
            std::vector<std::string> lines;
            const auto wait = std::chrono::steady_clock::now() + 10s;
            while (lines.empty() && std::chrono::steady_clock::now() < wait)
            {
                for (std::string& line : runner.TakeLines())
                {
                    lines.push_back(std::move(line));
                }
                std::this_thread::sleep_for(10ms);
            }
            Check(!lines.empty() && lines[0] == "started" && runner.IsRunning(), "the long process runs and has said something");
            const auto cancelled = std::chrono::steady_clock::now();
            runner.Cancel();
            int code = 0;
            const std::vector<std::string> rest = RunToEnd(runner, code, 10s);
            const auto took = std::chrono::steady_clock::now() - cancelled;
            Check(took < 8s, "Cancel stops the whole tree quickly");
            Check(code != 0, "a killed process has no code 0");
            for (const std::string& line : rest)
            {
                Check(line != "never", "what comes after the cancel does not run");
            }
        }
    }
}

int main()
{
    struct Case
    {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"manifest parsing", TestManifestParsing},
        {"manifest errors are readable", TestManifestErrors},
        {"games of a folder", TestLoadGames},
        {"the games of the repository", TestTheGamesOfTheRepository},
        {"cmake targets and external data", TestCmakeTargets},
        {"the lines of the script", TestLines},
        {"options and the command line", TestOptionsAndCommandLine},
        {"free place", TestDiskSpace},
        {"the state of a run", TestSession},
        {"the runner on a child process", TestRunner},
    };
    int failed = 0;
    for (const Case& test : cases)
    {
        try
        {
            test.run();
            std::cout << "[ OK ] " << test.name << "\n";
        }
        catch (const std::exception& ex)
        {
            ++failed;
            std::cout << "[FAIL] " << test.name << ": " << ex.what() << "\n";
        }
    }
    std::cout << (failed == 0 ? "build_panel_model: all passed\n" : "build_panel_model: FAILED\n");
    return failed == 0 ? 0 : 1;
}
