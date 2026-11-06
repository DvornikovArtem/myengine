#include "PrankExplorer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <imgui/imgui_internal.h>

#include "EditorWidgets.h"
#include "PrankUi.h"

namespace myengine::ui::prank
{
    namespace
    {
        constexpr float kTitleH = 32.0f;
        constexpr float kNavH = 44.0f;
        constexpr float kCmdH = 40.0f;
        constexpr float kSideW = 210.0f;
        constexpr float kStatusH = 26.0f;
        constexpr float kTileW = 116.0f;
        constexpr float kTileH = 124.0f;
        constexpr float kTileGap = 8.0f;
        constexpr float kGridPad = 16.0f;
        constexpr float kAddressH = 30.0f;
        constexpr float kSearchW = 230.0f;
        constexpr float kPi = 3.14159265358979f;

        ImVec2 V(const float x, const float y)
        {
            return ImVec2(x, y);
        }

        ImVec2 operator+(const ImVec2 a, const ImVec2 b)
        {
            return ImVec2(a.x + b.x, a.y + b.y);
        }

        // ---- icons (draw list only, no textures) ----

        // The folder of the spec: 72 x 58 at scale 1
        void DrawFolder(ImDrawList* list, const ImVec2 pos, const float scale, const float alpha)
        {
            auto P = [&](const float x, const float y) { return ImVec2(pos.x + x * scale, pos.y + y * scale); };
            const ImU32 back = WithAlpha(Rgb(0xE2A12E), alpha);
            list->AddConvexPolyFilled(std::vector<ImVec2>{P(0, 6), P(4, 0), P(28, 0), P(34, 8), P(0, 8)}.data(), 5, back);
            list->AddRectFilled(P(0, 8), P(72, 58), back, 3.0f * scale);
            list->AddRectFilled(P(6, 12), P(66, 52), WithAlpha(kWhite, alpha), 2.0f * scale);
            GradientV(list, P(0, 16), P(72, 58), WithAlpha(Rgb(0xFFD96B), alpha), WithAlpha(Rgb(0xF5BB3A), alpha), 3.0f * scale);
        }

        struct FileLabel
        {
            const char* text;
            unsigned color;
        };

        FileLabel LabelFor(const str::FileKind kind)
        {
            switch (kind)
            {
                case str::FileKind::Txt: return {"TXT", 0x7A7A7A};
                case str::FileKind::Png: return {"PNG", 0x2E9D4F};
                case str::FileKind::Jpg: return {"JPG", 0x2E9D4F};
                case str::FileKind::Pdf: return {"PDF", 0xD0342C};
                case str::FileKind::Obj: return {"OBJ", 0x2C8EC9};
                case str::FileKind::Exe: return {"EXE", 0x5B3FD0};
                case str::FileKind::Mp4: return {"MP4", 0x9B2FC2};
            }
            return {"", 0x7A7A7A};
        }

        // The file of the spec: 52 x 64 at scale 1
        void DrawFile(ImDrawList* list, const ImVec2 pos, const float scale, const str::FileKind kind, const float alpha)
        {
            auto P = [&](const float x, const float y) { return ImVec2(pos.x + x * scale, pos.y + y * scale); };
            const ImVec2 sheet[] = {P(0, 0), P(38, 0), P(52, 14), P(52, 64), P(0, 64)};
            list->AddConvexPolyFilled(sheet, 5, WithAlpha(kWhite, alpha));
            list->AddPolyline(sheet, 5, WithAlpha(Rgb(0xB9B9B9), alpha), ImDrawFlags_Closed, 1.0f);
            const ImVec2 fold[] = {P(38, 0), P(38, 14), P(52, 14)};
            list->AddConvexPolyFilled(fold, 3, WithAlpha(Rgb(0xDCDCDC), alpha));
            for (int i = 0; i < 4; ++i)
            {
                list->AddRectFilled(P(26.0f, 22.0f + static_cast<float>(i) * 6.0f), P(26.0f + 18.0f - static_cast<float>(i % 2) * 6.0f, 24.0f + static_cast<float>(i) * 6.0f), WithAlpha(Rgb(0xC8C8C8), alpha));
            }
            const FileLabel label = LabelFor(kind);
            list->AddRectFilled(P(4, 46), P(30, 60), WithAlpha(Rgb(label.color), alpha), 2.0f * scale);
            TextCentered(list, P(17, 53), 10.0f * scale, WithAlpha(kWhite, alpha), label.text);
        }

