#include "SceneEditorInternal.h"
#include "PrankUi.h"

#include <deque>
#include <sstream>

#include <myengine/assistant/AssistantSettings.h>
#include <myengine/assistant/AssistantTools.h>
#include <myengine/core/AppPaths.h>
#include <myengine/physics/PhysicsWorldState.h>

#include <windows.h>

namespace myengine::ui
{
    using namespace detail;

    namespace
    {
        // The scripted input of the tests (`editor prank click ...`): one event of the mouse or the keyboard per frame, so that ImGui sees every press and release
        struct PrankInputEvent
        {
            enum class Kind
            {
                Move,
                Down,
                Up,
                Key,
                Text,
            };
            Kind kind = Kind::Move;
            float x = 0.0f;
            float y = 0.0f;
            ImGuiKey key = ImGuiKey_None;
            bool down = false;
            std::string text;
        };

        std::deque<PrankInputEvent>& InputQueue()
        {
            static std::deque<PrankInputEvent> queue;
            return queue;
        }

        void PushClick(const float x, const float y, const int times)
        {
            auto& queue = InputQueue();
            queue.push_back({PrankInputEvent::Kind::Move, x, y, ImGuiKey_None, false, {}});
            for (int i = 0; i < times; ++i)
            {
                queue.push_back({PrankInputEvent::Kind::Down, x, y, ImGuiKey_None, true, {}});
                queue.push_back({PrankInputEvent::Kind::Up, x, y, ImGuiKey_None, false, {}});
            }
        }

        void PushKey(const ImGuiKey key)
        {
            auto& queue = InputQueue();
            queue.push_back({PrankInputEvent::Kind::Key, 0.0f, 0.0f, key, true, {}});
            queue.push_back({PrankInputEvent::Kind::Key, 0.0f, 0.0f, key, false, {}});
        }

        // Feeds one queued event to ImGui (the context is the current one while the editor builds its windows)
        void DrainInput()
        {
            auto& queue = InputQueue();
            if (queue.empty())
            {
                return;
            }
            const PrankInputEvent event = queue.front();
            queue.pop_front();
            ImGuiIO& io = ImGui::GetIO();
            switch (event.kind)
            {
                case PrankInputEvent::Kind::Move: io.AddMousePosEvent(event.x, event.y); break;
                case PrankInputEvent::Kind::Down: io.AddMouseButtonEvent(0, true); break;
                case PrankInputEvent::Kind::Up: io.AddMouseButtonEvent(0, false); break;
                case PrankInputEvent::Kind::Key: io.AddKeyEvent(event.key, event.down); break;
                case PrankInputEvent::Kind::Text: io.AddInputCharactersUTF8(event.text.c_str()); break;
            }
        }
    }

    // The easter egg tab (docs/editor/prank-tab.md): the editor creates it when it is on and hands it what it needs - the scene snapshots with the undo, the
    // Play button, and a second assistant for the generator at the end of the folders.
    void SceneEditor::EnsurePrankTab()
    {
#ifdef MYENGINE_SHIPPING
        return; // the game for players has no easter egg
#else
        if (prankTab_ != nullptr || !prank::Enabled() || !initialized_)
        {
            return;
        }
        prank::PrankSceneServices scene;
        scene.root = core::apppaths::SourceRoot();
        scene.capture = [this]() { return CaptureSceneSnapshot(); };
        scene.restore = [this](const std::string& snapshot) { return services_.restoreSceneSnapshot != nullptr && services_.restoreSceneSnapshot(snapshot); };
        scene.recordUndo = [this](const std::string& label, const std::string& before) { RecordSceneMutationImmediate(label.c_str(), before); };
        scene.sceneDirty = []() { return core::ServiceLocator::GetEditorRuntimeState().sceneDirty; };
        scene.saveScene = services_.saveScene;
        scene.isPlaying = []() { return core::ServiceLocator::GetEditorRuntimeState().mode == editor::RuntimeMode::Play; };
        scene.stopPlay = [this]() { StopPlayMode(); };
        scene.playNow = [this]()
        {
            auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
            if (editorState.mode != editor::RuntimeMode::Play && services_.captureSceneSnapshot)
            {
                editorState.playModeSnapshot = services_.captureSceneSnapshot();
                editorState.mode = editor::RuntimeMode::Play;
                core::ServiceLocator::GetPhysicsWorldState().physicsPaused = false;
            }
            ImGui::SetWindowFocus(kViewportWindowName);
        };
        scene.makePanel = [this]() -> std::unique_ptr<AssistantPanel>
        {
            if (assistantTools_ == nullptr)
            {
                return nullptr;
            }
            AssistantPanelConfig config;
            config.repositoryRoot = core::apppaths::SourceRoot();
            config.logger = services_.logger;
            config.tools = assistantTools_.get();
            config.createTexture = services_.createUiTexture;
            config.destroyTexture = services_.destroyUiTexture;
            // The choice of the CLI is the user's and is the same as in the usual panel; this panel never writes the file and never opens the bridge to others
            config.settings = assistantPanel_ != nullptr ? assistantPanel_->GetSettings() : assistant::AssistantSettings();
            config.settings.allowExternalMcp = false;
            config.settingsFile.clear();
            config.extraPromptFile = core::apppaths::SourceRoot() / "assets" / "assistant" / "prank_prompt.md";
            config.bridgePipeName = L"\\\\.\\pipe\\myengine-assistant-prank-" + std::to_wstring(GetCurrentProcessId());
            return std::make_unique<AssistantPanel>(config);
        };
        prankTab_ = std::make_unique<prank::PrankTab>(std::move(scene), prank::AgeCheckFace{});
#endif
    }

