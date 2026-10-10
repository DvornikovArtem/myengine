// AssistantPanel.cpp

#include <myengine/ui/AssistantPanel.h>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <system_error>
#include <utility>

#include <imgui/imgui.h>
#include <imgui/misc/imgui_stdlib.h>

#include <myengine/assistant/ClaudeCliBackend.h>

namespace myengine::ui
{
    namespace
    {
        using Kind = assistant::AssistantMessage::Kind;

        constexpr ImVec4 kUserColor{0.55f, 0.75f, 1.0f, 1.0f};
        constexpr ImVec4 kToolColor{0.62f, 0.68f, 0.78f, 1.0f};
        constexpr ImVec4 kErrorColor{1.0f, 0.4f, 0.35f, 1.0f};
        constexpr ImVec4 kAddedColor{0.45f, 0.85f, 0.45f, 1.0f};
        constexpr ImVec4 kRemovedColor{0.95f, 0.5f, 0.45f, 1.0f};
        constexpr ImVec4 kFilesColor{0.95f, 0.8f, 0.4f, 1.0f};
        constexpr std::size_t kMaxShownBytes = 64 * 1024;

        void WrappedText(const std::string& text)
        {
            const std::size_t shown = std::min(text.size(), kMaxShownBytes);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(text.c_str(), text.c_str() + shown);
            if (shown < text.size())
            {
                ImGui::TextDisabled("... (%zu more bytes)", text.size() - shown);
            }
            ImGui::PopTextWrapPos();
        }

