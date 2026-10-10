// ClaudeStreamParser.cpp

#include <myengine/assistant/ClaudeStreamParser.h>

#include <algorithm>
#include <cctype>
#include <utility>

#include <nlohmann/json.hpp>

namespace myengine::assistant
{
    namespace
    {
        using json = nlohmann::json;

        constexpr std::size_t kMaxSummaryBytes = 200;
        constexpr std::size_t kMaxOutputBytes = 400;
        constexpr std::size_t kMaxDiffLines = 40;

        std::string GetString(const json& object, const char* key)
        {
            if (!object.is_object())
            {
                return {};
            }
            const auto it = object.find(key);
            return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
        }

        std::uint64_t GetUnsigned(const json& object, const char* key)
        {
            if (!object.is_object())
            {
                return 0;
            }
            const auto it = object.find(key);
            if (it == object.end() || !it->is_number())
            {
                return 0;
            }
            const double value = it->get<double>();
            return value > 0.0 ? static_cast<std::uint64_t>(value) : 0;
        }

        const json* GetChild(const json& object, const char* key)
        {
            if (!object.is_object())
            {
                return nullptr;
            }
            const auto it = object.find(key);
            return it != object.end() ? &*it : nullptr;
        }

        std::string Shorten(std::string text, const std::size_t limit)
        {
            // Never cut inside a UTF-8 sequence
            if (text.size() > limit)
            {
                std::size_t cut = limit;
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
                {
                    --cut;
                }
                text.resize(cut);
                text += "...";
            }
            return text;
        }

