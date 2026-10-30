// main.cpp

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <states/LoadingState.h>

#include <brainview/BrainViewWindow.h>
#include <brainview/DoomMenuBackdrop.h>

#include <myengine/config/AppConfig.h>
#include <myengine/core/AppPaths.h>
#include <myengine/core/Application.h>
#include <myengine/core/CrashHandler.h>
#include <myengine/core/ProjectContext.h>
#include <doomfly/FlyModes.h>
#include <myengine/render/dx12/ShaderLayoutDump.h>

// If the WIN32_LEAN_AND_MEAN macro is defined before including windows.h, rarely used parts are excluded from the header to speed up compilation
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

namespace
{
    struct LaunchOptions
    {
        std::filesystem::path scenePath;
        std::filesystem::path projectFile;
        bool startInPlay = false;
        bool gameMode = false;                      // --game: the Rocket League game without the editor
        bool doomMode = false;                      // --game doom (and FlyDoom.exe): the classic DOOM (docs/game/doom.md)
        myengine::core::Application::DoomOptions doom;
        bool fly = false;                           // --fly [doomfly|bio]: FlyDoom starts with the fly as the player (docs/game/flydoom-fly.md)
        bool flyBio = false;
        std::string flyMode;                        // --fly-mode human|fly|human-vs-fly|fly-vs-fly: starts this mode without the menu
        bool flyReward = false;                     // --fly-reward
        bool flyForget = false;                     // --fly-forget: "Fly learns" starts from the weights of the connectome
        std::uint64_t flySeed = 1;                  // --fly-seed N
        std::filesystem::path fbrain;               // --fbrain <file>: the connectome
        std::string flybrain = "auto";              // --flybrain auto|cpu|gpu: where the brain of the fly runs
        bool showProjectBrowser = false;
        bool openFlyBrain = false;                  // --fly-brain: Window > Fly Brain is open at the start
        DWORD waitForPid = 0;
        myengine::core::DiagnosticsOptions diagnostics;
        std::uint32_t exitAfterFrames = 0; // 0: run until the window is closed
        std::filesystem::path dumpShaderLayouts; // not empty: write the shader layouts to this file and exit
        myengine::ecs::systems::RlOnlineOptions rlOnline; // --rl-host / --rl-join
    };

    bool ParseCount(const std::wstring_view text, std::uint64_t& value)
    {
        if (text.empty() || text.find_first_not_of(L"0123456789") != std::wstring_view::npos)
        {
            return false;
        }
        value = std::wcstoull(std::wstring(text).c_str(), nullptr, 10);
        return value > 0;
    }

    // addresses and net-sim texts are ASCII
    std::string Narrow(const std::wstring_view text)
    {
        std::string result;
        result.reserve(text.size());
        for (const wchar_t c : text)
        {
            result.push_back(static_cast<char>(c));
        }
        return result;
    }

    bool IsDigits(const std::wstring_view text) { return !text.empty() && text.find_first_not_of(L"0123456789") == std::wstring_view::npos; }

