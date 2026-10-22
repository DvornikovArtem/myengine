#include "SceneEditorInternal.h"

#include <myengine/assistant/AssistantSettings.h>
#include <myengine/assistant/AssistantTools.h>
#include <myengine/core/AppPaths.h>
#include <myengine/editor/LoadingService.h>
#include <myengine/scripting/PrefabLibrary.h>

namespace myengine::ui
{
    using namespace detail;

    // Hands the assistant the pieces of the editor it needs for its tools: the world, the prefabs, the snapshot undo
    // and the script information. Everything stays on the main thread.
    void SceneEditor::InitializeAssistant()
    {
#ifdef MYENGINE_SHIPPING
        // The game for players has no assistant: no panel, no pipe for the MCP bridge, no CLI process
        return;
#else
        assistant::AssistantToolContext context;
        context.world = services_.world;
        context.prefabs = services_.prefabLibrary;
        context.scriptsDirectory = core::apppaths::SourceRoot() / "assets" / "scripts";
        context.captureSnapshot = [this]() { return CaptureSceneSnapshot(); };
        context.restoreSnapshot = services_.restoreSceneSnapshot;
        context.recordUndo = [this](const std::string& label, const std::string& before)
        {
            RecordSceneMutationImmediate(label.c_str(), before);
        };
        context.saveScene = services_.saveScene;
        context.mapLoading = [this]() { return services_.loading != nullptr && services_.loading->HasModal(); };
        context.describeScriptFields = services_.describeScriptFields;
        context.recentErrors = services_.scriptConsole.errors;
        context.scriptStatus = services_.scriptStatus;
        context.liveScriptFields = services_.liveScriptFields;
        assistantTools_ = std::make_unique<assistant::AssistantTools>(std::move(context));

        AssistantPanelConfig config;
        config.repositoryRoot = core::apppaths::SourceRoot();
        config.logger = services_.logger;
        config.tools = assistantTools_.get();
        config.createTexture = services_.createUiTexture;
        config.destroyTexture = services_.destroyUiTexture;

        // The choice of the CLI is the user's, per machine: %APPDATA%\myengine\editor_settings.json. A clean
        // installation has no file and the assistant is off (MYENGINE_ASSISTANT_SETTINGS points to another file)
        config.settingsFile = assistant::AssistantSettings::DefaultFile();
        if (wchar_t override[1024]{}; GetEnvironmentVariableW(L"MYENGINE_ASSISTANT_SETTINGS", override, static_cast<DWORD>(std::size(override))) > 0)
        {
            config.settingsFile = std::filesystem::path(override);
        }
        std::string settingsProblem;
        config.settings = assistant::AssistantSettings::Load(config.settingsFile, &settingsProblem);
        if (!settingsProblem.empty() && services_.logger != nullptr)
        {
            services_.logger->Warning("Assistant: " + settingsProblem);
        }
        assistantPanel_ = std::make_unique<AssistantPanel>(config);
#endif
    }
}
