#include "SceneEditorInternal.h"

#include <myengine/assistant/AssistantTools.h>
#include <myengine/scripting/PrefabLibrary.h>

namespace myengine::ui
{
    using namespace detail;

    // Hands the assistant the pieces of the editor it needs for its tools: the world, the prefabs, the snapshot undo
    // and the script information. Everything stays on the main thread.
    void SceneEditor::InitializeAssistant()
    {
        assistant::AssistantToolContext context;
        context.world = services_.world;
        context.prefabs = services_.prefabLibrary;
        context.scriptsDirectory = std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "assets" / "scripts";
        context.captureSnapshot = [this]() { return CaptureSceneSnapshot(); };
        context.restoreSnapshot = services_.restoreSceneSnapshot;
        context.recordUndo = [this](const std::string& label, const std::string& before)
        {
            RecordSceneMutationImmediate(label.c_str(), before);
        };
        context.saveScene = services_.saveScene;
        context.describeScriptFields = services_.describeScriptFields;
        context.recentErrors = services_.scriptConsole.errors;
        context.scriptStatus = services_.scriptStatus;
        context.liveScriptFields = services_.liveScriptFields;
        assistantTools_ = std::make_unique<assistant::AssistantTools>(std::move(context));

        AssistantPanelConfig config;
        config.repositoryRoot = std::filesystem::u8path(MYENGINE_SOURCE_DIR);
        config.logger = services_.logger;
        config.tools = assistantTools_.get();
        assistantPanel_ = std::make_unique<AssistantPanel>(config);
    }
}
