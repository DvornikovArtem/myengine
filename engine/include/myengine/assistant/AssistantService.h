// AssistantService.h

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <myengine/assistant/IAssistantBackend.h>

namespace myengine::assistant
{
    class ChatStore;

    struct AssistantMessage
    {
        enum class Kind : std::uint8_t
        {
            User,
            Assistant,
            Tool,    // a tool call: toolName + text (arguments) + optional detail (diff)
            Notice,  // service line
            Error,
            Summary, // end of a turn: duration, cost, tokens
            Files,   // files changed by the turn, one per line in text
            Thinking, // the model's reasoning (shown folded)
        };

        Kind kind = Kind::Notice;
        std::string text;
        std::string detail;
        std::string toolName;
        std::string toolId;
        std::string toolOutput;
        bool toolDone = false;
        bool toolFailed = false;
        std::int64_t timeUnix = 0;                  // when the message was written (0: unknown)
        std::int64_t endTimeUnix = 0;               // Thinking: when the reasoning ended (0: still going)
        std::string model;                          // Assistant: the model that answered
        std::vector<AssistantAttachment> attachments; // User: images and files that came with the message
    };

    // Turns one backend event into the transcript (text, reasoning, tool calls and their results). Everything that
    // does not touch the transcript - session ids, cost, the end of a turn - stays with the service. Also used when an
    // old session is read from its file. `changedFiles` (optional) collects files that tools have changed.
    void ApplyContentEvent(std::deque<AssistantMessage>& messages, const AssistantEvent& event, std::vector<std::string>* changedFiles);

    // Main-thread facade over a backend: keeps the transcript and the session id, pumps events every frame.
    class AssistantService
    {
    public:
        explicit AssistantService(std::unique_ptr<IAssistantBackend> backend);
        ~AssistantService();

        // Call once per frame. Non-blocking
        void Update();

        // False when a turn is running, the text is empty or the backend is unavailable (the reason goes to the transcript)
        bool Send(const std::string& text);
        bool Send(const std::string& text, std::vector<AssistantAttachment> attachments);
        void Cancel();
        // Cancels a running turn, clears the transcript and forgets the session. The settings (model, effort) stay:
        // a new chat starts with what the user chose last
        void NewConversation();

        // ---- chats ----
        // The history and the per-chat settings live in the store; without one the service keeps a single conversation
        void SetChatStore(std::unique_ptr<ChatStore> store);
        ChatStore* GetChatStore() { return chats_.get(); }
        // Loads a stored conversation; the next message continues it (--resume). False with a reason in `error`
        bool OpenChat(const std::string& id, std::string& error);
        void RenameChat(const std::string& id, const std::string& title);
        // Removes a chat from the list. Opened right now: the panel moves to an empty new chat
        void HideChat(const std::string& id);
        std::string GetChatTitle() const;
        // File paths of old tool calls are shown relative to this directory (the project root)
        void SetRootDirectory(std::string root) { rootDirectory_ = std::move(root); }

        // Settings of the current chat. Empty model / effort: the CLI's default. Changes are stored with the chat
        // and apply from the next message on
        const std::string& GetModel() const { return model_; }
        const std::string& GetEffort() const { return effort_; }
        void SetModel(const std::string& model);
        void SetEffort(const std::string& effort);
        // The model that really answered (system/init of the last turn), empty before the first answer
        const std::string& GetActualModel() const { return actualModel_; }
        void Shutdown();
        // A service line in the transcript (confirmations, bridge events)
        void AddNotice(const std::string& text);

        bool IsBusy() const;
        const std::deque<AssistantMessage>& GetMessages() const { return messages_; }
        const std::string& GetSessionId() const { return sessionId_; }
        double GetTotalCostUsd() const { return totalCostUsd_; }
        // Tokens of the open chat (input includes cached tokens)
        std::uint64_t GetTotalInputTokens() const { return totalInputTokens_; }
        std::uint64_t GetTotalOutputTokens() const { return totalOutputTokens_; }
        // Sends the last message of the user once more (same text, same attachments). False while a turn runs
        bool Retry();
        std::uint64_t GetRevision() const { return revision_; }
        IAssistantBackend& GetBackend() { return *backend_; }
        // Re-asks the backend whether it can run (after the user changed a path)
        std::string RefreshAvailability();
        const std::string& GetAvailabilityMessage() const { return availability_; }

    private:
        void Append(AssistantMessage message);
        void ApplyEvent(const AssistantEvent& event);
        void FinishTurn();

        std::unique_ptr<IAssistantBackend> backend_;
        std::unique_ptr<ChatStore> chats_;
        std::string model_;
        std::string effort_;
        std::string actualModel_;
        std::string rootDirectory_;
        std::string firstPrompt_; // of the conversation that has no session yet: its title
        std::deque<AssistantMessage> messages_;
        std::vector<AssistantEvent> pending_;
        std::vector<std::string> changedFiles_;
        std::string sessionId_;
        std::string availability_;
        double totalCostUsd_ = 0.0;
        std::uint64_t totalInputTokens_ = 0;
        std::uint64_t totalOutputTokens_ = 0;
        std::uint64_t revision_ = 0;
        bool availabilityChecked_ = false;
        bool turnOpen_ = false;
    };
}
