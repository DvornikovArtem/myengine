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
        };

        Kind kind = Kind::Notice;
        std::string text;
        std::string detail;
        std::string toolName;
        std::string toolId;
        std::string toolOutput;
        bool toolDone = false;
        bool toolFailed = false;
    };

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
        void Cancel();
        // Cancels a running turn, clears the transcript and forgets the session
        void NewConversation();
        void Shutdown();

        bool IsBusy() const;
        const std::deque<AssistantMessage>& GetMessages() const { return messages_; }
        const std::string& GetSessionId() const { return sessionId_; }
        double GetTotalCostUsd() const { return totalCostUsd_; }
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
        std::deque<AssistantMessage> messages_;
        std::vector<AssistantEvent> pending_;
        std::vector<std::string> changedFiles_;
        std::string sessionId_;
        std::string availability_;
        double totalCostUsd_ = 0.0;
        std::uint64_t revision_ = 0;
        bool availabilityChecked_ = false;
        bool turnOpen_ = false;
    };
}
