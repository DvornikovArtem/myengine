#include "PrankUi.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "EditorWidgets.h"

namespace myengine::ui::prank
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;

        // The code points the prank fonts were baked with (UiManager.cpp); anything else is left out
        bool Baked(const std::uint32_t c)
        {
            for (const ImWchar* range = kBakedRanges; range[0] != 0; range += 2)
            {
                if (c >= range[0] && c <= range[1])
                {
                    return true;
                }
            }
            return false;
        }

        std::size_t Decode(const char* text, std::uint32_t& code)
        {
            const auto byte = static_cast<unsigned char>(text[0]);
            if (byte < 0x80)
            {
                code = byte;
                return 1;
            }
            const std::size_t length = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 1;
            code = length == 1 ? byte : (byte & (0xFFu >> (length + 1)));
            for (std::size_t k = 1; k < length; ++k)
            {
                const auto next = static_cast<unsigned char>(text[k]);
                if ((next & 0xC0) != 0x80)
                {
                    return k; // a broken sequence: what was read so far is skipped
                }
                code = (code << 6) | (next & 0x3Fu);
            }
            return length;
        }

        float Clamp01(const float v)
        {
            return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        }
    }

    ImU32 Lerp(const ImU32 a, const ImU32 b, const float t)
    {
        const float k = Clamp01(t);
        auto channel = [&](const int shift)
        {
            const float x = static_cast<float>((a >> shift) & 0xFF);
            const float y = static_cast<float>((b >> shift) & 0xFF);
            return static_cast<ImU32>(x + (y - x) * k + 0.5f);
        };
        return (channel(IM_COL32_R_SHIFT) << IM_COL32_R_SHIFT) | (channel(IM_COL32_G_SHIFT) << IM_COL32_G_SHIFT) | (channel(IM_COL32_B_SHIFT) << IM_COL32_B_SHIFT) |
               (channel(IM_COL32_A_SHIFT) << IM_COL32_A_SHIFT);
    }

    ImU32 Scaled(const ImU32 color, const float factor)
    {
        auto channel = [&](const int shift)
        {
            const float x = static_cast<float>((color >> shift) & 0xFF) * factor;
            return static_cast<ImU32>(std::min(255.0f, std::max(0.0f, x)) + 0.5f);
        };
        return (channel(IM_COL32_R_SHIFT) << IM_COL32_R_SHIFT) | (channel(IM_COL32_G_SHIFT) << IM_COL32_G_SHIFT) | (channel(IM_COL32_B_SHIFT) << IM_COL32_B_SHIFT) |
               (color & IM_COL32_A_MASK);
    }

    float Pulse(const double time, const float period)
    {
        return 0.5f + 0.5f * std::sin(static_cast<float>(std::fmod(time, static_cast<double>(period)) / static_cast<double>(period)) * 2.0f * kPi);
    }

    float Approach(const float value, const float target, const float dt, const float seconds)
    {
        return value + (target - value) * std::min(1.0f, dt / std::max(seconds, 0.0001f));
    }

    ImFont* FontFor(const float em, float& size)
    {
        if (ImFont* font = PrankFontNear(em, size); font != nullptr)
        {
            return font;
        }
        size = ImGui::GetFontSize();
        return ImGui::GetFont();
    }

    const std::string& Clean(const char* text)
    {
        static std::unordered_map<std::string, std::string> cache;
        static const std::string empty;
        if (text == nullptr)
        {
            return empty;
        }
        const auto found = cache.find(text);
        if (found != cache.end())
        {
            return found->second;
        }
        std::string out;
        out.reserve(std::char_traits<char>::length(text));
        for (const char* p = text; *p != '\0';)
        {
            std::uint32_t code = 0;
            const std::size_t length = Decode(p, code);
            if (code == '\n' || Baked(code))
            {
                out.append(p, length);
            }
            p += length;
        }
        // The symbol left out leaves a space behind it: trim the ends and the doubled spaces that come from it
        std::string trimmed;
        trimmed.reserve(out.size());
        for (const char c : out)
        {
            if (c == ' ' && (trimmed.empty() || trimmed.back() == ' '))
            {
                continue;
            }
            trimmed += c;
        }
        while (!trimmed.empty() && trimmed.back() == ' ')
        {
            trimmed.pop_back();
        }
        if (cache.size() > 4096)
        {
            cache.clear();
        }
        return cache.emplace(text, std::move(trimmed)).first->second;
    }

    float TextWidth(const float em, const char* text)
    {
        float size = 0.0f;
        ImFont* font = FontFor(em, size);
        const std::string& clean = Clean(text);
        return font->CalcTextSizeA(size, 100000.0f, 0.0f, clean.c_str()).x;
    }

    float TextHeight(const float em)
    {
        float size = 0.0f;
        FontFor(em, size);
        return size;
    }

    void Text(ImDrawList* list, const ImVec2 position, const float em, const ImU32 color, const char* text)
    {
        float size = 0.0f;
        ImFont* font = FontFor(em, size);
        const std::string& clean = Clean(text);
        if (!clean.empty())
        {
            list->AddText(font, size, ImVec2(std::floor(position.x), std::floor(position.y)), color, clean.c_str());
        }
    }

    void TextCentered(ImDrawList* list, const ImVec2 center, const float em, const ImU32 color, const char* text)
    {
        float size = 0.0f;
        ImFont* font = FontFor(em, size);
        const std::string& clean = Clean(text);
        if (clean.empty())
        {
            return;
        }
        const ImVec2 extent = font->CalcTextSizeA(size, 100000.0f, 0.0f, clean.c_str());
        list->AddText(font, size, ImVec2(std::floor(center.x - extent.x * 0.5f), std::floor(center.y - extent.y * 0.5f)), color, clean.c_str());
    }

    void TextRight(ImDrawList* list, const float right, const float top, const float em, const ImU32 color, const char* text)
    {
        Text(list, ImVec2(right - TextWidth(em, text), top), em, color, text);
    }

    namespace
    {
        // Splits `text` into lines no wider than `width` (words; a word that is too long is cut by characters)
        std::vector<std::string> WrapLines(const float width, const float em, const char* text)
        {
            float size = 0.0f;
            ImFont* font = FontFor(em, size);
            const std::string& clean = Clean(text);
            std::vector<std::string> lines;
            std::string line;
            auto measure = [&](const std::string& s) { return font->CalcTextSizeA(size, 100000.0f, 0.0f, s.c_str()).x; };
            std::size_t i = 0;
            while (i <= clean.size())
            {
                std::size_t end = clean.find_first_of(" \n", i);
                if (end == std::string::npos)
                {
                    end = clean.size();
                }
                std::string word = clean.substr(i, end - i);
                const bool newline = end < clean.size() && clean[end] == '\n';
                const std::string candidate = line.empty() ? word : line + " " + word;
                if (measure(candidate) <= width || line.empty())
                {
                    line = candidate;
                    // a single word wider than the width: cut it by characters
                    while (measure(line) > width && line.size() > 1)
                    {
                        std::size_t cut = line.size();
                        while (cut > 1 && measure(line.substr(0, cut)) > width)
                        {
                            --cut;
                            while (cut > 1 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
                            {
                                --cut;
                            }
                        }
                        lines.push_back(line.substr(0, cut));
                        line = line.substr(cut);
                    }
                }
                else
                {
                    lines.push_back(line);
                    line = word;
                }
                if (newline)
                {
                    lines.push_back(line);
                    line.clear();
                }
                if (end >= clean.size())
                {
                    break;
                }
                i = end + 1;
            }
            if (!line.empty() || lines.empty())
            {
                lines.push_back(line);
            }
            return lines;
        }
    }

    float TextWrapped(ImDrawList* list, const ImVec2 position, const float width, const float em, const ImU32 color, const char* text, const bool center, const float lineHeight,
                      const int maxLines)
    {
        float size = 0.0f;
        ImFont* font = FontFor(em, size);
        std::vector<std::string> lines = WrapLines(width, em, text);
        if (maxLines > 0 && static_cast<int>(lines.size()) > maxLines)
        {
            lines.resize(static_cast<std::size_t>(maxLines));
        }
        float y = position.y;
        for (const std::string& line : lines)
        {
            if (!line.empty())
            {
                float x = position.x;
                if (center)
                {
                    x += (width - font->CalcTextSizeA(size, 100000.0f, 0.0f, line.c_str()).x) * 0.5f;
                }
                list->AddText(font, size, ImVec2(std::floor(x), std::floor(y)), color, line.c_str());
            }
            y += lineHeight;
        }
        return y - position.y;
    }

    float WrappedHeight(const float width, const float em, const char* text, const float lineHeight)
    {
        return static_cast<float>(WrapLines(width, em, text).size()) * lineHeight;
    }

    void TextFit(ImDrawList* list, const ImVec2 position, const float width, const float em, const ImU32 color, const char* text)
    {
        const float natural = TextWidth(em, text);
        if (natural <= width || natural <= 0.0f)
        {
            Text(list, position, em, color, text);
            return;
        }
        Text(list, position, em * width / natural, color, text);
    }

    void Glow(ImDrawList* list, const ImVec2 min, const ImVec2 max, const ImU32 color, const float radius, const int layers, const float rounding)
    {
        for (int i = layers; i >= 1; --i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(layers);
            const float grow = radius * t;
            const float alpha = (1.0f - t) * 0.5f + 0.06f;
            list->AddRect(ImVec2(min.x - grow, min.y - grow), ImVec2(max.x + grow, max.y + grow), WithAlpha(color, alpha * 0.9f), rounding + grow, 0, std::max(2.0f, radius / static_cast<float>(layers) * 1.6f));
        }
    }

    void Sign18(ImDrawList* list, const ImVec2 center, const float diameter, const ImU32 ringColor, const ImU32 fill, const ImU32 textColor, const float ring, const bool plus)
    {
        const float radius = diameter * 0.5f;
        list->AddCircleFilled(center, radius, fill, 40);
        list->AddCircle(center, radius - ring * 0.5f, ringColor, 40, ring);
        // The size of the digits: about 0.42 of the diameter (plus sign: a little smaller)
        TextCentered(list, ImVec2(center.x, center.y - 0.5f), diameter * (plus ? 0.30f : 0.40f), textColor, plus ? "18+" : "18");
    }

    void GradientV(ImDrawList* list, const ImVec2 min, const ImVec2 max, const ImU32 top, const ImU32 bottom, const float rounding)
    {
        if (rounding <= 0.0f)
        {
            list->AddRectFilledMultiColor(min, max, top, top, bottom, bottom);
            return;
        }
        // Rounded: the middle as a gradient, the ends as rounded rectangles of the end colours
        list->AddRectFilled(min, ImVec2(max.x, min.y + rounding * 2.0f), top, rounding, ImDrawFlags_RoundCornersTop);
        list->AddRectFilled(ImVec2(min.x, max.y - rounding * 2.0f), max, bottom, rounding, ImDrawFlags_RoundCornersBottom);
        list->AddRectFilledMultiColor(ImVec2(min.x, min.y + rounding * 2.0f), ImVec2(max.x, max.y - rounding * 2.0f), top, top, bottom, bottom);
    }

    void Spinner(ImDrawList* list, const ImVec2 center, const float radius, const float width, const ImU32 color, const double time, const float period)
    {
        const float start = static_cast<float>(std::fmod(time, static_cast<double>(period)) / static_cast<double>(period)) * 2.0f * kPi;
        list->PathClear();
        constexpr int kSegments = 24;
        for (int i = 0; i <= kSegments; ++i)
        {
            const float a = start + (static_cast<float>(i) / static_cast<float>(kSegments)) * 1.5f * kPi;
            list->PathLineTo(ImVec2(center.x + std::cos(a) * radius, center.y + std::sin(a) * radius));
        }
        list->PathStroke(color, 0, width);
    }

    void ScaleRotate(ImDrawList* list, const int start, const ImVec2 center, const float scale, const float degrees)
    {
        if (scale == 1.0f && degrees == 0.0f)
        {
            return;
        }
        const float radians = degrees * kPi / 180.0f;
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        for (int i = start; i < list->VtxBuffer.Size; ++i)
        {
            ImDrawVert& v = list->VtxBuffer[i];
            const float x = (v.pos.x - center.x) * scale;
            const float y = (v.pos.y - center.y) * scale;
            v.pos.x = center.x + x * c - y * s;
            v.pos.y = center.y + x * s + y * c;
        }
    }

    void FadeVertices(ImDrawList* list, const int start, const float alpha)
    {
        if (alpha >= 1.0f)
        {
            return;
        }
        for (int i = start; i < list->VtxBuffer.Size; ++i)
        {
            ImDrawVert& v = list->VtxBuffer[i];
            const float a = static_cast<float>((v.col >> IM_COL32_A_SHIFT) & 0xFF) * (alpha < 0.0f ? 0.0f : alpha);
            v.col = (v.col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
        }
    }

    namespace
    {
        struct HitRecord
        {
            std::string id;
            ImVec2 center;
        };

        std::vector<HitRecord>& HitRecords()
        {
            static std::vector<HitRecord> records;
            return records;
        }
    }

    void ClearHitRecords()
    {
        HitRecords().clear();
    }

    bool FindHitRecord(const std::string& id, ImVec2& center)
    {
        const auto& records = HitRecords();
        for (auto it = records.rbegin(); it != records.rend(); ++it)
        {
            if (it->id == id)
            {
                center = it->center;
                return true;
            }
        }
        return false;
    }

    bool Hit(const char* id, const ImVec2 min, const ImVec2 max, bool* hovered, bool* held)
    {
        {
            std::string name = id;
            if (name.rfind("##", 0) == 0)
            {
                name.erase(0, 2);
            }
            HitRecords().push_back({std::move(name), ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f)});
        }
        ImGui::SetCursorScreenPos(min);
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, max.x - min.x), std::max(1.0f, max.y - min.y)));
        if (hovered != nullptr)
        {
            *hovered = ImGui::IsItemHovered();
        }
        if (held != nullptr)
        {
            *held = ImGui::IsItemActive();
        }
        return clicked;
    }

    bool ColorButton(ImDrawList* list, const char* id, const ImVec2 min, const ImVec2 max, const char* label, const float em, const ImU32 fill, const ImU32 hoverFill, const ImU32 text,
                     const ImU32 ring, const float rounding)
    {
        bool hovered = false;
        bool held = false;
        const bool clicked = Hit(id, min, max, &hovered, &held);
        const ImU32 color = held ? Scaled(hoverFill, 0.85f) : hovered ? hoverFill : fill;
        if ((color & IM_COL32_A_MASK) != 0)
        {
            list->AddRectFilled(min, max, color, rounding);
        }
        if ((ring & IM_COL32_A_MASK) != 0)
        {
            list->AddRect(min, max, ring, rounding, 0, 1.0f);
        }
        TextCentered(list, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), em, text, label);
        return clicked;
    }

    bool Button(ImDrawList* list, const char* id, const ImVec2 min, const ImVec2 max, const char* label, const ButtonKind kind, const float em, const bool enabled)
    {
        bool hovered = false;
        bool held = false;
        bool clicked = false;
        if (enabled)
        {
            clicked = Hit(id, min, max, &hovered, &held);
        }
        const float alpha = enabled ? 1.0f : 0.45f;
        if (kind == ButtonKind::Primary)
        {
            Glow(list, min, max, WithAlpha(kHot, 0.45f * alpha), 6.0f, 3, 6.0f);
            const ImU32 fill = held ? kHotPressed : hovered ? kHotHover : kHot;
            list->AddRectFilled(min, max, WithAlpha(fill, alpha), 6.0f);
            TextCentered(list, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), em, WithAlpha(kWhite, alpha), label);
        }
        else
        {
            if (hovered)
            {
                list->AddRectFilled(min, max, WithAlpha(kHot, 0.12f), 6.0f);
            }
            list->AddRect(min, max, WithAlpha(kHot, alpha), 6.0f, 0, 1.5f);
            TextCentered(list, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), em, WithAlpha(kHot, alpha), label);
        }
        return clicked;
    }
}
