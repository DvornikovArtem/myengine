// ClaudeCliBackend.cpp

#include <myengine/assistant/ClaudeCliBackend.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

#include <windows.h>

#include <nlohmann/json.hpp>

#include <myengine/core/Logger.h>

namespace myengine::assistant
{
    namespace
    {
        constexpr std::size_t kMaxPromptBytes = 128 * 1024;
        constexpr std::size_t kMaxLineBytes = 16 * 1024 * 1024;
        constexpr std::size_t kBytesPerFrame = 256 * 1024;     // limit of stdout read in one Poll while the process runs
        constexpr std::size_t kBytesAtExit = 64 * 1024 * 1024; // what is left in the pipe after the process ended
        constexpr std::size_t kErrorTailBytes = 2048;
        constexpr DWORD kPipeBufferBytes = 1024 * 1024;

        bool HasExtension(const std::filesystem::path& path, const wchar_t* extension)
        {
            std::wstring value = path.extension().wstring();
            std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return value == extension;
        }

        std::wstring Quote(const std::wstring& text)
        {
            return L"\"" + text + L"\"";
        }

        std::wstring GetEnvironmentValue(const wchar_t* name)
        {
            std::wstring value(1024, L'\0');
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                const DWORD length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
                if (length == 0)
                {
                    return {};
                }
                if (length < value.size())
                {
                    value.resize(length);
                    return value;
                }
                value.assign(static_cast<std::size_t>(length) + 1, L'\0');
            }
            return {};
        }

        bool IsRegularFile(const std::filesystem::path& path)
        {
            std::error_code error;
            return std::filesystem::is_regular_file(path, error);
        }

        bool IsSafeModelName(const std::string& model)
        {
            return !model.empty() && model.size() <= 64 && std::all_of(model.begin(), model.end(), [](const char c)
            {
                return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '-' || c == '_' || c == '[' || c == ']';
            });
        }

