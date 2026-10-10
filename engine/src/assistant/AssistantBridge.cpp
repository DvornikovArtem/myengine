// AssistantBridge.cpp

#include <myengine/assistant/AssistantBridge.h>

#include <algorithm>
#include <cctype>
#include <random>
#include <utility>

#include <myengine/assistant/AssistantTools.h>
#include <myengine/assistant/ClaudeStreamParser.h>

namespace myengine::assistant
{
    namespace
    {
        using json = nlohmann::json;

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
            entry.view.detail = input.empty() ? std::string() : input.dump(2, ' ', false, json::error_handler_t::replace);
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

        if (allow)
        {
            if (!entry.mcpTool.empty())
            {
                ++tickets_[entry.mcpTool];
            }
            ReplyPermission(entry.request, true, entry.input, {});
            Notice("Applied: " + entry.view.title);
        }
        else
        {
            ReplyPermission(entry.request, false, entry.input,
                reason.empty() ? std::string("The user rejected this action. Do not repeat it; ask what they want instead.") : reason);
            Notice("Rejected: " + entry.view.title);
        }
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
}
