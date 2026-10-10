// ScriptConsole.h

#pragma once

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include <myengine/scripting/ScriptSystem.h>

struct ImGuiInputTextCallbackData;

namespace myengine::ui
{
    struct ScriptConsoleServices
    {
        std::function<scripting::ScriptConsoleResult(const std::string&)> execute;
        std::function<void()> reset;
        std::function<std::vector<scripting::ScriptError>()> errors;
        std::function<void()> clearErrors;
        std::function<std::uint64_t()> generation;
    };

    // ImGui owns no Python objects. The command history and visible transcript are bounded.
    class ScriptConsole
    {
    public:
        bool Submit(const ScriptConsoleServices& services, const std::string& source);
        void Reset(const ScriptConsoleServices& services);
        void ClearOutput();
        std::string PreviousCommand();
        std::string NextCommand();
        void Draw(const ScriptConsoleServices& services, bool commandsEnabled); // inside an ImGui window
        bool IsIncomplete() const;
        std::size_t GetOutputCount() const;
        std::size_t GetHistoryCount() const;

    private:
        enum class LineKind
        {
            Output, // a result printed by the interpreter
            Echo,   // the command that was entered (">>> ..." / "... ...")
            Error,
            Notice, // a message of the console itself
        };

        struct Line
        {
            std::string text;
            bool error = false;
            LineKind kind = LineKind::Output;
        };

        void Append(std::string text, bool error = false, LineKind kind = LineKind::Output);
        void DrawToolbar(const ScriptConsoleServices& services, std::size_t errorCount);
        void DrawConsole(const ScriptConsoleServices& services, bool commandsEnabled);
        void DrawErrors(const std::vector<scripting::ScriptError>& errors);
        void ObserveGeneration(const ScriptConsoleServices& services);
        static int InputCallback(ImGuiInputTextCallbackData* data);

        std::deque<Line> output_;
        std::deque<std::string> history_;
        int historyPosition_ = -1;
        std::string input_;
        std::string historyDraft_;
        bool incomplete_ = false;
        bool scrollToBottom_ = false;
        bool autoScroll_ = true;
        bool showErrors_ = false; // the Errors view of the segmented switch
        bool reclaimFocus_ = false;
        std::uint64_t generation_ = 0;
        bool observedGeneration_ = false;
    };
}