        // Moves what is available in a pipe to `buffer`, at most `budget` bytes. Never blocks
        std::size_t DrainPipe(void* pipe, std::string& buffer, const std::size_t budget)
        {
            std::size_t total = 0;
            char chunk[16384];
            while (total < budget)
            {
                DWORD available = 0;
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0)
                {
                    break;
                }
                const auto wanted = std::min<std::size_t>({static_cast<std::size_t>(available), sizeof(chunk), budget - total});
                DWORD read = 0;
                if (!ReadFile(pipe, chunk, static_cast<DWORD>(wanted), &read, nullptr) || read == 0)
                {
                    break;
                }
                buffer.append(chunk, read);
                total += read;
            }
            return total;
        }

        std::string SanitizeTail(std::string text)
        {
            for (auto& c : text)
            {
                if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t')
                {
                    c = ' ';
                }
            }
            const auto first = text.find_first_not_of(" \r\n\t");
            if (first == std::string::npos)
            {
                return {};
            }
            return text.substr(first);
        }
    }

    struct ClaudeCliBackend::Process
    {
        void* process = nullptr;
        void* thread = nullptr;
        void* job = nullptr;
        void* stdoutRead = nullptr;
        void* stderrRead = nullptr;
    };

    ClaudeCliBackend::ClaudeCliBackend(ClaudeCliConfig config) : config_(std::move(config))
    {
        if (config_.stateDirectory.empty())
        {
            std::error_code error;
            config_.stateDirectory = std::filesystem::temp_directory_path(error) / "myengine-assistant";
        }
    }

    ClaudeCliBackend::~ClaudeCliBackend()
    {
        Shutdown();
    }

    void ClaudeCliBackend::SetExecutableOverride(std::filesystem::path path)
    {
        config_.executable = std::move(path);
        resolved_.clear();
    }

    std::filesystem::path ClaudeCliBackend::FindExecutable(const std::filesystem::path& override, std::string& problem)
    {
        problem.clear();
        if (!override.empty())
        {
            if (IsRegularFile(override))
            {
                return override;
            }
            problem = "The configured Claude Code CLI path does not exist: " + override.u8string();
            return {};
        }

        const auto fromEnvironment = GetEnvironmentValue(L"MYENGINE_CLAUDE_PATH");
        if (!fromEnvironment.empty())
        {
            const std::filesystem::path path(fromEnvironment);
            if (IsRegularFile(path))
            {
                return path;
            }
            problem = "MYENGINE_CLAUDE_PATH points to a file that does not exist: " + path.u8string();
            return {};
        }

        for (const wchar_t* extension : {L".exe", L".cmd"})
        {
            std::wstring buffer(MAX_PATH * 4, L'\0');
            const DWORD length = SearchPathW(nullptr, L"claude", extension, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
            if (length > 0 && length < buffer.size())
            {
                buffer.resize(length);
                return std::filesystem::path(buffer);
            }
        }

        // The native installer and npm do not always put their folder into PATH of a GUI process
        const auto home = GetEnvironmentValue(L"USERPROFILE");
        if (!home.empty())
        {
            const auto candidate = std::filesystem::path(home) / L".local" / L"bin" / L"claude.exe";
            if (IsRegularFile(candidate))
            {
                return candidate;
            }
        }
        const auto appData = GetEnvironmentValue(L"APPDATA");
        if (!appData.empty())
        {
            const auto candidate = std::filesystem::path(appData) / L"npm" / L"claude.cmd";
            if (IsRegularFile(candidate))
            {
                return candidate;
            }
        }

        problem = "Claude Code CLI was not found. Install it (https://code.claude.com/docs/en/setup) and make sure `claude` "
                  "is in PATH, or set its full path below or in the MYENGINE_CLAUDE_PATH environment variable.";
        return {};
    }

    bool ClaudeCliBackend::ResolveExecutable(std::string& problem)
    {
        resolved_ = FindExecutable(config_.executable, problem);
        return !resolved_.empty();
    }

    std::string ClaudeCliBackend::CheckAvailability()
    {
        std::string problem;
        ResolveExecutable(problem);
        return problem;
    }

    std::string ClaudeCliBackend::BuildSettingsJson(const std::vector<std::string>& editableGlobs)
    {
        using json = nlohmann::json;
        json allow = json::array();
        for (const auto& glob : editableGlobs)
        {
            allow.push_back("Edit(./" + glob + ")"); // Edit rules cover Write as well
        }
        const json settings = {
            {"permissions", {
                {"allow", allow},
                // The tool set is also limited with --tools; the deny list is a second lock
                {"deny", json::array({"Bash", "PowerShell", "WebFetch", "WebSearch"})},
            }},
        };
        return settings.dump(2);
    }

    std::string ClaudeCliBackend::BuildSettingsJson(const ClaudeCliConfig& config)
    {
        if (config.bridge.executable.empty())
        {
            return BuildSettingsJson(config.editableGlobs);
        }
        // With the bridge every file edit goes through the permission prompt tool (a card in the panel), so no Edit rule is
        // allowed here: the editor decides. Only the engine's read-only tools run without asking.
        using json = nlohmann::json;
        json allow = json::array();
        for (const auto& tool : config.bridge.allowedTools)
        {
            allow.push_back(tool);
        }
        json deny = json::array({"Bash", "PowerShell", "WebFetch", "WebSearch", "Edit(./assets/scenes/**)"});
        const json settings = {{"permissions", {{"allow", allow}, {"deny", deny}}}};
        return settings.dump(2);
    }

    std::string ClaudeCliBackend::BuildMcpConfigJson(const ClaudeCliBridge& bridge)
    {
        using json = nlohmann::json;
        std::string pipe;
        for (const wchar_t c : bridge.pipeName)
        {
            pipe += static_cast<char>(c); // the pipe name is ASCII
        }
        const json config = {
            {"mcpServers", {
                {bridge.serverName, {
                    {"command", bridge.executable.u8string()},
                    {"args", json::array({"--pipe", pipe})},
                    {"env", {{"MYENGINE_BRIDGE_TOKEN", bridge.token}}},
                }},
            }},
        };
        return config.dump(2);
    }

    bool ClaudeCliBackend::IsValidSessionId(const std::string& sessionId)
    {
        return !sessionId.empty() && sessionId.size() <= 64 && std::all_of(sessionId.begin(), sessionId.end(), [](const char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
        });
    }

    std::wstring ClaudeCliBackend::BuildCommandLine(
        const std::filesystem::path& executable,
        const ClaudeCliConfig& config,
        const std::filesystem::path& settingsFile,
        const std::string& sessionId,
        const std::filesystem::path& mcpConfigFile)
    {
        const bool bridge = !config.bridge.executable.empty() && !mcpConfigFile.empty() && !config.bridge.approveTool.empty();
        std::wstring line = Quote(executable.wstring());
        line += L" -p --input-format stream-json --output-format stream-json --verbose --include-partial-messages";
        if (bridge)
        {
            // Prompts go to the editor through the permission prompt tool and wait for the user's card
            line += L" --permission-mode default";
            line += L" --mcp-config " + Quote(mcpConfigFile.wstring());
            line += L" --strict-mcp-config";
            line += L" --permission-prompt-tool " + std::wstring(config.bridge.approveTool.begin(), config.bridge.approveTool.end());
        }
        else
        {
            // Nobody can answer a permission prompt in -p mode: whatever is not allowed in the settings file is denied
            line += L" --permission-mode dontAsk";
        }
        line += L" --tools \"Read,Glob,Grep,Edit,Write\"";
        // --tools limits the built-in tools only; connectors of the user's account (claude.ai MCP tools) stay in the context otherwise
        line += bridge ? L" --disallowedTools \"mcp__claude_ai_*\"" : L" --disallowedTools \"mcp__*\"";
        line += L" --settings " + Quote(settingsFile.wstring());
        if (!config.systemPromptFile.empty() && IsRegularFile(config.systemPromptFile))
        {
            line += L" --append-system-prompt-file " + Quote(config.systemPromptFile.wstring());
        }
        if (config.maxTurns > 0)
        {
            line += L" --max-turns " + std::to_wstring(config.maxTurns);
        }
        if (config.maxBudgetUsd > 0.0)
        {
            wchar_t budget[32];
            std::swprintf(budget, sizeof(budget) / sizeof(budget[0]), L"%.2f", config.maxBudgetUsd);
            line += L" --max-budget-usd ";
            line += budget;
        }
        if (IsSafeModelName(config.model))
        {
            line += L" --model " + std::wstring(config.model.begin(), config.model.end());
        }
        if (IsValidSessionId(sessionId))
        {
            line += L" --resume " + std::wstring(sessionId.begin(), sessionId.end());
        }
        return line;
    }

    std::string ClaudeCliBackend::BuildUserMessageLine(const std::string& prompt)
    {
        using json = nlohmann::json;
        const json message = {
            {"type", "user"},
            {"message", {
                {"role", "user"},
                {"content", json::array({{{"type", "text"}, {"text", prompt}}})},
            }},
        };
        return message.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    }

    bool ClaudeCliBackend::IsBusy() const
    {
        return process_ != nullptr;
    }

    bool ClaudeCliBackend::BeginTurn(const std::string& prompt, const std::string& sessionId, std::string& error)
    {
        if (IsBusy())
        {
            error = "The assistant is still answering.";
            return false;
        }
        if (!ResolveExecutable(error))
        {
            return false;
        }
        if (prompt.size() > kMaxPromptBytes)
        {
            error = "The message is too long (limit " + std::to_string(kMaxPromptBytes / 1024) + " KB).";
            return false;
        }
        if (!sessionId.empty() && !IsValidSessionId(sessionId))
        {
            error = "The stored session id is not valid; start a new chat.";
            return false;
        }

        std::error_code fileError;
        std::filesystem::create_directories(config_.stateDirectory, fileError);
        settingsFile_ = config_.stateDirectory / ("settings-" + std::to_string(GetCurrentProcessId()) + ".json");
        {
            std::ofstream settings(settingsFile_, std::ios::binary | std::ios::trunc);
            settings << BuildSettingsJson(config_);
            if (!settings)
            {
                error = "Could not write the assistant settings file: " + settingsFile_.u8string();
                return false;
            }
        }
        mcpConfigFile_.clear();
        if (!config_.bridge.executable.empty())
        {
            mcpConfigFile_ = config_.stateDirectory / ("mcp-" + std::to_string(GetCurrentProcessId()) + ".json");
            std::ofstream mcp(mcpConfigFile_, std::ios::binary | std::ios::trunc);
            mcp << BuildMcpConfigJson(config_.bridge);
            if (!mcp)
            {
                error = "Could not write the MCP config file: " + mcpConfigFile_.u8string();
                return false;
            }
        }

        std::wstring commandLine = BuildCommandLine(resolved_, config_, settingsFile_, sessionId, mcpConfigFile_);
        const bool viaShell = HasExtension(resolved_, L".cmd") || HasExtension(resolved_, L".bat");
        if (viaShell)
        {
            // npm installs a .cmd shim, which only cmd.exe can run
            auto shell = GetEnvironmentValue(L"ComSpec");
            if (shell.empty())
            {
                shell = L"cmd.exe";
            }
            commandLine = Quote(shell) + L" /d /s /c " + Quote(commandLine);
        }

        SECURITY_ATTRIBUTES inheritable{};
        inheritable.nLength = sizeof(inheritable);
        inheritable.bInheritHandle = TRUE;

        HANDLE stdinRead = nullptr;
        HANDLE stdinWrite = nullptr;
        HANDLE stdoutRead = nullptr;
        HANDLE stdoutWrite = nullptr;
        HANDLE stderrRead = nullptr;
        HANDLE stderrWrite = nullptr;
        const auto closeAll = [&]()
        {
            for (HANDLE* handle : {&stdinRead, &stdinWrite, &stdoutRead, &stdoutWrite, &stderrRead, &stderrWrite})
            {
                if (*handle != nullptr)
                {
                    CloseHandle(*handle);
                    *handle = nullptr;
                }
            }
        };

        if (!CreatePipe(&stdinRead, &stdinWrite, &inheritable, kPipeBufferBytes) ||
            !CreatePipe(&stdoutRead, &stdoutWrite, &inheritable, kPipeBufferBytes) ||
            !CreatePipe(&stderrRead, &stderrWrite, &inheritable, 64 * 1024))
        {
            error = "Could not create pipes for the Claude Code process.";
            closeAll();
            return false;
        }
        // The parent's ends must not leak into the child
        SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = stdinRead;
        startup.hStdOutput = stdoutWrite;
        startup.hStdError = stderrWrite;

        PROCESS_INFORMATION information{};
        const std::wstring directory = config_.workingDirectory.wstring();
        const std::wstring application = resolved_.wstring();
        const BOOL created = CreateProcessW(
            viaShell ? nullptr : application.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr,
            directory.empty() ? nullptr : directory.c_str(),
            &startup,
            &information);
        if (!created)
        {
            error = "Could not start Claude Code (error " + std::to_string(GetLastError()) + "): " + resolved_.u8string();
            closeAll();
            return false;
        }

        auto process = std::make_unique<Process>();
        process->process = information.hProcess;
        process->thread = information.hThread;
        // The job lets Stop kill the whole tree (claude starts helpers) and ends the child if the editor dies
        process->job = CreateJobObjectW(nullptr, nullptr);
        if (process->job != nullptr)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(process->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            if (!AssignProcessToJobObject(process->job, information.hProcess))
            {
                CloseHandle(process->job);
                process->job = nullptr;
            }
        }
        ResumeThread(information.hThread);
        CloseHandle(information.hThread);
        process->thread = nullptr;

        CloseHandle(stdinRead);
        stdinRead = nullptr;
        CloseHandle(stdoutWrite);
        stdoutWrite = nullptr;
        CloseHandle(stderrWrite);
        stderrWrite = nullptr;
        process->stdoutRead = stdoutRead;
        process->stderrRead = stderrRead;
        stdoutRead = nullptr;
        stderrRead = nullptr;

        process_ = std::move(process);
        outputBuffer_.clear();
        errorTail_.clear();
        discardingLine_ = false;
        parser_ = ClaudeStreamParser();
        parser_.SetRootDirectory(config_.workingDirectory.u8string());

        // The whole prompt fits into the 1 MB pipe buffer, so this does not wait for the child
        const std::string line = BuildUserMessageLine(prompt);
        std::size_t written = 0;
        bool writeFailed = false;
        while (written < line.size())
        {
            DWORD count = 0;
            if (!WriteFile(stdinWrite, line.data() + written, static_cast<DWORD>(line.size() - written), &count, nullptr) || count == 0)
            {
                writeFailed = true;
                break;
            }
            written += count;
        }
        CloseHandle(stdinWrite); // end of input: the CLI answers this one message and exits
        stdinWrite = nullptr;
        if (writeFailed)
        {
            ReleaseProcess(true);
            error = "Could not send the message to Claude Code.";
            return false;
        }

        if (config_.logger != nullptr)
        {
            config_.logger->Info("Assistant: started " + resolved_.u8string() + (sessionId.empty() ? " (new session)" : " (resume " + sessionId + ")"));
        }
        return true;
    }

    void ClaudeCliBackend::ReleaseProcess(const bool terminate)
    {
        if (process_ == nullptr)
        {
            return;
        }
        if (terminate)
        {
            if (process_->job != nullptr)
            {
                TerminateJobObject(process_->job, 1);
            }
            else if (process_->process != nullptr)
            {
                TerminateProcess(process_->process, 1);
            }
        }
        for (void* handle : {process_->process, process_->thread, process_->job, process_->stdoutRead, process_->stderrRead})
        {
            if (handle != nullptr)
            {
                CloseHandle(handle);
            }
        }
        process_.reset();
        outputBuffer_.clear();
        discardingLine_ = false;
    }

    void ClaudeCliBackend::Cancel()
    {
        if (process_ == nullptr)
        {
            return;
        }
        ReleaseProcess(true);
        if (config_.logger != nullptr)
        {
            config_.logger->Info("Assistant: the turn was stopped by the user");
        }
    }

    void ClaudeCliBackend::Shutdown()
    {
        ReleaseProcess(true);
        if (!settingsFile_.empty())
        {
            std::error_code error;
            std::filesystem::remove(settingsFile_, error);
            settingsFile_.clear();
        }
        if (!mcpConfigFile_.empty())
        {
            std::error_code error;
            std::filesystem::remove(mcpConfigFile_, error); // holds the pipe token
            mcpConfigFile_.clear();
        }
    }

    void ClaudeCliBackend::ExtractLines(std::vector<AssistantEvent>& events)
    {
        std::size_t start = 0;
        while (start < outputBuffer_.size())
        {
            const auto end = outputBuffer_.find('\n', start);
            if (end == std::string::npos)
            {
                break;
            }
            if (discardingLine_)
            {
                discardingLine_ = false; // the tail of an oversized line
            }
            else
            {
                std::string_view line(outputBuffer_.data() + start, end - start);
                if (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }
                parser_.ParseLine(line, events);
            }
            start = end + 1;
        }
        outputBuffer_.erase(0, start);
        if (outputBuffer_.size() > kMaxLineBytes)
        {
            outputBuffer_.clear();
            discardingLine_ = true;
        }
    }

    void ClaudeCliBackend::Poll(std::vector<AssistantEvent>& events)
    {
        if (process_ == nullptr)
        {
            return;
        }

        DrainPipe(process_->stdoutRead, outputBuffer_, kBytesPerFrame);
        ExtractLines(events);

        std::string errorChunk;
        DrainPipe(process_->stderrRead, errorChunk, kBytesPerFrame);
        if (!errorChunk.empty())
        {
            errorTail_ += errorChunk;
            if (errorTail_.size() > kErrorTailBytes)
            {
                errorTail_.erase(0, errorTail_.size() - kErrorTailBytes);
            }
        }

        if (WaitForSingleObject(process_->process, 0) != WAIT_OBJECT_0)
        {
            return;
        }

        // The process is gone: take everything it wrote before the pipe closed
        DrainPipe(process_->stdoutRead, outputBuffer_, kBytesAtExit);
        if (!outputBuffer_.empty() && outputBuffer_.back() != '\n')
        {
            outputBuffer_ += '\n';
        }
        ExtractLines(events);
        errorChunk.clear();
        DrainPipe(process_->stderrRead, errorChunk, kBytesPerFrame);
        errorTail_ += errorChunk;
        if (errorTail_.size() > kErrorTailBytes)
        {
            errorTail_.erase(0, errorTail_.size() - kErrorTailBytes);
        }

        DWORD exitCode = 0;
        GetExitCodeProcess(process_->process, &exitCode);
        const bool finished = parser_.IsFinished();
        const auto tail = SanitizeTail(errorTail_);
        ReleaseProcess(false);

        if (config_.logger != nullptr)
        {
            config_.logger->Info("Assistant: the process ended with code " + std::to_string(exitCode) + (finished ? "" : " without a result"));
        }
        if (!finished)
        {
            AssistantEvent event;
            event.type = AssistantEventType::Error;
            event.text = "Claude Code ended without an answer (exit code " + std::to_string(static_cast<long>(exitCode)) + ").";
            if (!tail.empty())
            {
                event.text += "\n" + tail;
                if (tail.find("unknown option") != std::string::npos || tail.find("Unknown option") != std::string::npos)
                {
                    event.text += "\nThe installed Claude Code may be too old for these options; update it with `claude update`.";
                }
            }
            events.push_back(std::move(event));
        }
    }
}