        void DrawCrossIcon(ImDrawList* list, const ImVec2 c, const float r, const ImU32 color, const float width)
        {
            list->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), color, width);
            list->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), color, width);
        }

        void DrawChevronDown(ImDrawList* list, const ImVec2 c, const float r, const ImU32 color)
        {
            list->AddLine(ImVec2(c.x - r, c.y - r * 0.5f), ImVec2(c.x, c.y + r * 0.5f), color, 1.4f);
            list->AddLine(ImVec2(c.x, c.y + r * 0.5f), ImVec2(c.x + r, c.y - r * 0.5f), color, 1.4f);
        }

        void DrawRefreshIcon(ImDrawList* list, const ImVec2 c, const float r, const ImU32 color)
        {
            list->PathClear();
            for (int i = 0; i <= 18; ++i)
            {
                const float a = -0.6f + static_cast<float>(i) / 18.0f * 4.9f;
                list->PathLineTo(ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r));
            }
            list->PathStroke(color, 0, 1.6f);
            const ImVec2 tip(c.x + std::cos(-0.6f) * r, c.y + std::sin(-0.6f) * r);
            list->AddTriangleFilled(ImVec2(tip.x + 4.0f, tip.y - 3.0f), ImVec2(tip.x - 4.0f, tip.y - 3.0f), ImVec2(tip.x + 1.0f, tip.y + 4.0f), color);
        }

        void DrawArrowIcon(ImDrawList* list, const ImVec2 c, const int direction, const ImU32 color)
        {
            // 0 left, 1 right, 2 up
            const float r = 6.0f;
            if (direction == 2)
            {
                list->AddLine(ImVec2(c.x, c.y + r), ImVec2(c.x, c.y - r), color, 1.6f);
                list->AddLine(ImVec2(c.x - 5.0f, c.y - 1.0f), ImVec2(c.x, c.y - r), color, 1.6f);
                list->AddLine(ImVec2(c.x + 5.0f, c.y - 1.0f), ImVec2(c.x, c.y - r), color, 1.6f);
                return;
            }
            const float s = direction == 0 ? -1.0f : 1.0f;
            list->AddLine(ImVec2(c.x - s * r, c.y), ImVec2(c.x + s * r, c.y), color, 1.6f);
            list->AddLine(ImVec2(c.x + s * 1.0f, c.y - 5.0f), ImVec2(c.x + s * r, c.y), color, 1.6f);
            list->AddLine(ImVec2(c.x + s * 1.0f, c.y + 5.0f), ImVec2(c.x + s * r, c.y), color, 1.6f);
        }

        void DrawBannerIcon(ImDrawList* list, const ImVec2 c, const float size, const str::BannerKind kind, const ImU32 color, const ImU32 mark)
        {
            const float r = size * 0.5f;
            if (kind == str::BannerKind::Info)
            {
                list->AddCircleFilled(c, r, color, 20);
                Text(list, ImVec2(c.x - r * 0.18f, c.y - r * 0.85f), size * 0.8f, mark, "i");
            }
            else if (kind == str::BannerKind::Warning || kind == str::BannerKind::Red)
            {
                list->AddTriangleFilled(ImVec2(c.x, c.y - r), ImVec2(c.x - r * 1.1f, c.y + r * 0.8f), ImVec2(c.x + r * 1.1f, c.y + r * 0.8f), color);
                TextCentered(list, ImVec2(c.x, c.y + r * 0.2f), size * 0.8f, mark, "!");
            }
            else
            {
                list->AddCircleFilled(c, r, color, 24);
                list->AddLine(ImVec2(c.x - r * 0.6f, c.y), ImVec2(c.x + r * 0.6f, c.y), mark, std::max(2.0f, size * 0.12f));
            }
        }

        // ---- the little viewers of the files (440 x 300), drawn with primitives ----

        void DrawDiskViewer(ImDrawList* list, const ImVec2 min, const ImVec2 max, const double time)
        {
            const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f - 6.0f);
            list->AddRectFilled(ImVec2(c.x - 120.0f, c.y - 80.0f), ImVec2(c.x + 120.0f, c.y + 80.0f), Rgb(0x8D939A), 10.0f);
            list->AddRect(ImVec2(c.x - 120.0f, c.y - 80.0f), ImVec2(c.x + 120.0f, c.y + 80.0f), Rgb(0x5C6168), 10.0f, 0, 3.0f);
            list->AddCircleFilled(c, 62.0f, Rgb(0xC9CED4), 48);
            list->AddCircle(c, 62.0f, Rgb(0x6C7279), 48, 2.0f);
            const float spin = static_cast<float>(time) * 6.0f;
            for (int i = 0; i < 3; ++i)
            {
                const float a = spin + static_cast<float>(i) * 2.0944f;
                list->AddLine(c, ImVec2(c.x + std::cos(a) * 58.0f, c.y + std::sin(a) * 58.0f), Rgb(0xA9AFB6), 3.0f);
            }
            list->AddCircleFilled(c, 11.0f, Rgb(0x7A8088), 20);
            list->AddCircleFilled(c, 4.0f, Rgb(0xE8EAEC), 12);
            list->AddLine(ImVec2(c.x + 110.0f, c.y - 70.0f), ImVec2(c.x + 36.0f, c.y - 26.0f), Rgb(0x474B50), 5.0f);
            list->AddCircleFilled(ImVec2(c.x + 110.0f, c.y - 70.0f), 7.0f, Rgb(0x474B50), 16);
            list->AddCircleFilled(ImVec2(c.x - 104.0f, c.y + 64.0f), 4.0f, Rgb(0x4CAF50), 12);
        }

        void DrawSinesViewer(ImDrawList* list, const ImVec2 min, const ImVec2 max, const double time)
        {
            const float x0 = min.x + 24.0f;
            const float x1 = max.x - 24.0f;
            const float cy = (min.y + max.y) * 0.5f - 4.0f;
            list->AddLine(ImVec2(x0, cy), ImVec2(x1, cy), Rgb(0xC9C9C9), 1.0f);
            list->AddLine(ImVec2(x0, min.y + 10.0f), ImVec2(x0, max.y - 10.0f), Rgb(0xC9C9C9), 1.0f);
            const float amps[3] = {42.0f, 22.0f, 14.0f};
            const float freqs[3] = {1.0f, 3.0f, 5.0f};
            const ImU32 colors[3] = {Rgb(0xE8B04A), Rgb(0x58A6E0), Rgb(0x6BBF6B)};
            const float phase = static_cast<float>(time) * 1.4f;
            for (int k = 0; k < 4; ++k)
            {
                list->PathClear();
                for (int i = 0; i <= 80; ++i)
                {
                    const float t = static_cast<float>(i) / 80.0f;
                    float y = 0.0f;
                    if (k < 3)
                    {
                        y = std::sin(t * 6.2832f * freqs[k] * 1.5f + phase) * amps[k];
                    }
                    else
                    {
                        for (int j = 0; j < 3; ++j)
                        {
                            y += std::sin(t * 6.2832f * freqs[j] * 1.5f + phase) * amps[j] * 0.8f;
                        }
                    }
                    list->PathLineTo(ImVec2(x0 + (x1 - x0) * t, cy - y));
                }
                list->PathStroke(k < 3 ? colors[k] : Rgb(0xD81B60), 0, k < 3 ? 1.5f : 2.6f);
            }
        }

        void DrawCubeViewer(ImDrawList* list, const ImVec2 min, const ImVec2 max)
        {
            const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f - 4.0f);
            const float r = 74.0f;
            const ImVec2 top[] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r * 0.87f, c.y - r * 0.5f), ImVec2(c.x, c.y), ImVec2(c.x - r * 0.87f, c.y - r * 0.5f)};
            const ImVec2 left[] = {ImVec2(c.x - r * 0.87f, c.y - r * 0.5f), ImVec2(c.x, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r * 0.87f, c.y + r * 0.5f)};
            const ImVec2 right[] = {ImVec2(c.x, c.y), ImVec2(c.x + r * 0.87f, c.y - r * 0.5f), ImVec2(c.x + r * 0.87f, c.y + r * 0.5f), ImVec2(c.x, c.y + r)};
            list->AddConvexPolyFilled(top, 4, Rgb(0xC9CDD2));
            list->AddConvexPolyFilled(left, 4, Rgb(0x9EA3A9));
            list->AddConvexPolyFilled(right, 4, Rgb(0x80858B));
            list->AddPolyline(top, 4, Rgb(0x4A4F55), ImDrawFlags_Closed, 1.5f);
            list->AddPolyline(left, 4, Rgb(0x4A4F55), ImDrawFlags_Closed, 1.5f);
            list->AddPolyline(right, 4, Rgb(0x4A4F55), ImDrawFlags_Closed, 1.5f);
        }

        void DrawTeapotViewer(ImDrawList* list, const ImVec2 min, const ImVec2 max)
        {
            const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
            const ImU32 body = Rgb(0xA4A9AF);
            const ImU32 dark = Rgb(0x5B6066);
            list->AddBezierCubic(ImVec2(c.x + 70.0f, c.y - 22.0f), ImVec2(c.x + 130.0f, c.y - 50.0f), ImVec2(c.x + 128.0f, c.y - 44.0f), ImVec2(c.x + 94.0f, c.y + 12.0f), dark, 9.0f);
            list->AddBezierCubic(ImVec2(c.x - 70.0f, c.y - 18.0f), ImVec2(c.x - 128.0f, c.y - 50.0f), ImVec2(c.x - 132.0f, c.y + 18.0f), ImVec2(c.x - 82.0f, c.y + 18.0f), dark, 9.0f);
            list->AddEllipseFilled(c, ImVec2(78.0f, 54.0f), body, 0.0f, 40);
            list->AddEllipse(c, ImVec2(78.0f, 54.0f), dark, 0.0f, 40, 2.0f);
            list->AddRectFilled(ImVec2(c.x - 40.0f, c.y - 66.0f), ImVec2(c.x + 40.0f, c.y - 44.0f), Rgb(0x8F949A), 8.0f);
            list->AddCircleFilled(ImVec2(c.x, c.y - 72.0f), 7.0f, Rgb(0x8F949A), 14);
            list->AddCircle(ImVec2(c.x, c.y - 72.0f), 7.0f, dark, 14, 2.0f);
            list->AddRect(ImVec2(c.x - 40.0f, c.y - 66.0f), ImVec2(c.x + 40.0f, c.y - 44.0f), dark, 8.0f, 0, 2.0f);
        }
    }

    // ------------------------------------------------------------------------------------------------------------------------

    PrankExplorer::Palette PrankExplorer::PaletteFor(const int level)
    {
        const str::PaletteDef& d = str::kPalettes[std::clamp(level, 0, str::kLevelCount - 1)];
        Palette p;
        p.bg = Rgb(d.bg);
        p.chrome = Rgb(d.chrome);
        p.title = Scaled(Rgb(d.chrome), 0.96f);
        p.side = Rgb(d.side);
        p.text = Rgb(d.text);
        p.dim = Rgb(d.dim);
        p.line = Rgb(d.line);
        p.field = Rgb(d.field);
        p.sel = Rgb(d.sel);
        p.selb = Rgb(d.selb);
        p.hover = Rgb(d.hover);
        return p;
    }

    // The colour of the text on the fields (the address bar, the search): the palette text, or a dark one when the field is light and the text is white
    ImU32 PrankExplorer::OnField() const
    {
        auto lum = [](const ImU32 c)
        {
            auto channel = [](const float v) { return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
            return 0.2126f * channel(static_cast<float>((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f) + 0.7152f * channel(static_cast<float>((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f) +
                   0.0722f * channel(static_cast<float>((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f);
        };
        const float field = lum(palette_.field);
        const float text = lum(palette_.text);
        const float contrast = (std::max(field, text) + 0.05f) / (std::min(field, text) + 0.05f);
        return contrast >= 4.5f ? palette_.text : Rgb(0x2B0010);
    }

    void PrankExplorer::Reset()
    {
        level_ = 0;
        furthest_ = 0;
        selected_ = -1;
        viewer_ = -1;
        openMenu_ = -1;
        addressFill_ = -1.0f;
        working_ = false;
        toastAge_ = 100.0f;
        search_[0] = '\0';
        for (bool& flag : passed_)
        {
            flag = false;
        }
        paletteReady_ = false;
        enterAge_ = 10.0f;
    }

    void PrankExplorer::GoTo(const int level)
    {
        level_ = std::clamp(level, 0, str::kLevelCount - 1);
        furthest_ = std::max(furthest_, level_);
        selected_ = -1;
        viewer_ = -1;
        openMenu_ = -1;
        enterAge_ = 0.0f;
        enterAmplitude_ = str::kLevels[level_].shake;
        bumpAge_ = 10.0f;
        nextBump_ = 3.0f;
    }

    void PrankExplorer::GoUp()
    {
        if (level_ > 0)
        {
            GoTo(level_ - 1);
        }
    }

    void PrankExplorer::GoForward()
    {
        if (level_ < furthest_)
        {
            GoTo(level_ + 1);
        }
    }

    void PrankExplorer::Toast(const std::string& text)
    {
        toast_ = text;
        toastAge_ = 0.0f;
    }

    void PrankExplorer::Shake(const float seconds, const float amplitude)
    {
        shakeLeft_ = seconds;
        shakeAmplitude_ = amplitude;
    }

    void PrankExplorer::OpenItem(const int index)
    {
        if (index >= 0 && index < str::kLevels[level_].itemCount)
        {
            viewer_ = index;
            viewerAge_ = 0.0f;
        }
    }

    std::string PrankExplorer::StatusText() const
    {
        const int count = str::kLevels[level_].itemCount;
        std::string text = count == 1 ? str::kStatusOneItem : count == 2 ? str::kStatusItems2 : str::kStatusItems3;
        if (selected_ >= 0)
        {
            text += "     ";
            text += str::kStatusSelected;
        }
        return text;
    }

    void PrankExplorer::UpdateMotion(const double time, const float dt)
    {
        const Palette target = PaletteFor(level_);
        if (!paletteReady_)
        {
            palette_ = target;
            paletteReady_ = true;
        }
        else
        {
            const float k = std::min(1.0f, dt / 0.25f);
            palette_.bg = Lerp(palette_.bg, target.bg, k);
            palette_.chrome = Lerp(palette_.chrome, target.chrome, k);
            palette_.title = Lerp(palette_.title, target.title, k);
            palette_.side = Lerp(palette_.side, target.side, k);
            palette_.text = Lerp(palette_.text, target.text, k);
            palette_.dim = Lerp(palette_.dim, target.dim, k);
            palette_.line = Lerp(palette_.line, target.line, k);
            palette_.field = Lerp(palette_.field, target.field, k);
            palette_.sel = Lerp(palette_.sel, target.sel, k);
            palette_.selb = Lerp(palette_.selb, target.selb, k);
            palette_.hover = Lerp(palette_.hover, target.hover, k);
        }
        enterAge_ += dt;
        bumpAge_ += dt;
        toastAge_ += dt;
        searchMessageAge_ += dt;
        viewerAge_ += dt;
        squeezeAge_ += dt;
        shakeLeft_ = std::max(0.0f, shakeLeft_ - dt);
        // A short bump every 4 +- 1 s while the level shakes at all
        if (str::kLevels[level_].shake > 0.0f && bumpAge_ > nextBump_)
        {
            bumpAge_ = 0.0f;
            random_ = random_ * 1664525u + 1013904223u;
            nextBump_ = 3.0f + static_cast<float>((random_ >> 16) & 0xFF) / 255.0f * 2.0f;
        }
        (void)time;
    }

    ImVec2 PrankExplorer::ShakeOffset(const double time) const
    {
        (void)time;
        float ax = 0.0f;
        float ay = 0.0f;
        const float level = str::kLevels[level_].shake;
        if (enterAge_ < 0.6f && enterAmplitude_ > 0.0f)
        {
            const float fade = 1.0f - enterAge_ / 0.6f;
            ax = enterAmplitude_ * 3.0f * fade;
            ay = enterAmplitude_ * 1.5f * fade;
        }
        else if (bumpAge_ < 0.15f && level > 0.0f)
        {
            ax = level;
            ay = level * 0.5f;
        }
        if (shakeLeft_ > 0.0f)
        {
            ax = std::max(ax, shakeAmplitude_ * 3.0f * std::min(1.0f, shakeLeft_ / 0.3f));
            ay = std::max(ay, shakeAmplitude_ * 1.5f * std::min(1.0f, shakeLeft_ / 0.3f));
        }
        if (ax == 0.0f && ay == 0.0f)
        {
            return ImVec2(0.0f, 0.0f);
        }
        // A new random offset every frame
        std::uint32_t r = random_ + static_cast<std::uint32_t>(ImGui::GetFrameCount()) * 2654435761u;
        r ^= r >> 13;
        r *= 0x5bd1e995u;
        r ^= r >> 15;
        const float rx = static_cast<float>(r & 0xFFFF) / 32767.5f - 1.0f;
        const float ry = static_cast<float>((r >> 16) & 0xFFFF) / 32767.5f - 1.0f;
        return ImVec2(rx * ax, ry * ay);
    }

    // ------------------------------------------------------------------------------------------------------------------------

    void PrankExplorer::Draw(ImDrawList* list, const ImVec2 origin, const ImVec2 size, const double time, const float dt, const bool interactive, ExplorerRequest& request)
    {
        origin_ = origin;
        size_ = size;
        time_ = time;
        UpdateMotion(time, dt);
        offset_ = ShakeOffset(time);
        searchActive_ = false;

        list->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
        list->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), palette_.chrome);
        const int firstVertex = list->VtxBuffer.Size;

        ImVec2 p(origin.x + offset_.x, origin.y + offset_.y);
        DrawTitleBar(list, p, size.x, interactive);
        p.y += kTitleH;
        DrawNavBar(list, p, size.x, interactive);
        p.y += kNavH;
        DrawCommandBar(list, p, size.x, interactive);
        p.y += kCmdH;
        const float bodyHeight = std::max(60.0f, size.y - kTitleH - kNavH - kCmdH - kStatusH);
        DrawSidebar(list, p, bodyHeight, interactive);
        DrawContent(list, ImVec2(p.x + kSideW, p.y), ImVec2(size.x - kSideW, bodyHeight), time, interactive, request);
        DrawStatusBar(list, ImVec2(origin.x + offset_.x, p.y + bodyHeight), size.x);

        // "Minimize": the content shrinks to 90 % for 120 ms and comes back
        float scale = 1.0f;
        if (squeezeAge_ < 0.24f)
        {
            scale = 1.0f - 0.1f * (squeezeAge_ < 0.12f ? squeezeAge_ / 0.12f : (0.24f - squeezeAge_) / 0.12f);
        }
        const float tilt = std::clamp(str::kLevels[level_].tiltDegrees * std::min(1.0f, enterAge_ / 0.4f), -2.0f, 2.0f);
        ScaleRotate(list, firstVertex, ImVec2(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f), scale, tilt);

        DrawMenus(list, interactive);
        DrawViewer(list, origin, ImVec2(origin.x + size.x, origin.y + size.y), time);
        DrawToast(list, origin, ImVec2(origin.x + size.x, origin.y + size.y));
        list->PopClipRect();

        HandleKeys(interactive, request);
    }

    void PrankExplorer::DrawTitleBar(ImDrawList* list, const ImVec2 min, const float width, const bool interactive)
    {
        list->AddRectFilled(min, ImVec2(min.x + width, min.y + kTitleH), palette_.title);
        DrawFolder(list, ImVec2(min.x + 10.0f, min.y + 9.0f), 0.22f, 1.0f);
        const std::string title = std::string(str::kLevels[level_].folderName) + str::kWindowTitleSuffix;
        Text(list, ImVec2(min.x + 34.0f, min.y + 8.0f), 13.0f, palette_.text, title.c_str());

        const float bx = min.x + width - 3.0f * 46.0f;
        for (int i = 0; i < 3; ++i)
        {
            const ImVec2 a(bx + static_cast<float>(i) * 46.0f, min.y);
            const ImVec2 b(a.x + 46.0f, a.y + kTitleH);
            bool hovered = false;
            char id[16];
            std::snprintf(id, sizeof(id), "##pwin%d", i);
            const bool clicked = interactive && Hit(id, a, b, &hovered);
            if (hovered)
            {
                list->AddRectFilled(a, b, i == 2 ? Rgb(0xC42B1C) : palette_.hover);
            }
            const ImU32 color = hovered && i == 2 ? kWhite : palette_.text;
            const ImVec2 c(a.x + 23.0f, a.y + 16.0f);
            if (i == 0)
            {
                list->AddLine(ImVec2(c.x - 5.0f, c.y), ImVec2(c.x + 5.0f, c.y), color, 1.2f);
            }
            else if (i == 1)
            {
                list->AddRect(ImVec2(c.x - 5.0f, c.y - 5.0f), ImVec2(c.x + 5.0f, c.y + 5.0f), color, 1.0f, 0, 1.2f);
            }
            else
            {
                DrawCrossIcon(list, c, 5.0f, color, 1.2f);
            }
            if (clicked)
            {
                if (i == 0)
                {
                    squeezeAge_ = 0.0f;
                    Toast(str::kToastMinimize);
                }
                else if (i == 2)
                {
                    Shake(0.3f, 2.0f);
                    Toast(str::kToastClose);
                }
            }
        }
    }

    void PrankExplorer::DrawNavBar(ImDrawList* list, const ImVec2 min, const float width, const bool interactive)
    {
        list->AddRectFilled(min, ImVec2(min.x + width, min.y + kNavH), palette_.chrome);
        // ← → ↑ ⟳
        const bool enabled[4] = {level_ > 0, level_ < furthest_, level_ > 0, true};
        for (int i = 0; i < 4; ++i)
        {
            const ImVec2 a(min.x + 8.0f + static_cast<float>(i) * 36.0f, min.y + 6.0f);
            const ImVec2 b(a.x + 32.0f, a.y + 32.0f);
            bool hovered = false;
            char id[16];
            std::snprintf(id, sizeof(id), "##pnav%d", i);
            const bool clicked = interactive && enabled[i] && Hit(id, a, b, &hovered);
            if (hovered)
            {
                list->AddRectFilled(a, b, palette_.hover, 4.0f);
            }
            const ImU32 color = enabled[i] ? palette_.text : WithAlpha(palette_.dim, 0.5f);
            const ImVec2 c(a.x + 16.0f, a.y + 16.0f);
            if (i == 3)
            {
                DrawRefreshIcon(list, c, 6.0f, color);
            }
            else
            {
                DrawArrowIcon(list, c, i == 0 ? 0 : i == 1 ? 1 : 2, color);
            }
            if (clicked)
            {
                if (i == 0 || i == 2)
                {
                    GoUp();
                }
                else if (i == 1)
                {
                    GoForward();
                }
                else
                {
                    Shake(0.3f, 1.0f);
                    Toast(str::kToastRefresh);
                }
            }
        }

        // The address bar
        const float addressX = min.x + 8.0f + 4.0f * 36.0f + 4.0f;
        const float searchX = min.x + width - kSearchW - 12.0f;
        const ImVec2 aMin(addressX, min.y + 7.0f);
        const ImVec2 aMax(searchX - 10.0f, aMin.y + kAddressH);
        list->AddRectFilled(aMin, aMax, palette_.field, 4.0f);
        if (addressFill_ >= 0.0f)
        {
            const float fillRight = aMin.x + (aMax.x - aMin.x) * std::min(1.0f, addressFill_);
            list->PushClipRect(aMin, ImVec2(fillRight, aMax.y), true);
            list->AddRectFilledMultiColor(ImVec2(aMin.x, aMin.y), ImVec2(aMax.x, aMax.y), Rgb(0xA6E3A1), Rgb(0x6DD36A), Rgb(0x6DD36A), Rgb(0xA6E3A1));
            list->PopClipRect();
        }
        list->AddRect(aMin, aMax, palette_.line, 4.0f, 0, 1.0f);

        // Breadcrumbs: this computer > level 0 > ... > current. The first ones become "..." when there is no room
        std::vector<std::string> names;
        names.emplace_back(str::kBreadcrumbRoot);
        for (int i = 0; i <= level_; ++i)
        {
            names.emplace_back(str::kLevels[i].folderName);
        }
        const float room = aMax.x - aMin.x - 24.0f;
        const float separator = 22.0f;
        std::size_t first = 0;
        auto total = [&]()
        {
            float sum = first > 0 ? TextWidth(13.0f, "…") + separator : 0.0f;
            for (std::size_t i = first; i < names.size(); ++i)
            {
                sum += TextWidth(13.0f, names[i].c_str()) + 14.0f + (i + 1 < names.size() ? separator : 0.0f);
            }
            return sum;
        };
        while (first + 3 < names.size() && total() > room)
        {
            ++first;
        }
        float x = aMin.x + 10.0f;
        const float textY = aMin.y + (kAddressH - TextHeight(13.0f)) * 0.5f;
        list->PushClipRect(aMin, aMax, true);
        if (first > 0)
        {
            Text(list, ImVec2(x, textY), 13.0f, OnField(), "…");
            x += TextWidth(13.0f, "…") + separator;
            Text(list, ImVec2(x - separator + 6.0f, textY), 11.0f, WithAlpha(OnField(), 0.6f), "›");
        }
        for (std::size_t i = first; i < names.size(); ++i)
        {
            const float w = TextWidth(13.0f, names[i].c_str()) + 14.0f;
            const ImVec2 cMin(x - 2.0f, aMin.y + 3.0f);
            const ImVec2 cMax(x + w - 4.0f, aMax.y - 3.0f);
            bool hovered = false;
            char id[24];
            std::snprintf(id, sizeof(id), "##pcrumb%d", static_cast<int>(i));
            const bool clicked = interactive && Hit(id, cMin, cMax, &hovered);
            if (hovered)
            {
                list->AddRectFilled(cMin, cMax, palette_.hover, 3.0f);
            }
            Text(list, ImVec2(x + 5.0f, textY), 13.0f, OnField(), names[i].c_str());
            x += w;
            if (i + 1 < names.size())
            {
                Text(list, ImVec2(x - 6.0f, textY + 1.0f), 11.0f, WithAlpha(OnField(), 0.6f), "›");
                x += separator - 8.0f;
            }
            if (clicked)
            {
                if (i == 0)
                {
                    Toast(str::kToastElsewhere);
                }
                else
                {
                    GoTo(static_cast<int>(i) - 1);
                }
            }
        }
        list->PopClipRect();

        // The search
        const ImVec2 sMin(searchX, min.y + 7.0f);
        const ImVec2 sMax(searchX + kSearchW, sMin.y + kAddressH);
        list->AddRectFilled(sMin, sMax, palette_.field, 4.0f);
        list->AddRect(sMin, sMax, palette_.line, 4.0f, 0, 1.0f);
        if (interactive)
        {
            ImGui::SetCursorScreenPos(ImVec2(sMin.x + 6.0f, sMin.y + 3.0f));
            ImGui::PushItemWidth(kSearchW - 12.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(OnField()));
            ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::ColorConvertU32ToFloat4(WithAlpha(OnField(), 0.6f)));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 3.0f));
            if (ImGui::InputTextWithHint("##psearch", str::kSearchHint, search_, sizeof(search_), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                searchMessageAge_ = 0.0f;
                search_[0] = '\0';
            }
            searchActive_ = ImGui::IsItemActive();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            ImGui::PopItemWidth();
        }
        if (searchMessageAge_ < 2.5f)
        {
            const float w = TextWidth(12.0f, str::kSearchNothing) + 20.0f;
            const ImVec2 mMin(sMax.x - w, sMax.y + 4.0f);
            list->AddRectFilled(mMin, ImVec2(sMax.x, mMin.y + 26.0f), Rgb(0x1B1B1B, 235), 4.0f);
            Text(list, ImVec2(mMin.x + 10.0f, mMin.y + 5.0f), 12.0f, kWhite, str::kSearchNothing);
        }
    }

    void PrankExplorer::DrawCommandBar(ImDrawList* list, const ImVec2 min, const float width, const bool interactive)
    {
        list->AddRectFilled(min, ImVec2(min.x + width, min.y + kCmdH), palette_.chrome);
        list->AddLine(ImVec2(min.x, min.y + kCmdH - 0.5f), ImVec2(min.x + width, min.y + kCmdH - 0.5f), palette_.line, 1.0f);
        float x = min.x + 16.0f;
        const float cy = min.y + kCmdH * 0.5f;
        struct Entry
        {
            const char* label;
            int menu;   // -1: nothing
            bool pale;
        };
        const Entry entries[] = {{str::kCommandCreate, 0, false}, {"", -1, true}, {str::kCommandSort, 1, false}, {str::kCommandView, 2, false}, {"", 3, false}};
        for (int i = 0; i < 5; ++i)
        {
            const Entry& entry = entries[i];
            float w = 0.0f;
            if (i == 1)
            {
                // three pale icons (cut, copy, paste): squares
                for (int k = 0; k < 3; ++k)
                {
                    const float ix = x + static_cast<float>(k) * 26.0f;
                    list->AddRect(ImVec2(ix, cy - 6.0f), ImVec2(ix + 12.0f, cy + 6.0f), WithAlpha(palette_.dim, 0.45f), 2.0f, 0, 1.4f);
                }
                x += 3.0f * 26.0f + 10.0f;
                continue;
            }
            if (i == 4)
            {
                w = 28.0f;
            }
            else
            {
                w = (i == 0 ? 20.0f : 22.0f) + TextWidth(13.0f, entry.label) + 20.0f;
            }
            const ImVec2 a(x - 8.0f, min.y + 5.0f);
            const ImVec2 b(x - 8.0f + w + 8.0f, min.y + kCmdH - 5.0f);
            bool hovered = false;
            char id[16];
            std::snprintf(id, sizeof(id), "##pcmd%d", i);
            const bool clicked = interactive && Hit(id, a, b, &hovered);
            if (hovered || openMenu_ == entry.menu)
            {
                list->AddRectFilled(a, b, palette_.hover, 4.0f);
            }
            float tx = x;
            if (i == 0)
            {
                list->AddLine(ImVec2(x + 2.0f, cy), ImVec2(x + 12.0f, cy), Rgb(0x0A7BDB), 2.0f);
                list->AddLine(ImVec2(x + 7.0f, cy - 5.0f), ImVec2(x + 7.0f, cy + 5.0f), Rgb(0x0A7BDB), 2.0f);
                tx = x + 20.0f;
            }
            else if (i == 2)
            {
                // sort: two arrows
                list->AddLine(ImVec2(x + 4.0f, cy - 5.0f), ImVec2(x + 4.0f, cy + 5.0f), palette_.text, 1.4f);
                list->AddLine(ImVec2(x + 11.0f, cy - 5.0f), ImVec2(x + 11.0f, cy + 5.0f), palette_.text, 1.4f);
                tx = x + 22.0f;
            }
            else if (i == 3)
            {
                for (int k = 0; k < 3; ++k)
                {
                    list->AddLine(ImVec2(x + 2.0f, cy - 4.0f + static_cast<float>(k) * 4.0f), ImVec2(x + 13.0f, cy - 4.0f + static_cast<float>(k) * 4.0f), palette_.text, 1.4f);
                }
                tx = x + 22.0f;
            }
            if (i == 4)
            {
                for (int k = 0; k < 3; ++k)
                {
                    list->AddCircleFilled(ImVec2(x + 2.0f + static_cast<float>(k) * 6.0f, cy), 1.6f, palette_.text, 8);
                }
            }
            else
            {
                Text(list, ImVec2(tx, cy - TextHeight(13.0f) * 0.5f), 13.0f, palette_.text, entry.label);
                DrawChevronDown(list, ImVec2(tx + TextWidth(13.0f, entry.label) + 10.0f, cy), 3.5f, palette_.dim);
            }
            if (clicked)
            {
                openMenu_ = openMenu_ == entry.menu ? -1 : entry.menu;
                menuAnchor_ = ImVec2(a.x, b.y + 2.0f);
            }
            x += w + 18.0f;
        }
    }

    void PrankExplorer::DrawMenus(ImDrawList* list, const bool interactive)
    {
        if (openMenu_ < 0)
        {
            return;
        }
        static const char* const labels[4] = {str::kMenuCreate, str::kMenuSort, str::kMenuView, str::kMenuMore};
        const char* label = labels[std::clamp(openMenu_, 0, 3)];
        const float w = TextWidth(13.0f, label) + 36.0f;
        const ImVec2 a(menuAnchor_.x + offset_.x, menuAnchor_.y + offset_.y);
        const ImVec2 b(a.x + w, a.y + 34.0f);
        list->AddRectFilled(ImVec2(a.x + 2.0f, a.y + 3.0f), ImVec2(b.x + 2.0f, b.y + 4.0f), Rgb(0x000000, 40), 6.0f);
        list->AddRectFilled(a, b, palette_.field, 6.0f);
        list->AddRect(a, b, palette_.line, 6.0f);
        Text(list, ImVec2(a.x + 18.0f, a.y + 8.0f), 13.0f, WithAlpha(palette_.dim, 0.9f), label);
        if (interactive && ImGui::IsMouseClicked(0) && !ImGui::IsMouseHoveringRect(a, b) && !ImGui::IsAnyItemHovered())
        {
            openMenu_ = -1;
        }
        else if (interactive && ImGui::IsMouseClicked(0) && ImGui::IsMouseHoveringRect(a, b))
        {
            openMenu_ = -1;
        }
    }

    void PrankExplorer::DrawSidebar(ImDrawList* list, const ImVec2 min, const float height, const bool interactive)
    {
        list->AddRectFilled(min, ImVec2(min.x + kSideW, min.y + height), palette_.side);
        list->AddLine(ImVec2(min.x + kSideW - 0.5f, min.y), ImVec2(min.x + kSideW - 0.5f, min.y + height), palette_.line, 1.0f);
        float y = min.y + 8.0f;
        struct Row
        {
            const char* text;
            int kind;   // 0 normal, 1 section header, 2 the selected one, 3 the disk
        };
        const Row rows[] = {{str::kNavQuick, 0},   {str::kNavDesktop, 0}, {str::kNavDownloads, 0}, {str::kNavDocuments, 0}, {str::kNavLab4, 0},
                            {str::kNavCourse, 0},  {str::kNavSection, 1}, {str::kNavHere, 2},      {str::kNavDisk, 3}};
        int index = 0;
        for (const Row& row : rows)
        {
            if (row.kind == 1)
            {
                y += 10.0f;
                Text(list, ImVec2(min.x + 14.0f, y + 6.0f), 12.0f, palette_.dim, row.text);
                y += 28.0f;
                ++index;
                continue;
            }
            const ImVec2 a(min.x + 6.0f, y);
            const ImVec2 b(min.x + kSideW - 8.0f, y + 28.0f);
            bool hovered = false;
            char id[16];
            std::snprintf(id, sizeof(id), "##pside%d", index);
            const bool clicked = interactive && Hit(id, a, b, &hovered);
            if (row.kind == 2)
            {
                list->AddRectFilled(a, b, palette_.sel, 4.0f);
                list->AddRectFilled(ImVec2(a.x, a.y + 6.0f), ImVec2(a.x + 3.0f, b.y - 6.0f), palette_.selb, 2.0f);
            }
            else if (hovered)
            {
                list->AddRectFilled(a, b, palette_.hover, 4.0f);
            }
            if (row.kind == 3)
            {
                list->AddRectFilled(ImVec2(a.x + 12.0f, y + 8.0f), ImVec2(a.x + 34.0f, y + 20.0f), Rgb(0x7A8088), 2.0f);
                list->AddRectFilled(ImVec2(a.x + 12.0f, y + 16.0f), ImVec2(a.x + 34.0f, y + 20.0f), Rgb(0xD33F49), 2.0f);
            }
            else
            {
                DrawFolder(list, ImVec2(a.x + 10.0f, y + 4.0f), 0.33f, 1.0f);
            }
            Text(list, ImVec2(a.x + 42.0f, y + 5.0f), 13.0f, palette_.text, row.text);
            if (clicked)
            {
                if (row.kind == 2)
                {
                    GoTo(0);
                }
                else
                {
                    Toast(row.kind == 3 ? str::kToastDisk : str::kToastElsewhere);
                }
            }
            y += 30.0f;
            ++index;
        }
    }

    void PrankExplorer::DrawBanner(ImDrawList* list, const ImVec2 min, const float width, const double time, float& heightOut)
    {
        const str::LevelDef& def = str::kLevels[level_];
        heightOut = 0.0f;
        if (def.banner == str::BannerKind::None)
        {
            return;
        }
        const float em = def.bannerSize;
        const float h = em + 16.0f;
        heightOut = h;
        ImU32 fill = kWhite;
        ImU32 textColor = kWhite;
        ImU32 border = 0;
        bool glow = false;
        switch (def.banner)
        {
            case str::BannerKind::Info: fill = Rgb(0xFFF4CE); textColor = Rgb(0x5C4400); border = Rgb(0xF0D78A); break;
            case str::BannerKind::Warning: fill = Rgb(0xFFD8A8); textColor = Rgb(0x6B2B00); border = Rgb(0xFFB066); break;
            case str::BannerKind::Red: fill = Rgb(0xFF1F3D); textColor = kWhite; glow = true; break;
            case str::BannerKind::White: fill = kWhite; textColor = Rgb(0xD10F37); glow = true; break;
            case str::BannerKind::None: break;
        }
        const ImVec2 a(min.x, min.y);
        const ImVec2 b(min.x + width, min.y + h);
        if (glow)
        {
            const float pulse = 0.75f + 0.25f * Pulse(time, 0.9f);
            Glow(list, a, b, WithAlpha(Rgb(0xFF1F3D), 0.6f * pulse), 3.0f * static_cast<float>(level_ + 1), 4, 4.0f);
        }
        list->AddRectFilled(a, b, fill, 4.0f);
        if (border != 0)
        {
            list->AddRect(a, b, border, 4.0f);
        }
        const float iconSize = std::min(h - 10.0f, em + 2.0f);
        DrawBannerIcon(list, ImVec2(a.x + 14.0f + iconSize * 0.5f, a.y + h * 0.5f), iconSize, def.banner, def.banner == str::BannerKind::Info ? Rgb(0x8A6A00) : def.banner == str::BannerKind::White ? Rgb(0xD10F37) : def.banner == str::BannerKind::Red ? kWhite : Rgb(0xC85A00),
                       def.banner == str::BannerKind::Red ? Rgb(0xFF1F3D) : kWhite);
        TextFit(list, ImVec2(a.x + 14.0f + iconSize + 10.0f, a.y + (h - TextHeight(em)) * 0.5f), width - iconSize - 40.0f, em, textColor, def.bannerText);
    }

    void PrankExplorer::DrawContent(ImDrawList* list, const ImVec2 min, const ImVec2 size, const double time, const bool interactive, ExplorerRequest& request)
    {
        const str::LevelDef& def = str::kLevels[level_];
        list->AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y), palette_.bg);
        list->PushClipRect(min, ImVec2(min.x + size.x, min.y + size.y), true);

        float bannerHeight = 0.0f;
        DrawBanner(list, ImVec2(min.x + 16.0f, min.y + 12.0f), size.x - 32.0f, time, bannerHeight);
        float gridTop = min.y + (bannerHeight > 0.0f ? 12.0f + bannerHeight + 14.0f : kGridPad);

        if (working_)
        {
            TextCentered(list, ImVec2(min.x + size.x * 0.5f, gridTop + 60.0f), 14.0f, palette_.dim, str::kWorking);
            list->PopClipRect();
            return;
        }

        const int columns = std::max(1, static_cast<int>((size.x - 2.0f * kGridPad + kTileGap) / (kTileW + kTileGap)));
        tileMin_.clear();
        for (int i = 0; i < def.itemCount; ++i)
        {
            const str::ItemDef& item = def.items[i];
            const int col = i % columns;
            const int row = i / columns;
            const ImVec2 a(min.x + kGridPad + static_cast<float>(col) * (kTileW + kTileGap), gridTop + static_cast<float>(row) * (kTileH + kTileGap));
            const ImVec2 b(a.x + kTileW, a.y + kTileH);
            tileMin_.push_back(a);
            bool hovered = false;
            char id[16];
            std::snprintf(id, sizeof(id), "##ptile%d", i);
            const bool clicked = interactive && viewer_ < 0 && Hit(id, a, b, &hovered);
            const bool doubled = interactive && viewer_ < 0 && hovered && ImGui::IsMouseDoubleClicked(0);
            if (clicked)
            {
                selected_ = i;
            }
            if (selected_ == i)
            {
                list->AddRectFilled(a, b, palette_.sel, 4.0f);
                list->AddRect(a, b, palette_.selb, 4.0f, 0, 1.0f);
            }
            else if (hovered)
            {
                list->AddRectFilled(a, b, palette_.hover, 4.0f);
            }

            const float breath = level_ >= 6 ? 1.0f + 0.04f * Pulse(time, 1.6f) : 1.0f;
            const float iconW = item.folder ? 72.0f : 52.0f;
            const float iconH = item.folder ? 58.0f : 64.0f;
            const ImVec2 center(a.x + kTileW * 0.5f, a.y + 8.0f + 32.0f);
            const ImVec2 iconPos(center.x - iconW * 0.5f * breath, center.y - iconH * 0.5f * breath);
            const bool glowing = item.viewer == str::Viewer::Generator;
            if (glowing)
            {
                const float pulse = Pulse(time, 1.2f);
                Glow(list, ImVec2(center.x - 30.0f, center.y - 36.0f), ImVec2(center.x + 30.0f, center.y + 36.0f), WithAlpha(kHot, 0.35f + 0.5f * pulse), 18.0f, 5, 12.0f);
            }
            if (item.folder)
            {
                DrawFolder(list, iconPos, breath, 1.0f);
            }
            else
            {
                DrawFile(list, iconPos, breath, item.kind, 1.0f);
            }
            TextWrapped(list, ImVec2(a.x + 4.0f, a.y + 8.0f + 64.0f + 6.0f), kTileW - 8.0f, 13.0f, palette_.text, item.name, true, 17.0f, 2);

            if (doubled)
            {
                Activate(i, request);
            }
        }
        list->PopClipRect();
    }

    void PrankExplorer::Activate(const int index, ExplorerRequest& request)
    {
        const str::LevelDef& def = str::kLevels[level_];
        if (index < 0 || index >= def.itemCount)
        {
            return;
        }
        const str::ItemDef& item = def.items[index];
        selected_ = index;
        if (item.folder)
        {
            const int target = level_ + 1;
            // The gag between the levels, once per run
            GagKind gag = GagKind::None;
            switch (level_)
            {
                case 3: gag = GagKind::Captcha; break;
                case 4: gag = GagKind::Paywall; break;
                case 5: gag = GagKind::Ivan; break;
                case 6: gag = GagKind::Age; break;
                case 7: gag = GagKind::Load99; break;
                default: break;
            }
            if (gag != GagKind::None && !Passed(gag))
            {
                request.gag = gag;
                request.targetLevel = target;
                return;
            }
            if (gag != GagKind::None)
            {
                Toast(str::kToastAlreadyProved);
            }
            GoTo(target);
            return;
        }
        if (item.viewer == str::Viewer::Generator)
        {
            if (!Passed(GagKind::Uac))
            {
                request.gag = GagKind::Uac;
                request.targetLevel = -1;
            }
            else
            {
                request.gag = GagKind::Uac;
                request.targetLevel = -2; // the generator at once
            }
            return;
        }
        if (item.viewer == str::Viewer::Video)
        {
            request.gag = GagKind::Load99;
            request.video = true;
            return;
        }
        OpenItem(index);
    }

    void PrankExplorer::DrawStatusBar(ImDrawList* list, const ImVec2 min, const float width)
    {
        list->AddRectFilled(min, ImVec2(min.x + width, min.y + kStatusH), palette_.chrome);
        list->AddLine(ImVec2(min.x, min.y + 0.5f), ImVec2(min.x + width, min.y + 0.5f), palette_.line, 1.0f);
        Text(list, ImVec2(min.x + 14.0f, min.y + 5.0f), 12.0f, palette_.dim, StatusText().c_str());
        const std::string free = std::string(str::kStatusFree) + ": " + str::kLevels[level_].freeSpace;
        TextRight(list, min.x + width - 14.0f, min.y + 5.0f, 12.0f, palette_.dim, free.c_str());
    }

    void PrankExplorer::DrawToast(ImDrawList* list, const ImVec2 areaMin, const ImVec2 areaMax)
    {
        constexpr float kLife = 2.0f;
        constexpr float kFade = 0.2f;
        if (toastAge_ >= kLife + kFade || toast_.empty())
        {
            return;
        }
        const float alpha = toastAge_ < kLife ? 1.0f : 1.0f - (toastAge_ - kLife) / kFade;
        const float w = TextWidth(13.0f, toast_.c_str()) + 32.0f;
        const float cx = (areaMin.x + areaMax.x) * 0.5f;
        const ImVec2 a(cx - w * 0.5f, areaMax.y - kStatusH - 60.0f);
        const ImVec2 b(cx + w * 0.5f, a.y + 38.0f);
        list->AddRectFilled(a, b, Rgb(0x1B1B1B, static_cast<unsigned>(235.0f * alpha)), 6.0f);
        TextCentered(list, ImVec2(cx, (a.y + b.y) * 0.5f), 13.0f, WithAlpha(kWhite, alpha), toast_.c_str());
    }

    void PrankExplorer::DrawViewer(ImDrawList* list, const ImVec2 areaMin, const ImVec2 areaMax, const double time)
    {
        if (viewer_ < 0)
        {
            return;
        }
        const str::LevelDef& def = str::kLevels[level_];
        if (viewer_ >= def.itemCount)
        {
            viewer_ = -1;
            return;
        }
        const str::ItemDef& item = def.items[viewer_];
        const bool notepad = item.viewer == str::Viewer::Notepad;
        const float alpha = std::min(1.0f, viewerAge_ / 0.16f);
        const float scale = 0.96f + 0.04f * alpha;
        const float w = 440.0f;
        const float h = notepad ? 230.0f : 330.0f;
        const ImVec2 center((areaMin.x + areaMax.x) * 0.5f, (areaMin.y + areaMax.y) * 0.5f);
        const ImVec2 a(center.x - w * 0.5f, center.y - h * 0.5f);
        const ImVec2 b(a.x + w, a.y + h);

        // The shade and the click outside
        list->AddRectFilled(areaMin, areaMax, Rgb(0x000000, static_cast<unsigned>(70.0f * alpha)));
        const int first = list->VtxBuffer.Size;
        list->AddRectFilled(ImVec2(a.x + 3.0f, a.y + 6.0f), ImVec2(b.x + 3.0f, b.y + 8.0f), Rgb(0x000000, 60), 8.0f);
        list->AddRectFilled(a, b, kWhite, 8.0f);
        list->AddRect(a, b, Rgb(0xB9B9B9), 8.0f);
        list->AddRectFilled(a, ImVec2(b.x, a.y + 30.0f), Rgb(0xF3F3F3), 8.0f, ImDrawFlags_RoundCornersTop);
        const std::string title = std::string(item.name) + (notepad ? str::kNotepadSuffix : str::kViewerSuffix);
        Text(list, ImVec2(a.x + 12.0f, a.y + 7.0f), 13.0f, Rgb(0x1B1B1B), title.c_str());
        const ImVec2 closeMin(b.x - 44.0f, a.y);
        const ImVec2 closeMax(b.x, a.y + 30.0f);
        bool closeHovered = false;
        const bool closeClicked = Hit("##pviewclose", closeMin, closeMax, &closeHovered);
        if (closeHovered)
        {
            list->AddRectFilled(closeMin, closeMax, Rgb(0xC42B1C), 8.0f, ImDrawFlags_RoundCornersTopRight);
        }
        DrawCrossIcon(list, ImVec2(closeMin.x + 22.0f, closeMin.y + 15.0f), 5.0f, closeHovered ? kWhite : Rgb(0x1B1B1B), 1.2f);

        if (notepad)
        {
            Text(list, ImVec2(a.x + 12.0f, a.y + 36.0f), 12.0f, Rgb(0x6B6B6B), str::kNotepadMenu);
            list->AddLine(ImVec2(a.x, a.y + 58.0f), ImVec2(b.x, a.y + 58.0f), Rgb(0xE5E5E5));
            PushFontRole(FontRole::Mono);
            ImFont* mono = ImGui::GetFont();
            const float monoSize = ImGui::GetFontSize();
            const std::string& text = Clean(item.text != nullptr ? item.text : "");
            // Words wrapped to the width of the page
            std::string line;
            float y = a.y + 70.0f;
            std::size_t pos = 0;
            const float pageWidth = w - 40.0f;
            ImVec2 caret(a.x + 20.0f, y);
            auto flush = [&]()
            {
                list->AddText(mono, monoSize, ImVec2(a.x + 20.0f, y), Rgb(0x1B1B1B), line.c_str());
                caret = ImVec2(a.x + 20.0f + mono->CalcTextSizeA(monoSize, 100000.0f, 0.0f, line.c_str()).x, y);
                y += monoSize + 4.0f;
                line.clear();
            };
            while (pos <= text.size())
            {
                std::size_t end = text.find(' ', pos);
                if (end == std::string::npos)
                {
                    end = text.size();
                }
                const std::string word = text.substr(pos, end - pos);
                const std::string candidate = line.empty() ? word : line + " " + word;
                if (!line.empty() && mono->CalcTextSizeA(monoSize, 100000.0f, 0.0f, candidate.c_str()).x > pageWidth)
                {
                    flush();
                    line = word;
                }
                else
                {
                    line = candidate;
                }
                if (end >= text.size())
                {
                    break;
                }
                pos = end + 1;
            }
            flush();
            if (std::fmod(time, 1.0) < 0.55)
            {
                list->AddRectFilled(ImVec2(caret.x + 2.0f, caret.y + 1.0f), ImVec2(caret.x + 4.0f, caret.y + monoSize), Rgb(0x1B1B1B));
            }
            PopFontRole();
        }
        else
        {
            const ImVec2 pa(a.x + 20.0f, a.y + 42.0f);
            const ImVec2 pb(b.x - 20.0f, b.y - 62.0f);
            list->AddRectFilled(pa, pb, Rgb(0xF6F7F8), 6.0f);
            list->PushClipRect(pa, pb, true);
            switch (item.viewer)
            {
                case str::Viewer::Disk: DrawDiskViewer(list, pa, pb, time); break;
                case str::Viewer::Sines: DrawSinesViewer(list, pa, pb, time); break;
                case str::Viewer::Cube: DrawCubeViewer(list, pa, pb); break;
                case str::Viewer::Teapot: DrawTeapotViewer(list, pa, pb); break;
                default: break;
            }
            list->PopClipRect();
            TextWrapped(list, ImVec2(a.x + 20.0f, b.y - 50.0f), w - 40.0f, 15.0f, Rgb(0x1B1B1B), item.text != nullptr ? item.text : "", true, 20.0f, 2);
        }
        ScaleRotate(list, first, center, scale, 0.0f);

        // Closing: the cross, Esc, a click outside the window
        if (closeClicked || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || (viewerAge_ > 0.1f && ImGui::IsMouseClicked(0) && !ImGui::IsMouseHoveringRect(a, b)))
        {
            viewer_ = -1;
        }
    }

    void PrankExplorer::HandleKeys(const bool interactive, ExplorerRequest& request)
    {
        if (!interactive || viewer_ >= 0 || searchActive_ || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        {
            return;
        }
        const bool alt = ImGui::GetIO().KeyAlt;
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) || (alt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) ||
            (!alt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) || ImGui::IsMouseClicked(3))
        {
            GoUp();
        }
        else if (ImGui::IsMouseClicked(4))
        {
            GoForward();
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
        {
            Activate(selected_ >= 0 ? selected_ : 0, request);
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
        {
            const int count = str::kLevels[level_].itemCount;
            selected_ = (selected_ + 1) % std::max(1, count);
        }
    }
}
