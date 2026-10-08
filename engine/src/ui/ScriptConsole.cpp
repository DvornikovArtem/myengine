// ScriptConsole.cpp

#include <myengine/ui/ScriptConsole.h>

#include <utility>

#include <imgui/imgui.h>
#include <imgui/misc/imgui_stdlib.h>

namespace myengine::ui
{
    namespace
    {
        constexpr std::size_t kMaxOutputLines = 256;
        constexpr std::size_t kMaxHistory = 64;
        constexpr std::size_t kMaxLineBytes = 64 * 1024;
    }

    void ScriptConsole::Append(std::string text, const bool error)
    {
        if (text.size() > kMaxLineBytes)
        {
            text.resize(kMaxLineBytes);
            text += "\n... truncated ...";
        }
        output_.push_back({std::move(text), error});
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
        Append(std::string(incomplete_ ? "... " : ">>> ") + source);
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
        Append("Python console variables and pending input reset. Game objects were not reset.");
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
            Append("Console variables reset on Stop / scene load. History and output were kept.");
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
        if (!ImGui::BeginTabBar("script_console_tabs"))
        {
            return;
        }
        if (ImGui::BeginTabItem("Console"))
        {
            if (ImGui::SmallButton("Clear output"))
            {
                ClearOutput();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset Python"))
            {
                Reset(services);
            }
            ImGui::SameLine();
            ImGui::Checkbox("Auto-scroll", &autoScroll_);
            ImGui::TextWrapped("me = myengine | Enter: run | Up/Down: history | Blocks: enter indented lines, then a blank line");
            ImGui::TextWrapped("%s", commandsEnabled ? "Commands run in Play. Stop / scene load reset console variables." : "Press Play to execute commands. Errors remain visible in Edit.");
            const float footer = ImGui::GetFrameHeightWithSpacing();
            if (ImGui::BeginChild("console_output", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders))
            {
                for (const auto& line : output_)
                {
                    if (line.error)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.35f, 1.0f));
                    }
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(line.text.c_str());
                    ImGui::PopTextWrapPos();
                    if (line.error)
                    {
                        ImGui::PopStyleColor();
                    }
                }
                if (scrollToBottom_ && autoScroll_)
                {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
            ImGui::EndChild();
            scrollToBottom_ = false;

            ImGui::BeginDisabled(!commandsEnabled || !services.execute);
            ImGui::TextUnformatted(incomplete_ ? "..." : ">>>");
            ImGui::SameLine();
            const float buttonWidth = ImGui::CalcTextSize("Run").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x);
            if (reclaimFocus_)
            {
                ImGui::SetKeyboardFocusHere();
                reclaimFocus_ = false;
            }
            const auto flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory;
            bool submit = ImGui::InputText("##command", &input_, flags, InputCallback, this);
            ImGui::SameLine();
            submit = ImGui::Button("Run") || submit;
            if (submit)
            {
                const auto source = input_; // Submit clears input_ only after this copy
                Submit(services, source);
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        const auto errors = services.errors ? services.errors() : std::vector<scripting::ScriptError>{};
        const std::string errorTab = "Errors (" + std::to_string(errors.size()) + ")###script_errors";
        if (ImGui::BeginTabItem(errorTab.c_str()))
        {
            if (ImGui::SmallButton("Clear errors") && services.clearErrors)
            {
                services.clearErrors();
            }
            ImGui::TextWrapped("Last 32 errors. Clearing the list does not revive Faulted scripts; fix the code and reload.");
            if (errors.empty())
            {
                ImGui::TextDisabled("No script errors.");
            }
            if (ImGui::BeginChild("script_errors_list"))
            {
                for (std::size_t index = errors.size(); index > 0; --index)
                {
                    const auto& error = errors[index - 1];
                    ImGui::PushID(static_cast<int>(index));
                    const auto firstLine = error.message.substr(0, error.message.find('\n'));
                    const std::string label = "[" + std::to_string(static_cast<int>(error.time)) + " s] " + firstLine;
                    if (ImGui::CollapsingHeader(label.c_str()))
                    {
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextUnformatted(error.message.c_str());
                        ImGui::PopTextWrapPos();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    bool ScriptConsole::IsIncomplete() const { return incomplete_; }
    std::size_t ScriptConsole::GetOutputCount() const { return output_.size(); }
    std::size_t ScriptConsole::GetHistoryCount() const { return history_.size(); }
}