    bool ParseArguments(LaunchOptions& options)
    {
        int argumentCount = 0;
        wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        if (arguments == nullptr)
        {
            return false;
        }

        bool valid = true;
        for (int index = 1; index < argumentCount && valid; ++index)
        {
            const std::wstring_view argument = arguments[index];
            if (argument == L"--play" && !options.startInPlay)
            {
                options.startInPlay = true;
                continue;
            }
            if (argument == L"--game" && !options.gameMode)
            {
                options.gameMode = true;
                // --game doom: FlyDoom instead of Rocket Arena
                if (index + 1 < argumentCount && std::wstring_view(arguments[index + 1]) == L"doom")
                {
                    options.doomMode = true;
                    ++index;
                }
                continue;
            }
            if (argument == L"--fly")
            {
                options.fly = true;
                if (index + 1 < argumentCount && (std::wstring_view(arguments[index + 1]) == L"bio" || std::wstring_view(arguments[index + 1]) == L"doomfly"))
                {
                    options.flyBio = std::wstring_view(arguments[index + 1]) == L"bio";
                    ++index;
                }
                continue;
            }
            if (argument == L"--fly-reward")
            {
                options.flyReward = true;
                continue;
            }
            if (argument == L"--fly-forget")
            {
                options.flyForget = true;
                continue;
            }
            if (argument == L"--doom-square-pixels")
            {
                options.doom.squarePixels = true;
                continue;
            }
            if (argument == L"--doom-no-music")
            {
                options.doom.music = false;
                continue;
            }
            if (argument == L"--faceid-db" && index + 1 < argumentCount)   // the shared face database to use (docs/editor/faceid.md)
            {
                ++index;
                _wputenv_s(L"MYENGINE_FACEID_DB", std::wstring(arguments[index]).c_str());
                continue;
            }
            if (argument == L"--no-faceid")   // the editor starts without the Face ID login (same as MYENGINE_NO_FACEID=1)
            {
                _putenv_s("MYENGINE_NO_FACEID", "1");
                continue;
            }
            if (argument == L"--fly-brain")   // the 3D fly brain window open at the start (docs/flybrain/brain-view.md)
            {
                options.openFlyBrain = true;
                continue;
            }
            // Steam starts the game of a friend who accepts an invitation with "+connect_lobby <lobby id>": the lobby screens read it from the command line
            if (argument == L"+connect_lobby")
            {
                if (index + 1 < argumentCount && IsDigits(arguments[index + 1]))
                {
                    ++index;
                }
                continue;
            }
            if (argument == L"--project-browser" && !options.showProjectBrowser)
            {
                options.showProjectBrowser = true;
                continue;
            }

            // GPU diagnostics (docs/dev/gpu-debugging.md). The DX12 switches reach the adapter as environment variables.
            if (argument == L"--exit-after-capture")
            {
                options.diagnostics.exitAfterCapture = true;
                continue;
            }
            if (argument == L"--renderdoc")
            {
                options.diagnostics.renderDoc = true;
                continue;
            }
            if (argument == L"--pix")
            {
                options.diagnostics.pix = true;
                continue;
            }
            if (argument == L"--dx12-debug")
            {
                _wputenv_s(L"MYENGINE_DX12_DEBUG", L"1"); // the CRT copy of the environment, which the adapter reads
                continue;
            }
            if (argument == L"--dx12-gbv")
            {
                _wputenv_s(L"MYENGINE_DX12_GBV", L"1");
                continue;
            }
            if (argument == L"--dx12-dred")
            {
                _wputenv_s(L"MYENGINE_DX12_DRED", L"1");
                continue;
            }

            // Rocket League online (docs/game/rl-online.md, 7): --rl-host [port] | --rl-join ip[:port], with --rl-clients N, --rl-mode N, --net-sim ...
            if (argument == L"--rl-host")
            {
                if (options.rlOnline.role != myengine::ecs::systems::RlOnlineOptions::Role::None)
                {
                    valid = false;
                    break;
                }
                options.rlOnline.role = myengine::ecs::systems::RlOnlineOptions::Role::Host;
                if (index + 1 < argumentCount && IsDigits(arguments[index + 1]))
                {
                    const unsigned long port = std::wcstoul(arguments[++index], nullptr, 10);
                    valid = port > 0 && port <= 65535;
                    options.rlOnline.port = static_cast<std::uint16_t>(port);
                }
                continue;
            }

            const bool isScene = argument == L"--scene";
            const bool isProject = argument == L"--project";
            const bool isWait = argument == L"--wait-for-pid";
            const bool isCaptureFrame = argument == L"--gpu-capture-frame";
            const bool isFrameStats = argument == L"--frame-stats";
            const bool isFrameStatsOut = argument == L"--frame-stats-out";
            const bool isFrames = argument == L"--exit-after-frames";
            const bool isDumpLayouts = argument == L"--dump-shader-layouts";
            const bool isRlJoin = argument == L"--rl-join";
            const bool isRlClients = argument == L"--rl-clients";
            const bool isRlMode = argument == L"--rl-mode";
            const bool isNetSim = argument == L"--net-sim";
            // FlyDoom: --iwad <file.wad> names the game data, --doom-args "<the command line of DOOM>" is given to DOOM as it is (-warp 1 1 -skill 3)
            const bool isIwad = argument == L"--iwad";
            const bool isFbrain = argument == L"--fbrain";
            const bool isFlySeed = argument == L"--fly-seed";
            const bool isFlybrain = argument == L"--flybrain";
            const bool isFlyMode = argument == L"--fly-mode";
            const bool isDoomArgs = argument == L"--doom-args";
            // The scene passes (docs/render/scene-passes.md): --render-settings shadows=2,msaa=4,bloom=1,instancing=1; --screenshot <file.tga> [--screenshot-frame N]
            const bool isRenderSettings = argument == L"--render-settings";
            const bool isScreenshot = argument == L"--screenshot";
            const bool isScreenshotFrame = argument == L"--screenshot-frame";
            if (!isScene && !isProject && !isWait && !isCaptureFrame && !isFrameStats && !isFrameStatsOut && !isFrames && !isDumpLayouts && !isRlJoin &&
                !isRlClients && !isRlMode && !isNetSim && !isRenderSettings && !isScreenshot && !isScreenshotFrame && !isIwad && !isDoomArgs && !isFbrain && !isFlySeed && !isFlybrain && !isFlyMode)
            {
                valid = false;
                break;
            }

            if (index + 1 >= argumentCount)
            {
                valid = false;
                break;
            }

            const std::wstring_view value = arguments[++index];
            if (value.empty() || value.rfind(L"--", 0) == 0)
            {
                valid = false;
                break;
            }

            if (isIwad && options.doom.iwad.empty())
            {
                options.doom.iwad = std::wstring(value);
            }
            else if (isFbrain && options.fbrain.empty())
            {
                options.fbrain = std::wstring(value);
            }
            else if (isFlybrain)
            {
                options.flybrain = Narrow(value);
            }
            else if (isFlyMode)
            {
                options.flyMode = Narrow(value);
            }
            else if (isFlySeed)
            {
                options.flySeed = std::wcstoull(std::wstring(value).c_str(), nullptr, 10);
            }
            else if (isDoomArgs && options.doom.args.empty())
            {
                // split at the spaces; a part in "quotes" keeps its spaces
                std::string current;
                bool quoted = false;
                for (const wchar_t c : value)
                {
                    if (c == L'"')
                    {
                        quoted = !quoted;
                    }
                    else if (c == L' ' && !quoted)
                    {
                        if (!current.empty())
                        {
                            options.doom.args.push_back(current);
                            current.clear();
                        }
                    }
                    else
                    {
                        current.push_back(static_cast<char>(c));
                    }
                }
                if (!current.empty())
                {
                    options.doom.args.push_back(current);
                }
            }
            else if (isScene && options.scenePath.empty())
            {
                options.scenePath = std::wstring(value);
            }
            else if (isProject && options.projectFile.empty())
            {
                options.projectFile = std::wstring(value);
            }
            else if (isWait && options.waitForPid == 0)
            {
                options.waitForPid = static_cast<DWORD>(std::wcstoul(std::wstring(value).c_str(), nullptr, 10));
            }
            else if (isCaptureFrame && options.diagnostics.captureFrame == 0)
            {
                valid = ParseCount(value, options.diagnostics.captureFrame);
            }
            else if (isFrameStats && options.diagnostics.frameStatsFrame == 0)
            {
                valid = ParseCount(value, options.diagnostics.frameStatsFrame);
            }
            else if (isFrameStatsOut && options.diagnostics.frameStatsOut.empty())
            {
                options.diagnostics.frameStatsOut = std::wstring(value);
            }
            else if (isFrames && options.exitAfterFrames == 0)
            {
                std::uint64_t frames = 0;
                valid = ParseCount(value, frames);
                options.exitAfterFrames = static_cast<std::uint32_t>(frames);
            }
            else if (isDumpLayouts && options.dumpShaderLayouts.empty())
            {
                options.dumpShaderLayouts = std::wstring(value);
            }
            else if (isRlJoin && options.rlOnline.role == myengine::ecs::systems::RlOnlineOptions::Role::None)
            {
                options.rlOnline.role = myengine::ecs::systems::RlOnlineOptions::Role::Client;
                options.rlOnline.address = Narrow(value);
            }
            else if (isRlClients || isRlMode)
            {
                const unsigned long number = IsDigits(value) ? std::wcstoul(std::wstring(value).c_str(), nullptr, 10) : 0;
                valid = number >= 1 && number <= (isRlMode ? 4ul : 7ul);
                (isRlMode ? options.rlOnline.playersPerTeam : options.rlOnline.expectedClients) = static_cast<int>(number);
            }
            else if (isNetSim && options.rlOnline.netSim.empty())
            {
                options.rlOnline.netSim = Narrow(value);
            }
            else if (isRenderSettings || isScreenshot || isScreenshotFrame)
            {
                // They reach the adapter as environment variables, like the DX12 switches
                const wchar_t* variable = isRenderSettings ? L"MYENGINE_RENDER_SETTINGS" : (isScreenshot ? L"MYENGINE_SCREENSHOT" : L"MYENGINE_SCREENSHOT_FRAME");
                _wputenv_s(variable, std::wstring(value).c_str());
            }
            else
            {
                valid = false;
            }
        }

        LocalFree(arguments);
        return valid;
    }

