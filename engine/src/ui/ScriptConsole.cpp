// ScriptConsole.cpp

#include <myengine/ui/ScriptConsole.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <imgui/imgui.h>
#include <imgui/misc/imgui_stdlib.h>

#include "editor/EditorWidgets.h"

namespace myengine::ui
{
    namespace
    {
        constexpr std::size_t kMaxOutputLines = 256;
        constexpr std::size_t kMaxHistory = 64;
        constexpr std::size_t kMaxLineBytes = 64 * 1024;
    }

    void ScriptConsole::Append(std::string text, const bool error, const LineKind kind)
    {
        if (text.size() > kMaxLineBytes)
        {
            text.resize(kMaxLineBytes);
            text += "\n... truncated ...";
        }
        output_.push_back({std::move(text), error, error ? LineKind::Error : kind});
        while (output_.size() > kMaxOutputLines)
        {
            output_.pop_front();
        }
        scrollToBottom_ = true;
    }

    bool ScriptConsole::Submit(const ScriptConsoleServices& services, const std::string& source)
    {
        ObserveGeneration(services);
        if (source.empty() && !incomplete_)
        {
            return false;
        }
        Append(std::string(incomplete_ ? "... " : ">>> ") + source, false, LineKind::Echo);
        if (!source.empty() && source.size() <= 16 * 1024 && (history_.empty() || history_.back() != source))
        {
            history_.push_back(source);
            if (history_.size() > kMaxHistory)
            {
                history_.pop_front();
            }
        }
        historyPosition_ = -1;
        historyDraft_.clear();
        input_.clear();
        reclaimFocus_ = true;
        if (!services.execute)
        {
            Append("The Python console is unavailable.", true);
            incomplete_ = false;
            return false;
        }
        const auto result = services.execute(source);
        incomplete_ = result.incomplete;
        if (!result.output.empty())
        {
            Append(result.output);
        }
        if (!result.success)
        {
            Append(result.error, true);
        }
        return result.success;
    }

    void ScriptConsole::Reset(const ScriptConsoleServices& services)
    {
        if (services.reset)
        {
            services.reset();
        }
        if (services.generation)
        {
            generation_ = services.generation();
            observedGeneration_ = true;
        }
        incomplete_ = false;
        input_.clear();
        historyPosition_ = -1;
        historyDraft_.clear();
        Append("Python console variables and pending input reset. Game objects were not reset.", false, LineKind::Notice);
    }

    void ScriptConsole::ClearOutput()
    {
        output_.clear();
        scrollToBottom_ = false;
    }

    void ScriptConsole::ObserveGeneration(const ScriptConsoleServices& services)
    {
        if (!services.generation)
        {
            return;
        }
        const auto generation = services.generation();
        if (observedGeneration_ && generation_ != generation)
        {
            incomplete_ = false;
            input_.clear();
            historyPosition_ = -1;
            historyDraft_.clear();
            Append("Console variables reset on Stop / scene load. History and output were kept.", false, LineKind::Notice);
        }
        generation_ = generation;
        observedGeneration_ = true;
    }

    std::string ScriptConsole::PreviousCommand()
    {
        if (history_.empty())
        {
            return input_;
        }
        if (historyPosition_ < 0)
        {
            historyDraft_ = input_;
            historyPosition_ = static_cast<int>(history_.size()) - 1;
        }
        else if (historyPosition_ > 0)
        {
            --historyPosition_;
        }
        return history_[static_cast<std::size_t>(historyPosition_)];
    }

    std::string ScriptConsole::NextCommand()
    {
        if (historyPosition_ < 0)
        {
            return input_;
        }
        if (++historyPosition_ >= static_cast<int>(history_.size()))
        {
            historyPosition_ = -1;
            return historyDraft_;
        }
        return history_[static_cast<std::size_t>(historyPosition_)];
    }

