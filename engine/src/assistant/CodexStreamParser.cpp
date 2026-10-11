// CodexStreamParser.cpp

#include <myengine/assistant/CodexStreamParser.h>

#include <algorithm>
#include <cctype>
#include <utility>

#include <nlohmann/json.hpp>

#include <myengine/assistant/ClaudeStreamParser.h>

#include "StreamText.h"

namespace myengine::assistant
{
    namespace
    {
        using json = nlohmann::json;
        using detail::FirstLines;
        using detail::GetChild;
        using detail::GetString;
        using detail::GetUnsigned;
        using detail::Shorten;

        constexpr std::size_t kMaxSummaryBytes = 200;
        constexpr std::size_t kMaxOutputBytes = 400;

        // The text of an MCP result: the "text" blocks of result.content, one per line
        std::string FlattenMcpResult(const json& result)
        {
            std::string text;
            const auto* content = GetChild(result, "content");
            if (content != nullptr && content->is_array())
            {
                for (const auto& block : *content)
                {
                    if (block.is_string())
                    {
                        text += (text.empty() ? "" : "\n") + block.get<std::string>();
                    }
                    else if (GetString(block, "type") == "text")
                    {
                        text += (text.empty() ? "" : "\n") + GetString(block, "text");
                    }
                }
            }
            if (text.empty())
            {
                const auto* structured = GetChild(result, "structured_content");
                if (structured != nullptr && !structured->is_null())
                {
                    text = structured->dump(-1, ' ', false, json::error_handler_t::replace);
                }
            }
            return text;
        }

        bool IsFileTool(const std::string& tool)
        {
            return tool == "edit_file" || tool == "write_file";
        }

        // Codex starts its shell through `bash -lc "<command>"` / `powershell -Command "<command>"`: show the command
        std::string CommandText(std::string command)
        {
            for (const char* prefix : {"bash -lc ", "bash -c ", "sh -c ", "/bin/bash -lc "})
            {
                const std::string text = prefix;
                if (command.compare(0, text.size(), text) == 0)
                {
                    command.erase(0, text.size());
                    if (command.size() >= 2 && (command.front() == '"' || command.front() == '\'') && command.back() == command.front())
                    {
                        command = command.substr(1, command.size() - 2);
                    }
                    break;
                }
            }
            return command;
        }
    }

    void CodexStreamParser::SetRootDirectory(std::string root)
    {
        std::replace(root.begin(), root.end(), '\\', '/');
        while (!root.empty() && root.back() == '/')
        {
            root.pop_back();
        }
        root_ = std::move(root);
    }

