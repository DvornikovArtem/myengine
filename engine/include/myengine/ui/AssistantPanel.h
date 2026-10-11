// AssistantPanel.h

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <myengine/assistant/AssistantService.h>
#include <myengine/assistant/ChatStore.h>
#include <myengine/assistant/Markdown.h>

namespace myengine::core
{
    class Logger;
}

namespace myengine::assistant
{
    class AssistantBridge;
    struct AssistantApproval;
    class AssistantTools;
    class ClaudeCliBackend;
}

namespace myengine::ui
{
    struct AssistantPanelConfig
    {
        std::filesystem::path repositoryRoot; // the assistant works here and may edit assets/scripts and assets/prefabs
        core::Logger* logger = nullptr;
        // Engine tools for the assistant (through the MCP bridge). Null: the panel works with files only
        assistant::AssistantTools* tools = nullptr;
        // myengine_mcp.exe; empty: next to the running executable
        std::filesystem::path bridgeExecutable;
        // Thumbnails of attached pictures: RGBA pixels -> an ImGui texture id (0 on failure), and back. Optional
        std::function<std::uint64_t(const unsigned char* rgba, std::uint32_t width, std::uint32_t height)> createTexture;
        std::function<void(std::uint64_t)> destroyTexture;
    };

    // Assistant panel: list of chats, header with model and effort, transcript in Markdown, composer with
    // attachments. Owns the AssistantService; Update() must run every frame even when the window is hidden.
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
        assistant::AssistantBridge* GetBridge() { return bridge_.get(); } // null without engine tools

        // Adds files to the message being written (also used by drops). Returns how many were added
        std::size_t AttachFiles(const std::vector<std::filesystem::path>& files);

    private:
        struct PendingAttachment
        {
            assistant::AssistantAttachment info;
            std::string problem;        // not empty: the file cannot be sent, the chip is red
            std::uint64_t texture = 0;  // thumbnail of a picture (0: an icon is shown instead)
            float aspect = 1.0f;
        };

        struct CachedMarkdown
        {
            std::size_t size = 0;
            std::size_t hash = 0;
            std::vector<assistant::MarkdownBlock> blocks;
        };

        void DrawChatList(float width, bool popup);
        void DrawChatRow(const assistant::ChatInfo& chat, float width, bool popup);
        void DrawChat(bool wide);
        void DrawChatHeader(bool wide);
        void DrawModelMenus();
        void DrawCliSettings();
        void DrawTranscript(float footerHeight);
        void DrawMessage(const assistant::AssistantMessage& message, std::size_t index, bool lastOfRun);
        void DrawAttachmentChips(const std::vector<assistant::AssistantAttachment>& attachments);
        void DrawComposer(float height);
        void DrawApprovals(const std::vector<assistant::AssistantApproval>& pending, float height);
        void DrawDropOverlay(const float min[2], const float max[2]);
        void TakeDroppedFiles(const float min[2], const float max[2]);
        void Submit();
        void RefreshChats(bool force);
        void ChooseFiles(bool imagesOnly);
        void PasteFromClipboard();
        void RemoveAttachment(std::size_t index);
        void ClearAttachments();
        const std::vector<assistant::MarkdownBlock>& Markdown(std::size_t index, const std::string& text);
        std::filesystem::path AttachmentsDirectory() const;
        std::filesystem::path ResolveProjectPath(const std::string& projectPath) const;

        std::unique_ptr<assistant::AssistantBridge> bridge_; // before service_: the backend refers to its pipe
        std::unique_ptr<assistant::AssistantService> service_;
        assistant::ClaudeCliBackend* cli_ = nullptr; // owned by service_, null for an injected backend
        AssistantPanelConfig config_;
        std::string input_;
        std::string pathInput_;
        std::vector<PendingAttachment> attachments_;
        std::vector<assistant::ChatInfo> chats_;
        std::string search_;
        std::string renameId_;
        std::string renameText_;
        bool renameFocus_ = false;
        bool titleRenameRequested_ = false;
        std::unordered_map<std::uint64_t, CachedMarkdown> markdown_;
        std::unordered_map<std::string, std::uint64_t> thumbnails_; // path -> texture
        std::vector<std::string> projectFiles_;                     // for the @ popup, scanned on demand
        std::string mentionSearch_;
        std::string attachmentNotice_;
        std::uint64_t drawnRevision_ = 0;
        std::uint64_t chatsRevision_ = ~0ull;
        double chatsRefreshedAt_ = 0.0;
        bool chatsDirty_ = true;
        bool scrollToBottom_ = false;
        bool newBelow_ = false;
        bool reclaimFocus_ = false;
        bool openSettingsRequested_ = false;
        bool openMentionRequested_ = false;
        bool composerFocused_ = false;
        bool wideLayout_ = true;
        int dropAgeFrames_ = 0;
        std::unordered_map<std::string, float> thumbnailAspect_;
        float settingsAnchor_[2]{};
        float panelMin_[2]{};
        float panelMax_[2]{};
        std::size_t drawnPending_ = 0;
    };
}
