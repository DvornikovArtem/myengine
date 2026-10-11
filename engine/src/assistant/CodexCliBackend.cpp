// CodexCliBackend.cpp

#include <myengine/assistant/CodexCliBackend.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <utility>

#include <windows.h>

#include <nlohmann/json.hpp>

#include <myengine/core/Logger.h>

#include "CliProcess.h"

namespace myengine::assistant
{
    namespace
    {
        constexpr std::size_t kMaxPromptBytes = 128 * 1024;
        constexpr std::size_t kMaxImageBytes = 5 * 1024 * 1024;
        constexpr std::size_t kMaxAttachments = 12;
        constexpr std::size_t kMaxLineBytes = 16 * 1024 * 1024;
        constexpr std::size_t kMaxRecordedLineBytes = 256 * 1024; // a longer event is not worth keeping in the history
        constexpr std::size_t kBytesPerFrame = 256 * 1024;
        constexpr std::size_t kBytesAtExit = 64 * 1024 * 1024;
        constexpr std::size_t kErrorTailBytes = 2048;

        std::int64_t NowUnix()
        {
            return static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        }

        std::string ReadTextFile(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                return {};
            }
            return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        }

        std::string Trimmed(std::string text)
        {
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
            {
                return {};
            }
            const auto last = text.find_last_not_of(" \t\r\n");
            return text.substr(first, last - first + 1);
        }

        // A path as it is shown to the model: relative to the project when the file is inside it
        std::string PromptPath(const std::filesystem::path& file, const std::filesystem::path& root)
        {
            std::error_code error;
            if (!root.empty())
            {
                const auto relative = std::filesystem::relative(file, root, error);
                const auto text = relative.generic_u8string();
                if (!error && !relative.empty() && text.rfind("..", 0) != 0 && !relative.is_absolute())
                {
                    return text;
                }
            }
            return file.generic_u8string();
        }

        std::wstring Widen(const std::string& ascii)
        {
            return std::wstring(ascii.begin(), ascii.end());
        }

        // `-c key=value` as one command line argument
        std::wstring ConfigArgument(const std::string& key, const std::string& value)
        {
            return L" -c " + detail::QuoteArgument(std::filesystem::u8path(key + "=" + value).wstring());
        }