    int ScriptConsole::InputCallback(ImGuiInputTextCallbackData* data)
    {
        auto& console = *static_cast<ScriptConsole*>(data->UserData);
        if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
        {
            const auto command = data->EventKey == ImGuiKey_UpArrow ? console.PreviousCommand() : console.NextCommand();
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, command.c_str());
        }
        return 0;
    }

    void ScriptConsole::Draw(const ScriptConsoleServices& services, const bool commandsEnabled)
    {
        ObserveGeneration(services);
        const auto errors = services.errors ? services.errors() : std::vector<scripting::ScriptError>{};

        // The tools row, the well and the input row stack without gaps: a spacing between them would push the
        // content past the window and bring an outer scrollbar
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        DrawToolbar(services, errors.size());
        if (showErrors_)
        {
            DrawErrors(errors);
        }
        else
        {
            DrawConsole(services, commandsEnabled);
        }
        ImGui::PopStyleVar();
    }

    // Tools row: Console | Errors (N) on the left, auto-scroll / clear / more on the right
    void ScriptConsole::DrawToolbar(const ScriptConsoleServices& services, const std::size_t errorCount)
    {
        constexpr char kMorePopup[] = "##console_more";

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(style::kPanel));
        ImGui::BeginChild("##console_tools", ImVec2(0.0f, style::kPanelToolsHeight), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        const std::string errorsLabel = "Errors (" + std::to_string(errorCount) + ")";
        const char* labels[2] = {"Console", errorsLabel.c_str()};
        const int selected = Segmented("##console_mode", labels, 2, showErrors_ ? 1 : 0);
        showErrors_ = selected == 1;

        if (showErrors_)
        {
            ImGui::SameLine();
            IconButton("##errors_info", ICON_INFO, nullptr, false, true, style::kTextDim, style::kFrameHeight, nullptr, IconSize::Row14);
            Tooltip("Last 32 errors", nullptr, "Clearing the list does not revive Faulted scripts; fix the code and reload.");
        }

        // Right group: measured so that it hugs the right edge
        const float buttonSize = style::kPanelIconButton;
        const float groupWidth = buttonSize * 3.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - groupWidth - ImGui::GetStyle().WindowPadding.x);
        if (IconButton("##autoscroll", ICON_ARROW_DOWN_TO_LINE, "Auto-scroll", autoScroll_, true, 0, buttonSize, nullptr, IconSize::Row14))
        {
            autoScroll_ = !autoScroll_;
        }
        ImGui::SameLine();
        if (IconButton("##clear", ICON_TRASH_2, showErrors_ ? "Clear errors" : "Clear output", false, true, 0, buttonSize, nullptr, IconSize::Row14))
        {
            if (showErrors_)
            {
                if (services.clearErrors)
                {
                    services.clearErrors();
                }
            }
            else
            {
                ClearOutput();
            }
        }
        ImGui::SameLine();
        const ImVec2 morePosition = ImGui::GetCursorScreenPos();
        if (IconButton("##more", ICON_ELLIPSIS_VERTICAL, "More", false, true, 0, buttonSize, nullptr, IconSize::Row14))
        {
            ImGui::OpenPopup(kMorePopup);
        }
        ImGui::SetNextWindowPos(ImVec2(morePosition.x + buttonSize, morePosition.y + buttonSize + 2.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        if (BeginMenuPopup(kMorePopup))
        {
            if (showErrors_)
            {
                if (MenuItemIcon(ICON_TRASH_2, "Clear Errors", nullptr, false, services.clearErrors != nullptr, false, 180.0f))
                {
                    services.clearErrors();
                }
            }
            else
            {
                if (MenuItemIcon(ICON_ROTATE_CCW, "Reset Python...", nullptr, false, true, false, 180.0f))
                {
                    Reset(services);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::EndChild();
    }

    void ScriptConsole::DrawConsole(const ScriptConsoleServices& services, const bool commandsEnabled)
    {
        const float inputRow = 34.0f;

        // Output well
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(style::kRecessed));
        const bool wellShown = ImGui::BeginChild("console_output", ImVec2(0.0f, -inputRow), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        if (wellShown)
        {
            PushFontRole(FontRole::Mono);
            for (const auto& line : output_)
            {
                ImU32 color = style::kText;
                switch (line.kind)
                {
                    case LineKind::Echo: color = style::kTextDim; break;
                    case LineKind::Notice: color = style::kTextDim; break;
                    case LineKind::Error: color = style::kErrorText; break;
                    case LineKind::Output: break;
                }
                if (line.error)
                {
                    color = style::kErrorText;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(line.text.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            PopFontRole();
            if (scrollToBottom_ && autoScroll_)
            {
                ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();
        scrollToBottom_ = false;

        // Input row: prompt chip, field, hint
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(rowMin, ImVec2(rowMin.x + rowWidth, rowMin.y + inputRow), style::kPanel);
        drawList->AddLine(rowMin, ImVec2(rowMin.x + rowWidth, rowMin.y), style::kInput, 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x + 8.0f, rowMin.y + 5.0f));

        ImGui::BeginDisabled(!commandsEnabled || !services.execute);
        PushFontRole(FontRole::Mono);
        const char* prompt = incomplete_ ? "..." : ">>>";
        const ImVec2 promptSize = ImGui::CalcTextSize(prompt);
        const float chipWidth = promptSize.x + 14.0f;
        const ImVec2 chipMin = ImGui::GetCursorScreenPos();
        drawList->AddRectFilled(chipMin, ImVec2(chipMin.x + chipWidth, chipMin.y + style::kFrameHeight), style::kControl, style::kRounding);
        drawList->AddText(ImVec2(chipMin.x + 7.0f, chipMin.y + (style::kFrameHeight - promptSize.y) * 0.5f), style::kTextDim, prompt);
        ImGui::Dummy(ImVec2(chipWidth, style::kFrameHeight));
        ImGui::SameLine(0.0f, 6.0f);

        const float infoWidth = style::kFrameHeight;
        ImGui::SetNextItemWidth(rowWidth - 8.0f - chipWidth - 6.0f - infoWidth - 12.0f);
        if (reclaimFocus_)
        {
            ImGui::SetKeyboardFocusHere();
            reclaimFocus_ = false;
        }
        const char* hint = commandsEnabled ? "Python (me = myengine) \xC2\xB7 Enter: run \xC2\xB7 Up/Down: history"
                                           : "Press Play to run Python commands";
        const auto flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory;
        const bool submit = ImGui::InputTextWithHint("##command", hint, &input_, flags, InputCallback, this);
        FocusOutline();
        PopFontRole();
        if (submit)
        {
            const auto source = input_; // Submit clears input_ only after this copy
            Submit(services, source);
        }
        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, 6.0f);
        IconButton("##console_info", ICON_INFO, nullptr, false, true, style::kTextDim, infoWidth, nullptr, IconSize::Row14);
        Tooltip("me = myengine",
                nullptr,
                "Enter: run. Up/Down: history. Blocks: enter indented lines, then a blank line. Stop and scene load reset the console variables.");
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMin.y + inputRow));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }

    void ScriptConsole::DrawErrors(const std::vector<scripting::ScriptError>& errors)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(style::kRecessed));
        const bool wellShown = ImGui::BeginChild("script_errors_list", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        if (!wellShown)
        {
            ImGui::EndChild();
            return;
        }

        if (errors.empty())
        {
            const ImVec2 region = ImGui::GetContentRegionAvail();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float centerX = origin.x + region.x * 0.5f;
            const float top = origin.y + std::max((region.y - 60.0f) * 0.5f, 8.0f);
            auto* drawList = ImGui::GetWindowDrawList();
            DrawIcon(drawList, IconSize::Empty30, ICON_CIRCLE_CHECK, ImVec2(centerX, top + 15.0f), IM_COL32(0x3D, 0x6B, 0x3F, 255));
            PushFontRole(FontRole::Secondary);
            const char* text = "No script errors.";
            const float width = ImGui::CalcTextSize(text).x;
            drawList->AddText(ImVec2(std::floor(centerX - width * 0.5f), top + 38.0f), style::kTextDim, text);
            PopFontRole();
            ImGui::EndChild();
            return;
        }

        ImGuiStorage* storage = ImGui::GetStateStorage();
        for (std::size_t index = errors.size(); index > 0; --index)
        {
            const auto& error = errors[index - 1];
            ImGui::PushID(static_cast<int>(index));
            const ImGuiID expandedId = ImGui::GetID("##expanded");
            bool expanded = storage->GetBool(expandedId, false);

            const auto firstLine = error.message.substr(0, error.message.find('\n'));
            const std::string location = error.file.empty() ? std::string("script") : error.file + ":" + std::to_string(error.line);
            const std::string timeText = "at " + std::to_string(static_cast<int>(error.time)) + " s";

            const float rowWidth = std::max(ImGui::GetContentRegionAvail().x, 40.0f);
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            const float rowHeight = 46.0f;
            const bool pressed = ImGui::InvisibleButton("##error_row", ImVec2(rowWidth, rowHeight));
            const bool hovered = ImGui::IsItemHovered();
            if (pressed)
            {
                expanded = !expanded;
                storage->SetBool(expandedId, expanded);
            }

            auto* drawList = ImGui::GetWindowDrawList();
            const ImVec2 rowMax(rowMin.x + rowWidth, rowMin.y + rowHeight);
            if (hovered)
            {
                drawList->AddRectFilled(rowMin, rowMax, style::kHeader, style::kRounding);
            }
            DrawIcon(drawList, IconSize::Row14, ICON_CIRCLE_X, ImVec2(rowMin.x + 14.0f, rowMin.y + 15.0f), style::kError);
            DrawIcon(drawList, IconSize::Chevron12, expanded ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, ImVec2(rowMax.x - 14.0f, rowMin.y + 15.0f), style::kTextDim);

            PushFontRole(FontRole::Mono);
            drawList->AddText(ImVec2(rowMin.x + 30.0f, rowMin.y + 6.0f), style::kTextDim, location.c_str());
            const float locationWidth = ImGui::CalcTextSize(location.c_str()).x;
            PopFontRole();
            PushFontRole(FontRole::Secondary);
            drawList->PushClipRect(ImVec2(rowMin.x + 30.0f + locationWidth + 10.0f, rowMin.y), ImVec2(rowMax.x - 28.0f, rowMax.y), true);
            drawList->AddText(ImVec2(rowMin.x + 30.0f + locationWidth + 10.0f, rowMin.y + 7.0f), style::kText, firstLine.c_str());
            drawList->PopClipRect();
            PopFontRole();
            PushFontRole(FontRole::Tiny);
            drawList->AddText(ImVec2(rowMin.x + 30.0f, rowMin.y + 27.0f), style::kTextDim, timeText.c_str());
            PopFontRole();

            if (expanded)
            {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(style::kInput));
                PushFontRole(FontRole::Mono);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::Indent(24.0f);
                ImGui::TextUnformatted(error.message.c_str());
                ImGui::Unindent(24.0f);
                ImGui::PopTextWrapPos();
                PopFontRole();
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0.0f, 4.0f));
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    bool ScriptConsole::IsIncomplete() const { return incomplete_; }
    std::size_t ScriptConsole::GetOutputCount() const { return output_.size(); }
    std::size_t ScriptConsole::GetHistoryCount() const { return history_.size(); }
}