    // The process that restarted the editor for another project is still shutting down: let it finish
    void WaitForProcess(const DWORD processId)
    {
        if (processId == 0)
        {
            return;
        }

        if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId); process != nullptr)
        {
            WaitForSingleObject(process, 10000);
            CloseHandle(process);
        }
    }

    // The folder with the shaders: next to the working directory, next to the executable or in the sources
    std::filesystem::path ResolveShadersDirectory()
    {
        std::vector<std::filesystem::path> candidates;
        candidates.emplace_back(std::filesystem::current_path() / "assets/shaders");

        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        candidates.emplace_back(std::filesystem::path(modulePath).parent_path() / "assets/shaders");

    #ifdef MYENGINE_SOURCE_DIR
        candidates.emplace_back(std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "assets/shaders");
    #endif

        for (const auto& candidate : candidates)
        {
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }
        }
        return candidates.front();
    }

    // --dump-shader-layouts <file>: the layouts of all the shader programs as text (no window, no GPU)
    int DumpShaderLayoutsToFile(const std::filesystem::path& outputFile)
    {
        std::string text;
        const int failures = myengine::render::dx12::DumpShaderLayouts(ResolveShadersDirectory(), text);

        std::ofstream stream(outputFile, std::ios::binary);
        if (!stream.is_open())
        {
            MessageBoxW(nullptr, (L"Cannot write " + outputFile.wstring()).c_str(), L"myengine", MB_OK | MB_ICONERROR);
            return -1;
        }
        stream << text;
        return failures == 0 ? 0 : 1;
    }

    // Searches for the application config path
    std::filesystem::path ResolveConfigPath()
    {
        std::vector<std::filesystem::path> candidates;

    #ifdef MYENGINE_SOURCE_DIR
        candidates.emplace_back(std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "config/app.json");
    #endif

        wchar_t modulePath[MAX_PATH]{};
        // Obtain the full path to the current executable:
        //    - nullptr - means the current module/application
        //    - the result is stored in modulePath
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);

        const std::filesystem::path executableDir = std::filesystem::path(modulePath).parent_path();
        candidates.emplace_back(executableDir / "config/app.json");

        candidates.emplace_back(std::filesystem::current_path() / "config/app.json");

        for (const auto& candidate : candidates)
        {
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }
        }

        return "config/app.json";
    }
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
#ifdef MYENGINE_FLYDOOM
    myengine::core::apppaths::SetProductFolder(L"FlyDoom");   // the files of FlyDoom: %LOCALAPPDATA%\FlyDoom