    float SceneEditor::PrankMenuButtonWidth() const
    {
        return prankTab_ != nullptr ? prankTab_->MenuButtonWidth() : 0.0f;
    }

    void SceneEditor::DrawPrankMenuButton(const float x, const float y)
    {
        if (prankTab_ != nullptr)
        {
            prankTab_->DrawMenuButton(ImVec2(x, y));
        }
    }

    void SceneEditor::BuildPrankTab(const SceneEditorWindowContext& windowContext)
    {
        // The scripted input of the tests works whether the tab is on or not (the checks of the menu and of --no-prank click the editor itself)
        if (prankOwner_ == 0 || prankOwner_ == windowContext.windowId || !prank::Enabled())
        {
            DrainInput();
        }
        if (!prank::Enabled())
        {
            // Off: no window, no trace in the layout file (the entry that was loaded from imgui.ini is dropped, so it is not written again)
            ImGui::ClearWindowSettings(prank::PrankTab::WindowName());
            if (prankTab_ != nullptr)
            {
                prankTab_->Shutdown();
                prankTab_.reset();
            }
            return;
        }
        EnsurePrankTab();
        if (prankTab_ == nullptr)
        {
            return;
        }
        // One editor window draws the tab (the others have none)
        if (prankOwner_ != 0 && prankOwner_ != windowContext.windowId)
        {
            return;
        }
        prankOwner_ = windowContext.windowId;
        const float dt = ImGui::GetIO().DeltaTime;
        prankTab_->Update(dt);

        // It lands in the node of the Viewport when the saved layout has no place for it
        ImGuiID dockId = 0;
        if (ImGuiWindow* viewport = ImGui::FindWindowByName(kViewportWindowName); viewport != nullptr)
        {
            dockId = viewport->DockId;
        }
        prankTab_->DrawWindow(dockId);
    }

