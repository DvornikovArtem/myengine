// main.cpp

#include <filesystem>
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
    bool ParseSceneArgument(std::filesystem::path& scenePath)
    {
        int argumentCount = 0;
        wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        if (arguments == nullptr)
        {
            return false;
        }

        bool valid = true;
        for (int index = 1; index < argumentCount; ++index)
        {
            if (std::wstring_view(arguments[index]) != L"--scene" || !scenePath.empty() || index + 1 >= argumentCount)
            {
                valid = false;
                break;
            }
            scenePath = arguments[++index];
            if (scenePath.empty() || std::wstring_view(arguments[index]) == L"--scene")
            {
                valid = false;
                break;
            }
        }

        LocalFree(arguments);
        return valid;
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
    std::filesystem::path scenePath;
    if (!ParseSceneArgument(scenePath))
    {
        MessageBoxW(nullptr, L"Usage: myengine.exe [--scene <path>]", L"myengine", MB_OK | MB_ICONERROR);
        return -1;
    }

    myengine::core::Application app(hInstance);

    const auto configPath = ResolveConfigPath();
    const auto config = myengine::config::AppConfig::LoadFromFile(configPath);

    if (!app.Initialize(config, scenePath))
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
