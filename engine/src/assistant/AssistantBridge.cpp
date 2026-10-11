// AssistantBridge.cpp

#include <myengine/assistant/AssistantBridge.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <random>
#include <utility>

#include <myengine/assistant/AssistantTools.h>
#include <myengine/assistant/ClaudeStreamParser.h>

namespace myengine::assistant
{
    namespace
    {
        using json = nlohmann::json;

        // The JSON shown on a confirmation card: objects and arrays of objects/arrays go one entry per line,
        // arrays of plain values stay on one line (`"position": [1, 0.5, 2]`)
        void AppendCardJson(const json& value, const int depth, std::string& out)
        {
            const auto scalar = [](const json& item) { return !item.is_array() && !item.is_object(); };
            const auto dumpScalar = [](const json& item) { return item.dump(-1, ' ', false, json::error_handler_t::replace); };
            const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
            const std::string inner(static_cast<std::size_t>(depth + 1) * 2, ' ');

            if (value.is_object())
            {
                if (value.empty())
                {
                    out += "{}";
                    return;
                }
                out += "{\n";
                std::size_t index = 0;
                for (const auto& [key, child] : value.items())
                {
                    out += inner + json(key).dump(-1, ' ', false, json::error_handler_t::replace) + ": ";
                    AppendCardJson(child, depth + 1, out);
                    out += ++index < value.size() ? ",\n" : "\n";
                }
                out += indent + "}";
            }
            else if (value.is_array())
            {
                if (value.empty())
                {
                    out += "[]";
                    return;
                }
                if (std::all_of(value.begin(), value.end(), scalar))
                {
                    out += "[";
                    for (std::size_t index = 0; index < value.size(); ++index)
                    {
                        out += (index == 0 ? "" : ", ") + dumpScalar(value[index]);
                    }
                    out += "]";
                    return;
                }
                out += "[\n";
                for (std::size_t index = 0; index < value.size(); ++index)
                {
                    out += inner;
                    AppendCardJson(value[index], depth + 1, out);
                    out += index + 1 < value.size() ? ",\n" : "\n";
                }
                out += indent + "]";
            }
            else
            {
                out += dumpScalar(value);
            }
        }

        std::string CardJson(const json& value)
        {
            std::string text;
            AppendCardJson(value, 0, text);
            return text;
        }

        constexpr std::size_t kMaxResultChars = 60 * 1024;
        const std::string kMcpPrefix = std::string("mcp__") + AssistantBridge::kServerName + "__";

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        std::string Slashes(std::string path)
        {
            std::replace(path.begin(), path.end(), '\\', '/');
            return path;
        }

        std::string RandomToken()
        {
            std::random_device device;
            std::string token;
            static const char kDigits[] = "0123456789abcdef";
            for (int index = 0; index < 32; ++index)
            {
                token += kDigits[device() % 16];
            }
            return token;
        }

        bool EndsWith(const std::string& text, const std::string& suffix)
        {
            return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
        }
    }

    AssistantBridge::AssistantBridge(AssistantTools& tools, AssistantBridgeConfig config)
        : tools_(tools), config_(std::move(config)), token_(RandomToken())
    {
        pipeName_ = config_.pipeName.empty() ? AssistantBridgeServer::DefaultPipeName() : config_.pipeName;
    }

    AssistantBridge::~AssistantBridge()
    {
        Stop();
    }

    bool AssistantBridge::Start(std::string& error)
    {
        return server_.Start(pipeName_, token_, error);
    }

    void AssistantBridge::Stop()
    {
        RejectAll("The editor closed the assistant bridge.");
        server_.Stop();
    }

    bool AssistantBridge::IsRunning() const
    {
        return server_.IsRunning();
    }

    std::string AssistantBridge::ApproveToolFullName()
    {
        return kMcpPrefix + kApproveTool;
    }

    std::vector<std::string> AssistantBridge::ReadOnlyToolNames() const
    {
        std::vector<std::string> names;
        for (const auto& info : tools_.List())
        {
            if (!info.mutatesWorld)
            {
                names.push_back(kMcpPrefix + info.name);
            }
        }
        return names;
    }

    void AssistantBridge::Notice(const std::string& text) const
    {
        if (onNotice)
        {
            onNotice(text);
        }
    }

