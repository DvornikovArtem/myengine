// AssistantPanel.cpp

#include <myengine/ui/AssistantPanel.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <windows.h>

#include <imgui/imgui.h>
#include <imgui/misc/imgui_stdlib.h>

#include <myengine/assistant/AssistantBridge.h>
#include <myengine/assistant/AssistantTools.h>
#include <myengine/assistant/ClaudeCliBackend.h>
#include <myengine/core/Logger.h>

#include "editor/EditorWidgets.h"

namespace myengine::ui
{
    namespace
    {
        using Kind = assistant::AssistantMessage::Kind;

        constexpr ImU32 kDiffAdded = IM_COL32(0x7F, 0xD6, 0x84, 255);
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

        // Natural heights of a confirmation card: the title, the detail block and the whole card
        struct ApprovalMetrics
        {
            float title = 0.0f;
            float detail = 0.0f;
            float card = 0.0f;
        };

        ApprovalMetrics MeasureApproval(const assistant::AssistantApproval& approval, const float width)
        {
            ApprovalMetrics metrics;
            const std::string title = "Confirm: " + approval.title;
            PushFontRole(FontRole::Strong);
            metrics.title = std::max(ImGui::CalcTextSize(title.c_str(), nullptr, false, std::max(width - 14.0f - 24.0f - 12.0f, 40.0f)).y, 18.0f);
            PopFontRole();
            if (!approval.detail.empty())
            {
                const std::size_t shown = std::min(approval.detail.size(), kMaxShownBytes);
                PushFontRole(FontRole::Mono);
                metrics.detail = ImGui::CalcTextSize(approval.detail.c_str(), approval.detail.c_str() + shown, false,
                                                     std::max(width - 14.0f - 12.0f - 16.0f, 40.0f)).y + 10.0f;
                PopFontRole();
            }
            // top 4 + title + gap 4 + detail + gap 6 + buttons + bottom 6
            metrics.card = 4.0f + metrics.title + (metrics.detail > 0.0f ? 4.0f + metrics.detail : 0.0f) + 6.0f + style::kFrameHeight + 6.0f;
            return metrics;
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
                const ImU32 color = added ? kDiffAdded : (removed ? style::kErrorText : style::kTextDim);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
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

        // Engine tools and confirmations go through myengine_mcp.exe (a separate process the CLI starts)
        if (config.tools != nullptr)
        {
            auto bridgeExecutable = config.bridgeExecutable;
            if (bridgeExecutable.empty())
            {
                wchar_t module[32768]{};
                const DWORD length = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
                if (length > 0 && length < std::size(module))
                {
                    bridgeExecutable = std::filesystem::path(module).parent_path() / "myengine_mcp.exe";
                }
            }
            if (!bridgeExecutable.empty() && std::filesystem::is_regular_file(bridgeExecutable, error))
            {
                assistant::AssistantBridgeConfig bridgeConfig;
                bridgeConfig.repositoryRoot = cli.workingDirectory;
                auto bridge = std::make_unique<assistant::AssistantBridge>(*config.tools, std::move(bridgeConfig));
                std::string bridgeError;
                if (bridge->Start(bridgeError))
                {
                    cli.bridge.executable = bridgeExecutable;
                    cli.bridge.pipeName = bridge->GetPipeName();
                    cli.bridge.token = bridge->GetToken();
                    cli.bridge.allowedTools = bridge->ReadOnlyToolNames();
                    cli.bridge.approveTool = assistant::AssistantBridge::ApproveToolFullName();
                    bridge_ = std::move(bridge);
                }
                else if (config.logger != nullptr)
                {
                    config.logger->Warning("Assistant: the engine tool bridge is off: " + bridgeError);
                }
            }
            else if (config.logger != nullptr)
            {
                config.logger->Warning("Assistant: myengine_mcp.exe was not found next to the editor; the assistant works with files only");
            }
        }

        auto backend = std::make_unique<assistant::ClaudeCliBackend>(std::move(cli));
        cli_ = backend.get();
        service_ = std::make_unique<assistant::AssistantService>(std::move(backend));
        if (bridge_ != nullptr)
        {
            bridge_->onNotice = [this](const std::string& text) { service_->AddNotice(text); };
        }
    }

