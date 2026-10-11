// ChatStore.h

#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <myengine/assistant/AssistantService.h>

namespace myengine::assistant
{
    // One conversation of the assistant. The session files of the Claude Code CLI are the source of the history
    // (~/.claude/projects/<project>/<session id>.jsonl); our own small index adds what the CLI does not keep: a title
    // chosen by the user, the model and the effort of the chat, and "deleted" (hidden) chats.
    struct ChatInfo
    {
        std::string id;        // CLI session id (a uuid)
        std::string title;
        std::string model;     // setting of the chat: alias or full name, empty = CLI default
        std::string effort;    // setting of the chat: low / medium / high / xhigh / max, empty = CLI default
        std::string lastModel; // the model that answered last, read from the transcript
        std::int64_t createdUnix = 0;
        std::int64_t updatedUnix = 0;
        std::uint64_t sizeBytes = 0;
        double costUsd = 0.0;
        std::uint64_t inputTokens = 0;  // the whole chat, cached tokens included
        std::uint64_t outputTokens = 0;
        bool hidden = false;
        bool inIndex = false;
        bool hasTranscript = false;
    };

    class ChatStore
    {
    public:
        // indexFile: <project>/Saved/assistant/chats.json. sessionsDirectory: where the CLI writes this project's sessions
        ChatStore(std::filesystem::path indexFile, std::filesystem::path sessionsDirectory);

        // "C:\\Users\\me\\Game Dev\\proj" -> "C--Users-me-Game-Dev-proj": the folder name the CLI uses for a working directory
        static std::string EncodeProjectDirectory(const std::filesystem::path& workingDirectory);
        // ~/.claude/projects/<encoded working directory>; empty when the home directory is unknown
        static std::filesystem::path DefaultSessionsDirectory(const std::filesystem::path& workingDirectory);
        static std::int64_t ParseIsoTime(const std::string& text); // "2026-10-10T21:43:30.564Z" -> unix seconds, 0 on failure

        // Reads the index and scans the sessions folder (a changed file is read again, the rest comes from a cache)
        void Refresh();

        // Visible chats (not hidden), newest first
        std::vector<ChatInfo> Chats() const;
        const ChatInfo* Find(const std::string& id) const;
        const std::filesystem::path& GetSessionsDirectory() const { return sessionsDirectory_; }
        const std::filesystem::path& GetIndexFile() const { return indexFile_; }

        // A chat that the assistant has just talked in: creates the index entry when it is missing, never overwrites
        // a title the user set. Saves the index.
        void Touch(const std::string& id, const std::string& title, const std::string& model, const std::string& effort);
        void SetModel(const std::string& id, const std::string& model);
        void SetEffort(const std::string& id, const std::string& effort);
        void Rename(const std::string& id, const std::string& title);
        // "Delete": the chat leaves the list, the CLI's file stays where it is
        void Hide(const std::string& id);

        // Loads the transcript of a session. `displayRoot`: file paths of tool calls inside it are shown relative.
        // At most `maxMessages` of the latest messages are kept. False when the file is missing or unreadable.
        bool LoadTranscript(const std::string& id, std::deque<AssistantMessage>& messages, const std::string& displayRoot,
                            std::size_t maxMessages, std::string& error) const;

        // Pure: the same parsing for a prepared text (tests)
        static void ParseTranscript(const std::string& jsonl, std::deque<AssistantMessage>& messages, const std::string& displayRoot,
                                    std::size_t maxMessages);

    private:
        struct Scan
        {
            std::uint64_t size = 0;
            std::int64_t mtime = 0;
            std::string entrypoint;
            std::string firstUserText;
            std::string customTitle;
            std::string lastModel;
            std::int64_t firstTime = 0;
            std::int64_t lastTime = 0;
            double costUsd = 0.0;
            std::uint64_t inputTokens = 0;
            std::uint64_t outputTokens = 0;
        };

        void LoadIndex();
        bool SaveIndex() const;
        ChatInfo& Entry(const std::string& id);
        bool ScanFile(const std::filesystem::path& file, Scan& scan) const;

        std::filesystem::path indexFile_;
        std::filesystem::path sessionsDirectory_;
        std::vector<ChatInfo> chats_;
        std::unordered_map<std::string, Scan> scans_; // by session id
    };

    // The title of a new chat from its first message: one line, at most `limit` characters
    std::string MakeChatTitle(const std::string& firstMessage, std::size_t limit = 48);

    // "claude-opus-5-5" -> "Opus 5.5", "claude-haiku-5-5-20260101" -> "Haiku 5.5", an alias "sonnet" -> "Sonnet",
    // empty -> empty. An id that does not look like a Claude model is returned as it is.
    std::string FriendlyModelName(const std::string& modelId);
}