    void AssistantBridge::RebuildView()
    {
        pendingView_.clear();
        for (const auto& entry : pending_)
        {
            pendingView_.push_back(entry.view);
        }
    }

    void AssistantBridge::Update()
    {
        if (!server_.IsRunning())
        {
            return;
        }
        std::vector<BridgeRequest> requests;
        server_.Poll(requests);
        for (const auto& request : requests)
        {
            Handle(request);
        }
        if (server_.TakeDisconnects() > 0)
        {
            // The CLI process (and its bridge) is gone: its prompts and its confirmations are void
            pending_.clear();
            RebuildView();
            tickets_.clear();
        }
    }

    void AssistantBridge::ReplyText(const BridgeRequest& request, const std::string& text, const bool isError)
    {
        std::string shown = text;
        if (shown.size() > kMaxResultChars)
        {
            shown.resize(kMaxResultChars);
            shown += "\n... (truncated)";
        }
        server_.Reply(request, {
            {"content", json::array({{{"type", "text"}, {"text", shown}}})},
            {"isError", isError},
        });
    }

    void AssistantBridge::ReplyPermission(const BridgeRequest& request, const bool allow, const json& input, const std::string& message)
    {
        json verdict = allow ? json{{"behavior", "allow"}, {"updatedInput", input}} : json{{"behavior", "deny"}, {"message", message}};
        ReplyText(request, verdict.dump(-1, ' ', false, json::error_handler_t::replace), false);
    }

