// ClaudeStreamParser.h

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include <myengine/assistant/IAssistantBackend.h>

namespace myengine::assistant
{
    // Turns the lines of `claude -p --output-format stream-json --verbose --include-partial-messages`
    // into AssistantEvents. One instance per turn. Unknown or malformed lines are ignored.
    class ClaudeStreamParser
    {
    public:
        // File paths in tool events are shown relative to this directory when they are inside it
        void SetRootDirectory(std::string root);

        void ParseLine(std::string_view line, std::vector<AssistantEvent>& events);

        // Diff-like preview of an Edit / MultiEdit / Write call ("- old" / "+ new" lines); empty for other tools
        static std::string BuildToolDetail(const std::string& toolName, const nlohmann::json& input);

        // The `result` line has been seen: the turn ended on the CLI's side
        bool IsFinished() const { return finished_; }

    private:
        std::string DisplayPath(std::string path) const;

        std::string root_;
        std::string streamingMessageId_; // message whose text already came as deltas
        std::unordered_map<std::string, std::string> toolFiles_; // tool_use id -> file the tool edits
        bool finished_ = false;
        bool thinkingStreamed_ = false;
    };
}
