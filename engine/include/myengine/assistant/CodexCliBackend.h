// CodexCliBackend.h

#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <myengine/assistant/CodexStreamParser.h>
#include <myengine/assistant/IAssistantBackend.h>

namespace myengine::core
{
    class Logger;
}

namespace myengine::assistant
{
    // The editor's MCP bridge (myengine_mcp.exe) as Codex sees it: a stdio MCP server given with -c overrides, so the
    // user's own ~/.codex/config.toml is never touched. Without it the assistant can only read
    struct CodexCliBridge
    {
        std::filesystem::path executable; // empty: bridge disabled
        std::wstring pipeName;
        std::string token;
        std::string serverName = "myengine";
        int toolTimeoutSec = 900; // a changing call waits for the user's card, so the default 60 s is too short
    };

    struct CodexCliConfig
    {
        // Explicit path to codex(.exe/.cmd). Empty: MYENGINE_CODEX_PATH, then PATH, then the usual install folders
        std::filesystem::path executable;
        // The child process runs here; it is the repository root, so the project is what the assistant reads
        std::filesystem::path workingDirectory;
        // Text put in front of the first message of a conversation (Codex has no "append to the system prompt" flag)
        std::vector<std::filesystem::path> systemPromptFiles;
        // Where the events of every thread are kept (<thread id>.jsonl) so that a chat can be opened again.
        // Empty: nothing is kept
        std::filesystem::path transcriptDirectory;
        CodexCliBridge bridge;
        std::string model; // empty: the CLI's own default
        core::Logger* logger = nullptr;
    };

    // Runs one `codex exec --json` process per turn. The prompt goes to stdin, the answer comes back as JSON lines
    // on stdout. The conversation continues through `codex exec resume <thread id>`.
    //
    // Limits (the CLI has no permission prompt, so they are made with its sandbox and with the bridge):
    //   - the sandbox is read-only: Codex cannot write a file or use the network itself, a shell command can only read;
    //   - files are changed with the bridge's edit_file / write_file, which allow only assets/scripts/*.py and
    //     assets/prefabs/*.prefab.json and show a card with a diff;
    //   - engine tools that change the scene wait for a card inside their own call.
    class CodexCliBackend final : public IAssistantBackend
    {
    public:
        explicit CodexCliBackend(CodexCliConfig config);
        ~CodexCliBackend() override;

        const char* GetName() const override { return "Codex CLI"; }
        const char* GetBackendId() const override { return "codex"; }
        std::string CheckAvailability() override;
        bool BeginTurn(const std::string& prompt, const std::string& sessionId, std::string& error) override;
        bool BeginTurn(const AssistantTurnRequest& request, std::string& error) override;
        void Cancel() override;
        bool IsBusy() const override;
        void Poll(std::vector<AssistantEvent>& events) override;
        void Shutdown() override;

        void SetExecutableOverride(std::filesystem::path path);
        const std::filesystem::path& GetExecutableOverride() const { return config_.executable; }
        // The path of the last successful search (empty when not found)
        const std::filesystem::path& GetResolvedExecutable() const { return resolved_; }
        // The model of new chats when a chat has none of its own
        void SetDefaultModel(std::string model) { config_.model = std::move(model); }

        // Pure helpers, covered by tests without starting a process
        static std::filesystem::path FindExecutable(const std::filesystem::path& override, std::string& problem);
        // The whole command line, the quoted executable first. The prompt is read from stdin ("-")
        static std::wstring BuildCommandLine(
            const std::filesystem::path& executable,
            const CodexCliConfig& config,
            const std::string& sessionId,
            const std::string& modelOverride = {},
            const std::string& effort = {},
            const std::vector<std::filesystem::path>& images = {});
        // What goes to stdin: the system prompt files (new conversations only), the message, the list of other files
        static std::string BuildPrompt(const AssistantTurnRequest& request, const CodexCliConfig& config, bool includeInstructions);
        // A TOML string for a -c value: a literal string ('...') when it can be one, else a basic one with escapes
        static std::string TomlString(const std::string& text);
        static bool IsValidEffort(const std::string& effort);
        static bool IsSafeModelName(const std::string& model);
        static bool IsValidSessionId(const std::string& sessionId);

    private:
        struct Process;

        bool ResolveExecutable(std::string& problem);
        void ReleaseProcess(bool terminate);
        void ExtractLines(std::vector<AssistantEvent>& events);
        void Record(const std::string& line);

        CodexCliConfig config_;
        std::filesystem::path resolved_;
        std::unique_ptr<Process> process_;
        CodexStreamParser parser_;
        std::string outputBuffer_;
        std::string errorTail_;
        std::string threadId_;
        std::string pendingRecord_; // lines that wait for the thread id (a new conversation)
        std::chrono::steady_clock::time_point turnStart_;
        std::int64_t turnStartUnix_ = 0;
        bool discardingLine_ = false;
    };
}
