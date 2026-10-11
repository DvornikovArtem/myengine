// AS4: the Codex CLI backend (fake_codex over the real pipes and CreateProcess), its event parser, the chat history of
// its threads and the per-user assistant settings.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <windows.h>

#include <nlohmann/json.hpp>

#include <myengine/assistant/AssistantService.h>
#include <myengine/assistant/AssistantSettings.h>
#include <myengine/assistant/ChatStore.h>
#include <myengine/assistant/CodexCliBackend.h>
#include <myengine/assistant/CodexStreamParser.h>

namespace
{
    namespace assistant = myengine::assistant;
    namespace fs = std::filesystem;
    using json = nlohmann::json;
    using Kind = assistant::AssistantMessage::Kind;
    using EventType = assistant::AssistantEventType;

    void Check(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    fs::path ExecutableDirectory()
    {
        wchar_t executable[32768]{};
        Check(GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))) != 0, "Executable path unavailable");
        return fs::path(executable).parent_path();
    }

    void SetMode(const wchar_t* mode)
    {
        SetEnvironmentVariableW(L"FAKE_CODEX_MODE", mode);
    }

    // A working folder with Cyrillic and a space: the same trap as C:\Users\<name>
    class Fixture
    {
    public:
        Fixture()
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            directory = fs::temp_directory_path() / fs::u8path("myengine AS4 ассистент " + std::to_string(stamp));
            Check(fs::create_directory(directory), "Could not create a test directory");
            fake = ExecutableDirectory() / "myengine_fake_codex.exe";
            Check(fs::exists(fake), "myengine_fake_codex.exe was not built next to the test");
            std::ofstream(directory / "system.md", std::ios::binary) << "SYSTEM_MARKER: you work in the editor.\n";
        }

        ~Fixture()
        {
            SetMode(nullptr);
            std::error_code error;
            fs::remove_all(directory, error);
        }

        assistant::CodexCliConfig Config() const
        {
            assistant::CodexCliConfig config;
            config.executable = fake;
            config.workingDirectory = directory;
            config.systemPromptFiles = {directory / "system.md"};
            config.transcriptDirectory = directory / "codex";
            return config;
        }

        fs::path directory;
        fs::path fake;
    };

    void WaitIdle(assistant::AssistantService& service, const int timeoutMs = 20000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        do
        {
            service.Update();
            if (!service.IsBusy())
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("The assistant turn did not finish in time");
    }

    const assistant::AssistantMessage* FindMessage(const assistant::AssistantService& service, const Kind kind, const std::string& contains = {})
    {
        for (const auto& message : service.GetMessages())
        {
            if (message.kind == kind && message.text.find(contains) != std::string::npos)
            {
                return &message;
            }
        }
        return nullptr;
    }

    const assistant::AssistantMessage* FindTool(const assistant::AssistantService& service, const std::string& name)
    {
        for (const auto& message : service.GetMessages())
        {
            if (message.kind == Kind::Tool && message.toolName == name)
            {
                return &message;
            }
        }
        return nullptr;
    }

    // ---------------------------------------------------------------- parser
    void TestParser()
    {
        assistant::CodexStreamParser parser;
        parser.SetRootDirectory("C:/proj");
        parser.SetModel("gpt-x");
        std::vector<assistant::AssistantEvent> events;
        const auto feed = [&](const json& line) { parser.ParseLine(line.dump(), events); };

        parser.ParseLine("Reading prompt from stdin...", events);
        parser.ParseLine("{ not json", events);
        Check(events.empty(), "A banner or a broken line produced an event");

        feed({{"type", "thread.started"}, {"thread_id", "019-abc"}});
        Check(events.size() == 1 && events[0].type == EventType::SessionStarted && events[0].sessionId == "019-abc" && events[0].text == "gpt-x",
            "thread.started is wrong");

        feed({{"type", "turn.started"}});
        feed({{"type", "item.completed"}, {"item", {{"id", "a"}, {"type", "reasoning"}, {"text", "hmm"}}}});
        feed({{"type", "item.started"}, {"item", {{"id", "b"}, {"type", "command_execution"}, {"command", "bash -lc \"Get-ChildItem\""}, {"status", "in_progress"}}}});
        feed({{"type", "item.completed"}, {"item", {{"id", "b"}, {"type", "command_execution"}, {"command", "bash -lc \"Get-ChildItem\""},
                                                    {"aggregated_output", "one\ntwo\nthree\nfour\n"}, {"exit_code", 1}, {"status", "failed"}}}});
        feed({{"type", "item.completed"}, {"item", {{"id", "c"}, {"type", "agent_message"}, {"text", "first"}}}});
        feed({{"type", "item.completed"}, {"item", {{"id", "d"}, {"type", "agent_message"}, {"text", "second"}}}});
        feed({{"type", "item.started"}, {"item", {{"id", "e"}, {"type", "mcp_tool_call"}, {"server", "myengine"}, {"tool", "write_file"},
                                                  {"arguments", {{"path", "C:\\proj\\assets\\scripts\\x.py"}, {"content", "a\nb\n"}}}, {"status", "in_progress"}}}});
        feed({{"type", "item.completed"}, {"item", {{"id", "e"}, {"type", "mcp_tool_call"}, {"server", "myengine"}, {"tool", "write_file"},
                                                    {"arguments", {{"path", "C:\\proj\\assets\\scripts\\x.py"}, {"content", "a\nb\n"}}}, {"status", "completed"},
                                                    {"result", {{"content", json::array({{{"type", "text"}, {"text", "Done"}}})}}}}}});
        feed({{"type", "item.completed"}, {"item", {{"id", "f"}, {"type", "file_change"}, {"status", "completed"},
                                                    {"changes", json::array({{{"path", "C:\\proj\\assets\\prefabs\\a.prefab.json"}, {"kind", "update"}},
                                                                             {{"path", "assets/prefabs/b.prefab.json"}, {"kind", "add"}}})}}}});
        feed({{"type", "item.completed"}, {"item", {{"id", "g"}, {"type", "todo_list"}, {"items", json::array()}}}});
        feed({{"type", "error"}, {"message", "Reconnecting... 2/5"}});
        feed({{"type", "turn.completed"}, {"usage", {{"input_tokens", 100}, {"cached_input_tokens", 50}, {"output_tokens", 7}, {"reasoning_output_tokens", 3}}}});
        Check(parser.IsFinished() && parser.GetThreadId() == "019-abc", "turn.completed did not finish the turn");

        std::vector<std::string> kinds;
        for (const auto& event : events)
        {
            kinds.push_back(std::to_string(static_cast<int>(event.type)));
        }
        const auto& e = events;
        std::size_t index = 1;
        Check(e[index].type == EventType::Thinking && e[index].text == "hmm", "reasoning is wrong");
        ++index;
        Check(e[index].type == EventType::ToolUse && e[index].toolName == "Shell" && e[index].text == "Get-ChildItem" && e[index].toolId == "b",
            "The shell call is wrong (the bash -lc wrapper must be cut)");
        ++index;
        Check(e[index].type == EventType::ToolResult && e[index].isError && e[index].text.find("one") != std::string::npos &&
            e[index].text.find("four") == std::string::npos, "The shell result is wrong (failed, three lines)");
        ++index;
        Check(e[index].type == EventType::TextDelta && e[index].text == "first", "The first message is wrong");
        ++index;
        Check(e[index].type == EventType::TextDelta && e[index].text == "\n\nsecond", "Two messages in a row must be separated");
        ++index;
        Check(e[index].type == EventType::ToolUse && e[index].toolName == "mcp__myengine__write_file" &&
            e[index].filePath == "assets/scripts/x.py" && e[index].detail.find("+ a") != std::string::npos,
            "An MCP file tool is wrong (name, relative path, diff)");
        ++index;
        Check(e[index].type == EventType::ToolResult && !e[index].isError && e[index].filePath == "assets/scripts/x.py" && e[index].text == "Done",
            "The MCP result is wrong");
        ++index;
        Check(e[index].type == EventType::ToolUse && e[index].toolName == "apply_patch" && e[index].text.find("a.prefab.json") != std::string::npos,
            "file_change is wrong");
        ++index;
        Check(e[index].type == EventType::ToolResult && e[index].filePath == "assets/prefabs/a.prefab.json", "file_change result: first file");
        ++index;
        Check(e[index].type == EventType::ToolResult && e[index].filePath == "assets/prefabs/b.prefab.json", "file_change result: second file");
        ++index;
        Check(e[index].type == EventType::Notice && e[index].text.find("Reconnecting") != std::string::npos, "An error event is not a notice");
        ++index;
        Check(e[index].type == EventType::Finished && !e[index].isError && e[index].inputTokens == 100 && e[index].outputTokens == 10 &&
            e[index].sessionId == "019-abc", "turn.completed is wrong");
        Check(index + 1 == e.size(), "An unknown item produced an event");

        // A failed turn
        assistant::CodexStreamParser failing;
        std::vector<assistant::AssistantEvent> failed;
        failing.ParseLine(R"({"type":"turn.failed","error":{"message":"boom"}})", failed);
        Check(failing.IsFinished() && failed.size() == 1 && failed[0].isError && failed[0].text == "boom", "turn.failed is wrong");
    }

    // ---------------------------------------------------------------- command line
    void TestCommandLine(const Fixture& fixture)
    {
        using Backend = assistant::CodexCliBackend;
        auto config = fixture.Config();
        config.model = "gpt-default";
        config.bridge.executable = fs::path("C:\\Program Files\\myengine\\myengine_mcp.exe");
        config.bridge.pipeName = L"\\\\.\\pipe\\myengine-assistant-77";
        config.bridge.token = "tok123";

        const auto line = Backend::BuildCommandLine(fs::path("C:\\tools\\codex.exe"), config, "", "", "", {});
        const auto has = [&](const std::wstring& text) { return line.find(text) != std::wstring::npos; };
        Check(line.rfind(L"\"C:\\tools\\codex.exe\" exec --json", 0) == 0, "The command line does not start with `codex exec --json`");
        Check(has(L"--skip-git-repo-check") && has(L"--sandbox read-only"), "The sandbox flags are missing");
        Check(has(L"approval_policy=never"), "approval_policy is not set");
        Check(has(L"--model gpt-default"), "The configured model is not passed");
        Check(has(L"mcp_servers.myengine.command='C:\\Program Files\\myengine\\myengine_mcp.exe'"), "The bridge command is missing");
        Check(has(L"mcp_servers.myengine.args=['--pipe', '\\\\.\\pipe\\myengine-assistant-77']"), "The bridge arguments are missing");
        Check(has(L"mcp_servers.myengine.env={ MYENGINE_BRIDGE_TOKEN = 'tok123' }"), "The bridge token is missing");
        Check(has(L"tool_timeout_sec=900") && has(L"default_tools_approval_mode='approve'"), "The bridge timeouts or approval mode are missing");
        Check(line.size() >= 2 && line.substr(line.size() - 2) == L" -" && !has(L" resume "), "A new conversation must end with `-` and have no resume");
        Check(!has(L"--dangerously-bypass") && !has(L"danger-full-access") && !has(L"workspace-write"), "The sandbox is not read-only");

        const auto resumed = Backend::BuildCommandLine(fs::path("codex.exe"), config, "0199a213-81c0", "o3", "high",
                                                       {fs::path("C:\\pics\\a b.png")});
        const auto hasResumed = [&](const std::wstring& text) { return resumed.find(text) != std::wstring::npos; };
        Check(hasResumed(L" resume 0199a213-81c0 -") && hasResumed(L"--model o3") && hasResumed(L"model_reasoning_effort=high") &&
            hasResumed(L"--image \"C:\\pics\\a b.png\"") && !hasResumed(L"gpt-default"), "The resume command line is wrong");

        // Values that would break a command line or a TOML value are not passed
        const auto unsafe = Backend::BuildCommandLine(fs::path("codex.exe"), config, "a b;calc", "x\" --yolo", "ultra", {});
        Check(unsafe.find(L"calc") == std::wstring::npos && unsafe.find(L"--yolo") == std::wstring::npos &&
            unsafe.find(L"ultra") == std::wstring::npos && unsafe.find(L"--model gpt-default") != std::wstring::npos,
            "An unsafe session id, model or effort reached the command line");

        Check(Backend::TomlString("C:\\a b\\c.exe") == "'C:\\a b\\c.exe'", "A path must be a literal string");
        Check(Backend::TomlString("it's") == "\"it's\"", "A text with a quote must be a basic string");
        Check(Backend::TomlString("a\\b'c") == "\"a\\\\b'c\"", "A basic string must escape backslashes");

        // The prompt: instructions only for a new conversation, then the message, then the files
        assistant::AssistantTurnRequest request;
        request.prompt = "hello";
        assistant::AssistantAttachment file;
        file.path = (fixture.directory / "notes.txt").u8string();
        file.name = "notes.txt";
        request.attachments.push_back(file);
        const auto first = Backend::BuildPrompt(request, fixture.Config(), true);
        Check(first.rfind("SYSTEM_MARKER", 0) == 0 && first.find("hello") != std::string::npos && first.find("notes.txt") != std::string::npos,
            "The first prompt does not have the instructions, the message and the file list");
        const auto next = Backend::BuildPrompt(request, fixture.Config(), false);
        Check(next.rfind("hello", 0) == 0, "A later prompt repeated the instructions");
    }

    // ---------------------------------------------------------------- not found
    void TestNotFound(const Fixture& fixture)
    {
        auto config = fixture.Config();
        config.executable = fixture.directory / "no_such_codex.exe";
        assistant::CodexCliBackend backend(config);
        const auto message = backend.CheckAvailability();
        Check(message.find("Codex") != std::string::npos && message.find("does not exist") != std::string::npos, "The missing path was not reported");

        assistant::AssistantService service(std::make_unique<assistant::CodexCliBackend>(config));
        Check(!service.Send("hello"), "A message was sent without a CLI");
        Check(FindMessage(service, Kind::Error, "Codex") != nullptr, "The user got no message about the missing CLI");
        Check(!service.IsBusy(), "The service is busy after a failed start");

        // No override: the message tells how to install it
        std::string problem;
        SetEnvironmentVariableW(L"MYENGINE_CODEX_PATH", (fixture.directory / "gone.exe").wstring().c_str());
        assistant::CodexCliBackend::FindExecutable({}, problem);
        SetEnvironmentVariableW(L"MYENGINE_CODEX_PATH", nullptr);
        Check(problem.find("MYENGINE_CODEX_PATH") != std::string::npos, "A wrong MYENGINE_CODEX_PATH was not reported");
    }

    // ---------------------------------------------------------------- a full turn, resume, history
    void TestTurnResumeHistory(const Fixture& fixture)
    {
        const auto index = fixture.directory / "chats.json";
        const auto claudeSessions = fixture.directory / "claude-sessions";
        fs::create_directories(claudeSessions);
        std::string threadId;
        {
            assistant::AssistantService service(std::make_unique<assistant::CodexCliBackend>(fixture.Config()));
            service.SetChatStore(std::make_unique<assistant::ChatStore>(index, claudeSessions, fixture.directory / "codex"));
            service.SetRootDirectory(fixture.directory.u8string());
            Check(service.Send("change the spin speed"), "Send failed");
            Check(service.IsBusy(), "The turn did not start");
            WaitIdle(service);

            threadId = service.GetSessionId();
            Check(threadId == "fake-thread-1", "The thread id was not taken from thread.started");
            const auto* answer = FindMessage(service, Kind::Assistant, "answer:");
            Check(answer != nullptr, "The answer is missing");
            Check(answer->text.find("exec --json") != std::string::npos && answer->text.find("--sandbox read-only") != std::string::npos,
                "The fake CLI did not get the arguments");
            Check(answer->text.find("stdin: SYSTEM_MARKER") != std::string::npos, "The first prompt did not carry the instructions");
            Check(FindMessage(service, Kind::Thinking, "Thinking") != nullptr, "The reasoning is missing");

            const auto* shell = FindTool(service, "Shell");
            Check(shell != nullptr && shell->toolDone && !shell->toolFailed && shell->text == "ls", "The shell call is wrong");
            const auto* edit = FindTool(service, "mcp__myengine__edit_file");
            Check(edit != nullptr && edit->toolDone && !edit->toolFailed && edit->text == "assets/scripts/coin.py" && !edit->detail.empty(),
                "The edit call is wrong");
            const auto* rejected = FindTool(service, "mcp__myengine__get_mode");
            Check(rejected != nullptr && rejected->toolFailed, "A failed MCP call is not shown as failed");
            const auto* files = FindMessage(service, Kind::Files);
            Check(files != nullptr && files->text.find("assets/scripts/coin.py") != std::string::npos, "The changed file is not reported");
            const auto* summary = FindMessage(service, Kind::Summary);
            Check(summary != nullptr && summary->text.find("1.2k in / 40 out") != std::string::npos, "The turn footer is wrong");
            Check(service.GetTotalInputTokens() == 1200, "The tokens were not counted");

            // The second message continues the same thread without the instructions
            Check(service.Send("and the second field"), "The second Send failed");
            WaitIdle(service);
            const auto& messages = service.GetMessages();
            const assistant::AssistantMessage* second = nullptr;
            for (auto it = messages.rbegin(); it != messages.rend(); ++it)
            {
                if (it->kind == Kind::Assistant)
                {
                    second = &*it;
                    break;
                }
            }
            Check(second != nullptr && second->text.find("resume fake-thread-1 -") != std::string::npos, "The second turn did not resume the thread");
            Check(second->text.find("stdin: and the second field") != std::string::npos, "The resumed prompt carried the instructions");
        }

        // A new session of the editor: the list and the transcript come from the files
        {
            assistant::ChatStore store(index, claudeSessions, fixture.directory / "codex");
            store.Refresh();
            const auto chats = store.Chats();
            Check(chats.size() == 1 && chats[0].id == threadId && chats[0].backend == "codex" && chats[0].title == "change the spin speed",
                "The Codex chat is not in the list (id, backend, title)");
            Check(chats[0].inputTokens == 2400, "The tokens of the chat were not summed");

            std::deque<assistant::AssistantMessage> messages;
            std::string error;
            Check(store.LoadTranscript(threadId, messages, fixture.directory.u8string(), 500, error), error.c_str());
            std::size_t users = 0;
            std::size_t answers = 0;
            std::size_t tools = 0;
            std::size_t summaries = 0;
            for (const auto& message : messages)
            {
                users += message.kind == Kind::User ? 1 : 0;
                answers += message.kind == Kind::Assistant ? 1 : 0;
                tools += message.kind == Kind::Tool ? 1 : 0;
                summaries += message.kind == Kind::Summary ? 1 : 0;
            }
            Check(users == 2 && answers == 2 && tools == 6 && summaries == 2, "The replayed transcript is wrong");
            Check(messages.front().kind == Kind::User && messages.front().text == "change the spin speed", "The transcript does not start with the user");

            // The same chat in a service whose backend is Claude: it must not be resumed by the wrong CLI
            assistant::AssistantService service(std::make_unique<assistant::CodexCliBackend>(fixture.Config()));
            service.SetChatStore(std::make_unique<assistant::ChatStore>(index, claudeSessions, fixture.directory / "codex"));
            std::string openError;
            Check(service.OpenChat(threadId, openError) && service.GetSessionId() == threadId, "The chat did not open");
            Check(service.Send("one more"), "A Codex chat refused a Codex backend");
            WaitIdle(service);
        }
    }

    void TestMismatchedChat(const Fixture& fixture)
    {
        // A session of Claude Code in the index: Codex must refuse to continue it
        const auto index = fixture.directory / "chats-mismatch.json";
        const auto sessions = fixture.directory / "claude-sessions-2";
        fs::create_directories(sessions);
        {
            std::ofstream session(sessions / "claude-aaa.jsonl", std::ios::binary);
            session << R"({"type":"user","entrypoint":"sdk-cli","timestamp":"2026-10-11T10:00:00.000Z","message":{"role":"user","content":"old question"}})" << '\n';
            session << R"({"type":"assistant","timestamp":"2026-10-11T10:00:05.000Z","message":{"model":"claude-opus-5-5","content":[{"type":"text","text":"old answer"}]}})" << '\n';
        }
        assistant::AssistantService service(std::make_unique<assistant::CodexCliBackend>(fixture.Config()));
        service.SetChatStore(std::make_unique<assistant::ChatStore>(index, sessions, fixture.directory / "codex-2"));
        std::string error;
        Check(service.OpenChat("claude-aaa", error), error.c_str());
        Check(!service.Send("continue"), "Codex continued a Claude Code chat");
        Check(FindMessage(service, Kind::Error, "Claude Code") != nullptr, "The user was not told which CLI the chat belongs to");
        Check(!service.IsBusy(), "A refused message started a process");
    }

    // ---------------------------------------------------------------- stop, crash, failed turn
    void TestCancelCrashFail(const Fixture& fixture)
    {
        {
            SetMode(L"hang");
            assistant::AssistantService service(std::make_unique<assistant::CodexCliBackend>(fixture.Config()));
            Check(service.Send("wait"), "Send failed");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            service.Update();
            Check(service.IsBusy(), "The hanging turn ended");
            service.Cancel();
            Check(!service.IsBusy() && FindMessage(service, Kind::Notice, "Stopped") != nullptr, "Stop did not end the turn");
        }
        {
            SetMode(L"crash");
            assistant::AssistantService service(std::make_unique<assistant::CodexCliBackend>(fixture.Config()));
            Check(service.Send("hi"), "Send failed");
            WaitIdle(service);
            const auto* error = FindMessage(service, Kind::Error, "ended without an answer");
            Check(error != nullptr && error->text.find("codex login") != std::string::npos, "A crash is not reported with the sign-in hint");
        }
        {
            SetMode(L"fail");
            assistant::AssistantService service(std::make_unique<assistant::CodexCliBackend>(fixture.Config()));
            Check(service.Send("hi"), "Send failed");
            WaitIdle(service);
            Check(FindMessage(service, Kind::Error, "overloaded") != nullptr, "turn.failed is not shown as an error");
            Check(FindMessage(service, Kind::Notice, "Reconnecting") != nullptr, "The retry notice is missing");
        }
        SetMode(nullptr);
    }

    // ---------------------------------------------------------------- settings
    void TestSettings(const Fixture& fixture)
    {
        const auto file = fixture.directory / "ui" / "editor_settings.json";
        std::string problem;
        auto settings = assistant::AssistantSettings::Load(file, &problem);
        Check(settings.backend == assistant::AssistantBackendKind::None && problem.empty() && settings.codexModel.empty(),
            "A missing file must give the defaults (off) without a problem");
        Check(assistant::AssistantSettings::Load({}).backend == assistant::AssistantBackendKind::None, "An empty path must give the defaults");

        // The file has other keys: they stay
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << R"({"theme":"dark","assistant":{"backend":"claude","extra":1}})";
        settings = assistant::AssistantSettings::Load(file);
        Check(settings.backend == assistant::AssistantBackendKind::Claude, "backend=claude was not read");

        settings.backend = assistant::AssistantBackendKind::Codex;
        settings.codexPath = fs::u8path("C:\\Users\\пользователь\\codex.cmd");
        settings.codexModel = "gpt-test";
        settings.allowExternalMcp = true;
        std::string error;
        Check(settings.Save(file, error), error.c_str());
        const auto loaded = assistant::AssistantSettings::Load(file);
        Check(loaded.backend == assistant::AssistantBackendKind::Codex && loaded.codexPath == settings.codexPath &&
            loaded.codexModel == "gpt-test" && loaded.allowExternalMcp, "The settings did not survive a round trip");
        Check(!assistant::AssistantSettings{}.allowExternalMcp, "External MCP clients must be off by default");
        std::ifstream stream(file, std::ios::binary);
        const json document = json::parse(stream);
        Check(document.value("theme", std::string()) == "dark" && document["assistant"]["backend"] == "codex",
            "Saving lost another key of the file");

        // Garbage gives the defaults and a reason
        std::ofstream(file, std::ios::binary | std::ios::trunc) << "{ broken";
        problem.clear();
        Check(assistant::AssistantSettings::Load(file, &problem).backend == assistant::AssistantBackendKind::None && !problem.empty(),
            "A broken file was not reported");
        Check(assistant::ParseBackendKind("nonsense") == assistant::AssistantBackendKind::None, "An unknown backend must be off");
    }
}

int main()
{
    int result = 1;
    try
    {
        Fixture fixture;
        const auto run = [](const char* name, const auto& test) { test(); std::cout << "  ok: " << name << '\n'; };
        run("event parser", [&] { TestParser(); });
        run("command line, prompt, TOML values", [&] { TestCommandLine(fixture); });
        run("CLI not found", [&] { TestNotFound(fixture); });
        run("turn, resume, history", [&] { TestTurnResumeHistory(fixture); });
        run("chat of another CLI", [&] { TestMismatchedChat(fixture); });
        run("stop, crash, failed turn", [&] { TestCancelCrashFail(fixture); });
        run("settings", [&] { TestSettings(fixture); });
        std::cout << "OK: Codex backend: event parser, read-only command line, resume, history of threads, other CLI's chats, settings\n";
        result = 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    return result;
}
