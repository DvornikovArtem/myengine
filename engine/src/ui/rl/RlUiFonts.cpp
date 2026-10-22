#include <myengine/ui/rl/RlUiFonts.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <imgui/imgui.h>

namespace myengine::rlui
{
    namespace
    {
        constexpr int kStyleCount = static_cast<int>(FontStyle::Count);

        const char* const kStyleFiles[kStyleCount] = {"exo2/Exo2-BlackItalic.ttf", "exo2/Exo2-ExtraBoldItalic.ttf", "exo2/Exo2-Bold.ttf", "exo2/Exo2-SemiBold.ttf"};
        const char* const kIconFile = "lucide.ttf";

        // The sizes of the spec (4.3) that every style needs. Black Italic: the digits, the headlines, the banners; Extra Bold Italic: the
        // buttons and the tabs; Bold and Semi Bold: the plates, the rows, the captions.
        const std::vector<float> kSizes[kStyleCount] = {
            {26.0f, 34.0f, 52.0f, 60.0f, 64.0f, 84.0f, 96.0f, 150.0f, 220.0f},
            {16.0f, 18.0f, 22.0f, 26.0f, 34.0f, 38.0f},
            {16.0f, 18.0f, 20.0f, 22.0f, 24.0f, 26.0f},
            {16.0f, 18.0f, 20.0f, 22.0f, 24.0f, 26.0f},
        };
        const std::vector<float> kIconSizes = {16.0f, 18.0f, 20.0f, 22.0f, 26.0f, 30.0f, 40.0f};

        // The Lucide icons the kit draws (Icon, in the order of the enum, None first): U+E140 play, E184 target, E158 settings, E112 log-out,
        // E149 refresh-cw, E0F8 house, E17A star, E19F user, E1BB bot, E1AE wifi, E1AF wifi-off, E15C shield, E1B4 zap, E349 circle-dot,
        // E157 server, E111 log-in, E0BE eye, E0FE info, E072 chevron-left, E073 chevron-right, E381 arrow-up-down, E24A arrow-left-right
        const ImWchar kIconCodepoints[] = {0, 0xE140, 0xE184, 0xE158, 0xE112, 0xE149, 0xE0F8, 0xE17A, 0xE19F, 0xE1BB, 0xE1AE, 0xE1AF, 0xE15C,
                                           0xE1B4, 0xE349, 0xE157, 0xE111, 0xE0BE, 0xE0FE, 0xE072, 0xE073, 0xE381, 0xE24A};

        // The glyphs of a size: big sizes get only what is drawn with them, otherwise the atlas would not fit a 4K screen
        const ImWchar kRangesFull[] = {0x0020, 0x007E, 0x00A0, 0x017F, 0x0400, 0x04FF, 0x2010, 0x2027, 0x2190, 0x2193, 0x2212, 0x2212, 0};
        const ImWchar kRangesLatin[] = {0x0020, 0x007E, 0x00B7, 0x00B7, 0x00D7, 0x00D7, 0x2013, 0x2014, 0};
        const ImWchar kRangesBanner[] = {0x0020, 0x005A, 0x2014, 0x2014, 0};   // space .. Z: the capitals, the digits, the signs
        const ImWchar kRangesDigits[] = {0x0020, 0x0021, 0x002B, 0x003A, 0x0047, 0x0047, 0x004F, 0x004F, 0};   // " !+,-./0-9:GO"

        const ImWchar* RangesFor(const float designSize)
        {
            if (designSize >= 200.0f)
            {
                return kRangesDigits;
            }
            if (designSize >= 120.0f)
            {
                return kRangesBanner;
            }
            if (designSize >= 80.0f)
            {
                return kRangesLatin;
            }
            return kRangesFull;
        }

        // The files live as long as the process: ImGui 1.92 keeps pointing into the data of a font for the whole life of the atlas
        struct FileData
        {
            std::vector<unsigned char> bytes;
            float lineScale = 1.0f;   // (ascent - descent) / unitsPerEm
            bool loaded = false;
        };
        FileData g_styleData[kStyleCount];
        FileData g_iconData;

        std::uint16_t Be16(const unsigned char* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }

        // (ascent - descent) / em from hhea and head; 1.2 when the file cannot be read
        float ReadLineScale(const std::vector<unsigned char>& bytes)
        {
            if (bytes.size() < 12)
            {
                return 1.2f;
            }
            const int tableCount = Be16(bytes.data() + 4);
            int unitsPerEm = 0;
            int ascent = 0;
            int descent = 0;
            for (int i = 0; i < tableCount; ++i)
            {
                const std::size_t entry = 12u + static_cast<std::size_t>(i) * 16u;
                if (entry + 16u > bytes.size())
                {
                    break;
                }
                const unsigned char* tag = bytes.data() + entry;
                const std::size_t offset = (static_cast<std::size_t>(tag[8]) << 24) | (static_cast<std::size_t>(tag[9]) << 16) | (static_cast<std::size_t>(tag[10]) << 8) | tag[11];
                if (std::equal(tag, tag + 4, "head") && offset + 20u <= bytes.size())
                {
                    unitsPerEm = Be16(bytes.data() + offset + 18);
                }
                else if (std::equal(tag, tag + 4, "hhea") && offset + 8u <= bytes.size())
                {
                    ascent = static_cast<std::int16_t>(Be16(bytes.data() + offset + 4));
                    descent = static_cast<std::int16_t>(Be16(bytes.data() + offset + 6));
                }
            }
            return unitsPerEm > 0 && ascent - descent > 0 ? static_cast<float>(ascent - descent) / static_cast<float>(unitsPerEm) : 1.2f;
        }