    AssistantPanel::AssistantPanel(std::unique_ptr<assistant::IAssistantBackend> backend)
        : service_(std::make_unique<assistant::AssistantService>(std::move(backend)))
    {
    }

    AssistantPanel::~AssistantPanel() = default;

    void AssistantPanel::Update()
    {
        if (bridge_ != nullptr)
        {
            bridge_->Update();
            if (!service_->IsBusy() && !bridge_->GetPending().empty())
            {
                bridge_->RejectAll("The turn ended before the action was confirmed.");
            }
        }
        service_->Update();
    }

    void AssistantPanel::Shutdown()
    {
        service_->Shutdown();
        if (bridge_ != nullptr)
        {
            bridge_->Stop();
        }
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
        DrawCliSettings(); // the settings popup opened from the tools row

        const float inputHeight = ImGui::GetTextLineHeight() * 3.0f + style::kFrameHeight * 0.5f + 12.0f;
        const float footerHeight = inputHeight + 20.0f;

        // While the model waits for a decision the card takes the input row's place: it is always fully
        // visible with its buttons, and a strip of the transcript stays above it
        std::vector<assistant::AssistantApproval> pending;
        if (bridge_ != nullptr)
        {
            pending = bridge_->GetPending();
        }
        if (!pending.empty())
        {
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const ApprovalMetrics metrics = MeasureApproval(pending.front(), std::max(available.x - 20.0f, 80.0f));
            const float preferred = metrics.card + 16.0f + (pending.size() > 1 ? 20.0f : 0.0f);
            const float cardsHeight = std::min(preferred, std::max(available.y - 44.0f, 110.0f));
            DrawTranscript(cardsHeight + ImGui::GetStyle().ItemSpacing.y);
            DrawApprovals(pending, cardsHeight);
        }
        else
        {
            DrawTranscript(footerHeight);
            DrawInput(inputHeight);
        }
    }

    // Tools row (36): New Chat, Stop, status, then backend chip, cost and settings on the right.
    // Under it: the warning banner when the CLI is missing.
    void AssistantPanel::DrawHeader()
    {
        const bool busy = service_->IsBusy();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(style::kPanel));
        ImGui::BeginChild("##assistant_tools", ImVec2(0.0f, style::kPanelToolsHeight), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        if (Button("New Chat", ICON_MESSAGE_SQUARE_PLUS))
        {
            service_->NewConversation();
            drawnRevision_ = 0;
        }
        ImGui::SameLine();
        if (Button("Stop", ICON_SQUARE, busy))
        {
            service_->Cancel();
        }
        ImGui::SameLine();
        if (busy)
        {
            const int dots = static_cast<int>(std::fmod(ImGui::GetTime() * 3.0, 4.0));
            PushFontRole(FontRole::Secondary);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
            ImGui::Text("Working%.*s", dots, "...");
            ImGui::PopStyleColor();
            PopFontRole();
        }

        // Right group: backend chip, session cost, settings
        const char* backendName = service_->GetBackend().GetName();
        char cost[32];
        std::snprintf(cost, sizeof(cost), "$%.4f", service_->GetTotalCostUsd());
        PushFontRole(FontRole::Tiny);
        const float chipWidth = ImGui::CalcTextSize(backendName).x + 16.0f;
        PopFontRole();
        PushFontRole(FontRole::Secondary);
        const float costWidth = ImGui::CalcTextSize(cost).x;
        PopFontRole();
        const float groupWidth = chipWidth + 10.0f + costWidth + 10.0f + style::kPanelIconButton;
        ImGui::SameLine(ImGui::GetWindowWidth() - groupWidth - ImGui::GetStyle().WindowPadding.x);
        Chip(backendName, ChipKind::Gray);
        ImGui::SameLine(0.0f, 10.0f);
        PushFontRole(FontRole::Secondary);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
        ImGui::TextUnformatted(cost);
        ImGui::PopStyleColor();
        PopFontRole();
        ImGui::SameLine(0.0f, 10.0f);
        const ImVec2 settingsPosition = ImGui::GetCursorScreenPos();
        if (IconButton("##assistant_settings_button", ICON_SETTINGS, "Assistant Settings", false, cli_ != nullptr, 0,
                       style::kPanelIconButton, nullptr, IconSize::Row14))
        {
            openSettingsRequested_ = true;
        }
        settingsAnchor_[0] = settingsPosition.x + style::kPanelIconButton;
        settingsAnchor_[1] = settingsPosition.y + style::kPanelIconButton + 2.0f;
        ImGui::EndChild();

        // CLI not found: warning with a link to the settings
        const auto& unavailable = service_->GetAvailabilityMessage();
        if (!unavailable.empty())
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(style::kPanel));
            ImGui::BeginChild("##assistant_banner", ImVec2(0.0f, 0.0f),
                              ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            Banner(BannerKind::Warning, unavailable.c_str());
            PushFontRole(FontRole::Secondary);
            const char* linkLabel = "Set path...";
            const float linkWidth = ImGui::CalcTextSize(linkLabel).x;
            const ImVec2 linkPosition = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##set_path", ImVec2(linkWidth, 18.0f));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::GetWindowDrawList()->AddText(linkPosition, hovered ? style::kPrimaryHover : style::kLink, linkLabel);
            PopFontRole();
            if (clicked)
            {
                openSettingsRequested_ = true;
            }
            ImGui::EndChild();
        }
    }