    void AssistantBridge::Handle(const BridgeRequest& request)
    {
        if (request.op == "tools")
        {
            json list = json::array();
            for (const auto& info : tools_.List())
            {
                list.push_back({{"name", info.name}, {"description", info.description}, {"inputSchema", info.inputSchema}});
            }
            if (config_.offerApproveTool)
            {
                list.push_back({
                    {"name", kApproveTool},
                    {"description", "Internal: asks the user to confirm an action. The host calls it, do not call it yourself."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", {
                            {"tool_name", {{"type", "string"}}},
                            {"input", {{"type", "object"}}},
                            {"tool_use_id", {{"type", "string"}}},
                        }},
                        {"required", json::array({"tool_name", "input"})},
                    }},
                });
            }
            if (config_.fileTools)
            {
                list.push_back({
                    {"name", kEditFileTool},
                    {"description", "Replaces text in a project file. Only .py files in assets/scripts and .prefab.json files in "
                                    "assets/prefabs may be edited. The user confirms the change in the editor. old_text must be "
                                    "found exactly once unless replace_all is true."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", {
                            {"path", {{"type", "string"}, {"description", "File path relative to the project root, e.g. assets/scripts/coin.py"}}},
                            {"old_text", {{"type", "string"}, {"description", "Text to replace, exactly as it is in the file"}}},
                            {"new_text", {{"type", "string"}, {"description", "Replacement text"}}},
                            {"replace_all", {{"type", "boolean"}, {"description", "Replace every occurrence"}}},
                        }},
                        {"required", json::array({"path", "old_text", "new_text"})},
                    }},
                });
                list.push_back({
                    {"name", kWriteFileTool},
                    {"description", "Creates or overwrites a project file with the given content. Only .py files in assets/scripts "
                                    "and .prefab.json files in assets/prefabs may be written. The user confirms the change in the editor."},
                    {"inputSchema", {
                        {"type", "object"},
                        {"properties", {
                            {"path", {{"type", "string"}, {"description", "File path relative to the project root, e.g. assets/scripts/coin.py"}}},
                            {"content", {{"type", "string"}, {"description", "The whole new content of the file"}}},
                        }},
                        {"required", json::array({"path", "content"})},
                    }},
                });
            }
            server_.Reply(request, {{"tools", std::move(list)}});
            return;
        }
        if (request.op == "call")
        {
            HandleCall(request);
            return;
        }
        server_.Reply(request, {{"error", "Unknown operation: " + request.op}});
    }

    void AssistantBridge::HandleCall(const BridgeRequest& request)
    {
        if (request.tool == kApproveTool)
        {
            HandleApprove(request);
            return;
        }
        if (config_.fileTools && (request.tool == kEditFileTool || request.tool == kWriteFileTool))
        {
            HandleFileTool(request);
            return;
        }
        const auto* info = tools_.Find(request.tool);
        if (info == nullptr)
        {
            ReplyText(request, "Unknown tool: " + request.tool, true);
            return;
        }
        if (info->mutatesWorld)
        {
            auto ticket = tickets_.find(request.tool);
            if (ticket == tickets_.end() || ticket->second <= 0)
            {
                if (config_.confirmInCall)
                {
                    // No separate permission prompt: this very call waits for the card
                    Pending entry;
                    entry.request = request;
                    entry.input = request.arguments;
                    entry.mcpTool = request.tool;
                    entry.reply = Pending::Reply::Call;
                    entry.view.id = nextApproval_++;
                    entry.view.tool = kMcpPrefix + request.tool;
                    entry.view.title = tools_.Describe(request.tool, request.arguments);
                    entry.view.detail = request.arguments.empty() ? std::string() : CardJson(request.arguments);
                    entry.view.client = request.client;
                    pending_.push_back(std::move(entry));
                    RebuildView();
                    return;
                }
                ReplyText(request, "This change was not confirmed in the editor, so it was not made.", true);
                return;
            }
            if (--ticket->second == 0)
            {
                tickets_.erase(ticket);
            }
        }
        const auto result = tools_.Call(request.tool, request.arguments);
        if (result.ok)
        {
            ReplyText(request, result.value.dump(-1, ' ', false, json::error_handler_t::replace), false);
        }
        else
        {
            ReplyText(request, result.message, true);
        }
    }

    bool AssistantBridge::IsInsideRepository(const std::string& path) const
    {
        std::filesystem::path file = std::filesystem::u8path(path);
        if (file.is_relative())
        {
            file = config_.repositoryRoot / file;
        }
        const auto normalized = Lower(Slashes(file.lexically_normal().u8string()));
        auto root = Lower(Slashes(config_.repositoryRoot.lexically_normal().u8string()));
        while (!root.empty() && root.back() == '/')
        {
            root.pop_back();
        }
        return normalized.size() > root.size() + 1 && normalized.compare(0, root.size(), root) == 0 && normalized[root.size()] == '/' &&
            normalized.find("/../") == std::string::npos;
    }

    bool AssistantBridge::IsEditable(const std::string& path, std::string& displayPath) const
    {
        if (!IsInsideRepository(path))
        {
            return false;
        }
        std::filesystem::path file = std::filesystem::u8path(path);
        if (file.is_relative())
        {
            file = config_.repositoryRoot / file;
        }
        const auto normalized = Slashes(file.lexically_normal().u8string());
        const auto lowered = Lower(normalized);
        auto root = Lower(Slashes(config_.repositoryRoot.lexically_normal().u8string()));
        while (!root.empty() && root.back() == '/')
        {
            root.pop_back();
        }
        const auto relative = lowered.substr(root.size() + 1);
        displayPath = normalized.substr(root.size() + 1);
        for (const auto& area : config_.editable)
        {
            const auto prefix = Lower(area.directory) + "/";
            // Directly inside the folder: no sub-folders, no surprises
            if (relative.compare(0, prefix.size(), prefix) == 0 && relative.find('/', prefix.size()) == std::string::npos &&
                EndsWith(relative, Lower(area.suffix)) && relative.size() > prefix.size() + area.suffix.size())
            {
                return true;
            }
        }
        return false;
    }

    void AssistantBridge::HandleFileTool(const BridgeRequest& request)
    {
        const auto& arguments = request.arguments;
        const auto text = [&](const char* key) -> std::string
        {
            return arguments.contains(key) && arguments[key].is_string() ? arguments[key].get<std::string>() : std::string();
        };
        const auto path = text("path");
        std::string displayPath;
        if (path.empty() || !IsEditable(path, displayPath))
        {
            ReplyText(request,
                "Only .py files in assets/scripts and .prefab.json files in assets/prefabs may be changed. "
                "Scenes are changed with the engine tools or in the Inspector, not as files.", true);
            return;
        }
        auto file = std::filesystem::u8path(path);
        if (file.is_relative())
        {
            file = config_.repositoryRoot / file;
        }
        file = file.lexically_normal();

        constexpr std::size_t kMaxFileBytes = 256 * 1024;
        std::string before;
        bool exists = false;
        {
            std::ifstream stream(file, std::ios::binary);
            if (stream)
            {
                exists = true;
                before.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            }
        }

        json shown;
        std::string content;
        std::string toolName;
        if (request.tool == kEditFileTool)
        {
            const auto oldText = text("old_text");
            const auto newText = text("new_text");
            const bool all = arguments.contains("replace_all") && arguments["replace_all"].is_boolean() && arguments["replace_all"].get<bool>();
            if (!exists)
            {
                ReplyText(request, "The file does not exist: " + displayPath + ". Use write_file to create it.", true);
                return;
            }
            if (oldText.empty())
            {
                ReplyText(request, "old_text is empty.", true);
                return;
            }
            std::size_t count = 0;
            for (auto position = before.find(oldText); position != std::string::npos; position = before.find(oldText, position + oldText.size()))
            {
                ++count;
            }
            if (count == 0)
            {
                ReplyText(request, "old_text was not found in " + displayPath + ". Read the file and copy the text exactly.", true);
                return;
            }
            if (count > 1 && !all)
            {
                ReplyText(request, "old_text was found " + std::to_string(count) + " times in " + displayPath +
                    ". Add more context to make it unique or set replace_all.", true);
                return;
            }
            content = before;
            for (auto position = content.find(oldText); position != std::string::npos;)
            {
                content.replace(position, oldText.size(), newText);
                position = all ? content.find(oldText, position + newText.size()) : std::string::npos;
            }
            shown = {{"file_path", displayPath}, {"old_string", oldText}, {"new_string", newText}};
            toolName = "Edit";
        }
        else
        {
            content = text("content");
            shown = {{"file_path", displayPath}, {"content", content}};
            toolName = "Write";
        }
        if (content.size() > kMaxFileBytes)
        {
            ReplyText(request, "The file would be larger than 256 KB.", true);
            return;
        }

        Pending entry;
        entry.request = request;
        entry.input = arguments;
        entry.reply = Pending::Reply::FileWrite;
        entry.file = file;
        entry.content = std::move(content);
        entry.displayPath = displayPath;
        entry.view.id = nextApproval_++;
        entry.view.tool = toolName;
        entry.view.title = (toolName == "Write" ? (exists ? "Overwrite " : "Create ") : "Edit ") + displayPath;
        entry.view.detail = ClaudeStreamParser::BuildToolDetail(toolName, shown);
        entry.view.client = request.client;
        pending_.push_back(std::move(entry));
        RebuildView();
    }

    void AssistantBridge::HandleApprove(const BridgeRequest& request)
    {
        const auto& arguments = request.arguments;
        const std::string toolName = arguments.contains("tool_name") && arguments["tool_name"].is_string() ? arguments["tool_name"].get<std::string>() : std::string();
        const json input = arguments.contains("input") && arguments["input"].is_object() ? arguments["input"] : json::object();
        const auto text = [&](const char* key) -> std::string
        {
            return input.contains(key) && input[key].is_string() ? input[key].get<std::string>() : std::string();
        };

        // Our own tools
        if (toolName.compare(0, kMcpPrefix.size(), kMcpPrefix) == 0)
        {
            const auto name = toolName.substr(kMcpPrefix.size());
            const auto* info = tools_.Find(name);
            if (info == nullptr)
            {
                ReplyPermission(request, false, input, "Unknown tool.");
                return;
            }
            if (!info->mutatesWorld)
            {
                ReplyPermission(request, true, input, {});
                return;
            }
            Pending entry;
            entry.request = request;
            entry.input = input;
            entry.mcpTool = name;
            entry.view.id = nextApproval_++;
            entry.view.tool = toolName;
            entry.view.title = tools_.Describe(name, input);
            entry.view.detail = input.empty() ? std::string() : CardJson(input);
            pending_.push_back(std::move(entry));
            RebuildView();
            return;
        }

        // Files
        if (toolName == "Edit" || toolName == "Write" || toolName == "MultiEdit" || toolName == "NotebookEdit")
        {
            const auto file = text("file_path").empty() ? text("notebook_path") : text("file_path");
            std::string displayPath;
            if (file.empty() || !IsEditable(file, displayPath))
            {
                ReplyPermission(request, false, input,
                    "Only .py files in assets/scripts and .prefab.json files in assets/prefabs may be edited. "
                    "Scenes are changed with the engine tools or in the Inspector, not as files.");
                return;
            }
            Pending entry;
            entry.request = request;
            entry.input = input;
            entry.view.id = nextApproval_++;
            entry.view.tool = toolName;
            entry.view.title = (toolName == "Write" ? "Write " : "Edit ") + displayPath;
            entry.view.detail = ClaudeStreamParser::BuildToolDetail(toolName, input);
            pending_.push_back(std::move(entry));
            RebuildView();
            return;
        }

        // Reading is free inside the repository
        if (toolName == "Read" || toolName == "Glob" || toolName == "Grep")
        {
            const auto path = text("file_path").empty() ? text("path") : text("file_path");
            if (path.empty() || IsInsideRepository(path))
            {
                ReplyPermission(request, true, input, {});
            }
            else
            {
                ReplyPermission(request, false, input, "Files outside the repository cannot be read.");
            }
            return;
        }

        ReplyPermission(request, false, input, "The tool " + toolName + " is not available to the assistant.");
    }

    void AssistantBridge::Resolve(const std::uint64_t id, const bool allow, const std::string& reason)
    {
        const auto it = std::find_if(pending_.begin(), pending_.end(), [id](const Pending& entry) { return entry.view.id == id; });
        if (it == pending_.end())
        {
            return;
        }
        Pending entry = std::move(*it);
        pending_.erase(it);
        RebuildView();
        FinishPending(entry, allow, reason);
    }

    void AssistantBridge::FinishPending(Pending& entry, const bool allow, const std::string& reason)
    {
        const std::string rejection = reason.empty() ? std::string(kUserRejectedText) : reason;
        switch (entry.reply)
        {
            case Pending::Reply::Permission:
                if (allow)
                {
                    if (!entry.mcpTool.empty())
                    {
                        ++tickets_[entry.mcpTool];
                    }
                    ReplyPermission(entry.request, true, entry.input, {});
                }
                else
                {
                    ReplyPermission(entry.request, false, entry.input, rejection);
                }
                break;

            case Pending::Reply::Call:
                if (allow)
                {
                    const auto result = tools_.Call(entry.mcpTool, entry.input);
                    ReplyText(entry.request, result.ok ? result.value.dump(-1, ' ', false, json::error_handler_t::replace) : result.message,
                              !result.ok);
                }
                else
                {
                    ReplyText(entry.request, rejection, true);
                }
                break;

            case Pending::Reply::FileWrite:
                if (!allow)
                {
                    ReplyText(entry.request, rejection, true);
                    break;
                }
                else
                {
                    std::error_code error;
                    std::filesystem::create_directories(entry.file.parent_path(), error);
                    std::ofstream stream(entry.file, std::ios::binary | std::ios::trunc);
                    stream.write(entry.content.data(), static_cast<std::streamsize>(entry.content.size()));
                    stream.close();
                    if (!stream)
                    {
                        ReplyText(entry.request, "Could not write " + entry.displayPath + ".", true);
                        Notice("Failed: " + entry.view.title);
                        return;
                    }
                    ReplyText(entry.request, "Done: " + entry.displayPath + " was written.", false);
                }
                break;
        }
        Notice((allow ? "Applied: " : "Rejected: ") + entry.view.title);
    }

    void AssistantBridge::RejectAll(const std::string& reason)
    {
        const auto ids = [this]
        {
            std::vector<std::uint64_t> list;
            for (const auto& entry : pending_)
            {
                list.push_back(entry.view.id);
            }
            return list;
        }();
        for (const auto id : ids)
        {
            Resolve(id, false, reason);
        }
        tickets_.clear();
    }

    void AssistantBridge::RejectTurnPrompts(const std::string& reason)
    {
        std::vector<std::uint64_t> ids;
        for (const auto& entry : pending_)
        {
            if (entry.reply == Pending::Reply::Permission)
            {
                ids.push_back(entry.view.id);
            }
        }
        for (const auto id : ids)
        {
            Resolve(id, false, reason);
        }
        tickets_.clear();
    }
}
