// ChatStore.cpp

#include <myengine/assistant/ChatStore.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include <myengine/assistant/ClaudeStreamParser.h>

namespace myengine::assistant
{
    namespace
    {
        using json = nlohmann::json;

        constexpr std::size_t kHeadBytes = 2 * 1024 * 1024;  // where the first user message is looked for
        constexpr std::size_t kTailBytes = 128 * 1024;       // where the title and the cost of a long session are
        constexpr std::size_t kMaxLineBytes = 16 * 1024 * 1024;
        constexpr std::uint64_t kMaxTranscriptBytes = 256ull * 1024 * 1024;

        std::string GetString(const json& object, const char* key)
        {
            if (object.is_object())
            {
                const auto it = object.find(key);
                if (it != object.end() && it->is_string())
                {
                    return it->get<std::string>();
                }
            }
            return {};
        }

        std::int64_t FileTimeToUnix(const std::filesystem::file_time_type time)
        {
            // file_clock to system_clock without C++20 clock_cast
            const auto delta = time - std::filesystem::file_time_type::clock::now();
            const auto system = std::chrono::system_clock::now() +
                std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
            return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count());
        }

        // The wide variant: a profile folder with letters of the user's language is not UTF-8 in the narrow environment
        std::wstring EnvironmentValue(const wchar_t* name)
        {
            wchar_t* value = nullptr;
            std::size_t length = 0;
            std::wstring result;
            if (_wdupenv_s(&value, &length, name) == 0 && value != nullptr)
            {
                result = value;
                std::free(value);
            }
            return result;
        }

