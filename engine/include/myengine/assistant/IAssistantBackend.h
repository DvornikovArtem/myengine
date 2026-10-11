// IAssistantBackend.h

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace myengine::assistant
{
    enum class AssistantEventType : std::uint8_t
    {
        SessionStarted, // sessionId, text = model name
        TextDelta,      // text = next piece of the answer
        Thinking,       // text = next piece of the model's reasoning (often empty: the CLI may send only a signature)
        ToolUse,        // toolId, toolName, text = short arguments, detail = diff / preview, filePath for file tools
        ToolResult,     // toolId, isError, text = first lines of the output, filePath = a file the tool has changed
        Notice,         // text = a service message (API retry and similar)
        Finished,       // the turn is over: isError, text = final text or reason, usage fields
        Error,          // the backend itself failed (not found, crashed): text = reason
    };

    struct AssistantEvent
    {
        AssistantEventType type = AssistantEventType::Notice;
        std::string text;
        std::string detail;
        std::string sessionId;
        std::string toolId;
        std::string toolName;
        std::string filePath;
        bool isError = false;
        double costUsd = 0.0;
        std::uint64_t inputTokens = 0;
        std::uint64_t outputTokens = 0;
        std::uint64_t durationMs = 0;
        std::uint32_t turns = 0;
    };

    // A file the user attached to a message. Images go to the model as image blocks, other files as a path in the text
    struct AssistantAttachment
    {
        std::string path;      // absolute path (UTF-8)
        std::string name;      // display name
        std::string mediaType; // "image/png", "image/jpeg", ... for images, empty for other files
        bool IsImage() const { return !mediaType.empty(); }
    };

    // Everything one turn needs. model / effort are the settings of the chat the message belongs to; empty means the
    // CLI's default (the backend's configured model for `model`).
    struct AssistantTurnRequest
    {
        std::string prompt;
        std::string sessionId; // empty for a new conversation
        std::string model;     // alias (opus, sonnet, haiku, fable) or a full model name
        std::string effort;    // low, medium, high, xhigh, max
        std::vector<AssistantAttachment> attachments;
    };

    // One conversation turn at a time. Everything is called from the main thread and never blocks:
    // a backend either keeps its own worker or polls a pipe / socket in Poll().
    class IAssistantBackend
    {
    public:
        virtual ~IAssistantBackend() = default;

        virtual const char* GetName() const = 0;
        // Empty string - the backend can run; otherwise a message for the user
        virtual std::string CheckAvailability() = 0;
        // Starts a turn. sessionId is empty for a new conversation. On failure fills `error` and returns false
        virtual bool BeginTurn(const std::string& prompt, const std::string& sessionId, std::string& error) = 0;
        // The same with the settings of the chat. Backends that know nothing about them ignore model / effort / attachments
        virtual bool BeginTurn(const AssistantTurnRequest& request, std::string& error)
        {
            return BeginTurn(request.prompt, request.sessionId, error);
        }
        // Interrupts the current turn without events; the backend is idle afterwards
        virtual void Cancel() = 0;
        virtual bool IsBusy() const = 0;
        // Appends everything that arrived since the last call
        virtual void Poll(std::vector<AssistantEvent>& events) = 0;
        virtual void Shutdown() = 0;
    };
}
