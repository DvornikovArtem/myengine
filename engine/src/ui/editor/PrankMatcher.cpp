#include "PrankMatcher.h"

#include "PrankStrings.h"

#include <cstddef>

namespace myengine::ui::prank
{
    namespace
    {
        // One UTF-8 code point at `i`; returns its length (at least 1, so that a broken text still moves on)
        std::size_t Decode(const std::string& text, const std::size_t i, std::uint32_t& code)
        {
            const auto byte = static_cast<unsigned char>(text[i]);
            if (byte < 0x80)
            {
                code = byte;
                return 1;
            }
            const std::size_t length = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 1;
            if (length == 1 || i + length > text.size())
            {
                code = byte;
                return 1;
            }
            code = byte & (0xFFu >> (length + 1));
            for (std::size_t k = 1; k < length; ++k)
            {
                code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
            }
            return length;
        }

        void Encode(std::string& out, const std::uint32_t code)
        {
            if (code < 0x80)
            {
                out += static_cast<char>(code);
            }
            else if (code < 0x800)
            {
                out += static_cast<char>(0xC0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
            else if (code < 0x10000)
            {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xF0 | (code >> 18));
                out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
        }

        bool ContainsAny(const std::string& folded, const char* const* words, const std::size_t count)
        {
            for (std::size_t i = 0; i < count; ++i)
            {
                if (folded.find(FoldCase(words[i])) != std::string::npos)
                {
                    return true;
                }
            }
            return false;
        }

        std::uint32_t Mix(std::uint32_t seed)
        {
            seed ^= seed >> 16;
            seed *= 0x7FEB352Du;
            seed ^= seed >> 15;
            seed *= 0x846CA68Bu;
            seed ^= seed >> 16;
            return seed;
        }
    }

    const char* SceneFile(const MiniScene scene)
    {
        switch (scene)
        {
            case MiniScene::Octopus: return "assets/prank/scenes/octopus.json";
            case MiniScene::Fox: return "assets/prank/scenes/fox.json";
            case MiniScene::Lava: return "assets/prank/scenes/lava.json";
            case MiniScene::Hardcore: return "assets/prank/scenes/hardcore.json";
            case MiniScene::NakedCars: return "assets/prank/scenes/nakedcars.json";
            case MiniScene::Count: break;
        }
        return "";
    }

    const char* SceneId(const MiniScene scene)
    {
        switch (scene)
        {
            case MiniScene::Octopus: return "octopus";
            case MiniScene::Fox: return "fox";
            case MiniScene::Lava: return "lava";
            case MiniScene::Hardcore: return "hardcore";
            case MiniScene::NakedCars: return "nakedcars";
            case MiniScene::Count: break;
        }
        return "";
    }

    std::string FoldCase(const std::string& utf8)
    {
        std::string out;
        out.reserve(utf8.size());
        for (std::size_t i = 0; i < utf8.size();)
        {
            std::uint32_t code = 0;
            const std::size_t length = Decode(utf8, i, code);
            if (code >= 'A' && code <= 'Z')
            {
                code += 32;
            }
            else if (code >= 0x410 && code <= 0x42F)
            {
                code += 32; // Cyrillic capitals -> small
            }
            else if (code == 0x401 || code == 0x451)
            {
                code = 0x435; // yo -> ye
            }
            else if (code >= 0x400 && code <= 0x40F)
            {
                code += 80;
            }
            if (length == 1 && code >= 0x80)
            {
                out += utf8[i]; // a broken byte stays
            }
            else
            {
                Encode(out, code);
            }
            i += length;
        }
        return out;
    }

    Match MatchRequest(const std::string& request, const std::uint32_t seed)
    {
        const std::string folded = FoldCase(request);
        Match match;
        // Refusals first: children, real people, violence against the living
        if (ContainsAny(folded, str::kRefusalWords.data(), str::kRefusalWords.size()))
        {
            match.refused = true;
            match.scene = static_cast<MiniScene>(Mix(seed) % kMiniSceneCount);
            return match;
        }
        struct Rule
        {
            MiniScene scene;
            const char* const* words;
            std::size_t count;
        };
        const Rule rules[] = {
            {MiniScene::Octopus, str::kOctopusWords.data(), str::kOctopusWords.size()},
            {MiniScene::Fox, str::kFoxWords.data(), str::kFoxWords.size()},
            {MiniScene::Lava, str::kLavaWords.data(), str::kLavaWords.size()},
            {MiniScene::Hardcore, str::kHardcoreWords.data(), str::kHardcoreWords.size()},
            {MiniScene::NakedCars, str::kNakedWords.data(), str::kNakedWords.size()},
        };
        for (const Rule& rule : rules)
        {
            if (ContainsAny(folded, rule.words, rule.count))
            {
                match.scene = rule.scene;
                match.keyword = true;
                return match;
            }
        }
        match.scene = static_cast<MiniScene>(Mix(seed) % kMiniSceneCount);
        return match;
    }

    std::string AnswerText(const Match& match, const std::uint32_t seed)
    {
        std::string text;
        if (match.refused)
        {
            text = str::kAnswerRefusal;
        }
        else if (!match.keyword)
        {
            text = str::kAnswerShrug;
        }
        else
        {
            switch (match.scene)
            {
                case MiniScene::Octopus: text = str::kAnswerOctopus; break;
                case MiniScene::Fox: text = str::kAnswerFox; break;
                case MiniScene::Lava: text = str::kAnswerLava; break;
                case MiniScene::Hardcore: text = str::kAnswerHardcore; break;
                case MiniScene::NakedCars: text = str::kAnswerNaked; break;
                case MiniScene::Count: break;
            }
        }
        text += "\n\n";
        text += str::kClosings[Mix(seed + 7u) % str::kClosings.size()];
        return text;
    }

    int StepCount(const MiniScene scene)
    {
        return static_cast<int>(str::SceneSteps(scene).size());
    }

    const char* StepText(const MiniScene scene, const int index)
    {
        const auto& steps = str::SceneSteps(scene);
        if (index < 0 || index >= static_cast<int>(steps.size()))
        {
            return "";
        }
        return steps[static_cast<std::size_t>(index)];
    }
}
