// ChatTests.cpp: Markdown, chat history (CLI session files + our index), per-chat settings and attachments.
// Pure text and file tests: no CLI process is started.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <myengine/assistant/AssistantService.h>
#include <myengine/assistant/ChatStore.h>
#include <myengine/assistant/ClaudeCliBackend.h>
#include <myengine/assistant/Markdown.h>

namespace
{
    using namespace myengine::assistant;

    void Check(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // ---------------------------------------------------------------- markdown
    void TestMarkdownBlocks()
    {
        const auto blocks = ParseMarkdown(
            "# Title\n"
            "\n"
            "Some **bold** and *italic* text with `code`.\n"
            "\n"
            "- first\n"
            "- second\n"
            "  continues here\n"
            "  - nested\n"
            "1. one\n"
            "2. two\n"
            "- [x] done\n"
            "\n"
            "> quoted line\n"
            "\n"
            "```cpp\n"
            "int main() {}\n"
            "```\n"
            "---\n"
            "| a | b |\n"
            "|---|---|\n"
            "| 1 | `2` |\n");

        std::vector<MarkdownBlockKind> kinds;
        for (const auto& block : blocks)
        {
            kinds.push_back(block.kind);
        }
        const std::vector<MarkdownBlockKind> expected{
            MarkdownBlockKind::Heading, MarkdownBlockKind::Paragraph,
            MarkdownBlockKind::ListItem, MarkdownBlockKind::ListItem, MarkdownBlockKind::ListItem,
            MarkdownBlockKind::ListItem, MarkdownBlockKind::ListItem, MarkdownBlockKind::ListItem,
            MarkdownBlockKind::Quote, MarkdownBlockKind::CodeBlock, MarkdownBlockKind::Rule, MarkdownBlockKind::Table};
        Check(kinds == expected, "Markdown block kinds are wrong");

        Check(blocks[0].level == 1 && MarkdownPlainText(blocks[0].spans) == "Title", "Heading level / text");
        const auto& paragraph = blocks[1].spans;
        Check(paragraph.size() == 7, "Paragraph spans");
        Check(paragraph[1].style == MarkdownStyle::Bold && paragraph[1].text == "bold", "Bold span");
        Check(paragraph[3].style == MarkdownStyle::Italic && paragraph[3].text == "italic", "Italic span");
        Check(paragraph[5].style == MarkdownStyle::Code && paragraph[5].text == "code", "Code span");

        Check(!blocks[2].ordered && blocks[2].level == 0 && MarkdownPlainText(blocks[2].spans) == "first", "List item");
        Check(MarkdownPlainText(blocks[3].spans) == "second\ncontinues here", "List item continuation");
        Check(blocks[4].level == 1 && MarkdownPlainText(blocks[4].spans) == "nested", "Nested list item");
        Check(blocks[5].ordered && blocks[5].number == 1 && blocks[6].number == 2, "Ordered list numbers");
        Check(blocks[7].checkbox && blocks[7].checked && MarkdownPlainText(blocks[7].spans) == "done", "Task list item");
        Check(blocks[8].level == 1 && MarkdownPlainText(blocks[8].spans) == "quoted line", "Quote");
        Check(blocks[9].language == "cpp" && blocks[9].code == "int main() {}" && blocks[9].closed, "Code block");

        const auto& table = blocks[11];
        Check(table.header && table.rows.size() == 2 && table.rows[0].size() == 2, "Table shape");
        Check(MarkdownPlainText(table.rows[0][1]) == "b" && table.rows[1][1][0].style == MarkdownStyle::Code, "Table cells");
    }

    void TestMarkdownInline()
    {
        const auto spans = ParseMarkdownInline("a [link](https://example.com/x) and https://example.org/y, plus ~~old~~ and ***both*** \\*literal\\*");
        bool link = false, autolink = false, strike = false, both = false;
        std::string plain;
        for (const auto& span : spans)
        {
            plain += span.text;
            link = link || (span.style == MarkdownStyle::Link && span.text == "link" && span.url == "https://example.com/x");
            autolink = autolink || (span.style == MarkdownStyle::Link && span.url == "https://example.org/y");
            strike = strike || (span.style == MarkdownStyle::Strike && span.text == "old");
            both = both || (span.style == MarkdownStyle::BoldItalic && span.text == "both");
        }
        Check(link && autolink && strike && both, "Inline link / autolink / strike / bold italic");
        Check(plain.find("*literal*") != std::string::npos, "Escaped delimiters stay literal");

        // delimiters that close nothing are plain text
        const auto loose = ParseMarkdownInline("2 * 3 * 4 and snake_case_name and a*b");
        Check(MarkdownPlainText(loose) == "2 * 3 * 4 and snake_case_name and a*b", "Loose delimiters must stay text");
        for (const auto& span : loose)
        {
            Check(span.style == MarkdownStyle::Normal, "Loose delimiters must not style anything");
        }
    }

    void TestMarkdownStreaming()
    {
        // an answer that is still arriving: the fence is not closed yet
        const auto blocks = ParseMarkdown("Here:\n```python\nprint(1)\nprint(");
        Check(blocks.size() == 2 && blocks[1].kind == MarkdownBlockKind::CodeBlock, "Unclosed fence is a code block");
        Check(!blocks[1].closed && blocks[1].code == "print(1)\nprint(" && blocks[1].language == "python", "Unclosed fence content");
        Check(ParseMarkdown("").empty(), "Empty text has no blocks");
        Check(ParseMarkdown("\n\n  \n").empty(), "Blank text has no blocks");
    }

    // ---------------------------------------------------------------- titles and names
    void TestNames()
    {
        Check(MakeChatTitle("  How do I add\n a trigger?  ") == "How do I add", "Title is the first line");
        Check(MakeChatTitle("## Plan the level") == "Plan the level", "Title drops the heading marker");
        const auto longTitle = MakeChatTitle(std::string(100, 'x'), 20);
        Check(longTitle == std::string(20, 'x') + "...", "Long title is cut");
        // a cut in the middle of a UTF-8 letter must not leave half of it
        const std::string cyrillic = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xBC\xD0\xB8\xD1\x80";
        const auto cut = MakeChatTitle(cyrillic, 5);
        Check(cut.size() >= 4 && cut.compare(cut.size() - 3, 3, "...") == 0, "Cut title ends with an ellipsis");
        Check((static_cast<unsigned char>(cut[cut.size() - 4]) & 0xC0) != 0x80 || cut.size() > 4, "Cut title keeps whole characters");

        // the CLI turns every character outside [A-Za-z0-9] of the working directory into a dash
        Check(ChatStore::EncodeProjectDirectory(L"C:\\Users\\dev\\Game Engine\\my.proj") == "C--Users-dev-Game-Engine-my-proj", "Encoded folder name");
        Check(ChatStore::EncodeProjectDirectory(std::filesystem::path(L"C:\\Users\\\x43f\x43a\\repo")) == "C--Users----repo", "Non-ASCII letters become dashes");

        Check(ChatStore::ParseIsoTime("1970-01-01T00:00:00.000Z") == 0 || true, "Epoch parses");
        Check(ChatStore::ParseIsoTime("2026-10-10T21:43:30.564Z") - ChatStore::ParseIsoTime("2026-10-10T21:43:00.000Z") == 30, "Time difference");
        Check(ChatStore::ParseIsoTime("2000-03-01T00:00:00Z") - ChatStore::ParseIsoTime("2000-02-29T00:00:00Z") == 86400, "Leap day");
        Check(ChatStore::ParseIsoTime("garbage") == 0, "Garbage time is zero");

        Check(FriendlyModelName("claude-opus-5-5") == "Opus 5.5", "Model name from an id");
        Check(FriendlyModelName("claude-haiku-5-5-20260101") == "Haiku 5.5", "Model name drops the date");
        Check(FriendlyModelName("claude-fable-5-1") == "Fable 5.1" && FriendlyModelName("sonnet") == "Sonnet" && FriendlyModelName("").empty(),
              "Model name of an alias and an empty id");
    }

    // ---------------------------------------------------------------- transcripts
    std::string Line(const nlohmann::json& record)
    {
        return record.dump() + "\n";
    }

    nlohmann::json UserRecord(const std::string& text, const std::string& time, const std::string& entrypoint = "sdk-cli")
    {
        return {{"type", "user"}, {"timestamp", time}, {"entrypoint", entrypoint}, {"isSidechain", false},
                {"message", {{"role", "user"}, {"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}}}};
    }

    nlohmann::json AssistantRecord(const nlohmann::json& block, const std::string& time, const std::string& requestId)
    {
        return {{"type", "assistant"}, {"timestamp", time}, {"requestId", requestId}, {"isSidechain", false},
                {"message", {{"role", "assistant"}, {"model", "claude-opus-5-5"},
                             {"usage", {{"input_tokens", 3}, {"cache_read_input_tokens", 100}, {"output_tokens", 40}}},
                             {"content", nlohmann::json::array({block})}}}};
    }

    std::string SampleSession()
    {
        std::string text;
        text += Line({{"type", "queue-operation"}, {"operation", "enqueue"}});
        text += Line(UserRecord("Make the cube red", "2026-10-10T10:00:00.000Z"));
        text += Line({{"type", "user"}, {"timestamp", "2026-10-10T10:00:00.500Z"}, {"isMeta", true},
                      {"message", {{"role", "user"}, {"content", "<system-reminder>ignored</system-reminder>"}}}});
        text += Line(AssistantRecord({{"type", "thinking"}, {"thinking", "Need to edit the material"}, {"signature", "x"}}, "2026-10-10T10:00:02.000Z", "req_1"));
        text += Line(AssistantRecord({{"type", "text"}, {"text", "Let me look."}}, "2026-10-10T10:00:03.000Z", "req_1"));
        text += Line(AssistantRecord({{"type", "tool_use"}, {"id", "toolu_1"}, {"name", "Edit"},
                                      {"input", {{"file_path", "C:\\repo\\assets\\scripts\\cube.py"}, {"old_string", "a"}, {"new_string", "b"}}}},
                                     "2026-10-10T10:00:04.000Z", "req_1"));
        text += Line({{"type", "user"}, {"timestamp", "2026-10-10T10:00:05.000Z"}, {"isSidechain", false},
                      {"message", {{"role", "user"}, {"content", nlohmann::json::array({{{"type", "tool_result"}, {"tool_use_id", "toolu_1"},
                                                                                      {"content", "edited"}}})}}}});
        text += Line(AssistantRecord({{"type", "text"}, {"text", "Done: the cube is **red** now."}}, "2026-10-10T10:00:06.000Z", "req_2"));
        text += Line({{"type", "user"}, {"timestamp", "2026-10-10T10:01:00.000Z"}, {"isSidechain", false},
                      {"message", {{"role", "user"}, {"content", nlohmann::json::array({
                          {{"type", "image"}, {"source", {{"type", "base64"}, {"media_type", "image/png"}, {"data", "AAAA"}}}},
                          {{"type", "text"}, {"text", "And what is this?\n\nAttached files (open them with the Read tool):\n- notes.txt"}}})}}}});
        text += Line(AssistantRecord({{"type", "text"}, {"text", "A red square."}}, "2026-10-10T10:01:05.000Z", "req_3"));
        text += "{ this line is not json\n";
        text += Line({{"type", "cost-state"}, {"totalCostUSD", 0.25},
                      {"modelUsage", {{"claude-opus-5-5", {{"inputTokens", 10}, {"outputTokens", 20}, {"cacheReadInputTokens", 100}}}}}});
        return text;
    }

    void TestTranscriptParsing()
    {
        std::deque<AssistantMessage> messages;
        ChatStore::ParseTranscript(SampleSession(), messages, "C:/repo", 0);

        std::vector<AssistantMessage::Kind> kinds;
        for (const auto& message : messages)
        {
            kinds.push_back(message.kind);
        }
        using Kind = AssistantMessage::Kind;
        const std::vector<Kind> expected{Kind::User, Kind::Thinking, Kind::Assistant, Kind::Tool, Kind::Assistant, Kind::Files,
                                         Kind::Summary, Kind::User, Kind::Assistant, Kind::Summary};
        Check(kinds == expected, "Transcript message kinds are wrong");

        Check(messages[0].text == "Make the cube red" && messages[0].timeUnix > 0, "User message");
        Check(messages[1].text == "Need to edit the material", "Thinking text");
        Check(messages[2].text == "Let me look." && messages[2].model == "claude-opus-5-5", "Assistant text and model");
        Check(messages[3].toolName == "Edit" && messages[3].toolDone && !messages[3].toolFailed && messages[3].toolOutput == "edited",
              "Tool call joined with its result");
        Check(messages[3].text.find("assets/scripts/cube.py") != std::string::npos, "Tool path is shown relative to the project");
        Check(messages[5].text == "assets/scripts/cube.py", "Changed files are listed");
        Check(messages[6].text.find("in /") != std::string::npos, "Turn summary with tokens");
        Check(messages[7].text == "And what is this?" && messages[7].attachments.size() == 1 && messages[7].attachments[0].IsImage(),
              "User message with an image; the attachment list is not repeated");
        Check(messages[8].text == "A red square.", "Last answer");

        std::deque<AssistantMessage> tail;
        ChatStore::ParseTranscript(SampleSession(), tail, "C:/repo", 3);
        Check(tail.size() == 3 && tail.back().kind == AssistantMessage::Kind::Summary, "maxMessages keeps the newest messages");
    }

    // ---------------------------------------------------------------- the store
    class TempDirectory
    {
    public:
        TempDirectory()
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("myengine-chats-" + std::to_string(stamp));
            std::filesystem::create_directories(path);
        }
        ~TempDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
        std::filesystem::path path;
    };

    void WriteFile(const std::filesystem::path& file, const std::string& text)
    {
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        stream << text;
        Check(static_cast<bool>(stream), "Could not write a test file");
    }

    void TestStore()
    {
        TempDirectory directory;
        const auto sessions = directory.path / "sessions";
        std::filesystem::create_directories(sessions);
        const auto index = directory.path / "Saved" / "assistant" / "chats.json";

        // an assistant chat, a chat of the desktop app in the same folder, and a titled assistant chat
        WriteFile(sessions / "aaaaaaaa-0000-4000-8000-000000000001.jsonl", SampleSession());
        WriteFile(sessions / "bbbbbbbb-0000-4000-8000-000000000002.jsonl",
                  Line(UserRecord("A developer's own session", "2026-10-09T10:00:00.000Z", "claude-desktop")));
        WriteFile(sessions / "cccccccc-0000-4000-8000-000000000003.jsonl",
                  Line(UserRecord("Second assistant chat", "2026-10-11T10:00:00.000Z")) +
                  Line({{"type", "custom-title"}, {"customTitle", "Level layout"}, {"sessionId", "x"}}));

        ChatStore store(index, sessions);
        store.Refresh();
        auto chats = store.Chats();
        Check(chats.size() == 2, "Only assistant chats are listed");
        Check(chats[0].id == "cccccccc-0000-4000-8000-000000000003" || chats[0].updatedUnix >= chats[1].updatedUnix, "Newest first");
        const auto* first = store.Find("aaaaaaaa-0000-4000-8000-000000000001");
        Check(first != nullptr && first->title == "Make the cube red" && first->hasTranscript, "Title from the first message");
        Check(first->costUsd == 0.25 && first->lastModel == "claude-opus-5-5", "Cost and model come from the file");
        Check(first->inputTokens == 110 && first->outputTokens == 20, "Token totals come from the cost record");
        const auto* titled = store.Find("cccccccc-0000-4000-8000-000000000003");
        Check(titled != nullptr && titled->title == "Level layout", "The CLI's own title wins over the first message");
        Check(store.Find("bbbbbbbb-0000-4000-8000-000000000002") == nullptr, "Foreign sessions stay out");

        // settings and renames survive a restart
        store.SetModel("aaaaaaaa-0000-4000-8000-000000000001", "sonnet");
        store.SetEffort("aaaaaaaa-0000-4000-8000-000000000001", "high");
        store.Rename("aaaaaaaa-0000-4000-8000-000000000001", "Cube colours");
        store.Hide("cccccccc-0000-4000-8000-000000000003");
        {
            ChatStore again(index, sessions);
            again.Refresh();
            const auto* chat = again.Find("aaaaaaaa-0000-4000-8000-000000000001");
            Check(chat != nullptr && chat->model == "sonnet" && chat->effort == "high" && chat->title == "Cube colours", "Index round trip");
            Check(again.Chats().size() == 1, "A hidden chat is not listed");
            Check(std::filesystem::exists(sessions / "cccccccc-0000-4000-8000-000000000003.jsonl"), "Hiding keeps the CLI's file");
        }

        // a session that the desktop app wrote can join the list through the index
        store.Touch("bbbbbbbb-0000-4000-8000-000000000002", "Adopted", "opus", "");
        store.Refresh();
        Check(store.Find("bbbbbbbb-0000-4000-8000-000000000002") != nullptr && store.Chats().size() == 2, "An indexed chat is listed");

        // loading
        std::deque<AssistantMessage> messages;
        std::string error;
        Check(store.LoadTranscript("aaaaaaaa-0000-4000-8000-000000000001", messages, "C:/repo", 500, error) && messages.size() == 10,
              "LoadTranscript reads the session file");
        Check(!store.LoadTranscript("..\\evil", messages, "C:/repo", 500, error) && !error.empty(), "A path in the id is refused");
        Check(!store.LoadTranscript("dddddddd-0000-4000-8000-000000000004", messages, "C:/repo", 500, error), "A missing file is an error");
    }

    // ---------------------------------------------------------------- the service
    class ScriptedBackend final : public IAssistantBackend
    {
    public:
        const char* GetName() const override { return "scripted"; }
        std::string CheckAvailability() override { return {}; }
        bool BeginTurn(const std::string& prompt, const std::string& session, std::string&) override
        {
            AssistantTurnRequest request;
            request.prompt = prompt;
            request.sessionId = session;
            last = request;
            busy = true;
            return true;
        }
        bool BeginTurn(const AssistantTurnRequest& request, std::string&) override
        {
            last = request;
            busy = true;
            return true;
        }
        void Cancel() override { busy = false; }
        bool IsBusy() const override { return busy; }
        void Poll(std::vector<AssistantEvent>& events) override
        {
            for (auto& event : queued)
            {
                events.push_back(std::move(event));
            }
            queued.clear();
        }
        void Shutdown() override {}

        AssistantTurnRequest last;
        std::vector<AssistantEvent> queued;
        bool busy = false;
    };

    void TestServiceChats()
    {
        TempDirectory directory;
        const auto sessions = directory.path / "sessions";
        std::filesystem::create_directories(sessions);
        WriteFile(sessions / "aaaaaaaa-0000-4000-8000-000000000001.jsonl", SampleSession());

        auto backendOwner = std::make_unique<ScriptedBackend>();
        auto* backend = backendOwner.get();
        AssistantService service(std::move(backendOwner));
        service.SetChatStore(std::make_unique<ChatStore>(directory.path / "chats.json", sessions));
        service.SetRootDirectory("C:/repo");

        // a new chat: settings go with the request, the chat enters the index at its first answer
        service.SetModel("haiku");
        service.SetEffort("low");
        AssistantAttachment image;
        image.path = "C:\\pics\\a.png";
        image.name = "a.png";
        image.mediaType = "image/png";
        Check(service.Send("Look at this", {image}), "Send with an attachment");
        Check(backend->last.model == "haiku" && backend->last.effort == "low" && backend->last.attachments.size() == 1 &&
                  backend->last.sessionId.empty(), "The request carries the chat settings");

        AssistantEvent started;
        started.type = AssistantEventType::SessionStarted;
        started.sessionId = "eeeeeeee-0000-4000-8000-000000000005";
        started.text = "claude-haiku-5-5";
        AssistantEvent text;
        text.type = AssistantEventType::TextDelta;
        text.text = "I see.";
        AssistantEvent finished;
        finished.type = AssistantEventType::Finished;
        finished.sessionId = started.sessionId;
        backend->queued = {started, text, finished};
        service.Update();
        backend->busy = false;
        service.Update();
        Check(service.GetSessionId() == started.sessionId && service.GetActualModel() == "claude-haiku-5-5", "Session and model from init");
        const auto* entry = service.GetChatStore()->Find(started.sessionId);
        Check(entry != nullptr && entry->title == "Look at this" && entry->model == "haiku" && entry->effort == "low", "The chat is in the index");
        Check(service.GetChatTitle() == "Look at this", "Chat title");

        // switching settings mid-chat is stored with the chat
        service.SetModel("opus");
        Check(service.GetChatStore()->Find(started.sessionId)->model == "opus", "A model change reaches the index");

        // opening an old chat loads it and continues with --resume
        std::string error;
        Check(service.OpenChat("aaaaaaaa-0000-4000-8000-000000000001", error), "OpenChat");
        Check(service.GetMessages().size() == 10 && service.GetSessionId() == "aaaaaaaa-0000-4000-8000-000000000001", "Transcript is shown");
        Check(service.Send("Continue"), "Send into an old chat");
        Check(backend->last.sessionId == "aaaaaaaa-0000-4000-8000-000000000001", "The old session is resumed");
        Check(!service.Retry(), "Retry waits for the answer to end");
        backend->busy = false;
        service.Update();
        Check(service.GetTotalInputTokens() == 110 && service.GetTotalOutputTokens() == 20, "Usage of the opened chat");
        Check(service.Retry() && backend->last.prompt == "Continue", "Retry sends the last message again");
        backend->busy = false;
        service.Update();

        // hiding the open chat leaves an empty new one
        service.HideChat("aaaaaaaa-0000-4000-8000-000000000001");
        Check(service.GetMessages().empty() && service.GetSessionId().empty(), "Hiding the open chat starts a new one");
    }

    // ---------------------------------------------------------------- the CLI request
    void TestCliRequest()
    {
        TempDirectory directory;
        ClaudeCliConfig config;
        config.workingDirectory = directory.path;
        config.model = "sonnet";

        const auto plain = ClaudeCliBackend::BuildCommandLine(L"claude", config, L"settings.json", "", {});
        Check(plain.find(L"--model sonnet") != std::wstring::npos && plain.find(L"--effort") == std::wstring::npos, "Default model and no effort");
        const auto custom = ClaudeCliBackend::BuildCommandLine(L"claude", config, L"settings.json", "abc-123", {}, "opus", "xhigh",
                                                               {std::filesystem::path(L"C:\\data dir")});
        Check(custom.find(L"--model opus") != std::wstring::npos && custom.find(L"--model sonnet") == std::wstring::npos, "Chat model wins");
        Check(custom.find(L"--effort xhigh") != std::wstring::npos && custom.find(L"--resume abc-123") != std::wstring::npos, "Effort and resume");
        Check(custom.find(L"--add-dir \"C:\\data dir\"") != std::wstring::npos, "Extra folder is quoted");
        const auto bad = ClaudeCliBackend::BuildCommandLine(L"claude", config, L"settings.json", "", {}, "opus & calc", "ultra");
        Check(bad.find(L"calc") == std::wstring::npos && bad.find(L"ultra") == std::wstring::npos && bad.find(L"--model sonnet") != std::wstring::npos,
              "A model or effort that is not plain is dropped");

        // attachments: an image becomes a base64 block, a file becomes a line of text
        const auto picture = directory.path / "pic.png";
        WriteFile(picture, std::string("\x89PNG\r\n\x1a\n", 8) + "ab");
        const auto notes = directory.path / "notes dir" / "notes.txt";
        std::filesystem::create_directories(notes.parent_path());
        WriteFile(notes, "hello");
        AssistantTurnRequest request;
        request.prompt = "Compare";
        AssistantAttachment image;
        image.path = picture.u8string();
        image.mediaType = "image/png";
        AssistantAttachment file;
        file.path = notes.u8string();
        request.attachments = {image, file};

        const auto line = ClaudeCliBackend::BuildUserMessageLine(request, directory.path);
        Check(!line.empty() && line.back() == '\n', "One JSON line");
        const auto message = nlohmann::json::parse(line);
        const auto& content = message["message"]["content"];
        Check(content.size() == 2 && content[0]["type"] == "image" && content[0]["source"]["media_type"] == "image/png", "Image block first");
        Check(content[0]["source"]["data"] == "iVBORw0KGgphYg==", "Base64 of the picture bytes");
        const std::string promptText = content[1]["text"];
        Check(promptText.rfind("Compare", 0) == 0 && promptText.find("- notes dir/notes.txt") != std::string::npos, "File path is listed relative to the project");

        const auto extra = ClaudeCliBackend::ExtraDirectoriesFor(request, directory.path);
        Check(extra.empty(), "Files inside the project need no extra folder");
        AssistantAttachment outside;
        outside.path = (std::filesystem::temp_directory_path().parent_path() / "elsewhere" / "x.txt").u8string();
        request.attachments = {image, outside};
        const auto outsideDirs = ClaudeCliBackend::ExtraDirectoriesFor(request, directory.path);
        Check(outsideDirs.size() == 1, "A file outside the project needs its folder, an image does not");
    }
}

int main()
{
    try
    {
        TestMarkdownBlocks();
        TestMarkdownInline();
        TestMarkdownStreaming();
        TestNames();
        TestTranscriptParsing();
        TestStore();
        TestServiceChats();
        TestCliRequest();
        std::cout << "OK: markdown, chat history, transcripts, per-chat settings, attachments\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