        void DrawDiff(const std::string& diff)
        {
            std::size_t position = 0;
            while (position < diff.size())
            {
                auto end = diff.find('\n', position);
                if (end == std::string::npos)
                {
                    end = diff.size();
                }
                const char* begin = diff.c_str() + position;
                const char* stop = diff.c_str() + end;
                const bool added = begin < stop && *begin == '+';
                const bool removed = begin < stop && *begin == '-';
                if (added || removed)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, added ? kAddedColor : kRemovedColor);
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                }
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(begin, stop);
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                position = end + 1;
            }
        }
    }

    AssistantPanel::AssistantPanel(const AssistantPanelConfig& config)
    {
        assistant::ClaudeCliConfig cli;
        std::error_code error;
        cli.workingDirectory = std::filesystem::is_directory(config.repositoryRoot, error) ? config.repositoryRoot : std::filesystem::current_path(error);
        cli.systemPromptFile = cli.workingDirectory / "assets" / "assistant" / "system_prompt.md";
        cli.logger = config.logger;
        auto backend = std::make_unique<assistant::ClaudeCliBackend>(std::move(cli));
        cli_ = backend.get();
        service_ = std::make_unique<assistant::AssistantService>(std::move(backend));
    }

    AssistantPanel::AssistantPanel(std::unique_ptr<assistant::IAssistantBackend> backend)
        : service_(std::make_unique<assistant::AssistantService>(std::move(backend)))
    {
    }

    AssistantPanel::~AssistantPanel() = default;

    void AssistantPanel::Update()
    {
        service_->Update();
    }

    void AssistantPanel::Shutdown()
    {
        service_->Shutdown();
    }

    void AssistantPanel::Submit()
    {
        if (service_->IsBusy())
        {
            return;
        }
        if (service_->Send(input_))
        {
            input_.clear();
            scrollToBottom_ = true;
        }
        reclaimFocus_ = true;
    }

    void AssistantPanel::Draw()
    {
        DrawHeader();
        DrawCliSettings();

        const float inputHeight = ImGui::GetTextLineHeightWithSpacing() * 3.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
        DrawTranscript(inputHeight + ImGui::GetStyle().ItemSpacing.y);
        DrawInput();
    }

    void AssistantPanel::DrawHeader()
    {
        const bool busy = service_->IsBusy();
        if (ImGui::SmallButton("New chat"))
        {
            service_->NewConversation();
            drawnRevision_ = 0;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!busy);
        if (ImGui::SmallButton("Stop"))
        {
            service_->Cancel();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (busy)
        {
            const int dots = static_cast<int>(std::fmod(ImGui::GetTime() * 3.0, 4.0));
            ImGui::TextDisabled("Working%.*s", dots, "...");
        }
        else if (!service_->GetAvailabilityMessage().empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
            ImGui::TextUnformatted("Claude Code CLI not found");
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::TextDisabled("%s | session cost $%.4f", service_->GetBackend().GetName(), service_->GetTotalCostUsd());
        }

        const auto& unavailable = service_->GetAvailabilityMessage();
        if (!unavailable.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(unavailable.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    }

    void AssistantPanel::DrawCliSettings()
    {
        if (cli_ == nullptr || !ImGui::CollapsingHeader("Claude Code CLI path"))
        {
            return;
        }
        const auto& resolved = cli_->GetResolvedExecutable();
        if (!resolved.empty())
        {
            ImGui::TextDisabled("Found: %s", resolved.u8string().c_str());
        }
        if (pathInput_.empty() && !cli_->GetExecutableOverride().empty())
        {
            pathInput_ = cli_->GetExecutableOverride().u8string();
        }
        const float buttonWidth = ImGui::CalcTextSize("Apply").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x);
        bool apply = ImGui::InputTextWithHint("##claude_path", "empty: PATH or MYENGINE_CLAUDE_PATH", &pathInput_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        apply = ImGui::Button("Apply") || apply;
        if (apply)
        {
            cli_->SetExecutableOverride(std::filesystem::u8path(pathInput_));
            service_->RefreshAvailability();
        }
    }

    void AssistantPanel::DrawTranscript(const float footerHeight)
    {
        if (!ImGui::BeginChild("assistant_transcript", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_Borders))
        {
            ImGui::EndChild();
            return;
        }
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
        const auto& messages = service_->GetMessages();
        if (messages.empty())
        {
            ImGui::TextDisabled("Ask where something lives in the project, or ask for a change to a script or a prefab.");
            ImGui::TextDisabled("The assistant can edit assets/scripts and assets/prefabs; hot reload picks the files up.");
        }
        int index = 0;
        for (const auto& message : messages)
        {
            ImGui::PushID(index++);
            DrawMessage(message);
            ImGui::PopID();
        }
        const auto revision = service_->GetRevision();
        if (revision != drawnRevision_)
        {
            if (atBottom || scrollToBottom_)
            {
                ImGui::SetScrollHereY(1.0f);
            }
            drawnRevision_ = revision;
        }
        scrollToBottom_ = false;
        ImGui::EndChild();
    }

    void AssistantPanel::DrawMessage(const assistant::AssistantMessage& message)
    {
        switch (message.kind)
        {
        case Kind::User:
            ImGui::TextColored(kUserColor, "You");
            WrappedText(message.text);
            break;

        case Kind::Assistant:
            WrappedText(message.text);
            break;

        case Kind::Tool:
        {
            const char* state = message.toolFailed ? "[failed]" : (message.toolDone ? "[done]" : "[...]");
            std::string header = std::string(state) + " " + message.toolName;
            if (!message.text.empty())
            {
                header += "  " + message.text;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, message.toolFailed ? kErrorColor : kToolColor);
            if (!message.detail.empty())
            {
                if (ImGui::TreeNodeEx("##tool", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", header.c_str()))
                {
                    ImGui::PopStyleColor();
                    DrawDiff(message.detail);
                    ImGui::TreePop();
                }
                else
                {
                    ImGui::PopStyleColor();
                }
            }
            else
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(header.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            if (message.toolFailed && !message.toolOutput.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(message.toolOutput.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            break;
        }

        case Kind::Notice:
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            WrappedText(message.text);
            ImGui::PopStyleColor();
            break;

        case Kind::Error:
            ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
            WrappedText(message.text);
            ImGui::PopStyleColor();
            break;

        case Kind::Summary:
            ImGui::TextDisabled("%s", message.text.c_str());
            ImGui::Separator();
            break;

        case Kind::Files:
        {
            ImGui::TextColored(kFilesColor, "Changed files (hot reload picks them up):");
            std::size_t position = 0;
            while (position < message.text.size())
            {
                auto end = message.text.find('\n', position);
                if (end == std::string::npos)
                {
                    end = message.text.size();
                }
                ImGui::BulletText("%.*s", static_cast<int>(end - position), message.text.c_str() + position);
                position = end + 1;
            }
            break;
        }
        }
    }

    void AssistantPanel::DrawInput()
    {
        const bool busy = service_->IsBusy();
        const float buttonWidth = ImGui::CalcTextSize("Send").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float height = ImGui::GetTextLineHeightWithSpacing() * 3.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
        const ImVec2 size(ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x, height);
        if (reclaimFocus_)
        {
            ImGui::SetKeyboardFocusHere();
            reclaimFocus_ = false;
        }
        // Enter sends, Ctrl+Enter adds a line
        const auto flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_WordWrap;
        bool submit = ImGui::InputTextMultiline("##assistant_input", &input_, size, flags);
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        submit = ImGui::Button("Send") || submit;
        ImGui::EndDisabled();
        if (submit)
        {
            Submit();
        }
    }
}
