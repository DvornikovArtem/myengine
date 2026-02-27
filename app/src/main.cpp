// main.cpp

#include <filesystem>
#include <vector>

#include <states/LoadingState.h>

#include <myengine/config/AppConfig.h>
#include <myengine/core/Application.h>

// If the WIN32_LEAN_AND_MEAN macro is defined before including windows.h, rarely used parts are excluded from the header to speed up compilation
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace
{
    // Searches for the application config path
    std::filesystem::path ResolveConfigPath()
    {
        std::vector<std::filesystem::path> candidates;

    #ifdef MYENGINE_SOURCE_DIR
        candidates.emplace_back(std::filesystem::path(MYENGINE_SOURCE_DIR) / "config/app.json");
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
    myengine::core::Application app(hInstance);

    const auto configPath = ResolveConfigPath();
    const auto config = myengine::config::AppConfig::LoadFromFile(configPath);

    if (!app.Initialize(config))
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