        bool LoadFile(const std::filesystem::path& path, FileData& data)
        {
            if (data.loaded)
            {
                return true;
            }
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                return false;
            }
            const std::streamoff size = file.tellg();
            if (size <= 0 || size > 64 * 1024 * 1024)
            {
                return false;
            }
            data.bytes.resize(static_cast<std::size_t>(size));
            file.seekg(0, std::ios::beg);
            file.read(reinterpret_cast<char*>(data.bytes.data()), size);
            if (!file)
            {
                data.bytes.clear();
                return false;
            }
            data.lineScale = ReadLineScale(data.bytes);
            data.loaded = true;
            return true;
        }
    }

    const std::vector<std::string>& RequiredFontFiles()
    {
        static const std::vector<std::string> files = {kStyleFiles[0], kStyleFiles[1], kStyleFiles[2], kStyleFiles[3], kIconFile};
        return files;
    }

    const std::vector<float>& Fonts::DesignSizes(const FontStyle style) { return kSizes[static_cast<int>(style)]; }

    void Fonts::Clear()
    {
        slots_.clear();
        ready_ = false;
    }

    bool Fonts::Bake(ImFontAtlas& atlas, const std::filesystem::path& fontDirectory, const float scale, std::string* error)
    {
        Clear();
        scale_ = std::max(scale, 0.25f);
        for (int s = 0; s < kStyleCount; ++s)
        {
            if (!LoadFile(fontDirectory / kStyleFiles[s], g_styleData[s]))
            {
                if (error != nullptr)
                {
                    *error = "the font " + std::string(kStyleFiles[s]) + " is missing in " + fontDirectory.string();
                }
                return false;
            }
        }
        const bool haveIcons = LoadFile(fontDirectory / kIconFile, g_iconData);

        for (int s = 0; s < kStyleCount; ++s)
        {
            for (const float design : kSizes[s])
            {
                ImFontConfig config;
                config.FontDataOwnedByAtlas = false;
                config.OversampleH = design >= 80.0f ? 1 : 2;
                config.OversampleV = 1;
                const float pixels = std::round(design * scale_ * g_styleData[s].lineScale * 4.0f) / 4.0f;
                ImFont* font = atlas.AddFontFromMemoryTTF(g_styleData[s].bytes.data(), static_cast<int>(g_styleData[s].bytes.size()), pixels, &config, RangesFor(design));
                Slot slot;
                slot.style = static_cast<FontStyle>(s);
                slot.designSize = design;
                slot.handle = {font, pixels, design, g_styleData[s].lineScale};
                slots_.push_back(slot);
            }
        }

        if (haveIcons)
        {
            // one range of two numbers per icon (a single codepoint)
            static std::vector<ImWchar> iconRanges;
            if (iconRanges.empty())
            {
                for (std::size_t i = 1; i < std::size(kIconCodepoints); ++i)
                {
                    iconRanges.push_back(kIconCodepoints[i]);
                    iconRanges.push_back(kIconCodepoints[i]);
                }
                iconRanges.push_back(0);
            }
            for (const float design : kIconSizes)
            {
                ImFontConfig config;
                config.FontDataOwnedByAtlas = false;
                config.PixelSnapH = true;
                config.OversampleH = 1;
                config.OversampleV = 1;
                const float pixels = std::round(design * scale_ * g_iconData.lineScale * 4.0f) / 4.0f;
                ImFont* font = atlas.AddFontFromMemoryTTF(g_iconData.bytes.data(), static_cast<int>(g_iconData.bytes.size()), pixels, &config, iconRanges.data());
                Slot slot;
                slot.icons = true;
                slot.designSize = design;
                slot.handle = {font, pixels, design, g_iconData.lineScale};
                slots_.push_back(slot);
            }
        }
        ready_ = true;
        return true;
    }

    FontHandle Fonts::Get(const FontStyle style, const float designSize) const
    {
        const Slot* best = nullptr;
        float bestDistance = 1e9f;
        for (const Slot& slot : slots_)
        {
            if (slot.icons || slot.style != style)
            {
                continue;
            }
            const float distance = std::fabs(std::log(slot.designSize / std::max(designSize, 1.0f)));
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = &slot;
            }
        }
        return best != nullptr ? best->handle : FontHandle{};
    }

    FontHandle Fonts::Icons(const float designSize) const
    {
        const Slot* best = nullptr;
        float bestDistance = 1e9f;
        for (const Slot& slot : slots_)
        {
            if (!slot.icons)
            {
                continue;
            }
            const float distance = std::fabs(std::log(slot.designSize / std::max(designSize, 1.0f)));
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = &slot;
            }
        }
        return best != nullptr ? best->handle : FontHandle{};
    }

    // The codepoint of an icon in lucide.ttf (0 for None); used by the paint functions
    unsigned int IconCodepoint(const Icon icon)
    {
        const int index = static_cast<int>(icon);
        return index > 0 && index < static_cast<int>(std::size(kIconCodepoints)) ? kIconCodepoints[index] : 0u;
    }
}
