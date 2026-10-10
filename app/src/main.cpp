// main.cpp

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <states/LoadingState.h>

#include <myengine/config/AppConfig.h>
#include <myengine/core/Application.h>

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
        bool showProjectBrowser = false;
        DWORD waitForPid = 0;
    };

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
            if (argument == L"--project-browser" && !options.showProjectBrowser)
            {
                options.showProjectBrowser = true;
                continue;
            }

            const bool isScene = argument == L"--scene";
            const bool isProject = argument == L"--project";
            const bool isWait = argument == L"--wait-for-pid";
            if (!isScene && !isProject && !isWait)
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

            if (isScene && options.scenePath.empty())
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

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    LaunchOptions options;
    if (!ParseArguments(options))
    {
        MessageBoxW(
            nullptr,
            L"Usage: myengine.exe [--project <file.myproject>] [--scene <path>] [--play] [--project-browser]",
            L"myengine",
            MB_OK | MB_ICONERROR);
        return -1;
    }

    WaitForProcess(options.waitForPid);

    myengine::core::Application app(hInstance);
    app.SetProjectFile(options.projectFile);
    app.SetShowProjectBrowser(options.showProjectBrowser);

    const auto configPath = ResolveConfigPath();
    const auto config = myengine::config::AppConfig::LoadFromFile(configPath);

    if (!app.Initialize(config, options.scenePath, options.startInPlay))
    {
        return -1;
    }

    app.GetLogger().Info("Config path: " + configPath.string());
    app.GetLogger().Info("Configured windows (from config): " + std::to_string(config.windows.size()));

    app.GetStateMachine().ChangeState(std::make_unique<myengine::appstate::LoadingState>(), app);

    const int exitCode = app.Run();

    app.Shutdown();
    return exitCode;
}
