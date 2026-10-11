// AssistantBridge.h

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <myengine/assistant/AssistantBridgeServer.h>

namespace myengine::assistant
{
    class AssistantTools;

    // What the model is told when the user rejects a confirmation card; the panel recognises it to show a
    // rejection as a neutral status, not as a tool failure
    inline constexpr const char* kUserRejectedText = "The user rejected this action. Do not repeat it; ask what they want instead.";

    struct AssistantBridgeConfig
    {
        struct EditableArea
        {
            std::string directory; // relative to the repository root
            std::string suffix;    // file name ending that may be written there
        };

        std::filesystem::path repositoryRoot;
        std::vector<EditableArea> editable = {{"assets/scripts", ".py"}, {"assets/prefabs", ".prefab.json"}};
        std::wstring pipeName; // empty: \\.\pipe\myengine-assistant-<pid>

        // A changing engine tool that was not confirmed beforehand waits for the user's card inside its own call.
        // This is how a client without a permission prompt tool (Codex, any external MCP client) gets confirmations.
        // Off: such a call is refused (Claude Code asks through `approve` first)
        bool confirmInCall = false;
        // The `approve` tool is listed (the permission prompt tool of Claude Code); a model must not see it otherwise
        bool offerApproveTool = true;
        // Offers edit_file / write_file: the only way for such a CLI to change a file. They go through the same
        // card with a diff and the same editable areas as Edit / Write do for Claude Code
        bool fileTools = false;
    };

    // One thing the assistant wants to do and the user has to confirm in the panel
    struct AssistantApproval
    {
        std::uint64_t id = 0;
        std::string tool;   // as the CLI names it: Edit, Write, mcp__myengine__spawn_prefab
        std::string title;  // one line: what will happen
        std::string detail; // diff or JSON of the arguments
        std::string client; // the external MCP client that asks (empty: the built-in assistant or unknown)
    };

    // Editor side of the MCP bridge: owns the pipe, answers tool calls, and holds permission prompts
    // (--permission-prompt-tool) until the user decides. Main thread only; Update() every frame.
    //
    // Rules for a permission prompt:
    //   - myengine read tools: allowed at once; myengine changing tools: a card, and the later call is
    //     accepted only after the user said yes (one confirmation covers one call);
    //   - Edit / Write / MultiEdit: a card with a diff, only inside the editable areas, otherwise refused;
    //   - Read / Glob / Grep: allowed inside the repository, refused outside;
    //   - everything else is refused.
    class AssistantBridge
    {
    public:
        static constexpr const char* kServerName = "myengine";
        static constexpr const char* kApproveTool = "approve";

        AssistantBridge(AssistantTools& tools, AssistantBridgeConfig config);
        ~AssistantBridge();

        bool Start(std::string& error);
        void Stop();
        bool IsRunning() const;
        void Update();

        const std::wstring& GetPipeName() const { return pipeName_; }
        const std::string& GetToken() const { return token_; }
        // Full MCP names (mcp__myengine__<tool>) of the tools that never change anything
        std::vector<std::string> ReadOnlyToolNames() const;
        static std::string ApproveToolFullName();
        static constexpr const char* kEditFileTool = "edit_file";
        static constexpr const char* kWriteFileTool = "write_file";

        const std::vector<AssistantApproval>& GetPending() const { return pendingView_; }
        void Resolve(std::uint64_t id, bool allow, const std::string& reason = {});
        // The turn ended or was stopped: nobody is waiting for these any more
        void RejectAll(const std::string& reason);
        // The turn of the built-in assistant ended: its permission prompts are void; calls of external clients that
        // wait for their cards are not
        void RejectTurnPrompts(const std::string& reason);

        // Lines for the transcript ("Applied: ...", "Rejected: ...")
        std::function<void(const std::string& text)> onNotice;

    private:
        struct Pending
        {
            BridgeRequest request;
            AssistantApproval view;
            nlohmann::json input;
            std::string mcpTool; // the myengine tool name when the prompt is about one of ours

            // What the answer to `request` is: a verdict for the permission prompt tool, the result of the call that
            // waited for the card, or the result of a file change that the card is about
            enum class Reply : std::uint8_t
            {
                Permission,
                Call,
                FileWrite,
            };
            Reply reply = Reply::Permission;
            std::filesystem::path file; // FileWrite: the file to write
            std::string content;        // FileWrite: its new content
            std::string displayPath;    // FileWrite: relative to the repository
        };

        void Handle(const BridgeRequest& request);
        void HandleCall(const BridgeRequest& request);
        void HandleApprove(const BridgeRequest& request);
        void HandleFileTool(const BridgeRequest& request);
        void FinishPending(Pending& entry, bool allow, const std::string& reason);
        bool IsInsideRepository(const std::string& path) const;
        bool IsEditable(const std::string& path, std::string& displayPath) const;
        void ReplyPermission(const BridgeRequest& request, bool allow, const nlohmann::json& input, const std::string& message);
        void ReplyText(const BridgeRequest& request, const std::string& text, bool isError);
        void RebuildView();
        void Notice(const std::string& text) const;

        AssistantTools& tools_;
        AssistantBridgeConfig config_;
        AssistantBridgeServer server_;
        std::wstring pipeName_;
        std::string token_;
        std::vector<Pending> pending_;
        std::vector<AssistantApproval> pendingView_;
        std::map<std::string, int> tickets_; // confirmed changing calls that have not arrived yet
        std::uint64_t nextApproval_ = 1;
    };
}