        // Reads an unsigned number of up to `maxDigits` digits at `position` and moves past it
        bool ReadNumber(const std::string& text, std::size_t& position, const std::size_t maxDigits, int& value)
        {
            value = 0;
            std::size_t digits = 0;
            while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position])) != 0 && digits < maxDigits)
            {
                value = value * 10 + (text[position] - '0');
                ++position;
                ++digits;
            }
            return digits > 0;
        }

        std::int64_t NowUnix()
        {
            return static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        }

        // The text that the user typed in a `user` record of a session file, or empty (tool results, injected context)
        std::string UserText(const json& message)
        {
            const auto content = message.find("content");
            if (content == message.end())
            {
                return {};
            }
            std::string text;
            if (content->is_string())
            {
                text = content->get<std::string>();
            }
            else if (content->is_array())
            {
                for (const auto& block : *content)
                {
                    if (GetString(block, "type") == "text")
                    {
                        if (!text.empty())
                        {
                            text += '\n';
                        }
                        text += GetString(block, "text");
                    }
                }
            }
            // Text that the CLI or the editor wove in is not something the user wrote
            if (text.rfind("<system-reminder>", 0) == 0 || text.rfind("<command-", 0) == 0 || text.rfind("<local-command", 0) == 0 ||
                text.rfind("Caveat:", 0) == 0)
            {
                return {};
            }
            return text;
        }

        // The text after the marker that BuildUserMessageLine adds for attachments is not part of the title
        std::string WithoutAttachmentList(std::string text)
        {
            const auto marker = text.find("\n\nAttached files (open them with the Read tool):");
            if (marker != std::string::npos)
            {
                text.resize(marker);
            }
            return text;
        }

        // Does the line have `"type":"<value>"` (the CLI writes compact JSON, a tool that rewrote the file may not)
        bool HasType(const std::string_view line, const char* value)
        {
            const std::string compact = std::string("\"type\":\"") + value + "\"";
            const std::string spaced = std::string("\"type\": \"") + value + "\"";
            return line.find(compact) != std::string_view::npos || line.find(spaced) != std::string_view::npos;
        }

        template <typename Callback>
        void ForEachLine(std::ifstream& stream, const bool skipFirstPartial, Callback&& callback)
        {
            std::string line;
            bool first = true;
            while (std::getline(stream, line))
            {
                if (first && skipFirstPartial)
                {
                    first = false;
                    continue;
                }
                first = false;
                if (line.size() > kMaxLineBytes)
                {
                    continue;
                }
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                if (!callback(line))
                {
                    break;
                }
            }
        }
    }

    std::string MakeChatTitle(const std::string& firstMessage, const std::size_t limit)
    {
        std::string line;
        bool pendingSpace = false;
        for (std::size_t i = 0; i < firstMessage.size(); ++i)
        {
            const char c = firstMessage[i];
            if (c == '\n' && !line.empty())
            {
                break; // the first non-empty line
            }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                pendingSpace = !line.empty();
                continue;
            }
            if (pendingSpace)
            {
                line += ' ';
                pendingSpace = false;
            }
            line += c;
        }
        // Headings and quotes of the message are not part of a title
        while (!line.empty() && (line.front() == '#' || line.front() == '>' || line.front() == '*' || line.front() == ' '))
        {
            line.erase(line.begin());
        }
        if (line.size() > limit)
        {
            std::size_t cut = limit;
            while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
            {
                --cut; // do not split a UTF-8 character
            }
            line.resize(cut);
            while (!line.empty() && line.back() == ' ')
            {
                line.pop_back();
            }
            line += "...";
        }
        return line;
    }

    std::string FriendlyModelName(const std::string& modelId)
    {
        if (modelId.empty())
        {
            return {};
        }
        std::string rest = modelId;
        if (rest.rfind("claude-", 0) == 0)
        {
            rest = rest.substr(7);
        }
        // family, then up to two version numbers, then an optional date
        std::vector<std::string> parts;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= rest.size(); ++i)
        {
            if (i == rest.size() || rest[i] == '-')
            {
                parts.push_back(rest.substr(start, i - start));
                start = i + 1;
            }
        }
        if (parts.empty() || parts[0].empty() || !std::isalpha(static_cast<unsigned char>(parts[0][0])))
        {
            return modelId;
        }
        std::string name = parts[0];
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        std::vector<std::string> version;
        for (std::size_t i = 1; i < parts.size() && version.size() < 2; ++i)
        {
            const auto& part = parts[i];
            const bool digits = !part.empty() && part.size() <= 2 &&
                std::all_of(part.begin(), part.end(), [](const char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
            if (!digits)
            {
                break;
            }
            version.push_back(part);
        }
        for (std::size_t i = 0; i < version.size(); ++i)
        {
            name += i == 0 ? " " : ".";
            name += version[i];
        }
        return name;
    }

    ChatStore::ChatStore(std::filesystem::path indexFile, std::filesystem::path sessionsDirectory)
        : indexFile_(std::move(indexFile)), sessionsDirectory_(std::move(sessionsDirectory))
    {
        LoadIndex();
    }

    std::string ChatStore::EncodeProjectDirectory(const std::filesystem::path& workingDirectory)
    {
        // Every character that is not a letter or a digit of ASCII becomes '-': separators, colons, dots and
        // letters of other scripts (one dash per UTF-16 unit, as the CLI does)
        const std::wstring text = workingDirectory.wstring();
        std::string encoded;
        encoded.reserve(text.size());
        for (const wchar_t c : text)
        {
            const bool alnum = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
            encoded += alnum ? static_cast<char>(c) : '-';
        }
        return encoded;
    }

    std::filesystem::path ChatStore::DefaultSessionsDirectory(const std::filesystem::path& workingDirectory)
    {
        std::filesystem::path home;
        if (const auto profile = EnvironmentValue(L"USERPROFILE"); !profile.empty())
        {
            home = std::filesystem::path(profile);
        }
        else if (const auto unixHome = EnvironmentValue(L"HOME"); !unixHome.empty())
        {
            home = std::filesystem::path(unixHome);
        }
        if (home.empty())
        {
            return {};
        }
        std::error_code error;
        auto absolute = std::filesystem::absolute(workingDirectory, error);
        // The CLI names the folder after the directory without a trailing separator
        auto text = absolute.wstring();
        while (text.size() > 1 && (text.back() == L'\\' || text.back() == L'/'))
        {
            text.pop_back();
        }
        return home / ".claude" / "projects" / EncodeProjectDirectory(std::filesystem::path(text));
    }

    std::int64_t ChatStore::ParseIsoTime(const std::string& text)
    {
        // "YYYY-MM-DDThh:mm:ss[.fff]Z"
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        std::size_t at = 0;
        const auto separator = [&](const char expected)
        {
            if (at < text.size() && text[at] == expected)
            {
                ++at;
                return true;
            }
            return false;
        };
        if (!ReadNumber(text, at, 4, year) || !separator('-') || !ReadNumber(text, at, 2, month) || !separator('-') ||
            !ReadNumber(text, at, 2, day) || !(separator('T') || separator(' ')) || !ReadNumber(text, at, 2, hour) ||
            !separator(':') || !ReadNumber(text, at, 2, minute) || !separator(':') || !ReadNumber(text, at, 2, second))
        {
            return 0;
        }
        if (month < 1 || month > 12 || day < 1 || day > 31)
        {
            return 0;
        }
        // days from civil (Howard Hinnant)
        const int y = month <= 2 ? year - 1 : year;
        const int era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153u * static_cast<unsigned>(month + (month > 2 ? -3 : 9)) + 2u) / 5u + static_cast<unsigned>(day) - 1u;
        const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
        const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
        return days * 86400 + hour * 3600 + minute * 60 + second;
    }

    void ChatStore::LoadIndex()
    {
        chats_.clear();
        std::ifstream file(indexFile_, std::ios::binary);
        if (!file)
        {
            return;
        }
        const json document = json::parse(file, nullptr, false);
        if (document.is_discarded() || !document.is_object())
        {
            return;
        }
        const auto list = document.find("chats");
        if (list == document.end() || !list->is_array())
        {
            return;
        }
        for (const auto& item : *list)
        {
            ChatInfo info;
            info.id = GetString(item, "id");
            if (info.id.empty())
            {
                continue;
            }
            info.title = GetString(item, "title");
            info.model = GetString(item, "model");
            info.effort = GetString(item, "effort");
            info.createdUnix = item.value("created", static_cast<std::int64_t>(0));
            info.hidden = item.value("hidden", false);
            info.inIndex = true;
            chats_.push_back(std::move(info));
        }
    }

    bool ChatStore::SaveIndex() const
    {
        if (indexFile_.empty())
        {
            return false;
        }
        json list = json::array();
        for (const auto& chat : chats_)
        {
            if (!chat.inIndex)
            {
                continue;
            }
            json item = {{"id", chat.id}, {"title", chat.title}, {"model", chat.model}, {"effort", chat.effort},
                         {"created", chat.createdUnix}, {"hidden", chat.hidden}};
            list.push_back(std::move(item));
        }
        const json document = {{"version", 1}, {"chats", std::move(list)}};
        std::error_code error;
        std::filesystem::create_directories(indexFile_.parent_path(), error);
        // Written next to the target and renamed: a crash does not leave half a file
        const auto temporary = indexFile_.parent_path() / (indexFile_.filename().string() + ".tmp");
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                return false;
            }
            file << document.dump(2, ' ', false, json::error_handler_t::replace);
            if (!file)
            {
                return false;
            }
        }
        std::filesystem::rename(temporary, indexFile_, error);
        if (error)
        {
            std::filesystem::remove(indexFile_, error);
            std::filesystem::rename(temporary, indexFile_, error);
        }
        return !error;
    }

    ChatInfo& ChatStore::Entry(const std::string& id)
    {
        for (auto& chat : chats_)
        {
            if (chat.id == id)
            {
                return chat;
            }
        }
        ChatInfo info;
        info.id = id;
        chats_.push_back(std::move(info));
        return chats_.back();
    }

    bool ChatStore::ScanFile(const std::filesystem::path& file, Scan& scan) const
    {
        std::error_code error;
        const auto size = std::filesystem::file_size(file, error);
        if (error)
        {
            return false;
        }
        scan.size = size;
        const auto time = std::filesystem::last_write_time(file, error);
        scan.mtime = error ? 0 : FileTimeToUnix(time);

        std::ifstream stream(file, std::ios::binary);
        if (!stream)
        {
            return false;
        }

        // The head: the first thing the user said and which program wrote the session
        std::uint64_t consumed = 0;
        ForEachLine(stream, false, [&](const std::string& line)
        {
            consumed += line.size() + 1;
            if (!HasType(line, "user"))
            {
                return consumed < kHeadBytes;
            }
            const json record = json::parse(line, nullptr, false);
            if (record.is_discarded() || GetString(record, "type") != "user" || record.value("isSidechain", false))
            {
                return consumed < kHeadBytes;
            }
            const auto message = record.find("message");
            const auto text = message != record.end() ? UserText(*message) : std::string();
            if (text.empty())
            {
                return consumed < kHeadBytes;
            }
            scan.entrypoint = GetString(record, "entrypoint");
            scan.firstUserText = WithoutAttachmentList(text);
            scan.firstTime = ChatStore::ParseIsoTime(GetString(record, "timestamp"));
            return false;
        });

        // The tail: the latest title, the cost, the model and the time of the last message
        stream.clear();
        const std::uint64_t tailStart = size > kTailBytes ? size - kTailBytes : 0;
        stream.seekg(static_cast<std::streamoff>(tailStart));
        ForEachLine(stream, tailStart > 0, [&](const std::string& line)
        {
            if (HasType(line, "custom-title") || HasType(line, "agent-name"))
            {
                const json record = json::parse(line, nullptr, false);
                if (!record.is_discarded())
                {
                    auto title = GetString(record, "customTitle");
                    if (title.empty())
                    {
                        title = GetString(record, "agentName");
                    }
                    if (!title.empty())
                    {
                        scan.customTitle = title;
                    }
                }
            }
            else if (HasType(line, "cost-state"))
            {
                const json record = json::parse(line, nullptr, false);
                if (!record.is_discarded() && record.contains("totalCostUSD") && record["totalCostUSD"].is_number())
                {
                    scan.costUsd = record["totalCostUSD"].get<double>();
                    // modelUsage: one object per model with the token counters of the whole session
                    scan.inputTokens = 0;
                    scan.outputTokens = 0;
                    const auto usage = record.find("modelUsage");
                    if (usage != record.end() && usage->is_object())
                    {
                        for (const auto& [model, counters] : usage->items())
                        {
                            const auto number = [&](const char* key) -> std::uint64_t
                            {
                                const auto it = counters.find(key);
                                return it != counters.end() && it->is_number() ? static_cast<std::uint64_t>(it->get<double>()) : 0;
                            };
                            scan.inputTokens += number("inputTokens") + number("cacheReadInputTokens") + number("cacheCreationInputTokens");
                            scan.outputTokens += number("outputTokens");
                        }
                    }
                }
            }
            else if (HasType(line, "assistant"))
            {
                const json record = json::parse(line, nullptr, false);
                if (!record.is_discarded())
                {
                    const auto message = record.find("message");
                    if (message != record.end())
                    {
                        const auto model = GetString(*message, "model");
                        if (!model.empty() && model != "<synthetic>")
                        {
                            scan.lastModel = model;
                        }
                    }
                    const auto time = ChatStore::ParseIsoTime(GetString(record, "timestamp"));
                    if (time > 0)
                    {
                        scan.lastTime = time;
                    }
                }
            }
            return true;
        });
        return true;
    }

    void ChatStore::Refresh()
    {
        LoadIndex();
        std::error_code error;
        if (!sessionsDirectory_.empty() && std::filesystem::is_directory(sessionsDirectory_, error))
        {
            std::set<std::string> seen;
            for (const auto& entry : std::filesystem::directory_iterator(sessionsDirectory_, error))
            {
                if (error || !entry.is_regular_file() || entry.path().extension() != ".jsonl")
                {
                    continue;
                }
                const auto id = entry.path().stem().string();
                seen.insert(id);

                std::error_code statError;
                const auto size = entry.file_size(statError);
                const auto time = entry.last_write_time(statError);
                const auto mtime = statError ? 0 : FileTimeToUnix(time);
                auto cached = scans_.find(id);
                if (cached == scans_.end() || cached->second.size != size || cached->second.mtime != mtime)
                {
                    Scan scan;
                    if (!ScanFile(entry.path(), scan))
                    {
                        continue;
                    }
                    scans_[id] = std::move(scan);
                    cached = scans_.find(id);
                }
                const Scan& scan = cached->second;

                // Our own chats are the ones the CLI wrote on behalf of an SDK client; sessions of the user's own
                // terminal or desktop app in the same folder are not offered unless the index knows them
                const bool ours = scan.entrypoint == "sdk-cli";
                ChatInfo* known = nullptr;
                for (auto& chat : chats_)
                {
                    if (chat.id == id)
                    {
                        known = &chat;
                        break;
                    }
                }
                if (known == nullptr && !ours)
                {
                    continue;
                }
                if (known == nullptr)
                {
                    chats_.push_back(ChatInfo{});
                    known = &chats_.back();
                    known->id = id;
                }
                known->hasTranscript = true;
                known->sizeBytes = scan.size;
                known->costUsd = scan.costUsd;
                known->inputTokens = scan.inputTokens;
                known->outputTokens = scan.outputTokens;
                known->lastModel = scan.lastModel;
                known->updatedUnix = std::max(scan.lastTime, scan.mtime);
                if (known->createdUnix == 0)
                {
                    known->createdUnix = scan.firstTime != 0 ? scan.firstTime : known->updatedUnix;
                }
                if (known->title.empty())
                {
                    known->title = !scan.customTitle.empty() ? scan.customTitle : MakeChatTitle(scan.firstUserText);
                }
            }
            for (auto it = scans_.begin(); it != scans_.end();)
            {
                it = seen.count(it->first) != 0 ? std::next(it) : scans_.erase(it);
            }
        }
        // Entries of the index whose file is gone and that were never talked in are not worth a row
        chats_.erase(std::remove_if(chats_.begin(), chats_.end(), [](const ChatInfo& chat)
        {
            return !chat.hasTranscript && chat.updatedUnix == 0 && chat.title.empty();
        }), chats_.end());
    }

    std::vector<ChatInfo> ChatStore::Chats() const
    {
        std::vector<ChatInfo> result;
        for (const auto& chat : chats_)
        {
            if (!chat.hidden)
            {
                result.push_back(chat);
            }
        }
        std::stable_sort(result.begin(), result.end(), [](const ChatInfo& a, const ChatInfo& b)
        {
            return a.updatedUnix > b.updatedUnix;
        });
        return result;
    }

    const ChatInfo* ChatStore::Find(const std::string& id) const
    {
        for (const auto& chat : chats_)
        {
            if (chat.id == id)
            {
                return &chat;
            }
        }
        return nullptr;
    }

    void ChatStore::Touch(const std::string& id, const std::string& title, const std::string& model, const std::string& effort)
    {
        if (id.empty())
        {
            return;
        }
        auto& chat = Entry(id);
        chat.inIndex = true;
        if (chat.title.empty())
        {
            chat.title = title;
        }
        chat.model = model;
        chat.effort = effort;
        chat.hidden = false;
        chat.updatedUnix = NowUnix();
        if (chat.createdUnix == 0)
        {
            chat.createdUnix = chat.updatedUnix;
        }
        SaveIndex();
    }

    void ChatStore::SetModel(const std::string& id, const std::string& model)
    {
        auto& chat = Entry(id);
        chat.inIndex = true;
        chat.model = model;
        SaveIndex();
    }

    void ChatStore::SetEffort(const std::string& id, const std::string& effort)
    {
        auto& chat = Entry(id);
        chat.inIndex = true;
        chat.effort = effort;
        SaveIndex();
    }

    void ChatStore::Rename(const std::string& id, const std::string& title)
    {
        auto& chat = Entry(id);
        chat.inIndex = true;
        chat.title = title;
        SaveIndex();
    }

    void ChatStore::Hide(const std::string& id)
    {
        auto& chat = Entry(id);
        chat.inIndex = true;
        chat.hidden = true;
        SaveIndex();
    }

    bool ChatStore::LoadTranscript(const std::string& id, std::deque<AssistantMessage>& messages, const std::string& displayRoot,
                                   const std::size_t maxMessages, std::string& error) const
    {
        messages.clear();
        if (id.empty() || id.find_first_of("/\\.") != std::string::npos)
        {
            error = "The chat id is not valid.";
            return false;
        }
        const auto file = sessionsDirectory_ / (id + ".jsonl");
        std::error_code fileError;
        const auto size = std::filesystem::file_size(file, fileError);
        if (fileError)
        {
            error = "The history file of this chat was not found.";
            return false;
        }
        if (size > kMaxTranscriptBytes)
        {
            error = "The history of this chat is too big to open here.";
            return false;
        }
        std::ifstream stream(file, std::ios::binary);
        if (!stream)
        {
            error = "The history file of this chat could not be read.";
            return false;
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        ParseTranscript(buffer.str(), messages, displayRoot, maxMessages);
        return true;
    }

    void ChatStore::ParseTranscript(const std::string& jsonl, std::deque<AssistantMessage>& messages, const std::string& displayRoot,
                                    const std::size_t maxMessages)
    {
        ClaudeStreamParser parser;
        parser.SetRootDirectory(displayRoot);
        std::vector<AssistantEvent> events;
        std::vector<std::string> changed;

        std::string currentModel;
        std::int64_t turnStart = 0;
        std::int64_t lastTime = 0;
        std::uint64_t turnInput = 0;
        std::uint64_t turnOutput = 0;
        std::uint32_t turnRequests = 0;
        std::string lastRequestId;
        bool turnOpen = false;

        const auto closeTurn = [&]()
        {
            if (!turnOpen)
            {
                return;
            }
            if (!changed.empty())
            {
                AssistantMessage files;
                files.kind = AssistantMessage::Kind::Files;
                for (const auto& path : changed)
                {
                    if (!files.text.empty())
                    {
                        files.text += '\n';
                    }
                    files.text += path;
                }
                files.timeUnix = lastTime;
                messages.push_back(std::move(files));
                changed.clear();
            }
            const double seconds = turnStart > 0 && lastTime >= turnStart ? static_cast<double>(lastTime - turnStart) : 0.0;
            AssistantMessage footer;
            footer.kind = AssistantMessage::Kind::Summary;
            footer.text = FormatTurnSummary(turnInput, turnOutput, seconds, 0.0);
            footer.timeUnix = lastTime;
            messages.push_back(std::move(footer));
            turnOpen = false;
            turnInput = turnOutput = 0;
            turnRequests = 0;
            lastRequestId.clear();
        };

        std::size_t position = 0;
        while (position < jsonl.size())
        {
            auto end = jsonl.find('\n', position);
            if (end == std::string::npos)
            {
                end = jsonl.size();
            }
            const std::string_view line(jsonl.data() + position, end - position);
            position = end + 1;
            if (line.size() < 2 || line.size() > kMaxLineBytes)
            {
                continue;
            }
            const bool isUser = HasType(line, "user");
            const bool isAssistant = !isUser && HasType(line, "assistant");
            if (!isUser && !isAssistant)
            {
                continue;
            }
            const json record = json::parse(line.begin(), line.end(), nullptr, false);
            if (record.is_discarded() || !record.is_object() || record.value("isSidechain", false) || record.value("isMeta", false))
            {
                continue;
            }
            const auto type = GetString(record, "type");
            const auto time = ParseIsoTime(GetString(record, "timestamp"));
            const auto message = record.find("message");
            if (message == record.end() || !message->is_object())
            {
                continue;
            }
            if (time > 0)
            {
                lastTime = time;
            }

            if (type == "user")
            {
                const auto text = UserText(*message);
                // Images that came with the message
                std::vector<AssistantAttachment> attachments;
                const auto content = message->find("content");
                if (content != message->end() && content->is_array())
                {
                    for (const auto& block : *content)
                    {
                        if (GetString(block, "type") == "image")
                        {
                            AssistantAttachment attachment;
                            attachment.name = "image";
                            const auto source = block.find("source");
                            attachment.mediaType = source != block.end() ? GetString(*source, "media_type") : std::string();
                            if (attachment.mediaType.empty())
                            {
                                attachment.mediaType = "image/png";
                            }
                            attachments.push_back(std::move(attachment));
                        }
                    }
                }
                if (!text.empty() || !attachments.empty())
                {
                    closeTurn();
                    AssistantMessage user;
                    user.kind = AssistantMessage::Kind::User;
                    user.text = WithoutAttachmentList(text);
                    user.timeUnix = time;
                    user.attachments = std::move(attachments);
                    messages.push_back(std::move(user));
                    turnOpen = true;
                    turnStart = time;
                    changed.clear();
                }
                else
                {
                    // tool results: the stream parser pairs them with the calls
                    events.clear();
                    parser.ParseLine(line, events);
                    for (const auto& event : events)
                    {
                        ApplyContentEvent(messages, event, &changed);
                    }
                }
            }
            else
            {
                const auto model = GetString(*message, "model");
                if (!model.empty() && model != "<synthetic>")
                {
                    currentModel = model;
                }
                // one request id covers all the lines of one model response; its usage repeats on every line
                const auto requestId = GetString(record, "requestId");
                if (!requestId.empty() && requestId != lastRequestId)
                {
                    lastRequestId = requestId;
                    ++turnRequests;
                    const auto usage = message->find("usage");
                    if (usage != message->end() && usage->is_object())
                    {
                        const auto number = [&](const char* key) -> std::uint64_t
                        {
                            const auto it = usage->find(key);
                            return it != usage->end() && it->is_number_unsigned() ? it->get<std::uint64_t>() : 0;
                        };
                        turnInput += number("input_tokens") + number("cache_creation_input_tokens") + number("cache_read_input_tokens");
                        turnOutput += number("output_tokens");
                    }
                }
                events.clear();
                parser.ParseLine(line, events);
                const std::size_t before = messages.size();
                for (const auto& event : events)
                {
                    ApplyContentEvent(messages, event, &changed);
                }
                if (before < messages.size() && before > 0 && messages[before - 1].kind == AssistantMessage::Kind::Thinking &&
                    messages[before - 1].endTimeUnix == 0)
                {
                    messages[before - 1].endTimeUnix = time; // the reasoning lasted until the next thing the model did
                }
                for (std::size_t i = before; i < messages.size(); ++i)
                {
                    if (messages[i].timeUnix == 0)
                    {
                        messages[i].timeUnix = time;
                    }
                    if (messages[i].kind == AssistantMessage::Kind::Assistant && messages[i].model.empty())
                    {
                        messages[i].model = currentModel;
                    }
                }
                turnOpen = true;
            }
        }
        closeTurn();

        while (maxMessages > 0 && messages.size() > maxMessages)
        {
            messages.pop_front();
        }
    }
}
