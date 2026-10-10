// McpBridgeCore.h
//
// Header-only: used by the myengine_mcp bridge executable and by the tests. No Windows or engine dependencies.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace myengine::assistant
{
    // MCP over stdio (newline-delimited JSON-RPC 2.0) in front of the editor.
    // The editor is reached through `upstream`, which takes {"op":"tools"} or {"op":"call","tool":..,"arguments":..}
    // and returns the tool list / an MCP call result, or {"error": "..."} when the editor is not reachable.
    class McpBridgeCore
    {
    public:
        using Json = nlohmann::json;
        using Upstream = std::function<Json(const Json& request)>;

        explicit McpBridgeCore(Upstream upstream) : upstream_(std::move(upstream)) {}

        // The response to write for a message, or nothing for a notification
        std::optional<Json> Handle(const Json& message) const
        {
            if (!message.is_object())
            {
                return std::nullopt;
            }
            const auto methodIt = message.find("method");
            const auto idIt = message.find("id");
            if (methodIt == message.end() || !methodIt->is_string())
            {
                return std::nullopt; // a response from the client or garbage: nothing to answer
            }
            if (idIt == message.end() || idIt->is_null())
            {
                return std::nullopt; // notification (initialized, cancelled, ...)
            }
            const Json id = *idIt;
            const std::string method = methodIt->get<std::string>();
            const Json params = message.contains("params") && message["params"].is_object() ? message["params"] : Json::object();

            if (method == "initialize")
            {
                std::string version = "2024-11-05";
                if (params.contains("protocolVersion") && params["protocolVersion"].is_string())
                {
                    version = params["protocolVersion"].get<std::string>();
                }
                return Result(id, {
                    {"protocolVersion", version},
                    {"capabilities", {{"tools", {{"listChanged", false}}}}},
                    {"serverInfo", {{"name", "myengine"}, {"version", "1.0"}}},
                });
            }
            if (method == "ping")
            {
                return Result(id, Json::object());
            }
            if (method == "tools/list")
            {
                const Json response = upstream_({{"op", "tools"}});
                if (!response.is_object() || !response.contains("tools") || !response["tools"].is_array())
                {
                    return Error(id, -32603, ErrorText(response));
                }
                Json tools = Json::array();
                for (const auto& tool : response["tools"])
                {
                    Json item = {
                        {"name", tool.value("name", std::string())},
                        {"description", tool.value("description", std::string())},
                        {"inputSchema", tool.contains("inputSchema") ? tool["inputSchema"] : Json{{"type", "object"}}},
                    };
                    tools.push_back(std::move(item));
                }
                return Result(id, {{"tools", std::move(tools)}});
            }
            if (method == "tools/call")
            {
                if (!params.contains("name") || !params["name"].is_string())
                {
                    return Error(id, -32602, "tools/call needs a tool name");
                }
                const Json response = upstream_({
                    {"op", "call"},
                    {"tool", params["name"]},
                    {"arguments", params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : Json::object()},
                });
                if (response.is_object() && response.contains("content") && response["content"].is_array())
                {
                    return Result(id, response);
                }
                // The editor is closed or the pipe broke: the model gets a normal tool error it can report
                return Result(id, {
                    {"content", Json::array({{{"type", "text"}, {"text", ErrorText(response)}}})},
                    {"isError", true},
                });
            }
            return Error(id, -32601, "Method not found: " + method);
        }

    private:
        static std::string ErrorText(const Json& response)
        {
            if (response.is_object() && response.contains("error") && response["error"].is_string())
            {
                return response["error"].get<std::string>();
            }
            return "The editor did not answer.";
        }

        static Json Result(const Json& id, Json result)
        {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
        }

        static Json Error(const Json& id, const int code, std::string message)
        {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", std::move(message)}}}};
        }

        Upstream upstream_;
    };
}
