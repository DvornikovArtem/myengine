// AssistantBridgeServer.h

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace myengine::assistant
{
    struct BridgeRequest
    {
        std::uint64_t id = 0;         // the bridge's own number, echoed in the reply
        std::uint64_t connection = 0; // which connection it came from
        std::string op;               // "tools" or "call"
        std::string tool;
        nlohmann::json arguments = nlohmann::json::object();
    };

    // Editor end of the named pipe the myengine_mcp bridge connects to (one client at a time).
    // Newline-delimited JSON. The pipe is polled from the main thread: no thread, no blocking.
    // The first line from a client must be {"op":"hello","token":"<token>"}; anything else is dropped.
    class AssistantBridgeServer
    {
    public:
        AssistantBridgeServer();
        ~AssistantBridgeServer();

        AssistantBridgeServer(const AssistantBridgeServer&) = delete;
        AssistantBridgeServer& operator=(const AssistantBridgeServer&) = delete;

        bool Start(const std::wstring& pipeName, const std::string& token, std::string& error);
        void Stop();
        bool IsRunning() const;
        bool IsConnected() const; // an authenticated bridge is connected

        // Appends the requests that arrived since the last call. Accepts a new client when the previous one left
        void Poll(std::vector<BridgeRequest>& requests);
        // Dropped silently if the client has gone or reconnected since the request
        bool Reply(const BridgeRequest& request, const nlohmann::json& response);
        // Number of clients that left since the last call (the turn they belonged to is over)
        std::uint32_t TakeDisconnects();

        static std::wstring DefaultPipeName();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