    // The commands of the scripted input (`editor prank ...`, docs/editor/prank-tab.md): the screenshots and the checks that cannot click
    std::string SceneEditor::RunPrankCommand(const std::string& command)
    {
        std::istringstream words(command);
        std::string verb;
        words >> verb;
        const bool inputVerb = verb == "click" || verb == "dclick" || verb == "move" || verb == "esc" || verb == "enter" || verb == "type";
        if (!prank::Enabled() && !inputVerb)
        {
            return "the easter egg tab is off";
        }
        if (prank::Enabled())
        {
            EnsurePrankTab();
        }
        if (prankTab_ == nullptr && !inputVerb)
        {
            return "no tab";
        }
        // The tab may be off: only the input verbs go on, and `@id` has nobody to answer
        prank::PrankExplorer dummy;
        prank::PrankExplorer& explorer = prankTab_ != nullptr ? prankTab_->Explorer() : dummy;
        // Clicks: `click <x> <y>` or `click @<id>` (a button of the tab by its id, see PrankUi.h), `dclick` is a double click
        if (verb == "click" || verb == "dclick")
        {
            std::string target;
            words >> target;
            float x = 0.0f;
            float y = 0.0f;
            if (!target.empty() && target[0] == '@')
            {
                ImVec2 center;
                if (!prank::FindHitRecord(target.substr(1), center))
                {
                    return "no such button: " + target;
                }
                x = center.x;
                y = center.y;
            }
            else
            {
                x = static_cast<float>(std::atof(target.c_str()));
                words >> y;
            }
            PushClick(x, y, verb == "dclick" ? 2 : 1);
            return verb + " " + std::to_string(static_cast<int>(x)) + " " + std::to_string(static_cast<int>(y));
        }
        if (verb == "move")
        {
            float x = 0.0f;
            float y = 0.0f;
            words >> x >> y;
            InputQueue().push_back({PrankInputEvent::Kind::Move, x, y, ImGuiKey_None, false, {}});
            return "move";
        }
        if (verb == "esc")
        {
            PushKey(ImGuiKey_Escape);
            return "esc";
        }
        if (verb == "enter")
        {
            PushKey(ImGuiKey_Enter);
            return "enter";
        }
        if (verb == "type")
        {
            std::string rest;
            std::getline(words, rest);
            if (!rest.empty() && rest.front() == ' ')
            {
                rest.erase(rest.begin());
            }
            InputQueue().push_back({PrankInputEvent::Kind::Text, 0.0f, 0.0f, ImGuiKey_None, false, rest});
            return "type";
        }
        if (verb == "focus")
        {
            prankTab_->RequestFocus();
            return "the tab comes to the front";
        }
        if (verb == "level")
        {
            int level = 0;
            words >> level;
            explorer.GoTo(level - 1);
            return "level " + std::to_string(explorer.Level() + 1);
        }
        if (verb == "item")
        {
            int index = 0;
            words >> index;
            explorer.OpenItem(index);
            return explorer.ViewerOpen() ? "the file is open" : "no such file";
        }
        if (verb == "close")
        {
            explorer.CloseViewer();
            return "closed";
        }
        if (verb == "gag")
        {
            std::string name;
            words >> name;
            using prank::GagKind;
            GagKind kind = GagKind::None;
            bool video = false;
            if (name == "captcha") kind = GagKind::Captcha;
            else if (name == "paywall") kind = GagKind::Paywall;
            else if (name == "ivan") kind = GagKind::Ivan;
            else if (name == "age") kind = GagKind::Age;
            else if (name == "load") kind = GagKind::Load99;
            else if (name == "video") { kind = GagKind::Load99; video = true; }
            else if (name == "uac") kind = GagKind::Uac;
            if (kind == GagKind::None)
            {
                return "unknown gag";
            }
            prankTab_->Gags().Open(kind, video);
            return "gag " + name;
        }
        if (verb == "gag-age")
        {
            float seconds = 0.0f;
            words >> seconds;
            prankTab_->Gags().ForceAge(seconds);
            return "the gag moves on";
        }
        if (verb == "generator")
        {
            prankTab_->EnterGenerator();
            return "the generator";
        }
        if (verb == "ask")
        {
            std::string rest;
            std::getline(words, rest);
            while (!rest.empty() && rest.front() == ' ')
            {
                rest.erase(rest.begin());
            }
            prankTab_->EnterGenerator();
            prankTab_->Generator().Submit(rest);
            return std::string(prankTab_->Generator().UsedAssistant() ? "the assistant" : "the prepared scene") + " answers: " + rest;
        }
        if (verb == "watch")
        {
            prankTab_->Generator().WatchLast();
            return prankTab_->Generator().LastTurnApplied() ? "applied" : "not applied (waiting)";
        }
        if (verb == "play")
        {
            // what the button "play" of the generator does
            auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
            if (editorState.mode != editor::RuntimeMode::Play && services_.captureSceneSnapshot)
            {
                editorState.playModeSnapshot = services_.captureSceneSnapshot();
                editorState.mode = editor::RuntimeMode::Play;
                core::ServiceLocator::GetPhysicsWorldState().physicsPaused = false;
            }
            ImGui::SetWindowFocus(kViewportWindowName);
            return "play";
        }
        if (verb == "stop")
        {
            StopPlayMode();
            return "stop";
        }
        return "unknown prank command";
    }
}