#endif
    // A crash leaves a minidump and a short report in <UserData>\crashes; the next start says so (docs/game/packaging.md)
    myengine::core::crash::Install(myengine::core::apppaths::UserDataDirectory() / L"crashes", myengine::core::apppaths::LogDirectory() / L"myengine.log");
    myengine::core::crash::CrashIfRequested();

    LaunchOptions options;
#ifdef MYENGINE_FLYDOOM
    // FlyDoom.exe is this program with the game fixed to DOOM
    options.gameMode = true;
    options.doomMode = true;
#endif
    if (!ParseArguments(options))
    {
        MessageBoxW(
            nullptr,
            L"Usage: myengine.exe [--project <file.myproject>] [--scene <path>] [--play] [--project-browser] [--game] [--no-faceid] [--faceid-db <file>] [--fly-brain [--fbrain <file>]]\n"
            L"  Game: --game opens the Rocket League game (menus, HUD, no editor); MYENGINE_UI_SCRIPT=<file> drives it from a script (docs/game/rl-ui.md)\n"
            L"  DOOM: --game doom (or FlyDoom.exe) [--iwad <file.wad>] [--doom-args \"-warp 1 1 -skill 3\"] [--doom-square-pixels] [--doom-no-music]  (docs/game/doom.md)\n"
            L"  Fly plays DOOM: [--fly [doomfly|bio]] [--fbrain <file>] [--fly-seed N] [--fly-reward] [--flybrain auto|cpu|gpu] [--fly-mode fly-learn|human-vs-fly|fly-vs-fly] [--fly-forget]  (docs/game/flydoom-fly.md)\n"
            L"  GPU: [--gpu-capture-frame N [--exit-after-capture]] [--frame-stats N [--frame-stats-out <file>]]\n"
            L"       [--renderdoc] [--pix] [--dx12-debug] [--dx12-gbv] [--dx12-dred]\n"
            L"  Other: [--exit-after-frames <n>] [--dump-shader-layouts <file>]\n"
            L"  Render: [--render-settings shadows=0..2,msaa=1|4,bloom=0|1,instancing=0|1] [--screenshot <file.tga> [--screenshot-frame N]]\n"
            L"  Rocket League online: --rl-host [port] [--rl-clients N] [--rl-mode 1..4] | --rl-join ip[:port]   [--net-sim latency=100,jitter=20,loss=5]",
            L"myengine",
            MB_OK | MB_ICONERROR);
        return -1;
    }

    if (!options.dumpShaderLayouts.empty())
    {
        return DumpShaderLayoutsToFile(options.dumpShaderLayouts);
    }

    WaitForProcess(options.waitForPid);

    if (options.rlOnline.role != myengine::ecs::systems::RlOnlineOptions::Role::None)
    {
        // an online match is the Rocket League map in Play (with --game: its screens start the network)
        if (options.scenePath.empty())
        {
            options.scenePath = L"assets/scenes/rocket_league.json";
        }
        options.startInPlay = true;
    }

    myengine::core::Application app(hInstance);
    app.SetGameMode(options.gameMode);
    if (options.doomMode)
    {
        // who can play: the human, and the fly when the connectome file is there (or the fly was asked for by name: the error says how to get the file)
        doomfly::FlyModeOptions fly;
        fly.connectome = options.fbrain;
        fly.exeDirectory = myengine::core::ProjectContext::ExecutableDirectory();
#ifdef MYENGINE_SOURCE_DIR
        fly.sourceDirectory = std::filesystem::u8path(MYENGINE_SOURCE_DIR);
#endif
#ifndef MYENGINE_FLYDOOM
        options.doom.relaunchArguments = L"--game doom";
#endif
        fly.seed = options.flySeed;
        options.doom.choice.backend = options.flybrain;
        options.doom.choice.seed = options.flySeed;
        fly.reward = options.flyReward;
        fly.forget = options.flyForget;
        fly.log = [&app](const std::string& text) { app.GetLogger().Info(text); };
        fly.renderDevice = [&app]() { return app.RenderDevice(); };
        options.doom.modes = doomfly::MakeModes(fly, options.fly || doomfly::ConnectomeAvailable(fly));
        if (!options.flyMode.empty())
        {
            for (std::size_t i = 0; i < options.doom.modes.size(); ++i)
            {
                if (options.doom.modes[i].id == options.flyMode)
                {
                    options.doom.initialMode = static_cast<int>(i);
                }
            }
            options.doom.choice.controlSet = options.flyBio ? "bio" : "doomfly";
        }
        else if (options.fly)
        {
            options.doom.initialMode = 1;
            options.doom.choice.controlSet = options.flyBio ? "bio" : "doomfly";
        }
        // The laboratory (docs/game/doom.md): the menu with the brain behind it asks what to play, unless the command line already said (--iwad, --doom-args, --fly) or the
        // environment asks for no menu (the scripts and the captures of the game itself: MYENGINE_DOOM_NO_MENU=1)
        {
            char* noMenu = nullptr;
            std::size_t noMenuLength = 0;
            const bool noMenuEnvironment = _dupenv_s(&noMenu, &noMenuLength, "MYENGINE_DOOM_NO_MENU") == 0 && noMenu != nullptr && noMenu[0] != '\0' && std::string_view(noMenu) != "0";
            std::free(noMenu);
            options.doom.showMenu = options.doom.iwad.empty() && options.doom.args.empty() && !options.fly && !noMenuEnvironment;
            options.doom.makeBrainView = [explicitPath = options.fbrain, exeDirectory = fly.exeDirectory, sourceDirectory = fly.sourceDirectory](const myengine::game::DoomBrainViewContext& context)
            {
                return std::make_unique<brainview::DoomMenuBackdrop>(context, brainview::DoomMenuBackdrop::FindConnectome(explicitPath, exeDirectory, sourceDirectory));
            };
        }
    }
    app.SetDoomMode(options.doomMode, options.doom);
    app.SetProjectFile(options.projectFile);
    app.SetShowProjectBrowser(options.showProjectBrowser);
    // The editor window is up while the start map is read: it shows the splash instead of waiting in the dark
    // (not for the GPU diagnostics: they count frames, and the map has to be there from the first one)
    const bool gpuDiagnostics = options.diagnostics.captureFrame != 0 || options.diagnostics.frameStatsFrame != 0;
    app.SetDeferredSceneLoad(!gpuDiagnostics);
    app.SetDiagnosticsOptions(options.diagnostics);
    app.SetExitAfterFrames(options.exitAfterFrames);
    app.SetRlOnlineOptions(options.rlOnline);

    const auto configPath = ResolveConfigPath();
    const auto config = myengine::config::AppConfig::LoadFromFile(configPath);

    if (!app.Initialize(config, options.scenePath, options.startInPlay))
    {
        return app.GetExitCode() != 0 ? app.GetExitCode() : -1;
    }

    app.GetLogger().Info("Config path: " + configPath.string());
    app.GetLogger().Info("Configured windows (from config): " + std::to_string(config.windows.size()));

    app.GetStateMachine().ChangeState(std::make_unique<myengine::appstate::LoadingState>(), app);

    // Window > Fly Brain: the 3D cloud of the connectome with a test stimulus (the module game/brainview); the simulation runs only while the window is open
    std::unique_ptr<brainview::BrainViewWindow> flyBrainWindow;
    if (!options.gameMode && app.GetRenderAdapter() != nullptr && app.GetThumbnailService() != nullptr)
    {
        flyBrainWindow = std::make_unique<brainview::BrainViewWindow>(*app.GetRenderAdapter(), *app.GetThumbnailService(), app.GetLogger(), options.fbrain);   // --fbrain, else the default connectome
        app.AddEditorToolWindow("Fly Brain", [window = flyBrainWindow.get()](bool& open, const double deltaSeconds) { window->Draw(open, deltaSeconds); }, options.openFlyBrain);
    }

    const int exitCode = app.Run();

    flyBrainWindow.reset();   // its GPU objects go back before the renderer shuts down
    app.Shutdown();
    return exitCode;
}
