// BuildPanelModel.cpp

#include <myengine/editor/BuildPanelModel.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace myengine::editor
{
    namespace fs = std::filesystem;
    using json = nlohmann::json;

    namespace
    {
        std::string Trim(std::string_view text)
        {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
            {
                text.remove_suffix(1);
            }
            return std::string(text);
        }

        std::string Lower(std::string_view text)
        {
            std::string out(text);
            for (char& c : out)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }

        bool Contains(std::string_view text, std::string_view part)
        {
            return text.find(part) != std::string_view::npos;
        }

        bool IsValidId(std::string_view id)
        {
            if (id.empty())
            {
                return false;
            }
            for (const char c : id)
            {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
                if (!ok)
                {
                    return false;
                }
            }
            return true;
        }

        // a string member: the value, or an error that names the member; `required`: it must be there and not empty
        bool ReadString(const json& object, const char* key, const char* where, bool required, std::string& out, std::string& error)
        {
            const auto it = object.find(key);
            if (it == object.end() || it->is_null())
            {
                if (required)
                {
                    error = std::string(where) + ": \"" + key + "\" is missing";
                    return false;
                }
                return true;
            }
            if (!it->is_string())
            {
                error = std::string(where) + ": \"" + key + "\" must be a string";
                return false;
            }
            out = it->get<std::string>();
            if (required && Trim(out).empty())
            {
                error = std::string(where) + ": \"" + key + "\" is empty";
                return false;
            }
            return true;
        }

        bool ReadFeature(const json& options, const char* key, GameFeature& out, std::string& error)
        {
            const auto it = options.find(key);
            if (it == options.end() || it->is_null())
            {
                return true;
            }
            if (!it->is_string())
            {
                error = std::string("options: \"") + key + "\" must be \"optional\" or \"no\"";
                return false;
            }
            const std::string value = Lower(it->get<std::string>());
            if (value == "optional")
            {
                out = GameFeature::Optional;
            }
            else if (value == "no")
            {
                out = GameFeature::No;
            }
            else
            {
                error = std::string("options: \"") + key + "\" is \"" + it->get<std::string>() + "\", it must be \"optional\" or \"no\"";
                return false;
            }
            return true;
        }

        std::string ReadFile(const fs::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
            {
                text.erase(0, 3);
            }
            return text;
        }

        std::string Quote(const std::string& value)
        {
            if (value.empty())
            {
                return "\"\"";
            }
            if (value.find_first_of(" \t\"") == std::string::npos)
            {
                return value;
            }
            std::string out = "\"";
            for (const char c : value)
            {
                if (c == '"')
                {
                    out += "\\\"";
                }
                else
                {
                    out += c;
                }
            }
            out += '"';
            return out;
        }

        std::string NativePath(const fs::path& path)
        {
            const auto text = path.lexically_normal().u8string();
            return std::string(text.begin(), text.end());
        }

        bool SkipFolder(const std::string& name)
        {
            const std::string lower = Lower(name);
            return lower == "external" || lower == ".git" || lower == ".team" || lower == "assets" || lower == "docs" || lower == "dist" || lower == "node_modules" ||
                   lower.rfind("build", 0) == 0 || lower == "wwise";
        }

        // the first word of a text after the command name: add_executable ( name ...
        bool TargetDefinedIn(const std::string& text, std::string_view target)
        {
            const std::string lower = Lower(text);
            for (const char* command : {"add_executable", "add_library"})
            {
                std::size_t at = 0;
                const std::string_view name(command);
                while ((at = lower.find(name, at)) != std::string::npos)
                {
                    std::size_t i = at + name.size();
                    at = i;
                    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
                    {
                        ++i;
                    }
                    if (i >= text.size() || text[i] != '(')
                    {
                        continue;
                    }
                    ++i;
                    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
                    {
                        ++i;
                    }
                    std::size_t end = i;
                    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) && text[end] != ')')
                    {
                        ++end;
                    }
                    if (std::string_view(text).substr(i, end - i) == target)
                    {
                        return true;
                    }
                }
            }
            return false;
        }
    }

    // ---------------------------------------------------------------- the manifests

    bool ParseGameManifest(std::string_view jsonText, std::string_view folderName, GameManifest& out, std::string& error)
    {
        out = GameManifest{};
        json root;
        try
        {
            root = json::parse(jsonText.begin(), jsonText.end());
        }
        catch (const json::parse_error& ex)
        {
            error = std::string("game.json is not valid JSON: ") + ex.what();
            return false;
        }
        if (!root.is_object())
        {
            error = "game.json must be a JSON object";
            return false;
        }

        if (!ReadString(root, "id", "game.json", true, out.id, error) || !ReadString(root, "displayName", "game.json", true, out.displayName, error) ||
            !ReadString(root, "name", "game.json", true, out.name, error) || !ReadString(root, "exe", "game.json", true, out.exe, error))
        {
            return false;
        }
        if (!IsValidId(out.id))
        {
            error = "game.json: \"id\" is \"" + out.id + "\", it may have only a-z, 0-9, - and _";
            return false;
        }
        if (out.id != folderName)
        {
            error = "game.json: \"id\" is \"" + out.id + "\", but the folder is games/" + std::string(folderName) + " (they must be the same)";
            return false;
        }
        if (Lower(out.exe).size() < 5 || Lower(out.exe).substr(Lower(out.exe).size() - 4) != ".exe")
        {
            error = "game.json: \"exe\" is \"" + out.exe + "\", it must be a file name that ends with .exe";
            return false;
        }

        const auto build = root.find("build");
        if (build != root.end() && !build->is_null())
        {
            if (!build->is_object())
            {
                error = "game.json: \"build\" must be an object";
                return false;
            }
            if (!ReadString(*build, "cmakeTarget", "build", false, out.cmakeTarget, error) || !ReadString(*build, "exePath", "build", false, out.exePath, error) ||
                !ReadString(*build, "userDataFolder", "build", false, out.userDataFolder, error) || !ReadString(*build, "icon", "build", false, out.icon, error) ||
                !ReadString(*build, "checkScript", "build", false, out.checkScript, error))
            {
                return false;
            }
        }
        if (out.exePath.empty())
        {
            out.exePath = "app/Release/" + out.exe;
        }
        if (out.userDataFolder.empty())
        {
            out.userDataFolder = out.name;
        }

        const auto options = root.find("options");
        if (options != root.end() && !options->is_null())
        {
            if (!options->is_object())
            {
                error = "game.json: \"options\" must be an object";
                return false;
            }
            if (!ReadFeature(*options, "steam", out.steam, error) || !ReadFeature(*options, "wwise", out.wwise, error))
            {
                return false;
            }
            const auto zip = options->find("zip");
            if (zip != options->end() && !zip->is_null())
            {
                if (!zip->is_boolean())
                {
                    error = "options: \"zip\" must be true or false";
                    return false;
                }
                out.zipByDefault = zip->get<bool>();
            }
        }

        const auto external = root.find("externalData");
        if (external != root.end() && !external->is_null())
        {
            if (!external->is_array())
            {
                error = "game.json: \"externalData\" must be an array";
                return false;
            }
            int index = 0;
            for (const json& entry : *external)
            {
                const std::string where = "externalData[" + std::to_string(index++) + "]";
                if (!entry.is_object())
                {
                    error = where + " must be an object";
                    return false;
                }
                ExternalDataItem item;
                if (!ReadString(entry, "name", where.c_str(), true, item.name, error) || !ReadString(entry, "check", where.c_str(), true, item.check, error) ||
                    !ReadString(entry, "fetch", where.c_str(), false, item.fetch, error))
                {
                    return false;
                }
                const auto optional = entry.find("optional");
                if (optional != entry.end() && !optional->is_null())
                {
                    if (!optional->is_boolean())
                    {
                        error = where + ": \"optional\" must be true or false";
                        return false;
                    }
                    item.optional = optional->get<bool>();
                }
                out.externalData.push_back(std::move(item));
            }
        }
        return true;
    }

    std::vector<GameEntry> LoadGameManifests(const fs::path& repoRoot)
    {
        std::vector<GameEntry> entries;
        std::error_code ec;
        const fs::path gamesDir = repoRoot / "games";
        if (!fs::is_directory(gamesDir, ec))
        {
            return entries;
        }
        for (const auto& item : fs::directory_iterator(gamesDir, ec))
        {
            if (!item.is_directory(ec))
            {
                continue;
            }
            const fs::path file = item.path() / "game.json";
            if (!fs::is_regular_file(file, ec))
            {
                continue;
            }
            GameEntry entry;
            entry.folder = NativePath(item.path().filename());
            GameManifest manifest;
            if (ParseGameManifest(ReadFile(file), entry.folder, manifest, entry.error))
            {
                entry.manifest = std::move(manifest);
            }
            entries.push_back(std::move(entry));
        }
        std::sort(entries.begin(), entries.end(), [](const GameEntry& a, const GameEntry& b) { return a.folder < b.folder; });
        return entries;
    }

    bool CmakeTargetExists(const fs::path& repoRoot, std::string_view target)
    {
        if (target.empty())
        {
            return false;
        }
        std::error_code ec;
        fs::recursive_directory_iterator it(repoRoot, fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (it->is_directory(ec))
            {
                if (SkipFolder(NativePath(it->path().filename())))
                {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (NativePath(it->path().filename()) == "CMakeLists.txt" && TargetDefinedIn(ReadFile(it->path()), target))
            {
                return true;
            }
        }
        return false;
    }

    bool ExternalDataPresent(const fs::path& repoRoot, const ExternalDataItem& item)
    {
        if (item.check.empty())
        {
            return false;
        }
        // %NAME% is an environment variable (the same as the script does): a path of the machine, outside the repository. The value is read as wide text:
        // %LOCALAPPDATA% of a user with a Russian name is not UTF-8.
        std::wstring text = fs::u8path(item.check).wstring();
        for (std::size_t open = text.find(L'%'); open != std::wstring::npos; open = text.find(L'%', open))
        {
            const std::size_t close = text.find(L'%', open + 1);
            if (close == std::wstring::npos)
            {
                break;
            }
            const std::wstring name = text.substr(open + 1, close - open - 1);
            wchar_t* value = nullptr;
            std::size_t valueSize = 0;
            if (name.empty() || _wdupenv_s(&value, &valueSize, name.c_str()) != 0 || value == nullptr)
            {
                open = close;   // not a variable: left as it is
                continue;
            }
            const std::wstring replacement(value);
            free(value);
            text.replace(open, close - open + 1, replacement);
            open += replacement.size();
        }
        std::error_code ec;
        const fs::path path(text);
        return fs::exists(path.is_absolute() ? path : repoRoot / path, ec);
    }

    // ---------------------------------------------------------------- the lines

    BuildLine ParseBuildLine(std::string_view line)
    {
        BuildLine result;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        {
            line.remove_suffix(1);
        }
        result.text = std::string(line);

        if (line.rfind("##progress ", 0) == 0)
        {
            std::string_view rest = line.substr(11);
            int percent = 0;
            const auto [end, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), percent);
            if (ec == std::errc() && end != rest.data())
            {
                result.kind = BuildLineKind::Progress;
                result.percent = std::clamp(percent, 0, 100);
                result.stage = Trim(std::string_view(end, static_cast<std::size_t>(rest.data() + rest.size() - end)));
                return result;
            }
        }
        else if (line.rfind("##result ", 0) == 0)
        {
            const std::string rest = Trim(line.substr(9));
            if (rest == "ok" || rest.rfind("ok ", 0) == 0)
            {
                result.kind = BuildLineKind::Result;
                result.ok = true;
                result.text = Trim(std::string_view(rest).substr(2));
                return result;
            }
            if (rest == "fail" || rest.rfind("fail ", 0) == 0)
            {
                result.kind = BuildLineKind::Result;
                result.ok = false;
                result.text = Trim(std::string_view(rest).substr(4));
                return result;
            }
        }
        else if (line.rfind("##zip ", 0) == 0)
        {
            result.kind = BuildLineKind::Zip;
            result.text = Trim(line.substr(6));
            return result;
        }

        // the summary lines of MSBuild ("0 Error(s)", "Ошибок: 0") say nothing
        const std::string lower = Lower(line);
        const std::string trimmed = Trim(lower);
        const bool summary = (trimmed.find("warning(s)") != std::string::npos || trimmed.find("error(s)") != std::string::npos) ||
                             trimmed.rfind("\xd0\xbf\xd1\x80\xd0\xb5\xd0\xb4\xd1\x83\xd0\xbf\xd1\x80\xd0\xb5\xd0\xb6\xd0\xb4\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb9:", 0) == 0 ||   // "предупреждений:"
                             trimmed.rfind("\xd0\xbe\xd1\x88\xd0\xb8\xd0\xb1\xd0\xbe\xd0\xba:", 0) == 0;                                                                         // "ошибок:"
        if (summary)
        {
            return result;
        }
        // "ошибка " / "предупреждение " (the Russian MSBuild and the compiler)
        const bool russianError = Contains(lower, "\xd0\xbe\xd1\x88\xd0\xb8\xd0\xb1\xd0\xba\xd0\xb0 ");
        const bool russianWarning = Contains(lower, "\xd0\xbf\xd1\x80\xd0\xb5\xd0\xb4\xd1\x83\xd0\xbf\xd1\x80\xd0\xb5\xd0\xb6\xd0\xb4\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5 ");
        if (Contains(lower, "error c") || Contains(lower, "error lnk") || Contains(lower, "error msb") || Contains(lower, "fatal error") || Contains(lower, "cmake error") ||
            Contains(lower, "failed with exit code") || lower.rfind("error:", 0) == 0 || lower.rfind("error ", 0) == 0 || lower.rfind("failed:", 0) == 0 ||
            Contains(lower, "build failed") || russianError)
        {
            result.kind = BuildLineKind::Error;
        }
        else if (lower.rfind("warning:", 0) == 0 || Contains(lower, "warning c") || Contains(lower, "warning msb") || Contains(lower, "cmake warning") ||
                 Contains(lower, "warning lnk") || russianWarning)
        {
            result.kind = BuildLineKind::Warning;
        }
        return result;
    }

    // ---------------------------------------------------------------- the options

    std::vector<std::string> MakeScriptArguments(const BuildOptions& options, const GameManifest& manifest)
    {
        std::vector<std::string> arguments;
        arguments.push_back("-Game");
        arguments.push_back(options.gameId);
        arguments.push_back("-Config");
        arguments.push_back(options.config == BuildConfig::RelWithDebInfo ? "RelWithDebInfo" : "Shipping");
        if (manifest.steam == GameFeature::Optional)
        {
            if (options.steam == SdkChoice::On)
            {
                arguments.push_back("-WithSteam");
            }
            else if (options.steam == SdkChoice::Off)
            {
                arguments.push_back("-NoSteam");
            }
        }
        if (manifest.wwise == GameFeature::Optional && options.wwise == SdkChoice::Off)
        {
            arguments.push_back("-NoWwise");
        }
        if (!options.zip)
        {
            arguments.push_back("-NoZip");
        }
        if (!Trim(options.outputFolder).empty())
        {
            arguments.push_back("-Out");
            arguments.push_back(Trim(options.outputFolder));
        }
        if (!Trim(options.buildFolder).empty())
        {
            arguments.push_back("-BuildDir");
            arguments.push_back(Trim(options.buildFolder));
        }
        return arguments;
    }

    namespace
    {
        std::string PowerShellPrefix(const fs::path& script)
        {
            return "powershell.exe -NoProfile -ExecutionPolicy Bypass -File " + Quote(NativePath(script));
        }
    }

    std::string MakeBuildCommandLine(const fs::path& repoRoot, const BuildOptions& options, const GameManifest& manifest)
    {
        std::string line = PowerShellPrefix(repoRoot / "tools" / "package_game.ps1");
        for (const std::string& argument : MakeScriptArguments(options, manifest))
        {
            line += ' ';
            line += Quote(argument);
        }
        return line;
    }

    std::string MakeCheckCommandLine(const fs::path& repoRoot, const GameManifest& manifest, const fs::path& packageFolder)
    {
        std::string line = PowerShellPrefix(repoRoot / fs::u8path(manifest.checkScript));
        line += " -Dist ";
        line += Quote(NativePath(packageFolder));
        return line;
    }

    fs::path PackageFolder(const fs::path& repoRoot, const BuildOptions& options, const GameManifest& manifest)
    {
        const fs::path out = fs::u8path(Trim(options.outputFolder).empty() ? std::string("dist") : Trim(options.outputFolder));
        return ((out.is_absolute() ? out : repoRoot / out) / fs::u8path(manifest.name)).lexically_normal();
    }

    bool IsDiskSpaceLow(std::uint64_t freeBytes)
    {
        return freeBytes < kLowDiskBytes;
    }

    std::string FormatSize(std::uint64_t bytes)
    {
        char text[32];
        const double gigabytes = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
        if (gigabytes >= 1.0)
        {
            std::snprintf(text, sizeof(text), "%.1f GB", gigabytes);
        }
        else
        {
            std::snprintf(text, sizeof(text), "%.0f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        }
        return text;
    }

    // ---------------------------------------------------------------- the state

    bool BuildSession::Begin(BuildJob job, std::string title)
    {
        if (state_ == BuildState::Running)
        {
            return false;
        }
        state_ = BuildState::Running;
        job_ = job;
        title_ = std::move(title);
        percent_ = 0;
        stage_ = "start";
        resultPath_.clear();
        zipPath_.clear();
        failureText_.clear();
        exitCode_ = 0;
        cancelRequested_ = false;
        resultSeen_ = false;
        resultOk_ = false;
        warnings_ = 0;
        errors_ = 0;
        log_.clear();
        ++logVersion_;
        return true;
    }

    void BuildSession::AddLine(std::string_view line)
    {
        if (state_ != BuildState::Running)
        {
            return;
        }
        const BuildLine parsed = ParseBuildLine(line);
        BuildLogLine entry;
        entry.kind = parsed.kind;
        entry.text = std::string(line);
        while (!entry.text.empty() && (entry.text.back() == '\r' || entry.text.back() == '\n'))
        {
            entry.text.pop_back();
        }
        switch (parsed.kind)
        {
            case BuildLineKind::Progress:
                percent_ = parsed.percent;
                stage_ = parsed.stage;
                break;
            case BuildLineKind::Result:
                resultSeen_ = true;
                resultOk_ = parsed.ok;
                if (parsed.ok)
                {
                    resultPath_ = parsed.text;
                }
                else
                {
                    failureText_ = parsed.text;
                    ++errors_;
                    entry.kind = BuildLineKind::Error;
                }
                break;
            case BuildLineKind::Zip:
                zipPath_ = parsed.text;
                break;
            case BuildLineKind::Warning:
                ++warnings_;
                break;
            case BuildLineKind::Error:
                ++errors_;
                break;
            case BuildLineKind::Plain:
                break;
        }
        log_.push_back(std::move(entry));
        if (log_.size() > kMaxLines)
        {
            log_.erase(log_.begin(), log_.begin() + static_cast<std::ptrdiff_t>(kMaxLines / 10));
        }
        ++logVersion_;
    }

    void BuildSession::Finish(int exitCode)
    {
        if (state_ != BuildState::Running)
        {
            return;
        }
        exitCode_ = exitCode;
        if (cancelRequested_)
        {
            state_ = BuildState::Cancelled;
            return;
        }
        const bool ok = resultSeen_ ? (resultOk_ && exitCode == 0) : exitCode == 0;
        if (ok)
        {
            state_ = BuildState::Done;
            percent_ = 100;
        }
        else
        {
            state_ = BuildState::Failed;
            if (failureText_.empty())
            {
                failureText_ = "The process ended with the code " + std::to_string(exitCode);
            }
        }
    }

    void BuildSession::RequestCancel()
    {
        if (state_ != BuildState::Running || cancelRequested_)
        {
            return;
        }
        cancelRequested_ = true;
        BuildLogLine entry;
        entry.text = "Cancel requested: the process tree is being stopped";
        entry.kind = BuildLineKind::Warning;
        log_.push_back(std::move(entry));
        ++logVersion_;
    }

    void BuildSession::FailToStart(std::string reason)
    {
        if (state_ != BuildState::Running)
        {
            return;
        }
        state_ = BuildState::Failed;
        failureText_ = std::move(reason);
        BuildLogLine entry;
        entry.text = failureText_;
        entry.kind = BuildLineKind::Error;
        log_.push_back(std::move(entry));
        ++errors_;
        ++logVersion_;
    }

    void BuildSession::Clear()
    {
        if (state_ == BuildState::Running)
        {
            return;
        }
        state_ = BuildState::Idle;
        percent_ = 0;
        stage_.clear();
        resultPath_.clear();
        zipPath_.clear();
        failureText_.clear();
        warnings_ = 0;
        errors_ = 0;
        cancelRequested_ = false;
        log_.clear();
        ++logVersion_;
    }
}