        std::string FirstLines(const std::string& text, const std::size_t lines, const std::size_t limit)
        {
            std::size_t end = 0;
            for (std::size_t count = 0; count < lines && end != std::string::npos; ++count)
            {
                end = text.find('\n', end == 0 ? 0 : end + 1);
            }
            const bool omitted = end != std::string::npos && text.find_first_not_of(" \t\r\n", end) != std::string::npos;
            std::string result = end == std::string::npos ? text : text.substr(0, end);
            while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == ' '))
            {
                result.pop_back();
            }
            if (omitted)
            {
                result += " ...";
            }
            return Shorten(std::move(result), limit);
        }

        void AppendDiffLines(std::string& out, const std::string& text, const char* prefix)
        {
            std::size_t position = 0;
            std::size_t count = 0;
            while (position <= text.size())
            {
                if (count == kMaxDiffLines)
                {
                    out += prefix;
                    out += "...\n";
                    return;
                }
                auto end = text.find('\n', position);
                const bool last = end == std::string::npos;
                if (last)
                {
                    end = text.size();
                }
                auto line = text.substr(position, end - position);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                if (!(last && line.empty()))
                {
                    out += prefix;
                    out += Shorten(std::move(line), 240);
                    out += '\n';
                    ++count;
                }
                if (last)
                {
                    break;
                }
                position = end + 1;
            }
        }

        std::string DescribeInput(const std::string& name, const json& input, const std::string& displayPath)
        {
            if (!displayPath.empty())
            {
                return displayPath;
            }
            if (name == "Glob" || name == "Grep")
            {
                auto text = GetString(input, "pattern");
                const auto where = GetString(input, "path");
                if (!where.empty())
                {
                    text += "  in " + where;
                }
                return Shorten(std::move(text), kMaxSummaryBytes);
            }
            if (name == "Bash" || name == "PowerShell")
            {
                return Shorten(GetString(input, "command"), kMaxSummaryBytes);
            }
            if (name == "WebFetch")
            {
                return Shorten(GetString(input, "url"), kMaxSummaryBytes);
            }
            if (input.is_object() && !input.empty())
            {
                return Shorten(input.dump(-1, ' ', false, json::error_handler_t::replace), kMaxSummaryBytes);
            }
            return {};
        }

        bool IsFileEditTool(const std::string& name)
        {
            return name == "Edit" || name == "Write" || name == "MultiEdit" || name == "NotebookEdit";
        }

        std::string FlattenToolResult(const json& content)
        {
            if (content.is_string())
            {
                return content.get<std::string>();
            }
            std::string text;
            if (content.is_array())
            {
                for (const auto& block : content)
                {
                    if (GetString(block, "type") == "text")
                    {
                        if (!text.empty())
                        {
                            text += '\n';
                        }
                        text += GetString(block, "text");
                    }
                }
            }
            return text;
        }
    }

    std::string ClaudeStreamParser::BuildToolDetail(const std::string& toolName, const nlohmann::json& input)
    {
        std::string detail;
        if (toolName == "Edit")
        {
            AppendDiffLines(detail, GetString(input, "old_string"), "- ");
            AppendDiffLines(detail, GetString(input, "new_string"), "+ ");
        }
        else if (toolName == "MultiEdit")
        {
            const auto* edits = GetChild(input, "edits");
            if (edits != nullptr && edits->is_array())
            {
                for (const auto& edit : *edits)
                {
                    AppendDiffLines(detail, GetString(edit, "old_string"), "- ");
                    AppendDiffLines(detail, GetString(edit, "new_string"), "+ ");
                    detail += "@@\n";
                }
            }
        }
        else if (toolName == "Write")
        {
            AppendDiffLines(detail, GetString(input, "content"), "+ ");
        }
        return detail;
    }

    void ClaudeStreamParser::SetRootDirectory(std::string root)
    {
        std::replace(root.begin(), root.end(), '\\', '/');
        while (!root.empty() && root.back() == '/')
        {
            root.pop_back();
        }
        root_ = std::move(root);
    }

    std::string ClaudeStreamParser::DisplayPath(std::string path) const
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

    void ClaudeStreamParser::ParseLine(const std::string_view line, std::vector<AssistantEvent>& events)
    {
        if (line.empty() || line.front() != '{')
        {
            return;
        }
        const json document = json::parse(line.begin(), line.end(), nullptr, false);
        if (document.is_discarded() || !document.is_object())
        {
            return;
        }
        const auto type = GetString(document, "type");

        if (type == "system")
        {
            const auto subtype = GetString(document, "subtype");
            if (subtype == "init")
            {
                AssistantEvent event;
                event.type = AssistantEventType::SessionStarted;
                event.sessionId = GetString(document, "session_id");
                event.text = GetString(document, "model");
                events.push_back(std::move(event));
            }
            else if (subtype == "api_retry")
            {
                AssistantEvent event;
                event.type = AssistantEventType::Notice;
                event.text = "API retry " + std::to_string(GetUnsigned(document, "attempt")) + "/" +
                    std::to_string(GetUnsigned(document, "max_retries")) + ": " + GetString(document, "error");
                events.push_back(std::move(event));
            }
            return;
        }

        if (type == "stream_event")
        {
            const auto* parent = GetChild(document, "parent_tool_use_id");
            if (parent != nullptr && parent->is_string())
            {
                return; // a subagent's text is not shown
            }
            const auto* streamEvent = GetChild(document, "event");
            if (streamEvent == nullptr)
            {
                return;
            }
            const auto streamType = GetString(*streamEvent, "type");
            if (streamType == "message_start")
            {
                const auto* message = GetChild(*streamEvent, "message");
                streamingMessageId_ = message != nullptr ? GetString(*message, "id") : std::string();
            }
            else if (streamType == "content_block_delta")
            {
                const auto* delta = GetChild(*streamEvent, "delta");
                if (delta != nullptr && GetString(*delta, "type") == "text_delta")
                {
                    AssistantEvent event;
                    event.type = AssistantEventType::TextDelta;
                    event.text = GetString(*delta, "text");
                    if (!event.text.empty())
                    {
                        events.push_back(std::move(event));
                    }
                }
            }
            return;
        }

        if (type == "assistant")
        {
            const auto* parent = GetChild(document, "parent_tool_use_id");
            if (parent != nullptr && parent->is_string())
            {
                return;
            }
            const auto* message = GetChild(document, "message");
            if (message == nullptr)
            {
                return;
            }
            const auto messageId = GetString(*message, "id");
            const bool textAlreadyStreamed = !messageId.empty() && messageId == streamingMessageId_;
            const auto* content = GetChild(*message, "content");
            if (content == nullptr || !content->is_array())
            {
                return;
            }
            for (const auto& block : *content)
            {
                const auto blockType = GetString(block, "type");
                if (blockType == "text")
                {
                    if (!textAlreadyStreamed)
                    {
                        AssistantEvent event;
                        event.type = AssistantEventType::TextDelta;
                        event.text = GetString(block, "text");
                        if (!event.text.empty())
                        {
                            events.push_back(std::move(event));
                        }
                    }
                }
                else if (blockType == "tool_use")
                {
                    AssistantEvent event;
                    event.type = AssistantEventType::ToolUse;
                    event.toolId = GetString(block, "id");
                    event.toolName = GetString(block, "name");
                    const auto* inputPointer = GetChild(block, "input");
                    const json input = inputPointer != nullptr ? *inputPointer : json::object();

                    std::string file = GetString(input, "file_path");
                    if (file.empty())
                    {
                        file = GetString(input, "notebook_path");
                    }
                    if (file.empty() && event.toolName == "Read")
                    {
                        file = GetString(input, "path");
                    }
                    const auto displayPath = file.empty() ? std::string() : DisplayPath(file);
                    event.text = DescribeInput(event.toolName, input, displayPath);

                    if (IsFileEditTool(event.toolName) && !file.empty())
                    {
                        event.filePath = displayPath;
                        toolFiles_[event.toolId] = displayPath;
                    }
                    event.detail = BuildToolDetail(event.toolName, input);
                    // `filePath` on ToolUse only tells the panel which file the call targets; the change is reported by ToolResult
                    events.push_back(std::move(event));
                }
            }
            return;
        }

        if (type == "user")
        {
            const auto* parent = GetChild(document, "parent_tool_use_id");
            if (parent != nullptr && parent->is_string())
            {
                return;
            }
            const auto* message = GetChild(document, "message");
            const auto* content = message != nullptr ? GetChild(*message, "content") : nullptr;
            if (content == nullptr || !content->is_array())
            {
                return;
            }
            for (const auto& block : *content)
            {
                if (GetString(block, "type") != "tool_result")
                {
                    continue;
                }
                AssistantEvent event;
                event.type = AssistantEventType::ToolResult;
                event.toolId = GetString(block, "tool_use_id");
                const auto* isError = GetChild(block, "is_error");
                event.isError = isError != nullptr && isError->is_boolean() && isError->get<bool>();
                const auto* result = GetChild(block, "content");
                if (result != nullptr)
                {
                    event.text = FirstLines(FlattenToolResult(*result), 3, kMaxOutputBytes);
                }
                const auto file = toolFiles_.find(event.toolId);
                if (file != toolFiles_.end())
                {
                    if (!event.isError)
                    {
                        event.filePath = file->second;
                    }
                    toolFiles_.erase(file);
                }
                events.push_back(std::move(event));
            }
            return;
        }

        if (type == "result")
        {
            finished_ = true;
            AssistantEvent event;
            event.type = AssistantEventType::Finished;
            event.sessionId = GetString(document, "session_id");
            const auto* isError = GetChild(document, "is_error");
            const auto subtype = GetString(document, "subtype");
            event.isError = (isError != nullptr && isError->is_boolean() && isError->get<bool>()) ||
                (!subtype.empty() && subtype != "success");
            const auto result = GetString(document, "result");
            if (event.isError)
            {
                event.text = result.empty() ? subtype : result;
                if (!subtype.empty() && subtype != "success" && event.text != subtype)
                {
                    event.text += " (" + subtype + ")";
                }
            }
            else
            {
                event.text = result;
            }
            const auto* cost = GetChild(document, "total_cost_usd");
            if (cost != nullptr && cost->is_number())
            {
                event.costUsd = cost->get<double>();
            }
            event.durationMs = GetUnsigned(document, "duration_ms");
            event.turns = static_cast<std::uint32_t>(GetUnsigned(document, "num_turns"));
            const auto* usage = GetChild(document, "usage");
            if (usage != nullptr)
            {
                event.inputTokens = GetUnsigned(*usage, "input_tokens") + GetUnsigned(*usage, "cache_creation_input_tokens") +
                    GetUnsigned(*usage, "cache_read_input_tokens");
                event.outputTokens = GetUnsigned(*usage, "output_tokens");
            }
            events.push_back(std::move(event));
        }
    }
}
