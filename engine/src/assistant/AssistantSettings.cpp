// AssistantSettings.cpp

#include <myengine/assistant/AssistantSettings.h>

#include <fstream>
#include <iterator>
#include <system_error>

#include <windows.h>

#include <nlohmann/json.hpp>

namespace myengine::assistant
{
    namespace
    {
        using json = nlohmann::json;

        bool ReadObject(const std::filesystem::path& file, json& out, std::string& problem)
        {
            std::ifstream stream(file, std::ios::binary);
            if (!stream)
            {
                problem = "The settings file is missing: " + file.u8string();
                return false;
            }
            const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            out = json::parse(text, nullptr, false);
            if (out.is_discarded() || !out.is_object())
            {
                problem = "The settings file is not a JSON object: " + file.u8string();
                return false;
            }
            return true;
        }

        std::string StringOf(const json& object, const char* key)
        {
            const auto it = object.find(key);
            return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
        }
    }

    const char* BackendKindId(const AssistantBackendKind kind)
    {
        switch (kind)
        {
            case AssistantBackendKind::Claude:
                return "claude";
            case AssistantBackendKind::Codex:
                return "codex";
            case AssistantBackendKind::None:
                break;
        }
        return "none";
    }

    const char* BackendKindLabel(const AssistantBackendKind kind)
    {
        switch (kind)
        {
            case AssistantBackendKind::Claude:
                return "Claude Code";
            case AssistantBackendKind::Codex:
                return "Codex";
            case AssistantBackendKind::None:
                break;
        }
        return "Off";
    }

    AssistantBackendKind ParseBackendKind(const std::string& text)
    {
        if (text == "claude")
        {
            return AssistantBackendKind::Claude;
        }
        if (text == "codex")
        {
            return AssistantBackendKind::Codex;
        }
        return AssistantBackendKind::None;
    }

    std::filesystem::path AssistantSettings::DefaultFile()
    {
        wchar_t buffer[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
        if (length == 0 || length >= std::size(buffer))
        {
            return {};
        }
        return std::filesystem::path(buffer) / L"myengine" / L"editor_settings.json";
    }

    AssistantSettings AssistantSettings::Load(const std::filesystem::path& file, std::string* problem)
    {
        AssistantSettings settings;
        if (file.empty())
        {
            return settings;
        }
        json root;
        std::string reason;
        if (!ReadObject(file, root, reason))
        {
            // No file is the normal state of a clean installation, not a problem worth a message
            std::error_code error;
            if (problem != nullptr && std::filesystem::exists(file, error))
            {
                *problem = reason;
            }
            return settings;
        }
        const auto it = root.find("assistant");
        if (it == root.end() || !it->is_object())
        {
            return settings;
        }
        settings.backend = ParseBackendKind(StringOf(*it, "backend"));
        const auto claude = StringOf(*it, "claudePath");
        const auto codex = StringOf(*it, "codexPath");
        if (!claude.empty())
        {
            settings.claudePath = std::filesystem::u8path(claude);
        }
        if (!codex.empty())
        {
            settings.codexPath = std::filesystem::u8path(codex);
        }
        settings.codexModel = StringOf(*it, "codexModel");
        if (const auto allow = it->find("allowExternalMcp"); allow != it->end() && allow->is_boolean())
        {
            settings.allowExternalMcp = allow->get<bool>();
        }
        return settings;
    }

    bool AssistantSettings::Save(const std::filesystem::path& file, std::string& error) const
    {
        if (file.empty())
        {
            error = "The settings folder (APPDATA) is unknown.";
            return false;
        }
        json root = json::object();
        {
            std::string ignored;
            json existing;
            if (ReadObject(file, existing, ignored))
            {
                root = std::move(existing); // other keys of the file stay as they are
            }
        }
        root["assistant"] = {
            {"backend", BackendKindId(backend)},
            {"claudePath", claudePath.u8string()},
            {"codexPath", codexPath.u8string()},
            {"codexModel", codexModel},
            {"allowExternalMcp", allowExternalMcp},
        };

        std::error_code fileError;
        std::filesystem::create_directories(file.parent_path(), fileError);
        auto temp = file;
        temp += L".tmp";
        {
            std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
            stream << root.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
            if (!stream)
            {
                error = "Could not write " + temp.u8string();
                return false;
            }
        }
        std::filesystem::rename(temp, file, fileError);
        if (fileError)
        {
            std::filesystem::remove(temp, fileError);
            error = "Could not replace " + file.u8string();
            return false;
        }
        return true;
    }
}