        std::vector<std::filesystem::path> ImagesOf(const AssistantTurnRequest& request)
        {
            std::vector<std::filesystem::path> images;
            std::size_t count = 0;
            for (const auto& attachment : request.attachments)
            {
                if (attachment.path.empty() || ++count > kMaxAttachments || !attachment.IsImage())
                {
                    continue;
                }
                const auto path = std::filesystem::u8path(attachment.path);
                std::error_code error;
                const auto size = std::filesystem::file_size(path, error);
                if (!error && size > 0 && size <= kMaxImageBytes)
                {
                    images.push_back(path);
                }
            }
            return images;
        }
    }

    struct CodexCliBackend::Process : detail::CliProcess
    {
    };

    CodexCliBackend::CodexCliBackend(CodexCliConfig config) : config_(std::move(config))
    {
    }

    CodexCliBackend::~CodexCliBackend()
    {
        Shutdown();
    }

    void CodexCliBackend::SetExecutableOverride(std::filesystem::path path)
    {
        config_.executable = std::move(path);
        resolved_.clear();
    }

    std::filesystem::path CodexCliBackend::FindExecutable(const std::filesystem::path& override, std::string& problem)
    {
        problem.clear();
        if (!override.empty())
        {
            if (detail::IsRegularFile(override))
            {
                return override;
            }
            problem = "The configured Codex CLI path does not exist: " + override.u8string();
            return {};
        }

        const auto fromEnvironment = detail::GetEnvironmentText(L"MYENGINE_CODEX_PATH");
        if (!fromEnvironment.empty())
        {
            const std::filesystem::path path(fromEnvironment);
            if (detail::IsRegularFile(path))
            {
                return path;
            }
            problem = "MYENGINE_CODEX_PATH points to a file that does not exist: " + path.u8string();
            return {};
        }

        for (const wchar_t* extension : {L".exe", L".cmd"})
        {
            std::wstring buffer(MAX_PATH * 4, L'\0');
            const DWORD length = SearchPathW(nullptr, L"codex", extension, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
            if (length > 0 && length < buffer.size())
            {
                buffer.resize(length);
                return std::filesystem::path(buffer);
            }
        }

        // npm (`npm install -g @openai/codex`) and cargo do not always put their folder into PATH of a GUI process
        const auto appData = detail::GetEnvironmentText(L"APPDATA");
        if (!appData.empty())
        {
            const auto candidate = std::filesystem::path(appData) / L"npm" / L"codex.cmd";
            if (detail::IsRegularFile(candidate))
            {
                return candidate;
            }
        }
        const auto home = detail::GetEnvironmentText(L"USERPROFILE");
        if (!home.empty())
        {
            const auto candidate = std::filesystem::path(home) / L".cargo" / L"bin" / L"codex.exe";
            if (detail::IsRegularFile(candidate))
            {
                return candidate;
            }
        }

        problem = "Codex CLI was not found. Install it (npm install -g @openai/codex), sign in with `codex login`, and make "
                  "sure `codex` is in PATH, or set its full path in the assistant settings or in the MYENGINE_CODEX_PATH "
                  "environment variable.";
        return {};
    }

    bool CodexCliBackend::ResolveExecutable(std::string& problem)
    {
        resolved_ = FindExecutable(config_.executable, problem);
        return !resolved_.empty();
    }

    std::string CodexCliBackend::CheckAvailability()
    {
        std::string problem;
        ResolveExecutable(problem);
        return problem;
    }

    bool CodexCliBackend::IsValidSessionId(const std::string& sessionId)
    {
        return !sessionId.empty() && sessionId.size() <= 64 && std::all_of(sessionId.begin(), sessionId.end(), [](const char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
        });
    }

    bool CodexCliBackend::IsSafeModelName(const std::string& model)
    {
        return !model.empty() && model.size() <= 64 && std::all_of(model.begin(), model.end(), [](const char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '-' || c == '_' || c == ':' || c == '/';
        });
    }

    bool CodexCliBackend::IsValidEffort(const std::string& effort)
    {
        return effort == "minimal" || effort == "low" || effort == "medium" || effort == "high" || effort == "xhigh";
    }

    std::string CodexCliBackend::TomlString(const std::string& text)
    {
        const bool literal = std::none_of(text.begin(), text.end(), [](const char c)
        {
            return c == '\'' || c == '\n' || c == '\r' || static_cast<unsigned char>(c) < 0x20;
        });
        if (literal)
        {
            return "'" + text + "'";
        }
        std::string out = "\"";
        for (const char c : text)
        {
            switch (c)
            {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char code[8];
                        std::snprintf(code, sizeof(code), "\\u%04X", static_cast<unsigned>(static_cast<unsigned char>(c)));
                        out += code;
                    }
                    else
                    {
                        out += c;
                    }
                    break;
            }
        }
        out += '"';
        return out;
    }

    std::wstring CodexCliBackend::BuildCommandLine(
        const std::filesystem::path& executable,
        const CodexCliConfig& config,
        const std::string& sessionId,
        const std::string& modelOverride,
        const std::string& effort,
        const std::vector<std::filesystem::path>& images)
    {
        std::wstring line = detail::QuoteArgument(executable.wstring());
        line += L" exec --json --skip-git-repo-check";
        // Nobody can answer an approval prompt of `exec`; the sandbox is what keeps the assistant read-only
        line += L" --sandbox read-only";
        line += ConfigArgument("approval_policy", "never");

        const std::string& model = IsSafeModelName(modelOverride) ? modelOverride : config.model;
        if (IsSafeModelName(model))
        {
            line += L" --model " + Widen(model);
        }
        if (IsValidEffort(effort))
        {
            line += ConfigArgument("model_reasoning_effort", effort);
        }

        if (!config.bridge.executable.empty() && !config.bridge.serverName.empty())
        {
            std::string pipe;
            for (const wchar_t c : config.bridge.pipeName)
            {
                pipe += static_cast<char>(c); // the pipe name is ASCII
            }
            const std::string server = "mcp_servers." + config.bridge.serverName;
            line += ConfigArgument(server + ".command", TomlString(config.bridge.executable.u8string()));
            line += ConfigArgument(server + ".args", "[" + TomlString("--pipe") + ", " + TomlString(pipe) + "]");
            line += ConfigArgument(server + ".env", "{ MYENGINE_BRIDGE_TOKEN = " + TomlString(config.bridge.token) + " }");
            line += ConfigArgument(server + ".startup_timeout_sec", "30");
            line += ConfigArgument(server + ".tool_timeout_sec", std::to_string(std::max(config.bridge.toolTimeoutSec, 60)));
            // The bridge asks the user itself (a card in the panel): Codex must not ask on top of it
            line += ConfigArgument(server + ".default_tools_approval_mode", TomlString("approve"));
        }

        for (const auto& image : images)
        {
            line += L" --image " + detail::QuoteArgument(image.wstring());
        }
        if (IsValidSessionId(sessionId))
        {
            line += L" resume " + Widen(sessionId);
        }
        line += L" -"; // the prompt comes from stdin
        return line;
    }

    std::string CodexCliBackend::BuildPrompt(const AssistantTurnRequest& request, const CodexCliConfig& config, const bool includeInstructions)
    {
        std::string text;
        if (includeInstructions)
        {
            for (const auto& file : config.systemPromptFiles)
            {
                const auto content = Trimmed(ReadTextFile(file));
                if (!content.empty())
                {
                    text += content + "\n\n";
                }
            }
            if (!text.empty())
            {
                text += "---\n\nThe user's message follows.\n\n";
            }
        }
        text += request.prompt;

        std::string fileList;
        std::size_t count = 0;
        for (const auto& attachment : request.attachments)
        {
            if (attachment.path.empty() || ++count > kMaxAttachments)
            {
                continue;
            }
            const auto path = std::filesystem::u8path(attachment.path);
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            const bool sentAsImage = attachment.IsImage() && !error && size > 0 && size <= kMaxImageBytes;
            if (!sentAsImage)
            {
                fileList += "\n- " + PromptPath(path, config.workingDirectory);
            }
        }
        if (!fileList.empty())
        {
            text += "\n\nAttached files (read them with a read-only shell command):" + fileList;
        }
        return text;
    }

    bool CodexCliBackend::IsBusy() const
    {
        return process_ != nullptr;
    }

    bool CodexCliBackend::BeginTurn(const std::string& prompt, const std::string& sessionId, std::string& error)
    {
        AssistantTurnRequest request;
        request.prompt = prompt;
        request.sessionId = sessionId;
        return BeginTurn(request, error);
    }

    bool CodexCliBackend::BeginTurn(const AssistantTurnRequest& request, std::string& error)
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
        if (request.prompt.size() > kMaxPromptBytes)
        {
            error = "The message is too long (limit " + std::to_string(kMaxPromptBytes / 1024) + " KB).";
            return false;
        }
        if (!request.sessionId.empty() && !IsValidSessionId(request.sessionId))
        {
            error = "The stored session id is not valid; start a new chat.";
            return false;
        }

        const bool resume = !request.sessionId.empty();
        const auto images = ImagesOf(request);
        const std::wstring commandLine = BuildCommandLine(resolved_, config_, request.sessionId, request.model, request.effort, images);
        const std::string stdinText = BuildPrompt(request, config_, !resume);

        auto process = std::make_unique<Process>();
        if (!process->Start(resolved_, commandLine, config_.workingDirectory, error))
        {
            error = "Could not start Codex: " + error;
            return false;
        }
        process_ = std::move(process);
        outputBuffer_.clear();
        errorTail_.clear();
        discardingLine_ = false;
        parser_ = CodexStreamParser();
        parser_.SetRootDirectory(config_.workingDirectory.u8string());
        parser_.SetModel(IsSafeModelName(request.model) ? request.model : config_.model);
        threadId_ = request.sessionId;
        pendingRecord_.clear();
        turnStart_ = std::chrono::steady_clock::now();
        turnStartUnix_ = NowUnix();

        if (!process_->WriteInputAndClose(stdinText))
        {
            ReleaseProcess(true);
            error = "Could not send the message to Codex.";
            return false;
        }

        // The history keeps the message of the user in the same file as the events that answer it
        {
            nlohmann::json attachments = nlohmann::json::array();
            for (const auto& attachment : request.attachments)
            {
                attachments.push_back({{"path", attachment.path}, {"name", attachment.name}, {"mediaType", attachment.mediaType}});
            }
            const nlohmann::json record = {
                {"type", "myengine.user"},
                {"time", turnStartUnix_},
                {"text", request.prompt},
                {"model", IsSafeModelName(request.model) ? request.model : config_.model},
                {"effort", request.effort},
                {"attachments", std::move(attachments)},
            };
            Record(record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        }

        if (config_.logger != nullptr)
        {
            config_.logger->Info("Assistant: started " + resolved_.u8string() + (resume ? " (resume " + request.sessionId + ")" : " (new thread)") +
                " model=" + (IsSafeModelName(request.model) ? request.model : (config_.model.empty() ? std::string("default") : config_.model)) +
                " effort=" + (IsValidEffort(request.effort) ? request.effort : std::string("default")) +
                " images=" + std::to_string(images.size()));
        }
        return true;
    }

    void CodexCliBackend::Record(const std::string& line)
    {
        if (config_.transcriptDirectory.empty() || line.empty())
        {
            return;
        }
        if (!IsValidSessionId(threadId_))
        {
            pendingRecord_ += line;
            pendingRecord_ += '\n';
            return;
        }
        std::error_code error;
        std::filesystem::create_directories(config_.transcriptDirectory, error);
        std::ofstream file(config_.transcriptDirectory / (threadId_ + ".jsonl"), std::ios::binary | std::ios::app);
        if (!pendingRecord_.empty())
        {
            file << pendingRecord_;
            pendingRecord_.clear();
        }
        file << line << '\n';
    }

    void CodexCliBackend::ReleaseProcess(const bool terminate)
    {
        if (process_ == nullptr)
        {
            return;
        }
        process_->Close(terminate);
        process_.reset();
        outputBuffer_.clear();
        discardingLine_ = false;
    }

    void CodexCliBackend::Cancel()
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

    void CodexCliBackend::Shutdown()
    {
        ReleaseProcess(true);
    }

    void CodexCliBackend::ExtractLines(std::vector<AssistantEvent>& events)
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
                const std::size_t before = events.size();
                parser_.ParseLine(line, events);
                if (threadId_.empty() && !parser_.GetThreadId().empty())
                {
                    threadId_ = parser_.GetThreadId(); // a new conversation: from here on the events have a file
                }
                for (std::size_t index = before; index < events.size(); ++index)
                {
                    if (events[index].type == AssistantEventType::Finished)
                    {
                        const auto elapsed = std::chrono::steady_clock::now() - turnStart_;
                        events[index].durationMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
                    }
                }
                if (!line.empty() && line.front() == '{' && line.size() <= kMaxRecordedLineBytes)
                {
                    Record(std::string(line));
                }
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

    void CodexCliBackend::Poll(std::vector<AssistantEvent>& events)
    {
        if (process_ == nullptr)
        {
            return;
        }

        process_->ReadOutput(outputBuffer_, kBytesPerFrame);
        ExtractLines(events);

        std::string errorChunk;
        process_->ReadError(errorChunk, kBytesPerFrame);
        errorTail_ += errorChunk;
        detail::KeepTail(errorTail_, kErrorTailBytes);

        if (process_->IsRunning())
        {
            return;
        }

        // The process is gone: take everything it wrote before the pipe closed
        process_->ReadOutput(outputBuffer_, kBytesAtExit);
        if (!outputBuffer_.empty() && outputBuffer_.back() != '\n')
        {
            outputBuffer_ += '\n';
        }
        ExtractLines(events);
        errorChunk.clear();
        process_->ReadError(errorChunk, kBytesPerFrame);
        errorTail_ += errorChunk;
        detail::KeepTail(errorTail_, kErrorTailBytes);

        const unsigned long exitCode = process_->ExitCode();
        const bool finished = parser_.IsFinished();
        const auto tail = detail::SanitizeTail(errorTail_);
        ReleaseProcess(false);

        if (config_.logger != nullptr)
        {
            config_.logger->Info("Assistant: the Codex process ended with code " + std::to_string(exitCode) + (finished ? "" : " without a result"));
        }
        if (finished)
        {
            const auto elapsed = std::chrono::steady_clock::now() - turnStart_;
            Record(nlohmann::json{
                {"type", "myengine.turn_end"},
                {"time", NowUnix()},
                {"durationMs", std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()},
            }.dump());
            return;
        }

        AssistantEvent event;
        event.type = AssistantEventType::Error;
        event.text = "Codex ended without an answer (exit code " + std::to_string(static_cast<long>(exitCode)) + ").";
        if (!tail.empty())
        {
            event.text += "\n" + tail;
            const auto lower = [&]
            {
                std::string text = tail;
                std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return text;
            }();
            if (lower.find("login") != std::string::npos || lower.find("not logged in") != std::string::npos ||
                lower.find("unauthorized") != std::string::npos || lower.find("api key") != std::string::npos)
            {
                event.text += "\nSign in once in a terminal with `codex login`, then try again.";
            }
            else if (lower.find("unexpected argument") != std::string::npos || lower.find("unknown option") != std::string::npos)
            {
                event.text += "\nThe installed Codex may be too old for these options; update it with `npm install -g @openai/codex`.";
            }
        }
        events.push_back(std::move(event));
    }
}
