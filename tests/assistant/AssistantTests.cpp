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
#include <imgui/imgui.h>

#include <nlohmann/json.hpp>

#include <myengine/assistant/AssistantService.h>
#include <myengine/assistant/ClaudeCliBackend.h>
#include <myengine/assistant/ClaudeStreamParser.h>
#include <myengine/ui/AssistantPanel.h>

namespace
{
    namespace assistant = myengine::assistant;
    namespace ui = myengine::ui;
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

    std::filesystem::path ExecutableDirectory()
    {
        wchar_t executable[32768]{};
        Check(GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))) != 0, "Executable path unavailable");
        return std::filesystem::path(executable).parent_path();
    }

    void SetMode(const wchar_t* mode)
    {
        SetEnvironmentVariableW(L"FAKE_CLAUDE_MODE", mode);
    }

    // Working directory has Cyrillic and a space: the same trap as C:\Users\<name>
    class Fixture
    {
    public:
        Fixture()
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            directory = std::filesystem::temp_directory_path() / std::filesystem::u8path("myengine AI1 ассистент " + std::to_string(stamp));
            Check(std::filesystem::create_directory(directory), "Could not create a test directory");
            fake = ExecutableDirectory() / "myengine_fake_claude.exe";
            Check(std::filesystem::exists(fake), "myengine_fake_claude.exe was not built next to the test");
        }

        ~Fixture()
        {
            SetMode(nullptr);
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }

        assistant::ClaudeCliConfig Config() const
        {
            assistant::ClaudeCliConfig config;
            config.executable = fake;
            config.workingDirectory = directory;
            config.stateDirectory = directory / "state";
            config.maxBudgetUsd = 1.5;
            config.maxTurns = 7;
            return config;
        }

        std::filesystem::path directory;
        std::filesystem::path fake;
    };

    // Polls until `done` or the timeout; returns everything that arrived
    template <typename Predicate>
    std::vector<assistant::AssistantEvent> Collect(assistant::IAssistantBackend& backend, const Predicate& done, const int timeoutMs = 20000)
    {
        std::vector<assistant::AssistantEvent> events;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline)
        {
            backend.Poll(events);
            if (done(events))
            {
                return events;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw std::runtime_error("Timed out waiting for the backend");
    }

    bool HasType(const std::vector<assistant::AssistantEvent>& events, const EventType type)
    {
        for (const auto& event : events)
        {
            if (event.type == type)
            {
                return true;
            }
        }
        return false;
    }

    const assistant::AssistantEvent* Find(const std::vector<assistant::AssistantEvent>& events, const EventType type)
    {
        for (const auto& event : events)
        {
            if (event.type == type)
            {
                return &event;
            }
        }
        return nullptr;
    }

    void WaitIdle(assistant::AssistantService& service, const int timeoutMs = 20000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (service.IsBusy())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                for (const auto& message : service.GetMessages())
                {
                    std::cerr << "  [" << static_cast<int>(message.kind) << "] " << message.text.substr(0, 200) << '\n';
                }
                throw std::runtime_error("The assistant turn did not finish in time");
            }
            service.Update();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        service.Update();
    }

    const assistant::AssistantMessage* FindMessage(const assistant::AssistantService& service, const Kind kind)
    {
        for (const auto& message : service.GetMessages())
        {
            if (message.kind == kind)
            {
                return &message;
            }
        }
        return nullptr;
    }

    std::string LastAssistantText(const assistant::AssistantService& service)
    {
        std::string text;
        for (const auto& message : service.GetMessages())
        {
            if (message.kind == Kind::Assistant)
            {
                text = message.text;
            }
        }
        return text;
    }

    class ScriptedBackend final : public assistant::IAssistantBackend
    {
    public:
        const char* GetName() const override { return "Scripted"; }
        std::string CheckAvailability() override { return unavailable; }
        bool BeginTurn(const std::string&, const std::string& session, std::string&) override
        {
            lastSession = session;
            busy = true;
            return true;
        }
        void Cancel() override { busy = false; queued.clear(); }
        bool IsBusy() const override { return busy; }
        void Poll(std::vector<assistant::AssistantEvent>& events) override
        {
            for (auto& event : queued)
            {
                if (event.type == EventType::Finished || event.type == EventType::Error)
                {
                    busy = false;
                }
                events.push_back(std::move(event));
            }
            queued.clear();
        }
        void Shutdown() override { busy = false; }

        std::string unavailable;
        std::string lastSession;
        std::vector<assistant::AssistantEvent> queued;
        bool busy = false;
    };

    void TestParser()
    {
        assistant::ClaudeStreamParser parser;
        parser.SetRootDirectory("C:\\Users\\Test\\repo");
        std::vector<assistant::AssistantEvent> events;
        const auto feed = [&](const json& line) { parser.ParseLine(line.dump(), events); };

        feed({{"type", "system"}, {"subtype", "init"}, {"session_id", "abc-123"}, {"model", "model-x"}});
        feed({{"type", "stream_event"}, {"event", {{"type", "message_start"}, {"message", {{"id", "m1"}}}}}});
        feed({{"type", "stream_event"}, {"event", {{"type", "content_block_delta"}, {"delta", {{"type", "text_delta"}, {"text", "Он"}}}}}});
        feed({{"type", "stream_event"}, {"event", {{"type", "content_block_delta"}, {"delta", {{"type", "text_delta"}, {"text", "лайн"}}}}}});
        feed({{"type", "stream_event"}, {"event", {{"type", "content_block_delta"}, {"delta", {{"type", "input_json_delta"}, {"partial_json", "{"}}}}}});
        parser.ParseLine("this is not json", events);
        parser.ParseLine("{\"type\": \"assist", events); // cut line
        parser.ParseLine("", events);
        feed({{"type", "assistant"}, {"message", {{"id", "m1"}, {"content", json::array({{{"type", "text"}, {"text", "Онлайн"}}})}}}});
        feed({{"type", "assistant"}, {"message", {{"id", "m2"}, {"content", json::array({
            {{"type", "text"}, {"text", "Not streamed"}},
            {{"type", "tool_use"}, {"id", "t1"}, {"name", "Edit"},
                {"input", {{"file_path", "c:\\users\\TEST\\Repo\\assets\\prefabs\\coin.prefab.json"}, {"old_string", "120.0"}, {"new_string", "300.0"}}}},
            {{"type", "tool_use"}, {"id", "t2"}, {"name", "Grep"}, {"input", {{"pattern", "spin_speed"}, {"path", "assets"}}}},
            {{"type", "tool_use"}, {"id", "t3"}, {"name", "Write"}, {"input", {{"file_path", "C:\\other\\x.py"}, {"content", "a\nb\n"}}}},
        })}}}});
        feed({{"type", "assistant"}, {"parent_tool_use_id", "sub-1"}, {"message", {{"id", "m9"}, {"content", json::array({{{"type", "text"}, {"text", "subagent"}}})}}}});
        feed({{"type", "user"}, {"message", {{"content", json::array({
            {{"type", "tool_result"}, {"tool_use_id", "t1"}, {"content", "updated"}},
            {{"type", "tool_result"}, {"tool_use_id", "t2"}, {"content", json::array({{{"type", "text"}, {"text", "a.py:1\nb.py:2\nc.py:3\nd.py:4"}}})}},
            {{"type", "tool_result"}, {"tool_use_id", "t3"}, {"content", "denied"}, {"is_error", true}},
        })}}}});
        feed({{"type", "result"}, {"subtype", "success"}, {"is_error", false}, {"result", "done"}, {"session_id", "abc-123"},
            {"total_cost_usd", 0.25}, {"duration_ms", 2500}, {"num_turns", 3},
            {"usage", {{"input_tokens", 100}, {"cache_creation_input_tokens", 10}, {"cache_read_input_tokens", 1000}, {"output_tokens", 40}}}});

        Check(parser.IsFinished(), "The result line did not finish the turn");
        std::string streamed;
        std::vector<const assistant::AssistantEvent*> tools;
        std::vector<const assistant::AssistantEvent*> results;
        for (const auto& event : events)
        {
            if (event.type == EventType::TextDelta)
            {
                streamed += event.text + "|";
            }
            else if (event.type == EventType::ToolUse)
            {
                tools.push_back(&event);
            }
            else if (event.type == EventType::ToolResult)
            {
                results.push_back(&event);
            }
        }
        Check(events.front().type == EventType::SessionStarted && events.front().sessionId == "abc-123" && events.front().text == "model-x",
            "Init did not produce SessionStarted");
        Check(streamed == "Он|лайн|Not streamed|", "Streamed text was duplicated, lost, or a subagent leaked in");
        Check(tools.size() == 3, "Tool calls were not parsed");
        Check(tools[0]->toolName == "Edit" && tools[0]->text == "assets/prefabs/coin.prefab.json", "The edited path was not shown relative to the root");
        Check(tools[0]->detail == "- 120.0\n+ 300.0\n", "Edit diff is wrong");
        Check(tools[1]->text == "spin_speed  in assets" && tools[1]->detail.empty(), "Grep summary is wrong");
        Check(tools[2]->detail == "+ a\n+ b\n" && tools[2]->text == "C:/other/x.py", "Write preview or path outside the root is wrong");
        Check(results.size() == 3, "Tool results were not parsed");
        Check(results[0]->filePath == "assets/prefabs/coin.prefab.json" && !results[0]->isError, "A successful edit did not report its file");
        Check(results[1]->filePath.empty() && results[1]->text == "a.py:1\nb.py:2\nc.py:3 ...", "A read-only tool result was not shortened");
        Check(results[2]->isError && results[2]->filePath.empty(), "A failed edit reported a changed file");

        const auto& finished = events.back();
        Check(finished.type == EventType::Finished && !finished.isError && finished.text == "done", "Result was not parsed");
        Check(finished.costUsd == 0.25 && finished.durationMs == 2500 && finished.turns == 3, "Result numbers are wrong");
        Check(finished.inputTokens == 1110 && finished.outputTokens == 40, "Token usage is wrong");

        assistant::ClaudeStreamParser errors;
        std::vector<assistant::AssistantEvent> errorEvents;
        errors.ParseLine(json{{"type", "result"}, {"subtype", "error_max_turns"}, {"is_error", true}, {"result", ""}}.dump(), errorEvents);
        Check(errorEvents.size() == 1 && errorEvents[0].isError && errorEvents[0].text == "error_max_turns", "An error result was not reported");
    }

    void TestCommandLineAndSettings(Fixture& fixture)
    {
        auto config = fixture.Config();
        const auto line = assistant::ClaudeCliBackend::BuildCommandLine(fixture.fake, config, config.stateDirectory / "s.json", "sess-1");
        const auto contains = [&](const wchar_t* part) { return line.find(part) != std::wstring::npos; };
        Check(contains(L" -p ") && contains(L"--input-format stream-json") && contains(L"--output-format stream-json") && contains(L"--verbose"),
            "The command line misses the headless flags");
        Check(contains(L"--include-partial-messages") && contains(L"--permission-mode dontAsk"), "Streaming or permission flags are missing");
        Check(contains(L"--tools \"Read,Glob,Grep,Edit,Write\"") && contains(L"--disallowedTools \"mcp__*\""), "The tool restrictions are missing");
        Check(contains(L"--max-turns 7") && contains(L"--max-budget-usd 1.50"), "Limits are missing");
        Check(contains(L"--resume sess-1") && contains(L"--settings \""), "Resume or settings flag is missing");
        Check(line.find(L"--append-system-prompt-file") == std::wstring::npos, "A missing prompt file must not be passed");

        const auto hostile = assistant::ClaudeCliBackend::BuildCommandLine(fixture.fake, config, "s.json", "x\" --dangerously-skip-permissions");
        Check(hostile.find(L"dangerously") == std::wstring::npos && hostile.find(L"--resume") == std::wstring::npos, "A bad session id reached the command line");
        Check(!assistant::ClaudeCliBackend::IsValidSessionId("") && !assistant::ClaudeCliBackend::IsValidSessionId("a b") &&
            assistant::ClaudeCliBackend::IsValidSessionId("0f8fad5b-d9cb-469f-a165-70867728950e"), "Session id validation is wrong");

        const auto prompt = fixture.directory / "prompt.md";
        std::ofstream(prompt) << "x";
        config.systemPromptFile = prompt;
        const auto withPrompt = assistant::ClaudeCliBackend::BuildCommandLine(fixture.fake, config, "s.json", "");
        Check(withPrompt.find(L"--append-system-prompt-file") != std::wstring::npos && withPrompt.find(L"--resume") == std::wstring::npos,
            "The system prompt flag is wrong");

        const auto settings = json::parse(assistant::ClaudeCliBackend::BuildSettingsJson(config.editableGlobs));
        const auto& allow = settings.at("permissions").at("allow");
        Check(allow.size() == 2 && allow[0] == "Edit(./assets/scripts/**)" && allow[1] == "Edit(./assets/prefabs/**)", "Edit rules are wrong");
        bool bashDenied = false;
        for (const auto& rule : settings.at("permissions").at("deny"))
        {
            bashDenied = bashDenied || rule == "Bash";
        }
        Check(bashDenied, "Bash is not denied");

        // With the engine bridge: prompts go to the editor, only read-only tools are pre-allowed, no file rule is
        auto bridged = fixture.Config();
        bridged.bridge.executable = fixture.fake;
        bridged.bridge.pipeName = L"\\\\.\\pipe\\myengine-test";
        bridged.bridge.token = "abc123";
        bridged.bridge.allowedTools = {"mcp__myengine__get_mode", "mcp__myengine__list_entities"};
        bridged.bridge.approveTool = "mcp__myengine__approve";
        const auto bridgedLine = assistant::ClaudeCliBackend::BuildCommandLine(fixture.fake, bridged, "s.json", "sess-2", "mcp.json");
        const auto bridgedHas = [&](const wchar_t* part) { return bridgedLine.find(part) != std::wstring::npos; };
        Check(bridgedHas(L"--permission-mode default") && bridgedHas(L"--mcp-config \"mcp.json\"") && bridgedHas(L"--strict-mcp-config") &&
            bridgedHas(L"--permission-prompt-tool mcp__myengine__approve") && !bridgedHas(L"dontAsk"), "The bridge flags are wrong");
        Check(bridgedHas(L"--resume sess-2") && bridgedHas(L"--tools \"Read,Glob,Grep,Edit,Write\""), "The bridge command line lost the basics");
        const auto bridgedSettings = json::parse(assistant::ClaudeCliBackend::BuildSettingsJson(bridged));
        Check(bridgedSettings.at("permissions").at("allow") == json::array({"mcp__myengine__get_mode", "mcp__myengine__list_entities"}),
            "With the bridge only the read-only engine tools may be pre-allowed (file edits must ask)");
        bool scenesDenied = false;
        for (const auto& rule : bridgedSettings.at("permissions").at("deny"))
        {
            scenesDenied = scenesDenied || rule == "Edit(./assets/scenes/**)";
        }
        Check(scenesDenied, "Scene files are not denied");
        const auto mcp = json::parse(assistant::ClaudeCliBackend::BuildMcpConfigJson(bridged.bridge));
        const auto& server = mcp.at("mcpServers").at("myengine");
        Check(server.at("args")[0] == "--pipe" && server.at("args")[1] == "\\\\.\\pipe\\myengine-test" &&
            server.at("env").at("MYENGINE_BRIDGE_TOKEN") == "abc123" && server.at("command") == fixture.fake.u8string(), "The MCP config is wrong");

        const auto message = json::parse(assistant::ClaudeCliBackend::BuildUserMessageLine("привет \"мир\"\nвторая строка"));
        Check(message.at("type") == "user" && message.at("message").at("content")[0].at("text") == "привет \"мир\"\nвторая строка",
            "The stdin message does not round-trip");
    }

    void TestNotFound(Fixture& fixture)
    {
        auto config = fixture.Config();
        config.executable = fixture.directory / "missing" / "claude.exe";
        auto backend = std::make_unique<assistant::ClaudeCliBackend>(config);
        const auto problem = backend->CheckAvailability();
        Check(!problem.empty() && problem.find("does not exist") != std::string::npos, "A missing CLI path was not reported");
        assistant::AssistantService service(std::move(backend));
        service.Update();
        Check(!service.GetAvailabilityMessage().empty(), "The service did not learn that the CLI is missing");
        Check(!service.Send("hello") && !service.IsBusy(), "Send worked without a CLI");
        const auto* error = FindMessage(service, Kind::Error);
        Check(error != nullptr && error->text == problem, "The panel has no message about the missing CLI");
        Check(FindMessage(service, Kind::User) == nullptr, "A rejected message was added to the transcript");
    }

    void TestFullTurnAndResume(Fixture& fixture)
    {
        SetMode(L"normal");
        assistant::AssistantService service(std::make_unique<assistant::ClaudeCliBackend>(fixture.Config()));
        Check(service.Send("  где лежат скрипты?  "), "Send failed");
        Check(service.IsBusy() && !service.Send("second"), "A second message started during a turn");
        WaitIdle(service);

        Check(service.GetSessionId() == "fake-session-1", "The session id was not stored");
        const auto* user = FindMessage(service, Kind::User);
        Check(user != nullptr && user->text == "где лежат скрипты?", "The user message is wrong");
        const auto answer = LastAssistantText(service);
        Check(answer.rfind("Hello world. cwd=", 0) == 0, "The streamed answer is wrong (duplicated?)");
        Check(answer.find("--settings") != std::string::npos && answer.find("--resume") == std::string::npos, "First turn arguments are wrong");
        const auto* tool = FindMessage(service, Kind::Tool);
        Check(tool != nullptr && tool->toolName == "Edit" && tool->toolDone && !tool->toolFailed, "The tool call is not shown as done");
        Check(tool->detail.find("- \"spin_speed\": 120.0") != std::string::npos && tool->detail.find("+ \"spin_speed\": 300.0") != std::string::npos,
            "The diff is missing");
        const auto* files = FindMessage(service, Kind::Files);
        Check(files != nullptr && files->text == "assets/prefabs/coin.prefab.json", "The changed files list is wrong");
        const auto* summary = FindMessage(service, Kind::Summary);
        Check(summary != nullptr && summary->text.find("$0.012") != std::string::npos && summary->text.find("100 in / 5 out") != std::string::npos &&
                summary->text.find("1.5 s") != std::string::npos,
            "The turn summary is wrong");
        Check(service.GetTotalCostUsd() > 0.012 && service.GetTotalCostUsd() < 0.013, "Cost was not accumulated");

        Check(service.Send("and one more thing"), "The second turn did not start");
        WaitIdle(service);
        Check(LastAssistantText(service).find("--resume fake-session-1") != std::string::npos, "The second turn did not resume the session");
        Check(service.GetTotalCostUsd() > 0.024, "Cost of the second turn was not added");

        service.NewConversation();
        Check(service.GetMessages().empty() && service.GetSessionId().empty() && service.GetTotalCostUsd() == 0.0, "New chat did not reset the state");
        Check(service.Send("fresh"), "Send failed after a new chat");
        WaitIdle(service);
        Check(LastAssistantText(service).find("--resume") == std::string::npos, "A new chat resumed the old session");
    }

    void TestCancel(Fixture& fixture)
    {
        SetMode(L"hang");
        assistant::ClaudeCliBackend backend(fixture.Config());
        std::string error;
        Check(backend.BeginTurn("wait", "", error), "BeginTurn failed");
        const auto events = Collect(backend, [](const auto& e) { return HasType(e, EventType::SessionStarted); });
        const auto model = Find(events, EventType::SessionStarted)->text;
        Check(model.rfind("fake-", 0) == 0, "The fake CLI did not report its pid");
        const DWORD pid = static_cast<DWORD>(std::stoul(model.substr(5)));

        std::vector<assistant::AssistantEvent> idle;
        double worstPollMs = 0.0;
        for (int index = 0; index < 50; ++index)
        {
            const auto begin = std::chrono::steady_clock::now();
            backend.Poll(idle);
            const auto spent = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            worstPollMs = std::max(worstPollMs, spent);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Check(worstPollMs < 50.0, "Poll blocked while the CLI was silent");
        Check(backend.IsBusy() && idle.empty(), "The hanging turn ended on its own");

        const auto begin = std::chrono::steady_clock::now();
        backend.Cancel();
        const auto cancelMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        Check(!backend.IsBusy() && cancelMs < 2000.0, "Cancel was slow or left the backend busy");
        if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid))
        {
            const DWORD wait = WaitForSingleObject(process, 5000);
            CloseHandle(process);
            Check(wait == WAIT_OBJECT_0, "The CLI process survived Cancel");
        }
        std::vector<assistant::AssistantEvent> after;
        backend.Poll(after);
        Check(after.empty(), "Events arrived after Cancel");

        // The same through the service: Stop leaves the editor usable and a new turn can start
        assistant::AssistantService service(std::make_unique<assistant::ClaudeCliBackend>(fixture.Config()));
        Check(service.Send("wait again"), "Send failed");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (service.GetSessionId().empty())
        {
            Check(std::chrono::steady_clock::now() < deadline, "The session did not start");
            service.Update();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        service.Cancel();
        Check(!service.IsBusy(), "The service is busy after Stop");
        const auto& last = service.GetMessages().back();
        Check(last.kind == Kind::Notice && last.text == "Stopped.", "Stop did not leave a note");
        SetMode(L"normal");
        Check(service.Send("after stop"), "Could not send after Stop");
        WaitIdle(service);
        Check(LastAssistantText(service).find("--resume fake-session-1") != std::string::npos, "The conversation did not continue after Stop");
    }

    void TestCrashSplitAndErrors(Fixture& fixture)
    {
        SetMode(L"crash");
        {
            assistant::ClaudeCliBackend backend(fixture.Config());
            std::string error;
            Check(backend.BeginTurn("x", "", error), "BeginTurn failed");
            const auto events = Collect(backend, [](const auto& e) { return HasType(e, EventType::Error); });
            const auto* failure = Find(events, EventType::Error);
            Check(failure->text.find("exit code 3") != std::string::npos && failure->text.find("boom") != std::string::npos &&
                failure->text.find("claude update") != std::string::npos, "A crash was not explained");
            Check(!backend.IsBusy(), "The backend stayed busy after a crash");
        }

        SetMode(L"split");
        {
            assistant::ClaudeCliBackend backend(fixture.Config());
            std::string error;
            Check(backend.BeginTurn("x", "", error), "BeginTurn failed");
            const auto events = Collect(backend, [](const auto& e) { return HasType(e, EventType::Finished); });
            const auto* init = Find(events, EventType::SessionStarted);
            Check(init != nullptr && init->sessionId == "fake-session-1", "A line split between reads was lost");
        }

        SetMode(L"error_result");
        {
            assistant::AssistantService service(std::make_unique<assistant::ClaudeCliBackend>(fixture.Config()));
            Check(service.Send("x"), "Send failed");
            WaitIdle(service);
            const auto* failure = FindMessage(service, Kind::Error);
            Check(failure != nullptr && failure->text == "error_max_turns", "An error result was not shown");
            Check(!service.IsBusy() && service.Send("again"), "The panel is stuck after an error result");
            WaitIdle(service);
        }
    }

    void TestServiceBounds()
    {
        auto scripted = std::make_unique<ScriptedBackend>();
        auto* backend = scripted.get();
        assistant::AssistantService service(std::move(scripted));
        Check(service.Send("go"), "Send failed");
        Check(backend->lastSession.empty(), "A new conversation carried a session id");
        for (int index = 0; index < 800; ++index)
        {
            assistant::AssistantEvent event;
            event.type = EventType::ToolUse;
            event.toolId = "t" + std::to_string(index);
            event.toolName = "Read";
            event.text = "file" + std::to_string(index);
            backend->queued.push_back(std::move(event));
        }
        assistant::AssistantEvent text;
        text.type = EventType::TextDelta;
        text.text = std::string(300 * 1024, 'x');
        backend->queued.push_back(text);
        backend->queued.push_back(text);
        service.Update();
        Check(service.GetMessages().size() <= 500, "The transcript is not bounded");
        Check(service.GetMessages().back().text.size() < 700 * 1024 / 2, "A single message is not bounded");

        assistant::AssistantEvent finished;
        finished.type = EventType::Finished;
        finished.sessionId = "s-2";
        backend->queued.push_back(finished);
        service.Update();
        Check(!service.IsBusy() && service.GetSessionId() == "s-2", "Finished did not end the turn");
        Check(service.Send("next") && backend->lastSession == "s-2", "The next turn did not carry the session id");
    }

    class PanelFixture
    {
    public:
        PanelFixture()
        {
            IMGUI_CHECKVERSION();
            context = ImGui::CreateContext();
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.DisplaySize = ImVec2(900.0f, 700.0f);
            io.DeltaTime = 1.0f / 60.0f;
            unsigned char* pixels = nullptr;
            int width = 0;
            int height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        }

        ~PanelFixture()
        {
            ImGui::DestroyContext(context);
        }

        void Frame(ui::AssistantPanel& panel)
        {
            panel.Update();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_Always);
            ImGui::Begin("Assistant test", nullptr, ImGuiWindowFlags_NoSavedSettings);
            panel.Draw();
            ImGui::End();
            ImGui::Render();
            Check(ImGui::GetDrawData()->TotalVtxCount > 0, "The panel did not draw anything");
        }

        ImGuiContext* context = nullptr;
    };

    void TestPanel(Fixture& fixture)
    {
        PanelFixture imgui;

        // CLI missing: the panel explains it and does not crash
        {
            auto scripted = std::make_unique<ScriptedBackend>();
            scripted->unavailable = "Claude Code CLI was not found. Install it.";
            ui::AssistantPanel panel(std::move(scripted));
            imgui.Frame(panel);
            imgui.Frame(panel);
            Check(!panel.GetService().GetAvailabilityMessage().empty(), "The panel did not check availability");
        }

        // A real turn through the fake CLI, drawn frame by frame
        SetMode(L"normal");
        {
            ui::AssistantPanel panel(std::make_unique<assistant::ClaudeCliBackend>(fixture.Config()));
            imgui.Frame(panel);
            Check(panel.GetService().Send("draw me"), "Send failed");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (panel.GetService().IsBusy())
            {
                Check(std::chrono::steady_clock::now() < deadline, "The turn did not finish");
                imgui.Frame(panel);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            imgui.Frame(panel);
            Check(FindMessage(panel.GetService(), Kind::Files) != nullptr, "The panel turn did not report changed files");
            panel.GetService().NewConversation();
            imgui.Frame(panel);
        }

        // A full transcript (500 messages, ~1 MB of text) must not make a frame expensive
        {
            auto scripted = std::make_unique<ScriptedBackend>();
            auto* backend = scripted.get();
            ui::AssistantPanel panel(std::move(scripted));
            Check(panel.GetService().Send("fill"), "Send failed");
            for (int index = 0; index < 250; ++index)
            {
                assistant::AssistantEvent tool;
                tool.type = EventType::ToolUse;
                tool.toolId = "t" + std::to_string(index);
                tool.toolName = "Edit";
                tool.text = "assets/scripts/file" + std::to_string(index) + ".py";
                tool.detail = "- old line\n+ new line\n";
                backend->queued.push_back(std::move(tool));
                assistant::AssistantEvent text;
                text.type = EventType::TextDelta;
                text.text = std::string(4000, 'a') + " ";
                backend->queued.push_back(std::move(text));
                // A tool call ends the text block: the next TextDelta starts a new message
            }
            panel.Update();
            Check(panel.GetService().GetMessages().size() == 500, "The transcript was not filled");
            imgui.Frame(panel);
            const auto begin = std::chrono::steady_clock::now();
            constexpr int kFrames = 20;
            for (int frame = 0; frame < kFrames; ++frame)
            {
                imgui.Frame(panel);
            }
            const auto perFrameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() / kFrames;
            std::cout << "    full transcript: " << perFrameMs << " ms per frame\n";
            Check(perFrameMs < 100.0, "A frame with a full transcript is too slow");
        }

        // The panel built from a repository root creates the CLI backend without starting anything
        {
            ui::AssistantPanelConfig config;
            config.repositoryRoot = fixture.directory;
            ui::AssistantPanel panel(config);
            imgui.Frame(panel);
            Check(!panel.GetService().IsBusy(), "The panel started a process by itself");
        }
    }
}

int main()
{
    int result = 1;
    try
    {
        Fixture fixture;
        const auto run = [](const char* name, const auto& test) { test(); std::cout << "  ok: " << name << '\n'; };
        run("parser", [&] { TestParser(); });
        run("command line and settings", [&] { TestCommandLineAndSettings(fixture); });
        run("CLI not found", [&] { TestNotFound(fixture); });
        run("full turn and resume", [&] { TestFullTurnAndResume(fixture); });
        run("cancel", [&] { TestCancel(fixture); });
        run("crash, split line, error result", [&] { TestCrashSplitAndErrors(fixture); });
        run("bounded transcript", [&] { TestServiceBounds(); });
        run("panel", [&] { TestPanel(fixture); });
        std::cout << "OK: AI1 stream parser, CLI arguments and settings, pipes without blocking, resume, Stop, crash handling, bounded transcript, panel\n";
        result = 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    return result;
}
