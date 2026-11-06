#include "PrankSettings.h"

#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

#include <windows.h>

#include <myengine/assistant/AssistantSettings.h>

namespace myengine::ui::prank
{
    namespace
    {
        using json = nlohmann::json;

        bool g_initialized = false;
        bool g_setting = true;
        bool g_noPrank = false;
        bool g_fontsWanted = true;

        void EnsureInitialized();

        bool ReadObject(const std::filesystem::path& file, json& out)
        {
            std::ifstream stream(file, std::ios::binary);
            if (!stream)
            {
                return false;
            }
            const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            out = json::parse(text, nullptr, false);
            return !out.is_discarded() && out.is_object();
        }
    }

    std::filesystem::path SettingsFile()
    {
        wchar_t override[1024]{};
        if (GetEnvironmentVariableW(L"MYENGINE_ASSISTANT_SETTINGS", override, static_cast<DWORD>(std::size(override))) > 0)
        {
            return std::filesystem::path(override);
        }
        return assistant::AssistantSettings::DefaultFile();
    }

    bool LoadEnabled(const std::filesystem::path& file)
    {
        json root;
        if (file.empty() || !ReadObject(file, root))
        {
            return true;
        }
        const auto it = root.find("prankTab");
        return it == root.end() || !it->is_boolean() || it->get<bool>();
    }

    bool SaveEnabled(const std::filesystem::path& file, const bool enabled, std::string& error)
    {
        if (file.empty())
        {
            error = "The settings folder (APPDATA) is unknown.";
            return false;
        }
        json root = json::object();
        {
            json existing;
            if (ReadObject(file, existing))
            {
                root = std::move(existing);
            }
        }
        root["prankTab"] = enabled;
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

    void Initialize(const bool noPrankOnCommandLine)
    {
        g_noPrank = noPrankOnCommandLine;
        g_setting = LoadEnabled(SettingsFile());
        g_fontsWanted = g_setting && !g_noPrank;
        g_initialized = true;
    }

    namespace
    {
        // --no-prank is MYENGINE_NO_PRANK=1 by the time the editor starts (app/src/main.cpp)
        void EnsureInitialized()
        {
            if (!g_initialized)
            {
                wchar_t value[16]{};
                Initialize(GetEnvironmentVariableW(L"MYENGINE_NO_PRANK", value, static_cast<DWORD>(std::size(value))) > 0 && value[0] != L'0');
            }
        }
    }

    bool IsInitialized()
    {
        return g_initialized;
    }

    bool Enabled()
    {
        EnsureInitialized();
        return g_setting && !g_noPrank;
    }

    bool Hidden()
    {
        EnsureInitialized();
        return g_noPrank;
    }

    bool SettingOn()
    {
        EnsureInitialized();
        return g_setting;
    }

    void SetEnabled(const bool enabled)
    {
        EnsureInitialized();
        g_setting = enabled;
        std::string ignored;
        SaveEnabled(SettingsFile(), enabled, ignored);
    }

    bool FontsWanted()
    {
        EnsureInitialized();
        return g_fontsWanted;
    }
}
