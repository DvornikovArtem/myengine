// RlName.h
// The name of a player (docs/game/rl-online.md, "Ник"): where it comes from is the business of the game (the Steam profile, else the Windows user), what
// may be in it is decided here, once, for every place that shows or sends it: the lobby, the slot of the net match, the table, the feed, the banners.
//
//   - a character that the font atlas of the UI kit has no glyph for (emoji, CJK, ...) becomes "?", control characters and bad UTF-8 as well;
//   - at most kPlayerNameBytes bytes (a slot of the match has kNameBytes with the zero), never cutting a character in the middle;
//   - the spaces at the ends are cut off.
// The glyph ranges are the ones of RlUiFonts.cpp (kRangesFull): the Latin, the Cyrillic and a few signs.

#pragma once

#include <rl/RlTypes.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rl
{
    constexpr std::size_t kPlayerNameBytes = static_cast<std::size_t>(kNameBytes - 1);

    // Has the font of the kit a glyph for the code point? (mirror of kRangesFull in engine/src/ui/rl/RlUiFonts.cpp)
    inline bool NameGlyphKnown(const std::uint32_t c)
    {
        return (c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0x17F) || (c >= 0x400 && c <= 0x4FF) || (c >= 0x2010 && c <= 0x2027) || (c >= 0x2190 && c <= 0x2193) ||
               c == 0x2212 || c == 0x2248;
    }

    inline std::string SanitizePlayerName(const std::string& text)
    {
        std::string out;
        for (std::size_t i = 0; i < text.size();)
        {
            const unsigned char lead = static_cast<unsigned char>(text[i]);
            std::size_t length = 1;
            std::uint32_t code = lead;
            bool valid = true;
            if (lead >= 0xC2 && lead <= 0xDF)
            {
                length = 2;
                code = lead & 0x1Fu;
            }
            else if (lead >= 0xE0 && lead <= 0xEF)
            {
                length = 3;
                code = lead & 0x0Fu;
            }
            else if (lead >= 0xF0 && lead <= 0xF4)
            {
                length = 4;
                code = lead & 0x07u;
            }
            else if (lead >= 0x80)
            {
                valid = false;
            }
            if (valid && length > 1)
            {
                if (i + length > text.size())
                {
                    valid = false;
                }
                for (std::size_t k = 1; valid && k < length; ++k)
                {
                    const unsigned char next = static_cast<unsigned char>(text[i + k]);
                    valid = (next & 0xC0) == 0x80;
                    code = (code << 6) | (next & 0x3Fu);
                }
            }
            if (!valid)
            {
                out += '?';
                ++i;
                continue;
            }
            const std::size_t used = length;
            i += used;
            if (code < 0x20 || code == 0x7F)
            {
                continue;  // control characters (tab, newline, ...) are dropped
            }
            if (NameGlyphKnown(code))
            {
                out.append(text, i - used, used);
            }
            else
            {
                out += '?';
            }
        }
        std::size_t begin = 0;
        while (begin < out.size() && out[begin] == ' ')
        {
            ++begin;
        }
        std::size_t end = out.size();
        while (end > begin && out[end - 1] == ' ')
        {
            --end;
        }
        out = out.substr(begin, end - begin);
        if (out.size() > kPlayerNameBytes)
        {
            std::size_t cut = kPlayerNameBytes;
            while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            {
                --cut;
            }
            out.resize(cut);
            while (!out.empty() && out.back() == ' ')
            {
                out.pop_back();
            }
        }
        return out;
    }

    // ---- the nickname that the player types (Settings, "Player name"; without Steam)
    constexpr int kNicknameMinChars = 2;
    constexpr int kNicknameMaxChars = 16;

    inline std::size_t Utf8CharLength(const unsigned char lead)
    {
        return lead >= 0xF0 ? 4 : (lead >= 0xE0 ? 3 : (lead >= 0xC0 ? 2 : 1));
    }

    inline int NameCharCount(const std::string& text)
    {
        int count = 0;
        for (const char c : text)
        {
            count += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
        }
        return count;
    }

    inline std::uint32_t Utf8CodePoint(const std::string& character)
    {
        const unsigned char lead = static_cast<unsigned char>(character[0]);
        std::uint32_t code = character.size() == 1 ? lead : (character.size() == 2 ? (lead & 0x1Fu) : (character.size() == 3 ? (lead & 0x0Fu) : (lead & 0x07u)));
        for (std::size_t k = 1; k < character.size(); ++k)
        {
            code = (code << 6) | (static_cast<unsigned char>(character[k]) & 0x3Fu);
        }
        return code;
    }

    // The characters of a typed nickname: Latin and Cyrillic letters, digits, the space, "_", "-" and "."
    inline bool NicknameCharAllowed(const std::uint32_t c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-' || c == '.' || (c >= 0xC0 && c <= 0x17F && c != 0xD7 && c != 0xF7) ||
               (c >= 0x400 && c <= 0x4FF);
    }

    // What the field keeps while the player types: only those characters (the others are not typed at all), no space at the start, at most kNicknameMaxChars
    // characters and kPlayerNameBytes bytes (a slot of the match: 16 Cyrillic letters fill it). The spaces at the end are cut off when the nickname is kept.
    inline std::string FilterNicknameInput(const std::string& text)
    {
        std::string out;
        int count = 0;
        for (std::size_t i = 0; i < text.size();)
        {
            const std::size_t length = std::min(Utf8CharLength(static_cast<unsigned char>(text[i])), text.size() - i);
            const std::string character = text.substr(i, length);
            i += length;
            if (SanitizePlayerName(character) != character && character != " ")
            {
                continue;  // no glyph, a control character, bad UTF-8
            }
            if (!NicknameCharAllowed(Utf8CodePoint(character)) || (character == " " && out.empty()))
            {
                continue;
            }
            if (count >= kNicknameMaxChars || out.size() + character.size() > kPlayerNameBytes)
            {
                break;
            }
            out += character;
            ++count;
        }
        return out;
    }

    // The nickname that is kept: the spaces at the ends are cut off, then it must be 2..16 characters of the allowed ones (an empty one, a single letter,
    // a control character are refused)
    inline std::string TrimNickname(const std::string& text)
    {
        std::size_t begin = 0;
        std::size_t end = text.size();
        while (begin < end && text[begin] == ' ')
        {
            ++begin;
        }
        while (end > begin && text[end - 1] == ' ')
        {
            --end;
        }
        return text.substr(begin, end - begin);
    }

    inline bool NicknameValid(const std::string& text)
    {
        if (text.empty() || text != TrimNickname(text) || FilterNicknameInput(text) != text)
        {
            return false;
        }
        const int count = NameCharCount(text);
        return count >= kNicknameMinChars && count <= kNicknameMaxChars;
    }
}