    std::string CodexStreamParser::DisplayPath(std::string path) const
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        if (!root_.empty() && path.size() > root_.size() + 1 && path[root_.size()] == '/' &&
            std::equal(root_.begin(), root_.end(), path.begin(), [](const char a, const char b)
            {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            }))
        {
            return path.substr(root_.size() + 1);
        }
        return path;
    }

    void CodexStreamParser::ParseLine(const std::string_view line, std::vector<AssistantEvent>& events)
    {
        if (line.empty() || line.front() != '{')
        {
            return; // Codex prints plain text (warnings, banners) between the JSON lines
        }
        const json document = json::parse(line.begin(), line.end(), nullptr, false);
        if (document.is_discarded() || !document.is_object())
        {
            return;
        }
        const auto type = GetString(document, "type");

        if (type == "thread.started")
        {
            threadId_ = GetString(document, "thread_id");
            AssistantEvent event;
            event.type = AssistantEventType::SessionStarted;
            event.sessionId = threadId_;
            event.text = model_;
            // The documented events do not carry it; a version that does is shown as it is
            event.effort = GetString(document, "reasoning_effort");
            if (event.effort.empty())
            {
                event.effort = GetString(document, "effort");
            }
            events.push_back(std::move(event));
            return;
        }

        if (type == "turn.completed")
        {
            finished_ = true;
            AssistantEvent event;
            event.type = AssistantEventType::Finished;
            event.sessionId = threadId_;
            if (const auto* usage = GetChild(document, "usage"); usage != nullptr)
            {
                event.inputTokens = GetUnsigned(*usage, "input_tokens"); // includes the cached part
                event.outputTokens = GetUnsigned(*usage, "output_tokens") + GetUnsigned(*usage, "reasoning_output_tokens");
            }
            event.turns = 1;
            events.push_back(std::move(event));
            return;
        }

        if (type == "turn.failed")
        {
            finished_ = true;
            AssistantEvent event;
            event.type = AssistantEventType::Finished;
            event.sessionId = threadId_;
            event.isError = true;
            if (const auto* error = GetChild(document, "error"); error != nullptr)
            {
                event.text = GetString(*error, "message");
            }
            events.push_back(std::move(event));
            return;
        }

        if (type == "error")
        {
            // Often a warning (reconnecting, a retry); a failed turn also sends turn.failed after it
            AssistantEvent event;
            event.type = AssistantEventType::Notice;
            event.text = GetString(document, "message");
            if (!event.text.empty())
            {
                events.push_back(std::move(event));
            }
            return;
        }

        const bool isStarted = type == "item.started";
        const bool isCompleted = type == "item.completed";
        if (!isStarted && !isCompleted)
        {
            return; // turn.started, item.updated (todo lists) and whatever comes later
        }
        const auto* itemPointer = GetChild(document, "item");
        if (itemPointer == nullptr || !itemPointer->is_object())
        {
            return;
        }
        const json& item = *itemPointer;
        const auto itemType = GetString(item, "type");
        const auto id = GetString(item, "id");

        if (itemType == "agent_message")
        {
            if (isCompleted)
            {
                const auto text = GetString(item, "text");
                if (!text.empty())
                {
                    AssistantEvent event;
                    event.type = AssistantEventType::TextDelta;
                    // Two messages in a row would run together in one bubble
                    event.text = (lastWasText_ ? "\n\n" : "") + text;
                    lastWasText_ = true;
                    events.push_back(std::move(event));
                }
            }
            return;
        }
        lastWasText_ = false;

        if (itemType == "reasoning")
        {
            if (isCompleted)
            {
                AssistantEvent event;
                event.type = AssistantEventType::Thinking;
                event.text = GetString(item, "text");
                if (!event.text.empty())
                {
                    events.push_back(std::move(event));
                }
            }
            return;
        }

        if (itemType == "error")
        {
            if (isCompleted)
            {
                AssistantEvent event;
                event.type = AssistantEventType::Notice;
                event.text = GetString(item, "message");
                if (!event.text.empty())
                {
                    events.push_back(std::move(event));
                }
            }
            return;
        }

        // ---- tool calls ----
        const auto status = GetString(item, "status");
        AssistantEvent use;
        use.type = AssistantEventType::ToolUse;
        use.toolId = id;
        std::string files; // changed files, one per line, reported with the result
        bool failed = status == "failed" || status == "declined";
        std::string output;

        if (itemType == "command_execution")
        {
            use.toolName = "Shell";
            use.text = Shorten(CommandText(GetString(item, "command")), kMaxSummaryBytes);
            if (isCompleted)
            {
                const auto* exitCode = GetChild(item, "exit_code");
                if (exitCode != nullptr && exitCode->is_number_integer() && exitCode->get<int>() != 0)
                {
                    failed = true;
                }
                output = GetString(item, "aggregated_output");
            }
        }
        else if (itemType == "mcp_tool_call")
        {
            const auto server = GetString(item, "server");
            const auto tool = GetString(item, "tool");
            use.toolName = "mcp__" + (server.empty() ? std::string("mcp") : server) + "__" + tool;
            const auto* arguments = GetChild(item, "arguments");
            const json input = arguments != nullptr && arguments->is_object() ? *arguments : json::object();
            if (IsFileTool(tool))
            {
                const auto file = DisplayPath(GetString(input, "path"));
                use.text = file;
                use.filePath = file;
                json shown = {{"file_path", file}};
                if (tool == "edit_file")
                {
                    shown["old_string"] = GetString(input, "old_text");
                    shown["new_string"] = GetString(input, "new_text");
                    use.detail = ClaudeStreamParser::BuildToolDetail("Edit", shown);
                }
                else
                {
                    shown["content"] = GetString(input, "content");
                    use.detail = ClaudeStreamParser::BuildToolDetail("Write", shown);
                }
                files = file;
            }
            else if (input.is_object() && !input.empty())
            {
                use.text = Shorten(input.dump(-1, ' ', false, json::error_handler_t::replace), kMaxSummaryBytes);
            }
            if (isCompleted)
            {
                if (const auto* error = GetChild(item, "error"); error != nullptr && error->is_object())
                {
                    failed = true;
                    output = GetString(*error, "message");
                }
                else if (const auto* result = GetChild(item, "result"); result != nullptr && result->is_object())
                {
                    output = FlattenMcpResult(*result);
                }
            }
        }
        else if (itemType == "file_change")
        {
            use.toolName = "apply_patch";
            if (const auto* changes = GetChild(item, "changes"); changes != nullptr && changes->is_array())
            {
                for (const auto& change : *changes)
                {
                    const auto file = DisplayPath(GetString(change, "path"));
                    if (file.empty())
                    {
                        continue;
                    }
                    files += (files.empty() ? "" : "\n") + file;
                    use.text += (use.text.empty() ? "" : ", ") + file;
                }
            }
            use.text = Shorten(std::move(use.text), kMaxSummaryBytes);
        }
        else if (itemType == "web_search")
        {
            use.toolName = "WebSearch";
            use.text = Shorten(GetString(item, "query"), kMaxSummaryBytes);
        }
        else
        {
            return; // todo_list, collab_tool_call and whatever a newer CLI adds
        }

        if (!id.empty() && !started_[id])
        {
            started_[id] = true;
            if (!files.empty())
            {
                toolFiles_[id] = files;
                use.filePath = files.substr(0, files.find('\n'));
            }
            events.push_back(use);
        }
        if (!isCompleted)
        {
            return;
        }

        AssistantEvent result;
        result.type = AssistantEventType::ToolResult;
        result.toolId = id;
        result.isError = failed;
        result.text = FirstLines(output, 3, kMaxOutputBytes);
        const auto changed = toolFiles_.find(id);
        if (changed != toolFiles_.end())
        {
            if (!failed)
            {
                std::size_t position = 0;
                bool first = true;
                while (position <= changed->second.size())
                {
                    auto end = changed->second.find('\n', position);
                    if (end == std::string::npos)
                    {
                        end = changed->second.size();
                    }
                    AssistantEvent one = result;
                    one.filePath = changed->second.substr(position, end - position);
                    if (!first)
                    {
                        one.text.clear(); // the first result carries the output, the rest only the file
                    }
                    if (!one.filePath.empty())
                    {
                        events.push_back(std::move(one));
                        first = false;
                    }
                    position = end + 1;
                }
                if (first)
                {
                    events.push_back(std::move(result));
                }
            }
            else
            {
                events.push_back(std::move(result));
            }
            toolFiles_.erase(changed);
        }
        else
        {
            events.push_back(std::move(result));
        }
    }
}
