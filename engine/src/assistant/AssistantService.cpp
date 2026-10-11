// AssistantService.cpp

#include <myengine/assistant/AssistantService.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

#include <myengine/assistant/ChatStore.h>

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

        std::int64_t NowUnix()
        {
            return static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        }
    }

    void ApplyContentEvent(std::deque<AssistantMessage>& messages, const AssistantEvent& event, std::vector<std::string>* changedFiles)
    {
        using Kind = AssistantMessage::Kind;
        switch (event.type)
        {
        case AssistantEventType::TextDelta:
        {
            if (event.text.empty())
            {
                break;
            }
            if (messages.empty() || messages.back().kind != Kind::Assistant)
            {
                AssistantMessage message;
                message.kind = Kind::Assistant;
                messages.push_back(std::move(message));
            }
            auto& text = messages.back().text;
            if (text.size() < kMaxTextBytes)
            {
                text += event.text;
            }
            break;
        }

        case AssistantEventType::Thinking:
        {
            if (event.text.empty())
            {
                break;
            }
            if (messages.empty() || messages.back().kind != Kind::Thinking)
            {
                AssistantMessage message;
                message.kind = Kind::Thinking;
                messages.push_back(std::move(message));
            }
            auto& text = messages.back().text;
            if (text.size() < kMaxTextBytes)
            {
                text += event.text;
            }
            break;
        }

        case AssistantEventType::ToolUse:
        {
            AssistantMessage message;
            message.kind = Kind::Tool;
            message.toolId = event.toolId;
            message.toolName = event.toolName;
            message.text = event.text;
            message.detail = event.detail;
            messages.push_back(std::move(message));
            break;
        }

        case AssistantEventType::ToolResult:
        {
            for (auto it = messages.rbegin(); it != messages.rend(); ++it)
            {
                if (it->kind == Kind::Tool && it->toolId == event.toolId)
                {
                    it->toolDone = true;
                    it->toolFailed = event.isError;
                    it->toolOutput = event.text;
                    break;
                }
            }
            if (changedFiles != nullptr && !event.isError && !event.filePath.empty() &&
                std::find(changedFiles->begin(), changedFiles->end(), event.filePath) == changedFiles->end())
            {
                changedFiles->push_back(event.filePath);
            }
            break;
        }

        default:
            break;
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

    void AssistantService::AddNotice(const std::string& text)
    {
        AssistantMessage message;
        message.kind = AssistantMessage::Kind::Notice;
        message.text = text;
        Append(std::move(message));
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
        if (message.timeUnix == 0)
        {
            message.timeUnix = NowUnix();
        }
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
        return Send(text, {});
    }

    bool AssistantService::Send(const std::string& text, std::vector<AssistantAttachment> attachments)
    {
        if (backend_ == nullptr || IsBusy())
        {
            return false;
        }
        const auto prompt = Trim(text);
        if (prompt.empty() && attachments.empty())
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
        user.attachments = attachments;
        Append(std::move(user));

        AssistantTurnRequest request;
        request.prompt = prompt.empty() ? std::string("See the attached files.") : prompt;
        request.sessionId = sessionId_;
        request.model = model_;
        request.effort = effort_;
        request.attachments = std::move(attachments);
        if (sessionId_.empty() && firstPrompt_.empty())
        {
            firstPrompt_ = prompt;
        }

        std::string error;
        if (!backend_->BeginTurn(request, error))
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
        firstPrompt_.clear();
        actualModel_.clear();
        totalCostUsd_ = 0.0;
        totalInputTokens_ = 0;
        totalOutputTokens_ = 0;
        turnOpen_ = false;
        ++revision_;
    }

    bool AssistantService::Retry()
    {
        if (backend_ == nullptr || IsBusy())
        {
            return false;
        }
        for (auto it = messages_.rbegin(); it != messages_.rend(); ++it)
        {
            if (it->kind == AssistantMessage::Kind::User)
            {
                const auto text = it->text;
                auto attachments = it->attachments;
                return Send(text, std::move(attachments));
            }
        }
        return false;
    }

    void AssistantService::SetChatStore(std::unique_ptr<ChatStore> store)
    {
        chats_ = std::move(store);
        if (chats_ != nullptr)
        {
            chats_->Refresh();
        }
    }

    bool AssistantService::OpenChat(const std::string& id, std::string& error)
    {
        if (chats_ == nullptr)
        {
            error = "Chat history is not available.";
            return false;
        }
        if (IsBusy())
        {
            error = "Stop the current answer before switching chats.";
            return false;
        }
        std::deque<AssistantMessage> loaded;
        if (!chats_->LoadTranscript(id, loaded, rootDirectory_, kMaxMessages, error))
        {
            return false;
        }
        messages_ = std::move(loaded);
        changedFiles_.clear();
        sessionId_ = id;
        firstPrompt_.clear();
        turnOpen_ = false;
        totalCostUsd_ = 0.0;
        totalInputTokens_ = 0;
        totalOutputTokens_ = 0;
        actualModel_.clear();
        if (const auto* info = chats_->Find(id); info != nullptr)
        {
            model_ = info->model;
            effort_ = info->effort;
            totalCostUsd_ = info->costUsd;
            totalInputTokens_ = info->inputTokens;
            totalOutputTokens_ = info->outputTokens;
            actualModel_ = info->lastModel;
        }
        ++revision_;
        return true;
    }

    void AssistantService::RenameChat(const std::string& id, const std::string& title)
    {
        if (chats_ != nullptr && !id.empty() && !Trim(title).empty())
        {
            chats_->Rename(id, Trim(title));
            ++revision_;
        }
    }

    void AssistantService::HideChat(const std::string& id)
    {
        if (chats_ == nullptr || id.empty())
        {
            return;
        }
        chats_->Hide(id);
        if (id == sessionId_)
        {
            NewConversation();
        }
        ++revision_;
    }

    std::string AssistantService::GetChatTitle() const
    {
        if (chats_ != nullptr && !sessionId_.empty())
        {
            if (const auto* info = chats_->Find(sessionId_); info != nullptr && !info->title.empty())
            {
                return info->title;
            }
        }
        return firstPrompt_.empty() ? std::string() : MakeChatTitle(firstPrompt_);
    }

    void AssistantService::SetModel(const std::string& model)
    {
        model_ = model;
        if (chats_ != nullptr && !sessionId_.empty())
        {
            chats_->SetModel(sessionId_, model_);
        }
        ++revision_;
    }

    void AssistantService::SetEffort(const std::string& effort)
    {
        effort_ = effort;
        if (chats_ != nullptr && !sessionId_.empty())
        {
            chats_->SetEffort(sessionId_, effort_);
        }
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
                if (chats_ != nullptr)
                {
                    // The chat exists from its first answer on: the index keeps its title and settings
                    chats_->Touch(sessionId_, MakeChatTitle(firstPrompt_), model_, effort_);
                }
            }
            if (!event.text.empty())
            {
                actualModel_ = event.text;
            }
            ++revision_;
            break;

        case AssistantEventType::TextDelta:
        case AssistantEventType::Thinking:
        case AssistantEventType::ToolUse:
        case AssistantEventType::ToolResult:
        {
            const std::size_t before = messages_.size();
            ApplyContentEvent(messages_, event, &changedFiles_);
            if (event.type != AssistantEventType::Thinking && !messages_.empty())
            {
                // anything after the reasoning ends it
                for (auto it = messages_.rbegin(); it != messages_.rend() && it->kind != AssistantMessage::Kind::User; ++it)
                {
                    if (it->kind == AssistantMessage::Kind::Thinking && it->endTimeUnix == 0)
                    {
                        it->endTimeUnix = NowUnix();
                    }
                }
            }
            for (std::size_t i = before; i < messages_.size(); ++i)
            {
                if (messages_[i].timeUnix == 0)
                {
                    messages_[i].timeUnix = NowUnix();
                }
                if (messages_[i].kind == AssistantMessage::Kind::Assistant && messages_[i].model.empty())
                {
                    messages_[i].model = actualModel_;
                }
            }
            while (messages_.size() > kMaxMessages)
            {
                messages_.pop_front();
            }
            ++revision_;
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
            totalInputTokens_ += event.inputTokens;
            totalOutputTokens_ += event.outputTokens;
            if (chats_ != nullptr && !sessionId_.empty())
            {
                chats_->Touch(sessionId_, MakeChatTitle(firstPrompt_), model_, effort_); // the time of the last answer
            }

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
