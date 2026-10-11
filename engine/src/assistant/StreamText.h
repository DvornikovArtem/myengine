// StreamText.h
//
// Small helpers of the CLI event parsers (Claude Code, Codex): reading a JSON field and cutting text for a one-line summary.
// Internal to the assistant module.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace myengine::assistant::detail
{
    inline std::string GetString(const nlohmann::json& object, const char* key)
    {
        if (!object.is_object())
        {
            return {};
        }
        const auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    inline std::uint64_t GetUnsigned(const nlohmann::json& object, const char* key)
    {
        if (!object.is_object())
        {
            return 0;
        }
        const auto it = object.find(key);
        if (it == object.end() || !it->is_number())
        {
            return 0;
        }
        const double value = it->get<double>();
        return value > 0.0 ? static_cast<std::uint64_t>(value) : 0;
    }

    inline const nlohmann::json* GetChild(const nlohmann::json& object, const char* key)
    {
        if (!object.is_object())
        {
            return nullptr;
        }
        const auto it = object.find(key);
        return it != object.end() ? &*it : nullptr;
    }

    // Never cuts inside a UTF-8 sequence
    inline std::string Shorten(std::string text, const std::size_t limit)
    {
        if (text.size() > limit)
        {
            std::size_t cut = limit;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
            {
                --cut;
            }
            text.resize(cut);
            text += "...";
        }
        return text;
    }

    inline std::string FirstLines(const std::string& text, const std::size_t lines, const std::size_t limit)
    {
        std::size_t end = 0;
        for (std::size_t count = 0; count < lines && end != std::string::npos; ++count)
        {
            end = text.find('\n', end == 0 ? 0 : end + 1);
        }
        const bool omitted = end != std::string::npos && text.find_first_not_of(" \t\r\n", end) != std::string::npos;
        std::string result = end == std::string::npos ? text : text.substr(0, end);
        while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == ' '))
        {
            result.pop_back();
        }
        if (omitted)
        {
            result += " ...";
        }
        return Shorten(std::move(result), limit);
    }
}
