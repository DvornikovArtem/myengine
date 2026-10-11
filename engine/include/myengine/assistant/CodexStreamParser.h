// CodexStreamParser.h

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <myengine/assistant/IAssistantBackend.h>

namespace myengine::assistant
{
    // Turns the lines of `codex exec --json` (JSONL: thread.started, turn.started, item.started / item.completed,
    // turn.completed, turn.failed, error) into AssistantEvents. One instance per turn. Unknown or malformed lines and
    // unknown item types are ignored, so a newer CLI does not break the panel.
    //
    // Items: agent_message -> text, reasoning -> thinking, command_execution -> a Shell tool call,
    // mcp_tool_call -> a tool call named mcp__<server>__<tool> (the panel shows engine tools as engine.<tool>),
    // file_change -> an apply_patch tool call with the changed files, web_search -> a WebSearch tool call.
    class CodexStreamParser
    {
    public:
        // File paths in tool events are shown relative to this directory when they are inside it
        void SetRootDirectory(std::string root);
        // thread.started does not carry the model; the one that was asked for is reported as the model of the chat
        void SetModel(std::string model) { model_ = std::move(model); }

        void ParseLine(std::string_view line, std::vector<AssistantEvent>& events);

        // turn.completed or turn.failed has been seen: the turn ended on the CLI's side
        bool IsFinished() const { return finished_; }
        const std::string& GetThreadId() const { return threadId_; }

    private:
        std::string DisplayPath(std::string path) const;

        std::string root_;
        std::string model_;
        std::string threadId_;
        std::unordered_map<std::string, std::string> toolFiles_; // tool id -> files the tool changes, one per line
        std::unordered_map<std::string, bool> started_;          // item ids whose tool call was already reported
        bool finished_ = false;
        bool lastWasText_ = false;
    };
}
