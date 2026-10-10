// AssistantPanel.h

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <myengine/assistant/AssistantService.h>

namespace myengine::core
{
    class Logger;
}

namespace myengine::assistant
{
    class ClaudeCliBackend;
}

namespace myengine::ui
{
    struct AssistantPanelConfig
    {
        std::filesystem::path repositoryRoot; // the assistant works here and may edit assets/scripts and assets/prefabs
        core::Logger* logger = nullptr;
    };

    // Chat panel: input line, streamed answer, tool calls with diffs, list of changed files.
    // Owns the AssistantService; Update() must run every frame even when the window is hidden.
    class AssistantPanel
    {
    public:
        explicit AssistantPanel(const AssistantPanelConfig& config);
        explicit AssistantPanel(std::unique_ptr<assistant::IAssistantBackend> backend); // for tests
        ~AssistantPanel();

        void Update();
        void Draw(); // inside an ImGui window
        void Shutdown();

        assistant::AssistantService& GetService() { return *service_; }

    private:
        void DrawHeader();
        void DrawCliSettings();
        void DrawTranscript(float footerHeight);
        void DrawMessage(const assistant::AssistantMessage& message);
        void DrawInput();
        void Submit();

        std::unique_ptr<assistant::AssistantService> service_;
        assistant::ClaudeCliBackend* cli_ = nullptr; // owned by service_, null for an injected backend
        std::string input_;
        std::string pathInput_;
        std::uint64_t drawnRevision_ = 0;
        bool scrollToBottom_ = false;
        bool reclaimFocus_ = false;
    };
}
