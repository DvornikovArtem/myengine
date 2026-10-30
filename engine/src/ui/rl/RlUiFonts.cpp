#include <myengine/ui/rl/RlUiFonts.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <imgui/imgui.h>
#include <imgui/misc/imgui_freetype.h>

namespace myengine::rlui
{
    namespace
    {
        constexpr int kStyleCount = static_cast<int>(FontStyle::Count);

        const char* const kIconFile = "lucide.ttf";

        // The sizes of the spec (4.3) that every style needs. Black Italic: the digits, the headlines, the banners; Extra Bold Italic: the
        // buttons and the tabs; Bold and Semi Bold: the plates, the rows, the captions.
        FontSet MakeDefaultSet()
        {
            FontSet set;
            const char* const files[kStyleCount] = {"exo2/Exo2-BlackItalic.ttf", "exo2/Exo2-ExtraBoldItalic.ttf", "exo2/Exo2-Bold.ttf", "exo2/Exo2-SemiBold.ttf"};
            const std::vector<float> sizes[kStyleCount] = {
                {26.0f, 34.0f, 52.0f, 60.0f, 64.0f, 84.0f, 96.0f, 150.0f, 220.0f},
                {16.0f, 18.0f, 22.0f, 26.0f, 34.0f, 38.0f},
                {16.0f, 18.0f, 20.0f, 22.0f, 24.0f, 26.0f},
                {16.0f, 18.0f, 20.0f, 22.0f, 24.0f, 26.0f},
            };
            for (int s = 0; s < kStyleCount; ++s)
            {
                set.files[s] = files[s];
                for (const float size : sizes[s])
                {
                    set.sizes[s].push_back({size, {}});
                }
            }
            set.requiredFile = files[0];
            return set;
        }

        FontSet g_set = MakeDefaultSet();   // the set in use
        // the ranges of the set in the form ImGui keeps (pairs and the zero at the end): ImGui points into them until the atlas is built
        std::vector<ImWchar> g_setRanges[kStyleCount];
        std::vector<std::vector<ImWchar>> g_sizeRanges[kStyleCount];
        const std::vector<float> kIconSizes = {16.0f, 18.0f, 20.0f, 22.0f, 26.0f, 30.0f, 40.0f};

        // The Lucide icons the kit draws (Icon, in the order of the enum, None first): U+E140 play, E184 target, E158 settings, E112 log-out,
        // E149 refresh-cw, E0F8 house, E17A star, E19F user, E1BB bot, E1AE wifi, E1AF wifi-off, E15C shield, E1B4 zap, E349 circle-dot,
        // E157 server, E111 log-in, E0BE eye, E0FE info, E072 chevron-left, E073 chevron-right, E381 arrow-up-down, E24A arrow-left-right,
        // E1D5 car, E070 check, E14F ruler
        const ImWchar kIconCodepoints[] = {0, 0xE140, 0xE184, 0xE158, 0xE112, 0xE149, 0xE0F8, 0xE17A, 0xE19F, 0xE1BB, 0xE1AE, 0xE1AF, 0xE15C,
                                           0xE1B4, 0xE349, 0xE157, 0xE111, 0xE0BE, 0xE0FE, 0xE072, 0xE073, 0xE381, 0xE24A, 0xE1D5, 0xE070, 0xE14F};

        // The glyphs of a size: big sizes get only what is drawn with them, otherwise the atlas would not fit a 4K screen
        const ImWchar kRangesFull[] = {0x0020, 0x007E, 0x00A0, 0x017F, 0x0400, 0x04FF, 0x2010, 0x2027, 0x2190, 0x2193, 0x2212, 0x2212, 0x2248, 0x2248, 0};
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

        // "FLYDOM" -> the ranges of the characters (sorted, each a range of its own, the neighbours joined)
        std::vector<ImWchar> RangesOfGlyphs(const std::string& glyphs)
        {
            std::vector<unsigned char> chars(glyphs.begin(), glyphs.end());
            std::sort(chars.begin(), chars.end());
            chars.erase(std::unique(chars.begin(), chars.end()), chars.end());
            std::vector<ImWchar> ranges;
            for (std::size_t i = 0; i < chars.size();)
            {
                std::size_t j = i;
                while (j + 1 < chars.size() && chars[j + 1] == chars[j] + 1)
                {
                    ++j;
                }
                ranges.push_back(chars[i]);
                ranges.push_back(chars[j]);
                i = j + 1;
            }
            ranges.push_back(0);
            return ranges;
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
        static std::vector<std::string> files;
        files.clear();
        for (int s = 0; s < kStyleCount; ++s)
        {
            files.push_back(g_set.files[s]);
        }
        files.push_back(kIconFile);
        return files;
    }

