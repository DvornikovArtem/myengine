// AssistantService.cpp

#include <myengine/assistant/AssistantService.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace myengine::assistant
{
    namespace
    {
        constexpr std::size_t kMaxMessages = 500;
        constexpr std::size_t kMaxTextBytes = 256 * 1024;

        std::string Trim(const std::string& text)
        {
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
            {
                return {};
            }
            const auto last = text.find_last_not_of(" \t\r\n");
            return text.substr(first, last - first + 1);
        }
    }

    AssistantService::AssistantService(std::unique_ptr<IAssistantBackend> backend) : backend_(std::move(backend))
    {
    }

    AssistantService::~AssistantService()
    {
        Shutdown();
    }

    void AssistantService::Shutdown()
    {
        if (backend_ != nullptr)
        {
            backend_->Shutdown();
        }
        turnOpen_ = false;
    }

    bool AssistantService::IsBusy() const
    {
        return backend_ != nullptr && backend_->IsBusy();
    }

    std::string AssistantService::RefreshAvailability()
    {
        availability_ = backend_ != nullptr ? backend_->CheckAvailability() : std::string("No assistant backend.");
        availabilityChecked_ = true;
        return availability_;
    }

    void AssistantService::Append(AssistantMessage message)
    {
        messages_.push_back(std::move(message));
        while (messages_.size() > kMaxMessages)
        {
            messages_.pop_front();
        }
        ++revision_;
    }

    void AssistantService::Update()
    {
        if (backend_ == nullptr)
        {
            return;
        }
        if (!availabilityChecked_)
        {
            RefreshAvailability();
        }
        pending_.clear();
        backend_->Poll(pending_);
        for (const auto& event : pending_)
        {
            ApplyEvent(event);
        }
        if (turnOpen_ && !backend_->IsBusy())
        {
            // A backend must end a turn with Finished or Error; this keeps the panel usable if it does not
            FinishTurn();
        }
    }

    bool AssistantService::Send(const std::string& text)
    {
        if (backend_ == nullptr || IsBusy())
        {
            return false;
        }
        const auto prompt = Trim(text);
        if (prompt.empty())
        {
            return false;
        }

        const auto unavailable = RefreshAvailability();
        if (!unavailable.empty())
        {
            AssistantMessage message;
            message.kind = AssistantMessage::Kind::Error;
            message.text = unavailable;
            Append(std::move(message));
            return false;
        }

        AssistantMessage user;
        user.kind = AssistantMessage::Kind::User;
        user.text = prompt;
        Append(std::move(user));

        std::string error;
        if (!backend_->BeginTurn(prompt, sessionId_, error))
        {
            AssistantMessage message;
            message.kind = AssistantMessage::Kind::Error;
            message.text = error.empty() ? std::string("The assistant could not start.") : error;
            Append(std::move(message));
            return false;
        }
        turnOpen_ = true;
        changedFiles_.clear();
        return true;
    }

    void AssistantService::Cancel()
    {
        if (backend_ == nullptr || !backend_->IsBusy())
        {
            return;
        }
        backend_->Cancel();
        AssistantMessage message;
        message.kind = AssistantMessage::Kind::Notice;
        message.text = "Stopped.";
        Append(std::move(message));
        for (auto it = messages_.rbegin(); it != messages_.rend() && it->kind != AssistantMessage::Kind::User; ++it)
        {
            if (it->kind == AssistantMessage::Kind::Tool && !it->toolDone)
            {
                it->toolDone = true;
                it->toolFailed = true;
                it->toolOutput = "interrupted";
            }
        }
        FinishTurn();
    }

    void AssistantService::NewConversation()
    {
        if (backend_ != nullptr && backend_->IsBusy())
        {
            backend_->Cancel();
        }
        messages_.clear();
        changedFiles_.clear();
        sessionId_.clear();
        totalCostUsd_ = 0.0;
        turnOpen_ = false;
        ++revision_;
    }

    void AssistantService::FinishTurn()
    {
        if (!changedFiles_.empty())
        {
            AssistantMessage files;
            files.kind = AssistantMessage::Kind::Files;
            for (const auto& file : changedFiles_)
            {
                if (!files.text.empty())
                {
                    files.text += '\n';
                }
                files.text += file;
            }
            Append(std::move(files));
            changedFiles_.clear();
        }
        turnOpen_ = false;
    }

    void AssistantService::ApplyEvent(const AssistantEvent& event)
    {
        switch (event.type)
        {
        case AssistantEventType::SessionStarted:
            if (!event.sessionId.empty())
            {
                sessionId_ = event.sessionId;
            }
            break;

        case AssistantEventType::TextDelta:
        {
            if (event.text.empty())
            {
                break;
            }
            if (messages_.empty() || messages_.back().kind != AssistantMessage::Kind::Assistant)
            {
                AssistantMessage message;
                message.kind = AssistantMessage::Kind::Assistant;
                Append(std::move(message));
            }
            auto& text = messages_.back().text;
            if (text.size() < kMaxTextBytes)
            {
                text += event.text;
                ++revision_;
            }
            break;
        }

        case AssistantEventType::ToolUse:
        {
            AssistantMessage message;
            message.kind = AssistantMessage::Kind::Tool;
            message.toolId = event.toolId;
            message.toolName = event.toolName;
            message.text = event.text;
            message.detail = event.detail;
            Append(std::move(message));
            break;
        }

        case AssistantEventType::ToolResult:
        {
            for (auto it = messages_.rbegin(); it != messages_.rend(); ++it)
            {
                if (it->kind == AssistantMessage::Kind::Tool && it->toolId == event.toolId)
                {
                    it->toolDone = true;
                    it->toolFailed = event.isError;
                    it->toolOutput = event.text;
                    ++revision_;
                    break;
                }
            }
            if (!event.isError && !event.filePath.empty() &&
                std::find(changedFiles_.begin(), changedFiles_.end(), event.filePath) == changedFiles_.end())
            {
                changedFiles_.push_back(event.filePath);
            }
            break;
        }

        case AssistantEventType::Notice:
        {
            AssistantMessage message;
            message.kind = AssistantMessage::Kind::Notice;
            message.text = event.text;
            Append(std::move(message));
            break;
        }

        case AssistantEventType::Finished:
        {
            if (!event.sessionId.empty())
            {
                sessionId_ = event.sessionId;
            }
            totalCostUsd_ += event.costUsd;

            bool hasAnswer = false; // an Assistant message since the user's prompt of this turn
            for (auto it = messages_.rbegin(); it != messages_.rend() && it->kind != AssistantMessage::Kind::User; ++it)
            {
                hasAnswer = hasAnswer || it->kind == AssistantMessage::Kind::Assistant;
            }
            if (event.isError)
            {
                AssistantMessage message;
                message.kind = AssistantMessage::Kind::Error;
                message.text = event.text.empty() ? std::string("The turn ended with an error.") : event.text;
                Append(std::move(message));
            }
            else if (!hasAnswer && !event.text.empty())
            {
                // The CLI delivered no text blocks (older version without partial messages): use the final text
                AssistantMessage message;
                message.kind = AssistantMessage::Kind::Assistant;
                message.text = event.text;
                Append(std::move(message));
            }

            char summary[160];
            std::snprintf(summary, sizeof(summary), "%.1f s | $%.4f | %llu in / %llu out tokens | %u turn(s)",
                static_cast<double>(event.durationMs) / 1000.0, event.costUsd,
                static_cast<unsigned long long>(event.inputTokens), static_cast<unsigned long long>(event.outputTokens), event.turns);
            AssistantMessage footer;
            footer.kind = AssistantMessage::Kind::Summary;
            footer.text = summary;
            Append(std::move(footer));
            FinishTurn();
            break;
        }

        case AssistantEventType::Error:
        {
            AssistantMessage message;
            message.kind = AssistantMessage::Kind::Error;
            message.text = event.text;
            Append(std::move(message));
            FinishTurn();
            break;
        }
        }
    }
}
