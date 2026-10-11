// ClaudeCliBackend.h

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <myengine/assistant/ClaudeStreamParser.h>
#include <myengine/assistant/IAssistantBackend.h>

namespace myengine::core
{
    class Logger;
}

namespace myengine::assistant
{
    // The editor's MCP bridge (myengine_mcp.exe): engine tools and confirmations. Without it the CLI only edits files
    struct ClaudeCliBridge
    {
        std::filesystem::path executable; // empty: bridge disabled
        std::wstring pipeName;
        std::string token;
        std::string serverName = "myengine";
        std::vector<std::string> allowedTools; // full MCP names the CLI may call without asking (the read-only ones)
        std::string approveTool;               // full MCP name of the permission prompt tool
    };

    struct ClaudeCliConfig
    {
        // Explicit path to claude(.exe/.cmd). Empty: MYENGINE_CLAUDE_PATH, then PATH, then the usual install folders
        std::filesystem::path executable;
        // The child process runs here; it is the repository root, so relative rules and Read/Glob/Grep see the project
        std::filesystem::path workingDirectory;
        // Appended to Claude Code's own system prompt (--append-system-prompt-file). Skipped if the file is missing
        std::filesystem::path systemPromptFile;
        // Where the generated settings JSON is written. Empty: <temp>/myengine-assistant
        std::filesystem::path stateDirectory;
        // Folders (relative to workingDirectory) the assistant may edit. Everything else is read-only
        std::vector<std::string> editableGlobs{"assets/scripts/**", "assets/prefabs/**"};
        ClaudeCliBridge bridge;
        std::string model; // empty: the CLI's default
        double maxBudgetUsd = 2.0; // per turn; 0 disables the flag
        int maxTurns = 40;
        core::Logger* logger = nullptr;
    };

    // Runs one `claude -p` process per turn. The prompt goes to stdin as a stream-json user message,
    // the answer comes back as JSON lines on stdout. The conversation continues through --resume <session_id>.
    // The pipes are read without blocking, once per frame.
    class ClaudeCliBackend final : public IAssistantBackend
    {
    public:
        explicit ClaudeCliBackend(ClaudeCliConfig config);
        ~ClaudeCliBackend() override;

        const char* GetName() const override { return "Claude Code CLI"; }
        std::string CheckAvailability() override;
        bool BeginTurn(const std::string& prompt, const std::string& sessionId, std::string& error) override;
        bool BeginTurn(const AssistantTurnRequest& request, std::string& error) override;
        void Cancel() override;
        bool IsBusy() const override;
        void Poll(std::vector<AssistantEvent>& events) override;
        void Shutdown() override;

        // An explicit path typed by the user; empty goes back to automatic search
        void SetExecutableOverride(std::filesystem::path path);
        const std::filesystem::path& GetExecutableOverride() const { return config_.executable; }
        // The path of the last successful search (empty when not found)
        const std::filesystem::path& GetResolvedExecutable() const { return resolved_; }

        // Pure helpers, covered by tests without starting a process
        static std::filesystem::path FindExecutable(const std::filesystem::path& override, std::string& problem);
        static std::string BuildSettingsJson(const std::vector<std::string>& editableGlobs);
        static std::string BuildSettingsJson(const ClaudeCliConfig& config);
        static std::string BuildMcpConfigJson(const ClaudeCliBridge& bridge);
        static std::wstring BuildCommandLine(
            const std::filesystem::path& executable,
            const ClaudeCliConfig& config,
            const std::filesystem::path& settingsFile,
            const std::string& sessionId,
            const std::filesystem::path& mcpConfigFile = {},
            const std::string& modelOverride = {},
            const std::string& effort = {},
            const std::vector<std::filesystem::path>& extraDirectories = {});
        static std::string BuildUserMessageLine(const std::string& prompt);
        // The prompt plus the attachments: images become base64 image blocks (a file that is too big or not readable
        // falls back to a path), other files are listed in the text so that the model reads them itself
        static std::string BuildUserMessageLine(const AssistantTurnRequest& request, const std::filesystem::path& root);
        static bool IsValidEffort(const std::string& effort);
        static bool IsSafeModelName(const std::string& model);
        // Folders of the non-image attachments that are outside `root`: they are passed to the CLI as --add-dir
        static std::vector<std::filesystem::path> ExtraDirectoriesFor(const AssistantTurnRequest& request, const std::filesystem::path& root);
        static bool IsValidSessionId(const std::string& sessionId);

    private:
        struct Process;

        bool ResolveExecutable(std::string& problem);
        void ReleaseProcess(bool terminate);
        void ExtractLines(std::vector<AssistantEvent>& events);

        ClaudeCliConfig config_;
        std::filesystem::path resolved_;
        std::filesystem::path settingsFile_;
        std::filesystem::path mcpConfigFile_;
        std::unique_ptr<Process> process_;
        ClaudeStreamParser parser_;
        std::string outputBuffer_;
        std::string errorTail_;
        bool discardingLine_ = false;
    };
}