    const FontSet& DefaultFontSet()
    {
        static const FontSet set = MakeDefaultSet();
        return set;
    }

    const FontSet& ActiveFontSet() { return g_set; }

    void SetFontSet(const FontSet& set)
    {
        g_set = set;
        for (int s = 0; s < kStyleCount; ++s)
        {
            // the data of the old files goes, the new ones are read by the next Bake
            g_styleData[s] = FileData{};
            g_setRanges[s].clear();
            for (const std::uint32_t value : g_set.ranges[s])
            {
                g_setRanges[s].push_back(static_cast<ImWchar>(value));
            }
            if (!g_setRanges[s].empty())
            {
                g_setRanges[s].push_back(0);
            }
            g_sizeRanges[s].clear();
            for (const FontSizeSpec& spec : g_set.sizes[s])
            {
                g_sizeRanges[s].push_back(spec.glyphs.empty() ? std::vector<ImWchar>() : RangesOfGlyphs(spec.glyphs));
            }
        }
    }

    const std::vector<float>& Fonts::DesignSizes(const FontStyle style)
    {
        static std::vector<float> sizes[kStyleCount];
        std::vector<float>& list = sizes[static_cast<int>(style)];
        list.clear();
        for (const FontSizeSpec& spec : g_set.sizes[static_cast<int>(style)])
        {
            list.push_back(spec.size);
        }
        return list;
    }

    void Fonts::Clear()
    {
        slots_.clear();
        ready_ = false;
    }

    namespace
    {
        // How a font of the kit is rasterised and sized. Text that is "soft" on a screen of 1920 x 1080 had three causes, all in this file or its user:
        //   - stb_truetype has no hinting and OversampleH = 2 blurs every glyph horizontally by a box filter: FreeType (the native hinting of the file where it
        //     has one, the auto-hinter where not) snaps the stems and the baseline to the pixels, and OversampleH = 1 leaves the glyph as it was rasterised;
        //   - ImGui keeps a baked font at a WHOLE number of pixels (ImGui::GetRoundedFontSize), a quarter of a pixel in the baked size came out as a different size
        //     than the one the text was drawn at, and the glyphs were scaled by a fraction (FontHandle::DrawSize draws at the baked size when it is close);
        //   - a glyph advance that is not a whole number of pixels puts the next glyph between two pixels: PixelSnapH rounds it.
        // The loader is the one of the atlas, so it is set for the atlas in Bake (the editor keeps stb_truetype in its own atlas, without the game UI).
        // Returns the size to bake in pixels.
        float Configure(ImFontConfig& config, const float pixelsWanted)
        {
            config.OversampleH = 1;
            config.OversampleV = 1;
            config.PixelSnapH = true;
            return std::max(std::round(pixelsWanted), 1.0f);
        }
    }

    bool Fonts::Bake(ImFontAtlas& atlas, const std::filesystem::path& fontDirectory, const float scale, std::string* error)
    {
        Clear();
        scale_ = std::max(scale, 0.25f);
        atlas.SetFontLoader(ImGuiFreeType::GetFontLoader());
        for (int s = 0; s < kStyleCount; ++s)
        {
            if (!LoadFile(fontDirectory / g_set.files[s], g_styleData[s]))
            {
                if (error != nullptr)
                {
                    *error = "the font " + g_set.files[s] + " is missing in " + fontDirectory.string();
                }
                return false;
            }
        }
        const bool haveIcons = LoadFile(fontDirectory / kIconFile, g_iconData);

        for (int s = 0; s < kStyleCount; ++s)
        {
            for (std::size_t k = 0; k < g_set.sizes[s].size(); ++k)
            {
                const float design = g_set.sizes[s][k].size;
                ImFontConfig config;
                config.FontDataOwnedByAtlas = false;
                const float pixels = Configure(config, design * scale_ * g_styleData[s].lineScale);
                const ImWchar* ranges = RangesFor(design);
                if (k < g_sizeRanges[s].size() && !g_sizeRanges[s][k].empty())
                {
                    ranges = g_sizeRanges[s][k].data();
                }
                else if (!g_setRanges[s].empty())
                {
                    ranges = g_setRanges[s].data();
                }
                ImFont* font = atlas.AddFontFromMemoryTTF(g_styleData[s].bytes.data(), static_cast<int>(g_styleData[s].bytes.size()), pixels, &config, ranges);
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
                const float pixels = Configure(config, design * scale_ * g_iconData.lineScale);
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