    // Assistant Settings popup: the CLI path (what used to be the blue CollapsingHeader)
    void AssistantPanel::DrawCliSettings()
    {
        constexpr char kPopup[] = "##assistant_settings_popup";
        if (cli_ == nullptr)
        {
            openSettingsRequested_ = false;
            return;
        }
        if (openSettingsRequested_)
        {
            ImGui::OpenPopup(kPopup);
            openSettingsRequested_ = false;
        }

        ImGui::SetNextWindowPos(ImVec2(settingsAnchor_[0], settingsAnchor_[1]), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(400.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        const bool open = ImGui::BeginPopup(kPopup);
        ImGui::PopStyleVar();
        if (!open)
        {
            return;
        }

        PushFontRole(FontRole::Strong);
        ImGui::TextUnformatted("Assistant Settings");
        PopFontRole();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        PushFontRole(FontRole::Tiny);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
        ImGui::TextUnformatted("CLAUDE CODE CLI PATH");
        ImGui::PopStyleColor();
        PopFontRole();

        const auto& resolved = cli_->GetResolvedExecutable();
        if (pathInput_.empty() && !cli_->GetExecutableOverride().empty())
        {
            pathInput_ = cli_->GetExecutableOverride().u8string();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter = ImGui::InputTextWithHint("##claude_path", "empty: PATH or MYENGINE_CLAUDE_PATH", &pathInput_,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        FocusOutline();
        if (!resolved.empty())
        {
            PushFontRole(FontRole::Tiny);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::Text("Found: %s", resolved.u8string().c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            PopFontRole();
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        const bool apply = PrimaryButton("Apply", ICON_CHECK) || enter;
        if (apply)
        {
            cli_->SetExecutableOverride(std::filesystem::u8path(pathInput_));
            service_->RefreshAvailability();
        }
        ImGui::EndPopup();
    }

    void AssistantPanel::DrawTranscript(const float footerHeight)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(style::kRecessed));
        const bool shown = ImGui::BeginChild("assistant_transcript", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        if (!shown)
        {
            ImGui::EndChild();
            return;
        }
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
        const auto& messages = service_->GetMessages();
        if (messages.empty())
        {
            EmptyState(ICON_SPARKLES, "Ask the assistant", "Where something lives in the project, or a change to a script or a prefab.");
        }
        int index = 0;
        Kind previous = Kind::Summary;
        for (const auto& message : messages)
        {
            ImGui::PushID(index++);
            DrawMessage(message, previous);
            previous = message.kind;
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

    // Confirmation card, pinned under the transcript while the model waits for the decision: light border,
    // panel fill, amber stripe on the left, a scrollable detail block and Reject / Apply that never scroll away
    void AssistantPanel::DrawApprovals(const std::vector<assistant::AssistantApproval>& pending, const float height)
    {
        constexpr float kRegionPad = 8.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, kRegionPad));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(style::kRecessed));
        const bool shown = ImGui::BeginChild("assistant_approvals", ImVec2(0.0f, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        if (!shown || pending.empty())
        {
            ImGui::EndChild();
            return;
        }

        const auto& approval = pending.front();
        const bool more = pending.size() > 1;
        auto* drawList = ImGui::GetWindowDrawList();
        const float width = std::max(ImGui::GetContentRegionAvail().x, 80.0f);
        const ApprovalMetrics metrics = MeasureApproval(approval, width);
        const float moreHeight = more ? 20.0f : 0.0f;
        const float cardHeight = std::min(metrics.card, std::max(height - 2.0f * kRegionPad - moreHeight, 60.0f));
        const float detailHeight = metrics.detail > 0.0f
            ? std::clamp(metrics.detail, 24.0f, std::max(cardHeight - (metrics.card - metrics.detail), 24.0f))
            : 0.0f;
        const float cardBottomPad = 6.0f;

        ImGui::PushID(static_cast<int>(approval.id));
        const ImVec2 cardMin = ImGui::GetCursorScreenPos();
        const ImVec2 cardMax(cardMin.x + width, cardMin.y + cardHeight);
        drawList->AddRectFilled(cardMin, cardMax, style::kPanel, style::kRoundingDialog);
        drawList->AddRectFilled(cardMin, ImVec2(cardMin.x + 3.0f, cardMax.y), style::kWarning, style::kRoundingDialog, ImDrawFlags_RoundCornersLeft);
        drawList->AddRect(cardMin, cardMax, style::kBorderLight, style::kRoundingDialog, 0, 1.0f);

        // Title row
        float y = cardMin.y + 4.0f;
        DrawIcon(drawList, IconSize::Row14, ICON_TRIANGLE_ALERT, ImVec2(cardMin.x + 14.0f + 7.0f, y + 9.0f), style::kWarning);
        ImGui::SetCursorScreenPos(ImVec2(cardMin.x + 14.0f + 24.0f, y));
        PushFontRole(FontRole::Strong);
        ImGui::PushTextWrapPos(cardMin.x + width - 12.0f);
        ImGui::Text("Confirm: %s", approval.title.c_str());
        ImGui::PopTextWrapPos();
        PopFontRole();
        y += metrics.title + 4.0f;

        // Detail: its own scroll area, so that long JSON or a diff never pushes the buttons out of view
        if (detailHeight > 0.0f)
        {
            ImGui::SetCursorScreenPos(ImVec2(cardMin.x + 14.0f, y));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 5.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, style::kRounding);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(style::kInput));
            if (ImGui::BeginChild("##approval_detail", ImVec2(width - 14.0f - 12.0f, detailHeight), ImGuiChildFlags_AlwaysUseWindowPadding))
            {
                PushFontRole(FontRole::Mono);
                if (approval.detail.front() == '+' || approval.detail.front() == '-')
                {
                    DrawDiff(approval.detail);
                }
                else
                {
                    WrappedText(approval.detail);
                }
                PopFontRole();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
        }

        // Buttons on the right, on the bottom edge of the card
        const float buttonsY = cardMax.y - cardBottomPad - style::kFrameHeight;
        ImGui::SetCursorScreenPos(ImVec2(cardMax.x - 12.0f - 2.0f * 88.0f - 8.0f, buttonsY));
        const bool reject = Button("Reject", ICON_X, true, 88.0f);
        ImGui::SameLine();
        const bool apply = PrimaryButton("Apply", ICON_CHECK, true, 88.0f);

        ImGui::SetCursorScreenPos(ImVec2(cardMin.x, cardMax.y + 4.0f));
        ImGui::Dummy(ImVec2(0.0f, 0.0f)); // ends the positioned drawing with an item
        if (more)
        {
            PushFontRole(FontRole::Secondary);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
            ImGui::Text("+%zu more waiting for your decision", pending.size() - 1);
            ImGui::PopStyleColor();
            PopFontRole();
        }
        ImGui::PopID();
        ImGui::EndChild();

        if (apply)
        {
            bridge_->Resolve(approval.id, true);
        }
        else if (reject)
        {
            bridge_->Resolve(approval.id, false);
        }
    }

    void AssistantPanel::DrawMessage(const assistant::AssistantMessage& message, const Kind previousKind)
    {
        auto* drawList = ImGui::GetWindowDrawList();

        // Role caption above the first message of a run
        auto caption = [&](const char* text)
        {
            PushFontRole(FontRole::Tiny);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
            ImGui::TextUnformatted(text);
            ImGui::PopStyleColor();
            PopFontRole();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 6.0f); // pull the message closer to its caption
        };

        switch (message.kind)
        {
        case Kind::User:
        {
            caption("You");
            const float available = ImGui::GetContentRegionAvail().x;
            const float wrap = std::max(available * 0.7f - 24.0f, 60.0f);
            const std::size_t shown = std::min(message.text.size(), kMaxShownBytes);
            const ImVec2 textSize = ImGui::CalcTextSize(message.text.c_str(), message.text.c_str() + shown, false, wrap);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max(min.x + textSize.x + 24.0f, min.y + textSize.y + 16.0f);
            drawList->AddRectFilled(min, max, style::kHeader, style::kRoundingDialog);
            PushFontRole(FontRole::Body);
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(min.x + 12.0f, min.y + 8.0f), style::kTextStrong,
                              message.text.c_str(), message.text.c_str() + shown, wrap);
            PopFontRole();
            ImGui::Dummy(ImVec2(textSize.x + 24.0f, textSize.y + 16.0f));
            break;
        }

        case Kind::Assistant:
            if (previousKind == Kind::User || previousKind == Kind::Summary || previousKind == Kind::Error)
            {
                caption("Assistant");
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextStrong));
            WrappedText(message.text);
            ImGui::PopStyleColor();
            break;

        case Kind::Tool:
        {
            std::string toolName = message.toolName;
            if (toolName.rfind("mcp__myengine__", 0) == 0)
            {
                toolName = "engine." + toolName.substr(15); // an engine tool through the bridge
            }
            std::string header = toolName;
            if (!message.text.empty())
            {
                header += "  " + message.text;
            }

            ImGuiStorage* storage = ImGui::GetStateStorage();
            const ImGuiID openId = ImGui::GetID("##tool_open");
            bool open = storage->GetBool(openId, false);
            const bool expandable = !message.detail.empty();

            const float rowWidth = std::max(ImGui::GetContentRegionAvail().x, 60.0f);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float rowHeight = 24.0f;
            const ImVec2 max(min.x + rowWidth, min.y + rowHeight);
            const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(min, max);
            if (hovered && expandable)
            {
                drawList->AddRectFilled(min, max, style::kHeader, style::kRounding);
            }
            const float centerY = min.y + rowHeight * 0.5f;
            // The user's own "no" is a decision, not a failure: neutral icon, text and chip
            const bool rejected = message.toolFailed && message.toolOutput.rfind(assistant::kUserRejectedText, 0) == 0;
            const bool failed = message.toolFailed && !rejected;
            DrawIcon(drawList, IconSize::Row14, rejected ? ICON_CIRCLE_X : ICON_WRENCH, ImVec2(min.x + 12.0f, centerY),
                     failed ? style::kError : style::kTextDim);

            const char* chipText = rejected ? "rejected" : (failed ? "failed" : (message.toolDone ? "done" : "running"));
            const ChipKind chipKind = (rejected || (!failed && !message.toolDone)) ? ChipKind::Gray : (failed ? ChipKind::Red : ChipKind::Green);
            PushFontRole(FontRole::Tiny);
            const float chipWidth = ImGui::CalcTextSize(chipText).x + 16.0f + 12.0f;
            PopFontRole();

            PushFontRole(FontRole::Mono);
            drawList->PushClipRect(ImVec2(min.x + 26.0f, min.y), ImVec2(max.x - chipWidth - 16.0f, max.y), true);
            drawList->AddText(ImVec2(min.x + 26.0f, std::floor(centerY - style::kFontMono * 0.5f - 0.5f)),
                              failed ? style::kErrorText : style::kTextDim, header.c_str());
            drawList->PopClipRect();
            PopFontRole();

            ImGui::SetCursorScreenPos(ImVec2(max.x - chipWidth - (expandable ? 14.0f : 4.0f), min.y + 3.0f));
            Chip(chipText, chipKind, !message.toolDone && !message.toolFailed);
            if (expandable)
            {
                DrawIcon(drawList, IconSize::Chevron12, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, ImVec2(max.x - 8.0f, centerY), style::kTextDim);
            }

            // The row's own button last: it ends the row with an item and keeps the cursor spacing
            ImGui::SetCursorScreenPos(min);
            const bool pressed = ImGui::InvisibleButton("##tool", ImVec2(rowWidth, rowHeight));
            if (pressed && expandable)
            {
                open = !open;
                storage->SetBool(openId, open);
            }

            if (expandable && open)
            {
                ImGui::Indent(24.0f);
                PushFontRole(FontRole::Mono);
                DrawDiff(message.detail);
                PopFontRole();
                ImGui::Unindent(24.0f);
            }
            if (failed && !message.toolOutput.empty())
            {
                ImGui::Indent(24.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kErrorText));
                PushFontRole(FontRole::Mono);
                WrappedText(message.toolOutput);
                PopFontRole();
                ImGui::PopStyleColor();
                ImGui::Unindent(24.0f);
            }
            break;
        }

        case Kind::Notice:
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
            WrappedText(message.text);
            ImGui::PopStyleColor();
            break;

        case Kind::Error:
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kErrorText));
            WrappedText(message.text);
            ImGui::PopStyleColor();
            break;

        case Kind::Summary:
            PushFontRole(FontRole::Secondary);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
            ImGui::TextUnformatted(message.text.c_str());
            ImGui::PopStyleColor();
            PopFontRole();
            ImGui::Separator();
            break;

        case Kind::Files:
        {
            const ImVec2 position = ImGui::GetCursorScreenPos();
            DrawIcon(drawList, IconSize::Row14, ICON_FILE_DIFF, ImVec2(position.x + 7.0f, position.y + 8.0f), style::kWarning);
            ImGui::SetCursorScreenPos(ImVec2(position.x + 24.0f, position.y));
            PushFontRole(FontRole::Secondary);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kText));
            ImGui::TextUnformatted("Changed files (hot reload picks them up)");
            ImGui::PopStyleColor();
            PopFontRole();
            PushFontRole(FontRole::Mono);
            std::size_t position2 = 0;
            while (position2 < message.text.size())
            {
                auto end = message.text.find('\n', position2);
                if (end == std::string::npos)
                {
                    end = message.text.size();
                }
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(style::kTextDim));
                ImGui::Text("%.*s", static_cast<int>(end - position2), message.text.c_str() + position2);
                ImGui::PopStyleColor();
                position2 = end + 1;
            }
            PopFontRole();
            break;
        }
        }
    }

    void AssistantPanel::DrawInput(const float inputHeight)
    {
        const bool busy = service_->IsBusy();
        const float sendWidth = 96.0f;

        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        const float left = 10.0f;
        const float available = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + left);
        const ImVec2 size(available - left * 2.0f - sendWidth - 8.0f, inputHeight);
        const ImVec2 inputTop = ImGui::GetCursorPos();
        if (reclaimFocus_)
        {
            ImGui::SetKeyboardFocusHere();
            reclaimFocus_ = false;
        }
        // Enter sends, Ctrl+Enter adds a line
        const auto flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_WordWrap;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 8.0f));
        bool submit = ImGui::InputTextMultiline("##assistant_input", &input_, size, flags);
        ImGui::PopStyleVar();
        const ImVec2 inputMin = ImGui::GetItemRectMin();
        const ImVec2 inputMax = ImGui::GetItemRectMax();
        FocusOutline();
        // The multiline input is a child window drawn after its parent, so the placeholder goes to the foreground list
        // (clipped to the field, skipped while a popup could cover it)
        if (input_.empty() && !ImGui::IsItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
        {
            PushFontRole(FontRole::Body);
            ImDrawList* foreground = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
            foreground->PushClipRect(inputMin, inputMax, true);
            foreground->AddText(ImVec2(inputMin.x + 10.0f, inputMin.y + 8.0f), style::kTextDim,
                                "Ask about the project or request a change... (Enter: send, Ctrl+Enter: new line)");
            foreground->PopClipRect();
            PopFontRole();
        }

        ImGui::SetCursorPos(ImVec2(inputTop.x + size.x + 8.0f, inputTop.y + inputHeight - style::kFrameHeight));
        const bool send = PrimaryButton("Send", ICON_SEND, !busy && !input_.empty(), sendWidth);
        submit = send || (submit && !busy);
        if (submit)
        {
            Submit();
        }
    }
}
