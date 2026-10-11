#define IMGUI_DEFINE_MATH_OPERATORS // before the first ImGui include (imgui_internal.h needs it)
#include "EditorWidgets.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <imgui/imgui_internal.h>
#include <imgui/misc/imgui_stdlib.h>

#include <myengine/editor/ThumbnailService.h>

namespace myengine::ui
{
    namespace
    {
        struct FontEntry
        {
            const ImGuiContext* context = nullptr;
            EditorFonts fonts;
        };

        std::vector<FontEntry>& FontRegistry()
        {
            static std::vector<FontEntry> registry;
            return registry;
        }

        const EditorFonts& CurrentFonts()
        {
            static const EditorFonts empty;
            const ImGuiContext* context = ImGui::GetCurrentContext();
            for (const FontEntry& entry : FontRegistry())
            {
                if (entry.context == context)
                {
                    return entry.fonts;
                }
            }
            return empty;
        }

        ImFont* RoleFont(const FontRole role, float& sizeOut)
        {
            const EditorFonts& fonts = CurrentFonts();
            ImFont* font = nullptr;
            switch (role)
            {
                case FontRole::Strong:
                    sizeOut = fonts.strongSize > 0.0f ? fonts.strongSize : style::kFontStrong;
                    font = fonts.strong;
                    break;
                case FontRole::Secondary:
                    sizeOut = fonts.secondarySize > 0.0f ? fonts.secondarySize : style::kFontSecondary;
                    font = fonts.secondary;
                    break;
                case FontRole::Tiny:
                    sizeOut = fonts.tinySize > 0.0f ? fonts.tinySize : style::kFontTiny;
                    font = fonts.tiny;
                    break;
                case FontRole::Mono:
                    sizeOut = fonts.monoSize > 0.0f ? fonts.monoSize : style::kFontMono;
                    font = fonts.mono;
                    break;
                case FontRole::Body:
                default:
                    sizeOut = fonts.bodySize > 0.0f ? fonts.bodySize : style::kFontBody;
                    font = fonts.body;
                    break;
            }
            if (font == nullptr)
            {
                // A context without editor fonts (headless tests): the font that is current, so that callers
                // never meet a null role font
                font = ImGui::GetFont();
                sizeOut = ImGui::GetFontSize();
            }
            return font;
        }

        ImFont* IconFont(const IconSize size, float& sizeOut)
        {
            const EditorFonts& fonts = CurrentFonts();
            switch (size)
            {
                case IconSize::Chevron12: sizeOut = 12.0f; return fonts.icon12;
                case IconSize::Button16: sizeOut = 16.0f; return fonts.icon16;
                case IconSize::Toolbar18: sizeOut = 18.0f; return fonts.icon18;
                case IconSize::Empty30: sizeOut = 30.0f; return fonts.icon30;
                case IconSize::Tile40: sizeOut = 40.0f; return fonts.icon40;
                case IconSize::Folder58: sizeOut = 58.0f; return fonts.icon58;
                case IconSize::Row14:
                default: sizeOut = 14.0f; return fonts.icon14;
            }
        }

        ImVec4 Vec4(const ImU32 color)
        {
            return ImGui::ColorConvertU32ToFloat4(color);
        }

        // Runs the width of `text` through the font of `role`
        float TextWidth(const FontRole role, const char* text)
        {
            PushFontRole(role);
            const float width = ImGui::CalcTextSize(text).x;
            PopFontRole();
            return width;
        }

        std::string WithIcon(const char* icon, const char* label)
        {
            std::string text;
            if (icon != nullptr && icon[0] != '\0')
            {
                text = icon;
                if (label != nullptr && label[0] != '\0')
                {
                    text += ' ';
                }
            }
            if (label != nullptr)
            {
                text += label;
            }
            return text;
        }

        // Semantic colours of chips and banners: stripe/fill colour and the text colour
        struct SemanticColors
        {
            ImU32 base;
            ImU32 text;
        };

        SemanticColors ChipColors(const ChipKind kind)
        {
            switch (kind)
            {
                case ChipKind::Green: return {style::kPlay, IM_COL32(0x7F, 0xD6, 0x84, 255)};
                case ChipKind::Red: return {style::kError, style::kErrorText};
                case ChipKind::Amber: return {style::kWarning, IM_COL32(0xFF, 0xCB, 0x4D, 255)};
                case ChipKind::Blue: return {style::kPrimary, IM_COL32(0x6C, 0xB2, 0xFF, 255)};
                case ChipKind::Gray:
                default: return {style::kTextDim, IM_COL32(0xB0, 0xB0, 0xB0, 255)};
            }
        }

        // Disabled items keep their own colours (kTextDisabled, flat background), so the global DisabledAlpha is not applied
        struct ScopedDisabled
        {
            explicit ScopedDisabled(const bool disabled)
                : active(disabled)
            {
                if (active)
                {
                    ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
                }
            }
            ~ScopedDisabled()
            {
                if (active)
                {
                    ImGui::PopItemFlag();
                }
            }
            bool active;
        };

        thread_local float g_gridIndent = 0.0f;
        thread_local float g_dialogWidth = 0.0f;
    }

    // ---- fonts ----

    void SetEditorFonts(const EditorFonts& fonts)
    {
        const ImGuiContext* context = ImGui::GetCurrentContext();
        for (FontEntry& entry : FontRegistry())
        {
            if (entry.context == context)
            {
                entry.fonts = fonts;
                return;
            }
        }
        FontRegistry().push_back({context, fonts});
    }

    void ClearEditorFonts()
    {
        const ImGuiContext* context = ImGui::GetCurrentContext();
        auto& registry = FontRegistry();
        registry.erase(
            std::remove_if(registry.begin(), registry.end(), [context](const FontEntry& entry) { return entry.context == context; }),
            registry.end());
    }

    void PushFontRole(const FontRole role)
    {
        float size = 0.0f;
        ImFont* font = RoleFont(role, size);
        if (font != nullptr)
        {
            ImGui::PushFont(font, size);
        }
        else
        {
            ImGui::PushFont(nullptr, 0.0f); // keep the current font so Pop stays balanced
        }
    }

    void PopFontRole()
    {
        ImGui::PopFont();
    }

    float FontRoleSize(const FontRole role)
    {
        float size = 0.0f;
        RoleFont(role, size);
        return size;
    }

    void DrawIcon(ImDrawList* drawList, const IconSize size, const char* icon, const ImVec2 center, const ImU32 color)
    {
        float sizePx = 0.0f;
        ImFont* font = IconFont(size, sizePx);
        if (font == nullptr || icon == nullptr)
        {
            return;
        }

        ImGui::PushFont(font, sizePx);
        const ImVec2 textSize = ImGui::CalcTextSize(icon);
        ImGui::PopFont();
        drawList->AddText(font, sizePx, ImVec2(std::floor(center.x - textSize.x * 0.5f), std::floor(center.y - textSize.y * 0.5f)), color, icon);
    }

    // ---- buttons ----

    namespace
    {
        bool StyledButton(const char* label, const char* icon, const bool enabled, const float width, const ImU32 normal,
                          const ImU32 hovered, const ImU32 active, const ImU32 textColor, const bool border,
                          const ImU32 borderColor)
        {
            const std::string text = WithIcon(icon, label);
            PushFontRole(FontRole::Secondary);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, (style::kFrameHeight - FontRoleSize(FontRole::Secondary)) * 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, style::kRounding);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, border ? 1.0f : 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, enabled ? Vec4(normal) : Vec4(style::kControlDisabled));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, enabled ? Vec4(hovered) : Vec4(style::kControlDisabled));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, enabled ? Vec4(active) : Vec4(style::kControlDisabled));
            ImGui::PushStyleColor(ImGuiCol_Text, Vec4(enabled ? textColor : style::kTextDisabled));
            ImGui::PushStyleColor(ImGuiCol_Border, Vec4(borderColor));

            bool pressed = false;
            {
                ScopedDisabled disabled(!enabled);
                pressed = ImGui::Button(text.c_str(), ImVec2(width, style::kFrameHeight));
            }

            ImGui::PopStyleColor(5);
            ImGui::PopStyleVar(3);
            PopFontRole();
            return pressed && enabled;
        }
    }

    bool Button(const char* label, const char* icon, const bool enabled, const float width)
    {
        return StyledButton(label, icon, enabled, width, style::kControl, style::kControlHover, style::kControlActive,
                            style::kText, false, 0);
    }

    bool PrimaryButton(const char* label, const char* icon, const bool enabled, const float width)
    {
        return StyledButton(label, icon, enabled, width, style::kPrimary, style::kPrimaryHover, style::kPrimaryActive,
                            style::kTextStrong, false, 0);
    }

    bool DestructiveButton(const char* label, const char* icon, const bool enabled)
    {
        return StyledButton(label, icon, enabled, 0.0f, 0, style::WithAlpha(style::kError, 0.14f),
                            style::WithAlpha(style::kError, 0.24f), style::kDestructiveText, true, style::kDestructiveBorder);
    }

    bool IconButton(const char* id, const char* icon, const char* tooltip, const bool selected, const bool enabled,
                    const ImU32 tint, const float size, const char* shortcut, const IconSize iconSize)
    {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        bool pressed = false;
        bool hovered = false;
        bool held = false;
        {
            ScopedDisabled disabled(!enabled);
            pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
            hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            held = ImGui::IsItemActive();
        }

        auto* drawList = ImGui::GetWindowDrawList();
        const ImVec2 max(min.x + size, min.y + size);
        if (selected && enabled)
        {
            drawList->AddRectFilled(min, max, held ? style::kPrimaryActive : (hovered ? style::kPrimaryHover : style::kPrimary), style::kRounding);
        }
        else if (enabled && (hovered || held))
        {
            drawList->AddRectFilled(min, max, held ? style::kControlActive : style::kControl, style::kRounding);
        }

        ImU32 iconColor = style::kText;
        if (!enabled)
        {
            iconColor = style::kTextDisabled;
        }
        else if (selected)
        {
            iconColor = style::kTextStrong;
        }
        else if (tint != 0)
        {
            iconColor = tint;
        }
        else if (hovered)
        {
            iconColor = style::kTextStrong;
        }
        DrawIcon(drawList, iconSize, icon, ImVec2(min.x + size * 0.5f, min.y + size * 0.5f), iconColor);

        if (tooltip != nullptr)
        {
            Tooltip(tooltip, shortcut);
        }
        return pressed && enabled;
    }

    SplitButtonPart SplitButton(const char* id, const char* icon, const char* label, const bool enabled,
                                const ImU32 iconTint, const bool wholeOpensPopup, const char* tooltip, const char* shortcut)
    {
        const float height = style::kToolButton;
        const float chevronZone = 18.0f;
        const float textWidth = (label != nullptr && label[0] != '\0') ? TextWidth(FontRole::Secondary, label) : 0.0f;
        const float mainWidth = 10.0f + 18.0f + (textWidth > 0.0f ? 6.0f + textWidth : 0.0f) + (wholeOpensPopup ? 4.0f : 8.0f);
        const float totalWidth = mainWidth + chevronZone;

        ImGui::PushID(id);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        SplitButtonPart result = SplitButtonPart::None;
        bool mainHovered = false;
        bool chevronHovered = false;
        bool mainHeld = false;
        bool chevronHeld = false;
        {
            ScopedDisabled disabled(!enabled);
            if (wholeOpensPopup)
            {
                if (ImGui::InvisibleButton("##whole", ImVec2(totalWidth, height)) && enabled)
                {
                    result = SplitButtonPart::Chevron;
                }
                mainHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
                mainHeld = ImGui::IsItemActive();
                chevronHovered = mainHovered;
                chevronHeld = mainHeld;
            }
            else
            {
                if (ImGui::InvisibleButton("##main", ImVec2(mainWidth, height)) && enabled)
                {
                    result = SplitButtonPart::Main;
                }
                mainHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
                mainHeld = ImGui::IsItemActive();
                ImGui::SameLine(0.0f, 0.0f);
                if (ImGui::InvisibleButton("##chevron", ImVec2(chevronZone, height)) && enabled)
                {
                    result = SplitButtonPart::Chevron;
                }
                chevronHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
                chevronHeld = ImGui::IsItemActive();
            }
        }

        auto* drawList = ImGui::GetWindowDrawList();
        if (enabled)
        {
            const float splitX = min.x + mainWidth;
            if (wholeOpensPopup)
            {
                if (mainHovered || mainHeld)
                {
                    drawList->AddRectFilled(min, ImVec2(min.x + totalWidth, min.y + height), mainHeld ? style::kControlActive : style::kControl, style::kRounding);
                }
            }
            else
            {
                if (mainHovered || mainHeld)
                {
                    drawList->AddRectFilled(min, ImVec2(splitX, min.y + height), mainHeld ? style::kControlActive : style::kControl,
                                            style::kRounding, ImDrawFlags_RoundCornersLeft);
                }
                if (chevronHovered || chevronHeld)
                {
                    drawList->AddRectFilled(ImVec2(splitX, min.y), ImVec2(min.x + totalWidth, min.y + height),
                                            chevronHeld ? style::kControlActive : style::kControl, style::kRounding, ImDrawFlags_RoundCornersRight);
                }
            }
        }

        const ImU32 contentColor = !enabled ? style::kTextDisabled : (mainHovered ? style::kTextStrong : style::kText);
        const ImU32 iconColor = !enabled ? style::kTextDisabled : (iconTint != 0 ? iconTint : contentColor);
        const float centerY = min.y + height * 0.5f;
        DrawIcon(drawList, IconSize::Toolbar18, icon, ImVec2(min.x + 10.0f + 9.0f, centerY), iconColor);
        if (textWidth > 0.0f)
        {
            float sizePx = 0.0f;
            ImFont* font = RoleFont(FontRole::Secondary, sizePx);
            drawList->AddText(font, sizePx, ImVec2(min.x + 10.0f + 18.0f + 6.0f, std::floor(centerY - sizePx * 0.5f - 0.5f)), contentColor, label);
        }
        DrawIcon(drawList, IconSize::Chevron12, ICON_CHEVRON_DOWN, ImVec2(min.x + mainWidth + chevronZone * 0.5f - (wholeOpensPopup ? 2.0f : 0.0f), centerY),
                 !enabled ? style::kTextDisabled : (chevronHovered ? style::kTextStrong : style::kTextDim));

        ImGui::PopID();
        if (tooltip != nullptr && ImGui::IsMouseHoveringRect(min, ImVec2(min.x + totalWidth, min.y + height)))
        {
            // Item state belongs to the last invisible button; show the tooltip after the delay for the whole control
            ImGui::SetCursorScreenPos(min);
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            Tooltip(tooltip, shortcut);
            ImGui::SetCursorScreenPos(ImVec2(min.x + totalWidth, min.y));
            ImGui::Dummy(ImVec2(0.0f, height));
        }
        return result;
    }

    void ToolbarSeparator()
    {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x + 6.5f, min.y + 6.0f), ImVec2(min.x + 6.5f, min.y + 26.0f), style::kControl, 1.0f);
        ImGui::Dummy(ImVec2(13.0f, style::kToolButton));
    }

    // ---- toggles and fields ----

    int Segmented(const char* id, const char* const* labels, const int count, const int current, const float segmentWidth)
    {
        int result = current;
        ImGui::PushID(id);
        PushFontRole(FontRole::Secondary);

        float widths[16] = {};
        const int n = std::min(count, 16);
        float total = 4.0f;
        for (int i = 0; i < n; ++i)
        {
            widths[i] = std::max(segmentWidth, ImGui::CalcTextSize(labels[i]).x + 20.0f);
            total += widths[i];
        }

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, ImVec2(origin.x + total, origin.y + style::kFrameHeight), style::kInput, style::kRounding);

        float x = origin.x + 2.0f;
        for (int i = 0; i < n; ++i)
        {
            ImGui::SetCursorScreenPos(ImVec2(x, origin.y + 2.0f));
            ImGui::PushID(i);
            const bool pressed = ImGui::InvisibleButton("##seg", ImVec2(widths[i], style::kFrameHeight - 4.0f));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (pressed)
            {
                result = i;
            }

            const ImVec2 segMin(x, origin.y + 2.0f);
            const ImVec2 segMax(x + widths[i], origin.y + style::kFrameHeight - 2.0f);
            const bool active = i == current;
            if (active)
            {
                drawList->AddRectFilled(segMin, segMax, style::kControl, style::kRoundingSmall);
            }
            const ImVec2 textSize = ImGui::CalcTextSize(labels[i]);
            drawList->AddText(ImVec2(std::floor(segMin.x + (widths[i] - textSize.x) * 0.5f), std::floor(segMin.y + (segMax.y - segMin.y - textSize.y) * 0.5f)),
                              active ? style::kTextStrong : (hovered ? style::kText : style::kTextDim), labels[i]);
            x += widths[i];
        }

        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(total, style::kFrameHeight));
        PopFontRole();
        ImGui::PopID();
        return result;
    }

    bool Checkbox(const char* label, bool* value, const bool enabled)
    {
        const char* labelEnd = ImGui::FindRenderedTextEnd(label);
        PushFontRole(FontRole::Body);
        const ImVec2 textSize = ImGui::CalcTextSize(label, labelEnd);
        const float boxSize = 16.0f;
        const float gap = 8.0f;
        const ImVec2 min = ImGui::GetCursorScreenPos();
        bool changed = false;
        bool hovered = false;
        {
            ScopedDisabled disabled(!enabled);
            if (ImGui::InvisibleButton(label, ImVec2(boxSize + gap + textSize.x, std::max(boxSize, textSize.y) + 2.0f)) && enabled)
            {
                *value = !*value;
                changed = true;
            }
            hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        }

        auto* drawList = ImGui::GetWindowDrawList();
        const float boxTop = min.y + 1.0f;
        const ImVec2 boxMin(min.x, boxTop);
        const ImVec2 boxMax(min.x + boxSize, boxTop + boxSize);
        if (*value)
        {
            drawList->AddRectFilled(boxMin, boxMax, enabled ? (hovered ? style::kPrimaryHover : style::kPrimary) : style::kControl, 3.0f);
            const ImU32 tick = enabled ? style::kTextStrong : style::kTextDisabled;
            drawList->AddLine(ImVec2(boxMin.x + 3.5f, boxMin.y + 8.5f), ImVec2(boxMin.x + 6.5f, boxMin.y + 11.5f), tick, 1.8f);
            drawList->AddLine(ImVec2(boxMin.x + 6.5f, boxMin.y + 11.5f), ImVec2(boxMin.x + 12.5f, boxMin.y + 4.5f), tick, 1.8f);
        }
        else
        {
            drawList->AddRectFilled(boxMin, boxMax, style::kInput, 3.0f);
            drawList->AddRect(boxMin, boxMax, hovered && enabled ? style::kTextDim : style::kCheckBorder, 3.0f, 0, 1.0f);
        }
        drawList->AddText(ImVec2(min.x + boxSize + gap, min.y + 1.0f), enabled ? style::kText : style::kTextDisabled, label, labelEnd);
        PopFontRole();
        return changed;
    }

    void FocusOutline()
    {
        if (ImGui::IsItemActive())
        {
            ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), style::kPrimary, style::kRounding, 0, 1.0f);
        }
    }

    bool SearchField(const char* id, std::string* text, const char* hint, const float width)
    {
        const float fieldWidth = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(28.0f, (style::kFrameHeight - FontRoleSize(FontRole::Body)) * 0.5f));
        const bool changed = ImGui::InputTextWithHint(id, hint, text);
        ImGui::PopStyleVar();

        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        FocusOutline();
        DrawIcon(ImGui::GetWindowDrawList(), IconSize::Row14, ICON_SEARCH, ImVec2(min.x + 15.0f, (min.y + max.y) * 0.5f), style::kTextDim);
        return changed;
    }

    // ---- categories and the property grid ----

    bool BeginCategory(const char* title, const bool defaultOpen, const char* rightText, const char* icon)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        ImGuiStorage* storage = ImGui::GetStateStorage();
        ImGui::PushID(title);
        const ImGuiID stateId = ImGui::GetID("##open");
        bool open = storage->GetBool(stateId, defaultOpen);

        const float scrollbar = window->ScrollbarY ? ImGui::GetStyle().ScrollbarSize : 0.0f;
        const float x0 = window->Pos.x;
        const float x1 = window->Pos.x + window->Size.x - scrollbar;
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const ImVec2 min(x0, cursor.y);
        const ImVec2 max(x1, cursor.y + style::kCategoryHeight);

        ImGui::SetCursorScreenPos(min);
        const bool pressed = ImGui::InvisibleButton("##header", ImVec2(x1 - x0, style::kCategoryHeight));
        const bool hovered = ImGui::IsItemHovered();
        if (pressed)
        {
            open = !open;
            storage->SetBool(stateId, open);
        }

        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, hovered ? IM_COL32(0x35, 0x35, 0x35, 255) : style::kHeader);
        drawList->AddLine(min, ImVec2(max.x, min.y), style::kInput, 1.0f);
        const float centerY = (min.y + max.y) * 0.5f;
        DrawIcon(drawList, IconSize::Chevron12, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, ImVec2(x0 + 16.0f, centerY), style::kTextDim);

        float textX = x0 + 30.0f;
        if (icon != nullptr)
        {
            DrawIcon(drawList, IconSize::Row14, icon, ImVec2(textX + 7.0f, centerY), style::kTextDim);
            textX += 22.0f;
        }
        float sizePx = 0.0f;
        ImFont* strong = RoleFont(FontRole::Strong, sizePx);
        const char* titleEnd = ImGui::FindRenderedTextEnd(title);
        drawList->AddText(strong, sizePx, ImVec2(textX, std::floor(centerY - sizePx * 0.5f - 0.5f)), open ? style::kTextStrong : style::kText, title, titleEnd);
        if (rightText != nullptr && rightText[0] != '\0')
        {
            float smallPx = 0.0f;
            ImFont* small = RoleFont(FontRole::Secondary, smallPx);
            PushFontRole(FontRole::Secondary);
            const float rightWidth = ImGui::CalcTextSize(rightText).x;
            PopFontRole();
            drawList->AddText(small, smallPx, ImVec2(x1 - 12.0f - rightWidth, std::floor(centerY - smallPx * 0.5f - 0.5f)), style::kTextDim, rightText);
        }

        ImGui::SetCursorScreenPos(ImVec2(cursor.x, max.y + 6.0f));
        if (!open)
        {
            ImGui::PopID();
        }
        return open;
    }

    void EndCategory()
    {
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::PopID();
    }

    bool BeginPropertyGrid(const char* id, const float indent)
    {
        const float available = ImGui::GetContentRegionAvail().x;
        const float labelWidth = std::clamp(available * 0.38f, 110.0f, 200.0f);
        ImGui::Indent(indent);
        // Row height is the 24 px field plus a pixel of padding on each side (26), not the global 4 px padding
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, 1.0f));
        const bool open = ImGui::BeginTable(id, 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings);
        if (!open)
        {
            ImGui::PopStyleVar();
            ImGui::Unindent(indent);
            return false;
        }
        g_gridIndent = indent;
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, std::max(labelWidth - indent, 80.0f));
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("reset", ImGuiTableColumnFlags_WidthFixed, 22.0f);
        return true;
    }

    void PropertyLabel(const char* text, const bool dim, const float rowHeight)
    {
        const float height = std::max(rowHeight, style::kPropRowHeight);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, height);
        ImGui::TableSetColumnIndex(0);
        if (height > style::kPropRowHeight)
        {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (height - style::kPropRowHeight) * 0.5f);
        }
        ImGui::AlignTextToFramePadding();

        const char* textEnd = ImGui::FindRenderedTextEnd(text);
        const ImVec2 position = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 size = ImGui::CalcTextSize(text, textEnd);
        const ImU32 color = dim ? style::kTextDim : style::kText;
        auto* drawList = ImGui::GetWindowDrawList();
        if (size.x > width)
        {
            ImGui::RenderTextEllipsis(drawList, position, ImVec2(position.x + width, position.y + size.y), position.x + width, text, textEnd, &size);
            // RenderTextEllipsis uses the style text colour; repaint in the role colour is not needed for the dim case
        }
        else
        {
            drawList->AddText(position, color, text, textEnd);
        }
        ImGui::Dummy(ImVec2(width, size.y));
        if (size.x > width && ImGui::IsItemHovered())
        {
            Tooltip(text);
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
    }

    bool PropertyReset(const char* tooltip)
    {
        ImGui::TableSetColumnIndex(2);
        ImGui::PushID("##reset");
        const bool clicked = IconButton("##undo", ICON_UNDO_2, tooltip, false, true, style::kReset, 22.0f, nullptr, IconSize::Row14);
        ImGui::PopID();
        return clicked;
    }

    void EndPropertyGrid()
    {
        ImGui::EndTable();
        ImGui::PopStyleVar();
        ImGui::Unindent(g_gridIndent);
    }

    bool DragVector3(const char* id, float values[3], const float speed, const char* format, const std::function<void(int axis)>& afterField)
    {
        static constexpr ImU32 kAxisColors[3] = {style::kAxisX, style::kAxisY, style::kAxisZ};
        const float gap = 4.0f;
        const float available = ImGui::GetContentRegionAvail().x;
        const float fieldWidth = std::max(std::floor((available - gap * 2.0f) / 3.0f), 24.0f);

        bool changed = false;
        ImGui::PushID(id);
        PushFontRole(FontRole::Secondary);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(11.0f, (style::kFrameHeight - FontRoleSize(FontRole::Secondary)) * 0.5f));
        for (int axis = 0; axis < 3; ++axis)
        {
            if (axis > 0)
            {
                ImGui::SameLine(0.0f, gap);
            }
            ImGui::SetNextItemWidth(fieldWidth);
            ImGui::PushID(axis);
            if (ImGui::DragFloat("##v", &values[axis], speed, 0.0f, 0.0f, format))
            {
                changed = true;
            }
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + 3.0f, max.y), kAxisColors[axis], style::kRounding, ImDrawFlags_RoundCornersLeft);
            FocusOutline();
            if (afterField)
            {
                afterField(axis);
            }
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        PopFontRole();
        ImGui::PopID();
        return changed;
    }

    namespace
    {
        struct PickerKindInfo
        {
            const char* icon;
            ImU32 color;
            const char* singular;
            const char* plural;
        };

        PickerKindInfo PickerInfo(const PickerKind kind)
        {
            switch (kind)
            {
                case PickerKind::Material: return {ICON_PALETTE, style::kTypeMaterial, "Material", "materials"};
                case PickerKind::Texture: return {ICON_IMAGE, style::kTypeTexture, "Texture", "textures"};
                case PickerKind::Shader: return {ICON_CODE_XML, style::kTypeShader, "Shader", "shaders"};
                case PickerKind::Mesh: break;
            }
            return {ICON_BOX, style::kTypeMesh, "Mesh", "meshes"};
        }

        // "assets/models/crate.obj" -> "crate", "default.material.json" -> "default"; an empty key is "None"
        std::string PickerName(const std::string& key)
        {
            if (key.empty())
            {
                return "None";
            }
            std::string name = key;
            const std::size_t slash = name.find_last_of("/\\");
            if (slash != std::string::npos)
            {
                name = name.substr(slash + 1);
            }
            while (true)
            {
                const std::size_t dot = name.find_last_of('.');
                if (dot == std::string::npos || dot == 0)
                {
                    break;
                }
                name = name.substr(0, dot);
            }
            return name;
        }

        std::string LowerCopy(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        bool g_pickerListView = false; // Tiles | List of the drop-down, kept for the session
        std::string g_pickerSearch;

        // The picture of an asset in a square: its thumbnail with the aspect kept, or the type icon
        void DrawPickerImage(
            ImDrawList* drawList,
            const ImVec2 min,
            const float size,
            const PickerKindInfo& info,
            const PickerKind kind,
            const std::string& key,
            editor::ThumbnailService* thumbnails,
            const std::uint32_t requestSize)
        {
            const ImVec2 max(min.x + size, min.y + size);
            drawList->AddRectFilled(min, max, IM_COL32(0x12, 0x12, 0x12, 255), 3.0f);
            if (thumbnails != nullptr && kind != PickerKind::Shader && !key.empty())
            {
                const editor::Thumbnail thumbnail = thumbnails->Request(key, requestSize);
                if (thumbnail.ready)
                {
                    float width = size;
                    float height = size;
                    if (thumbnail.sourceWidth > 0 && thumbnail.sourceHeight > 0)
                    {
                        const float aspect = static_cast<float>(thumbnail.sourceWidth) / static_cast<float>(thumbnail.sourceHeight);
                        if (aspect >= 1.0f)
                        {
                            height = size / aspect;
                        }
                        else
                        {
                            width = size * aspect;
                        }
                    }
                    const ImVec2 imageMin(min.x + (size - width) * 0.5f, min.y + (size - height) * 0.5f);
                    drawList->AddImage(
                        static_cast<ImTextureID>(thumbnail.ImGuiTextureId()), imageMin, ImVec2(imageMin.x + width, imageMin.y + height));
                    return;
                }
            }
            const IconSize iconSize = size >= 60.0f ? IconSize::Tile40 : (size >= 30.0f ? IconSize::Toolbar18 : IconSize::Row14);
            DrawIcon(drawList, iconSize, info.icon, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), IM_COL32(0x3D, 0x3D, 0x3D, 255));
        }

        void DrawPickerPreviewTooltip(
            const std::string& key,
            const PickerKindInfo& info,
            const PickerKind kind,
            const AssetPickerOptions& options)
        {
            ImGui::BeginTooltip();
            const float size = 256.0f;
            const ImVec2 min = ImGui::GetCursorScreenPos();
            DrawPickerImage(ImGui::GetWindowDrawList(), min, size, info, kind, key, options.thumbnails, 256);
            ImGui::Dummy(ImVec2(size, size));
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            PushFontRole(FontRole::Strong);
            ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextStrong));
            ImGui::TextUnformatted(PickerName(key).c_str());
            ImGui::PopStyleColor();
            PopFontRole();
            PushFontRole(FontRole::Secondary);
            ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextDim));
            const std::string meta = options.meta ? options.meta(key) : std::string(info.singular);
            ImGui::TextUnformatted(meta.c_str());
            ImGui::PushTextWrapPos(size);
            ImGui::TextUnformatted(key.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            PopFontRole();
            ImGui::EndTooltip();
        }
    }

    bool AssetPicker(
        const char* id,
        const std::string& current,
        const std::vector<std::string>& keys,
        const AssetPickerOptions& options,
        std::string& chosen)
    {
        const PickerKindInfo info = PickerInfo(options.kind);
        ImGui::PushID(id);

        constexpr float kHeight = 48.0f;
        constexpr float kThumb = 40.0f;
        const float width = std::max(ImGui::GetContentRegionAvail().x, 60.0f);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + width, min.y + kHeight);
        const char* popupId = "##asset_picker_popup";

        const bool pressed = ImGui::InvisibleButton("##picker", ImVec2(width, kHeight));
        const bool hovered = ImGui::IsItemHovered();
        const bool delayedHover = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
        if (pressed)
        {
            ImGui::OpenPopup(popupId);
        }
        const bool popupOpen = ImGui::IsPopupOpen(popupId);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, (hovered || popupOpen) ? style::kControlHover : style::kControl, style::kRounding);
        if (popupOpen)
        {
            drawList->AddRect(min, max, style::kPrimary, style::kRounding, 0, 1.0f);
        }
        drawList->AddRectFilled(min, ImVec2(min.x + 3.0f, max.y), info.color, style::kRounding, ImDrawFlags_RoundCornersLeft);

        const ImVec2 thumbMin(min.x + 3.0f + 6.0f, min.y + (kHeight - kThumb) * 0.5f);
        DrawPickerImage(drawList, thumbMin, kThumb, info, options.kind, current, options.thumbnails, 64);

        // Two lines: the name and what it is
        const float textX = thumbMin.x + kThumb + 10.0f;
        drawList->PushClipRect(ImVec2(textX, min.y), ImVec2(max.x - 26.0f, max.y), true);
        {
            float sizePx = 0.0f;
            ImFont* nameFont = RoleFont(FontRole::Strong, sizePx);
            const std::string name = PickerName(current);
            drawList->AddText(nameFont, sizePx, ImVec2(textX, std::floor(min.y + 7.0f)), current.empty() ? style::kTextDim : style::kTextStrong, name.c_str());
            float metaPx = 0.0f;
            ImFont* metaFont = RoleFont(FontRole::Tiny, metaPx);
            const std::string meta = current.empty() ? std::string("Nothing assigned")
                                                     : (options.meta ? options.meta(current) : std::string(info.singular));
            drawList->AddText(metaFont, metaPx, ImVec2(textX, std::floor(min.y + kHeight - 7.0f - metaPx)), style::kTextDim, meta.c_str());
        }
        drawList->PopClipRect();
        DrawIcon(drawList, IconSize::Chevron12, ICON_CHEVRON_DOWN, ImVec2(max.x - 14.0f, (min.y + max.y) * 0.5f), style::kTextDim);

        // The big preview on the thumbnail
        if (!popupOpen && delayedHover && !current.empty() &&
            ImGui::IsMouseHoveringRect(thumbMin, ImVec2(thumbMin.x + kThumb, thumbMin.y + kThumb)))
        {
            DrawPickerPreviewTooltip(current, info, options.kind, options);
        }
        else if (!popupOpen && hovered && !current.empty() &&
            !ImGui::IsMouseHoveringRect(thumbMin, ImVec2(thumbMin.x + kThumb, thumbMin.y + kThumb)))
        {
            Tooltip(current.c_str());
        }

        // ---- the drop-down: search, tiles or list, a footer
        bool changed = false;
        const float popupWidth = std::max(262.0f, width);
        // Inside the viewport: shifted left at the right edge, below the picker or above it where there is more room,
        // with a list that shrinks to what fits
        float listHeight = 300.0f;
        {
            constexpr float kChrome = 8.0f + 24.0f + 4.0f + 2.0f + 24.0f + 8.0f; // padding, search, gaps, footer
            const ImGuiViewport* viewport = ImGui::GetWindowViewport();
            const float rightLimit = viewport->WorkPos.x + viewport->WorkSize.x - 4.0f;
            const float x = std::max(std::min(min.x, rightLimit - popupWidth), viewport->WorkPos.x + 4.0f);
            const float spaceBelow = viewport->WorkPos.y + viewport->WorkSize.y - (max.y + 2.0f) - 6.0f;
            const float spaceAbove = min.y - 2.0f - viewport->WorkPos.y - 6.0f;
            const bool below = spaceBelow >= kChrome + 180.0f || spaceBelow >= spaceAbove;
            const float space = below ? spaceBelow : spaceAbove;
            listHeight = std::clamp(space - kChrome, 90.0f, 300.0f);
            if (below)
            {
                ImGui::SetNextWindowPos(ImVec2(x, max.y + 2.0f));
            }
            else
            {
                ImGui::SetNextWindowPos(ImVec2(x, min.y - 2.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
            }
        }
        ImGui::SetNextWindowSizeConstraints(ImVec2(popupWidth, 0.0f), ImVec2(popupWidth, FLT_MAX));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, style::kRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        const bool popupVisible = ImGui::BeginPopup(popupId);
        ImGui::PopStyleVar(3);
        if (popupVisible)
        {
            if (ImGui::IsWindowAppearing())
            {
                g_pickerSearch.clear();
                ImGui::SetKeyboardFocusHere();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                ImGui::CloseCurrentPopup(); // also while the search field is active
            }
            const std::string hint = std::string("Search ") + info.plural;
            SearchField("##picker_search", &g_pickerSearch, hint.c_str());
            ImGui::Dummy(ImVec2(0.0f, 4.0f));

            const std::string needle = LowerCopy(g_pickerSearch);
            std::vector<const std::string*> matches;
            matches.reserve(keys.size());
            for (const std::string& key : keys)
            {
                if (needle.empty() || LowerCopy(PickerName(key)).find(needle) != std::string::npos)
                {
                    matches.push_back(&key);
                }
            }

            const auto isCurrent = [&](const std::string& key)
            {
                return options.samePath ? options.samePath(key, current) : key == current;
            };

            constexpr float kGap = 6.0f;
            constexpr float kTileImage = 76.0f;
            constexpr float kNameHeight = 20.0f;
            const float listWidth = popupWidth - 16.0f;
            if (ImGui::BeginChild("##picker_items", ImVec2(listWidth, listHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
            {
                ImDrawList* childDraw = ImGui::GetWindowDrawList();
                if (matches.empty())
                {
                    ImGui::Dummy(ImVec2(0.0f, 8.0f));
                    PushFontRole(FontRole::Secondary);
                    ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextDim));
                    ImGui::TextUnformatted(needle.empty() ? "Nothing to pick." : "No match.");
                    ImGui::PopStyleColor();
                    PopFontRole();
                }
                else if (!g_pickerListView)
                {
                    const float scrollbar = ImGui::GetCurrentWindow()->ScrollbarY ? ImGui::GetStyle().ScrollbarSize : 0.0f;
                    const float cellWidth = std::floor((listWidth - scrollbar - 2.0f * kGap) / 3.0f);
                    const float cellHeight = kTileImage + kNameHeight + 8.0f;
                    for (std::size_t index = 0; index < matches.size(); ++index)
                    {
                        const std::string& key = *matches[index];
                        const int column = static_cast<int>(index % 3);
                        if (column == 0)
                        {
                            if (index > 0)
                            {
                                ImGui::Dummy(ImVec2(0.0f, kGap));
                            }
                        }
                        else
                        {
                            ImGui::SameLine(0.0f, kGap);
                        }

                        ImGui::PushID(static_cast<int>(index));
                        const ImVec2 cellMin = ImGui::GetCursorScreenPos();
                        const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(cellWidth, cellHeight));
                        const bool cellHovered = ImGui::IsItemHovered();
                        const ImVec2 cellMax(cellMin.x + cellWidth, cellMin.y + cellHeight);
                        const bool selected = isCurrent(key);
                        if (selected)
                        {
                            childDraw->AddRectFilled(cellMin, cellMax, IM_COL32(0x23, 0x35, 0x4D, 255), style::kRounding);
                            childDraw->AddRect(cellMin, cellMax, style::kPrimary, style::kRounding, 0, 2.0f);
                        }
                        else if (cellHovered)
                        {
                            childDraw->AddRectFilled(cellMin, cellMax, style::kControl, style::kRounding);
                        }
                        DrawPickerImage(
                            childDraw,
                            ImVec2(cellMin.x + (cellWidth - kTileImage) * 0.5f, cellMin.y + 4.0f),
                            kTileImage,
                            info,
                            options.kind,
                            key,
                            options.thumbnails,
                            128);

                        float sizePx = 0.0f;
                        ImFont* font = RoleFont(FontRole::Tiny, sizePx);
                        std::string name = PickerName(key);
                        while (name.size() > 3 && font->CalcTextSizeA(sizePx, FLT_MAX, 0.0f, name.c_str()).x > cellWidth - 8.0f)
                        {
                            name.erase(name.size() - 1);
                            if (font->CalcTextSizeA(sizePx, FLT_MAX, 0.0f, (name + "..").c_str()).x <= cellWidth - 8.0f)
                            {
                                name += "..";
                                break;
                            }
                        }
                        const float textWidth = std::min(font->CalcTextSizeA(sizePx, FLT_MAX, 0.0f, name.c_str()).x, cellWidth - 8.0f);
                        childDraw->PushClipRect(ImVec2(cellMin.x + 4.0f, cellMin.y), ImVec2(cellMax.x - 4.0f, cellMax.y), true);
                        childDraw->AddText(
                            font, sizePx,
                            ImVec2(std::floor(cellMin.x + (cellWidth - textWidth) * 0.5f), cellMin.y + 4.0f + kTileImage + 4.0f),
                            style::kTextStrong, name.c_str());
                        childDraw->PopClipRect();
                        if (cellHovered)
                        {
                            Tooltip(key.c_str());
                        }
                        if (clicked)
                        {
                            chosen = key;
                            changed = true;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::PopID();
                    }
                }
                else
                {
                    for (std::size_t index = 0; index < matches.size(); ++index)
                    {
                        const std::string& key = *matches[index];
                        ImGui::PushID(static_cast<int>(index));
                        const float rowWidth = ImGui::GetContentRegionAvail().x;
                        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                        const bool clicked = ImGui::InvisibleButton("##row", ImVec2(rowWidth, style::kPropRowHeight));
                        const bool rowHovered = ImGui::IsItemHovered();
                        const ImVec2 rowMax(rowMin.x + rowWidth, rowMin.y + style::kPropRowHeight);
                        if (isCurrent(key))
                        {
                            childDraw->AddRectFilled(rowMin, rowMax, IM_COL32(0x23, 0x35, 0x4D, 255), style::kRounding);
                        }
                        else if (rowHovered)
                        {
                            childDraw->AddRectFilled(rowMin, rowMax, style::kControl, style::kRounding);
                        }
                        childDraw->AddRectFilled(ImVec2(rowMin.x, rowMin.y + 3.0f), ImVec2(rowMin.x + 3.0f, rowMax.y - 3.0f), info.color);
                        DrawPickerImage(childDraw, ImVec2(rowMin.x + 8.0f, rowMin.y + 3.0f), 20.0f, info, options.kind, key, options.thumbnails, 64);
                        float sizePx = 0.0f;
                        ImFont* font = RoleFont(FontRole::Body, sizePx);
                        childDraw->PushClipRect(ImVec2(rowMin.x + 36.0f, rowMin.y), ImVec2(rowMax.x - 4.0f, rowMax.y), true);
                        childDraw->AddText(font, sizePx, ImVec2(rowMin.x + 36.0f, std::floor(rowMin.y + (style::kPropRowHeight - sizePx) * 0.5f - 0.5f)),
                            style::kTextStrong, PickerName(key).c_str());
                        childDraw->PopClipRect();
                        if (rowHovered)
                        {
                            Tooltip(key.c_str());
                        }
                        if (clicked)
                        {
                            chosen = key;
                            changed = true;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();

            // Footer: how many, and Tiles | List
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            const ImVec2 footerMin = ImGui::GetCursorScreenPos();
            {
                const std::string count = std::to_string(matches.size()) + " " + info.plural;
                PushFontRole(FontRole::Tiny);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(footerMin.x + 2.0f, footerMin.y + (style::kFrameHeight - ImGui::GetFontSize()) * 0.5f), style::kTextDim, count.c_str());
                PopFontRole();

                static const char* const kViews[] = {"Tiles", "List"};
                ImGui::SetCursorScreenPos(ImVec2(footerMin.x + listWidth - 112.0f, footerMin.y));
                const int next = Segmented("##picker_view", kViews, 2, g_pickerListView ? 1 : 0, 56.0f);
                g_pickerListView = next == 1;
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
        return changed;
    }

    // ---- chips, banners, empty states ----

    float Chip(const char* text, const ChipKind kind, const bool dot)
    {
        const SemanticColors colors = ChipColors(kind);
        PushFontRole(FontRole::Tiny);
        const ImVec2 textSize = ImGui::CalcTextSize(text);
        const float dotSpace = dot ? 12.0f : 0.0f;
        const float width = textSize.x + 16.0f + dotSpace;
        const float height = 18.0f;
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + width, min.y + height);
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, style::WithAlpha(colors.base, 0.16f), 9.0f);
        if (dot)
        {
            drawList->AddCircleFilled(ImVec2(min.x + 11.0f, (min.y + max.y) * 0.5f), 3.0f, colors.text);
        }
        drawList->AddText(ImVec2(min.x + 8.0f + dotSpace, std::floor(min.y + (height - textSize.y) * 0.5f)), colors.text, text);
        PopFontRole();
        ImGui::Dummy(ImVec2(width, height));
        return width;
    }

    void Banner(const BannerKind kind, const char* text, const char* icon)
    {
        ImU32 base = style::kLink;
        const char* defaultIcon = ICON_INFO;
        switch (kind)
        {
            case BannerKind::Play: base = style::kPlay; defaultIcon = ICON_PLAY; break;
            case BannerKind::Warning: base = style::kWarning; defaultIcon = ICON_TRIANGLE_ALERT; break;
            case BannerKind::Error: base = style::kError; defaultIcon = ICON_CIRCLE_X; break;
            case BannerKind::Info: break;
        }

        const float available = std::max(ImGui::GetContentRegionAvail().x, 60.0f);
        const float wrapWidth = std::max(available - 10.0f - 14.0f - 8.0f - 10.0f, 20.0f);
        PushFontRole(FontRole::Secondary);
        const ImVec2 textSize = ImGui::CalcTextSize(text, nullptr, false, wrapWidth);
        PopFontRole();
        const float height = std::max(textSize.y, 14.0f) + 14.0f;
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + available, min.y + height);

        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, style::WithAlpha(base, 0.14f), style::kRounding);
        drawList->AddRectFilled(min, ImVec2(min.x + 2.0f, max.y), base, style::kRounding, ImDrawFlags_RoundCornersLeft);
        DrawIcon(drawList, IconSize::Row14, icon != nullptr ? icon : defaultIcon, ImVec2(min.x + 10.0f + 9.0f, min.y + 7.0f + 7.0f), base);
        float sizePx = 0.0f;
        ImFont* font = RoleFont(FontRole::Secondary, sizePx);
        drawList->AddText(font, sizePx, ImVec2(min.x + 10.0f + 22.0f, min.y + 7.0f), style::kText, text, nullptr, wrapWidth);
        ImGui::Dummy(ImVec2(available, height));
    }

    bool EmptyState(const char* icon, const char* title, const char* text, const char* linkLabel)
    {
        const ImVec2 region = ImGui::GetContentRegionAvail();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float wrapWidth = std::min(280.0f, std::max(region.x - 24.0f, 60.0f));

        const bool hasTitle = title != nullptr && title[0] != '\0';
        float totalHeight = 30.0f + 10.0f;
        float titleHeight = 0.0f;
        if (hasTitle)
        {
            PushFontRole(FontRole::Body);
            titleHeight = ImGui::CalcTextSize(title).y;
            PopFontRole();
            totalHeight += titleHeight + 4.0f;
        }
        PushFontRole(FontRole::Secondary);
        const ImVec2 textSize = ImGui::CalcTextSize(text, nullptr, false, wrapWidth);
        const float linkWidth = linkLabel != nullptr ? ImGui::CalcTextSize(linkLabel).x : 0.0f;
        PopFontRole();
        totalHeight += textSize.y;
        if (linkLabel != nullptr)
        {
            totalHeight += 8.0f + 18.0f;
        }

        const float offsetY = std::max((region.y - totalHeight) * 0.5f, 8.0f);
        const float centerX = origin.x + region.x * 0.5f;
        float y = origin.y + offsetY;
        auto* drawList = ImGui::GetWindowDrawList();

        DrawIcon(drawList, IconSize::Empty30, icon, ImVec2(centerX, y + 15.0f), style::kEmptyStateIcon);
        y += 40.0f;
        if (hasTitle)
        {
            float sizePx = 0.0f;
            ImFont* font = RoleFont(FontRole::Body, sizePx);
            PushFontRole(FontRole::Body);
            const float titleWidth = ImGui::CalcTextSize(title).x;
            PopFontRole();
            drawList->AddText(font, sizePx, ImVec2(std::floor(centerX - titleWidth * 0.5f), y), style::kText, title);
            y += titleHeight + 4.0f;
        }

        {
            float sizePx = 0.0f;
            ImFont* font = RoleFont(FontRole::Secondary, sizePx);
            // centre each wrapped line: split manually by wrapping width
            const ImVec2 textOrigin(centerX - wrapWidth * 0.5f, y);
            PushFontRole(FontRole::Secondary);
            const char* cursor = text;
            const char* textEnd = text + std::strlen(text);
            float lineY = y;
            while (cursor < textEnd)
            {
                const char* wrapEnd = font->CalcWordWrapPosition(sizePx, cursor, textEnd, wrapWidth);
                if (wrapEnd == cursor)
                {
                    wrapEnd = cursor + 1;
                }
                const char* lineEnd = wrapEnd;
                while (lineEnd > cursor && (lineEnd[-1] == ' ' || lineEnd[-1] == '\n'))
                {
                    --lineEnd;
                }
                const float lineWidth = ImGui::CalcTextSize(cursor, lineEnd).x;
                drawList->AddText(font, sizePx, ImVec2(std::floor(centerX - lineWidth * 0.5f), lineY), style::kTextDim, cursor, lineEnd);
                lineY += ImGui::GetTextLineHeight();
                cursor = wrapEnd;
                while (cursor < textEnd && (*cursor == ' ' || *cursor == '\n'))
                {
                    ++cursor;
                }
            }
            PopFontRole();
            (void)textOrigin;
            y += textSize.y;
        }

        bool linkClicked = false;
        if (linkLabel != nullptr)
        {
            y += 8.0f;
            ImGui::SetCursorScreenPos(ImVec2(centerX - linkWidth * 0.5f, y));
            if (ImGui::InvisibleButton("##emptyLink", ImVec2(linkWidth, 18.0f)))
            {
                linkClicked = true;
            }
            const bool hovered = ImGui::IsItemHovered();
            float sizePx = 0.0f;
            ImFont* font = RoleFont(FontRole::Secondary, sizePx);
            const ImVec2 position(centerX - linkWidth * 0.5f, y);
            drawList->AddText(font, sizePx, position, hovered ? style::kPrimaryHover : style::kLink, linkLabel);
            if (hovered)
            {
                drawList->AddLine(ImVec2(position.x, position.y + sizePx + 1.0f), ImVec2(position.x + linkWidth, position.y + sizePx + 1.0f), style::kPrimaryHover, 1.0f);
            }
        }

        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(region.x, std::min(offsetY + totalHeight + 4.0f, std::max(region.y, 1.0f))));
        return linkClicked;
    }

    // ---- selection colours ----

    void PushSelectionColors(const bool panelFocused)
    {
        ImGui::PushStyleColor(ImGuiCol_Header, Vec4(panelFocused ? style::kPrimary : style::kSelectUnfocused));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Vec4(panelFocused ? style::kPrimaryHover : style::kSelectUnfocused));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, Vec4(panelFocused ? style::kPrimaryActive : style::kSelectUnfocused));
    }

    void PopSelectionColors()
    {
        ImGui::PopStyleColor(3);
    }

    // ---- dialogs ----

    bool BeginDialog(const char* popupName, const char* title, const char* icon, const float width, bool* open)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, style::kRoundingDialog);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Vec4(style::kPanel));
        ImGui::PushStyleColor(ImGuiCol_Border, Vec4(style::kBorderLight));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
        const bool visible = ImGui::BeginPopupModal(popupName, open, flags);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        if (!visible)
        {
            return false;
        }

        g_dialogWidth = width;
        const ImVec2 position = ImGui::GetWindowPos();
        const float headerHeight = 38.0f;
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(position, ImVec2(position.x + width, position.y + headerHeight), style::kRecessed, style::kRoundingDialog, ImDrawFlags_RoundCornersTop);
        drawList->AddLine(ImVec2(position.x, position.y + headerHeight), ImVec2(position.x + width, position.y + headerHeight), style::kBorderLight, 1.0f);
        float textX = position.x + 16.0f;
        if (icon != nullptr)
        {
            DrawIcon(drawList, IconSize::Row14, icon, ImVec2(textX + 7.5f, position.y + headerHeight * 0.5f), style::kTextDim);
            textX += 24.0f;
        }
        float sizePx = 0.0f;
        ImFont* strong = RoleFont(FontRole::Strong, sizePx);
        drawList->AddText(strong, sizePx, ImVec2(textX, std::floor(position.y + headerHeight * 0.5f - sizePx * 0.5f - 0.5f)), style::kTextStrong, title);

        if (open != nullptr)
        {
            ImGui::SetCursorScreenPos(ImVec2(position.x + width - 34.0f, position.y + 6.0f));
            if (IconButton("##dialogClose", ICON_X, "Close", false, true, 0, 26.0f, nullptr, IconSize::Row14))
            {
                *open = false;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(position.x + 16.0f, position.y + headerHeight + 14.0f));
        ImGui::Indent(16.0f);
        ImGui::PushItemWidth(width - 32.0f);
        return true;
    }

    void DialogFooter()
    {
        ImGui::PopItemWidth();
        ImGui::Unindent(16.0f);
        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        const ImVec2 position = ImGui::GetWindowPos();
        const float y = ImGui::GetCursorScreenPos().y;
        const float footerHeight = 12.0f + style::kFrameHeight + 12.0f;
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(ImVec2(position.x, y), ImVec2(position.x + g_dialogWidth, y + footerHeight), IM_COL32(0x20, 0x20, 0x20, 255),
                                style::kRoundingDialog, ImDrawFlags_RoundCornersBottom);
        drawList->AddLine(ImVec2(position.x, y), ImVec2(position.x + g_dialogWidth, y), style::kBorderLight, 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(position.x + 16.0f, y + 12.0f));
    }

    void DialogAlignRight(const float buttonsWidth)
    {
        ImGui::SetCursorPosX(std::max(g_dialogWidth - 16.0f - buttonsWidth, 16.0f));
    }

    void EndDialog()
    {
        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        ImGui::EndPopup();
    }

    // ---- tooltips ----

    void Tooltip(const char* text, const char* shortcut, const char* detail)
    {
        if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay | ImGuiHoveredFlags_AllowWhenDisabled))
        {
            return;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, style::kRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Vec4(style::kRecessed));
        ImGui::PushStyleColor(ImGuiCol_Border, Vec4(style::kBorderLight));
        if (ImGui::BeginTooltip())
        {
            PushFontRole(FontRole::Secondary);
            ImGui::PushStyleColor(ImGuiCol_Text, Vec4(style::kTextStrong));
            ImGui::TextUnformatted(text);
            ImGui::PopStyleColor();
            if (shortcut != nullptr && shortcut[0] != '\0')
            {
                ImGui::SameLine(0.0f, 14.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, Vec4(style::kTextDim));
                ImGui::TextUnformatted(shortcut);
                ImGui::PopStyleColor();
            }
            PopFontRole();
            if (detail != nullptr && detail[0] != '\0')
            {
                PushFontRole(FontRole::Tiny);
                ImGui::PushStyleColor(ImGuiCol_Text, Vec4(style::kTextDim));
                ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 320.0f);
                ImGui::TextUnformatted(detail);
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                PopFontRole();
            }
            ImGui::EndTooltip();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    }

    // ---- menus (D2) ----

    bool MenuItemIcon(const char* icon, const char* label, const char* shortcut, const bool checked, const bool enabled,
                      const bool showCheck, const float width)
    {
        const float height = 26.0f;
        const std::string id = std::string("##mi_") + label;
        PushFontRole(FontRole::Body);
        ImGui::PushStyleColor(ImGuiCol_Text, Vec4(enabled ? style::kText : style::kTextDisabled));
        const bool clicked = ImGui::Selectable(id.c_str(), false, enabled ? 0 : ImGuiSelectableFlags_Disabled, ImVec2(width, height));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopStyleColor();

        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        auto* drawList = ImGui::GetWindowDrawList();
        const float centerY = (min.y + max.y) * 0.5f;
        float x = min.x + 10.0f;
        if (showCheck)
        {
            if (checked)
            {
                DrawIcon(drawList, IconSize::Row14, ICON_CHECK, ImVec2(x + 7.0f, centerY), enabled ? style::kText : style::kTextDisabled);
            }
            x += 22.0f;
        }
        if (icon != nullptr && icon[0] != '\0')
        {
            DrawIcon(drawList, IconSize::Row14, icon, ImVec2(x + 7.0f, centerY), enabled ? style::kTextDim : style::kTextDisabled);
        }
        x += 24.0f;

        float sizePx = 0.0f;
        ImFont* font = RoleFont(FontRole::Body, sizePx);
        const ImU32 textColor = !enabled ? style::kTextDisabled : (hovered ? style::kTextStrong : style::kText);
        drawList->AddText(font, sizePx, ImVec2(x, std::floor(centerY - sizePx * 0.5f - 0.5f)), textColor, label);
        if (shortcut != nullptr && shortcut[0] != '\0')
        {
            float smallPx = 0.0f;
            ImFont* small = RoleFont(FontRole::Secondary, smallPx);
            const float shortcutWidth = TextWidth(FontRole::Secondary, shortcut);
            drawList->AddText(small, smallPx, ImVec2(max.x - 12.0f - shortcutWidth, std::floor(centerY - smallPx * 0.5f - 0.5f)),
                              enabled ? style::kTextDim : style::kTextDisabled, shortcut);
        }
        PopFontRole();
        return clicked && enabled;
    }

    void MenuSection(const char* title)
    {
        PushFontRole(FontRole::Tiny);
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        const ImVec2 position = ImGui::GetCursorScreenPos();
        float sizePx = 0.0f;
        ImFont* font = RoleFont(FontRole::Tiny, sizePx);
        ImGui::GetWindowDrawList()->AddText(font, sizePx, ImVec2(position.x + 10.0f, position.y), style::kTextDim, title);
        ImGui::Dummy(ImVec2(0.0f, sizePx + 4.0f));
        PopFontRole();
    }

    bool BeginMenuPopup(const char* id)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, style::kRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        const bool open = ImGui::BeginPopup(id);
        ImGui::PopStyleVar(3);
        return open;
    }

    // ---- developer: widgets gallery ----

    void DrawWidgetsGallery(bool* open)
    {
        if (open != nullptr && !*open)
        {
            return;
        }

        static bool checkA = true;
        static bool checkB = false;
        static int segment = 0;
        static int shape = 0;
        static std::string search;
        static float vec[3] = {1.0f, 0.5f, -2.0f};
        static bool showDialog = false;
        static int clicks = 0;

        ImGui::SetNextWindowSize(ImVec2(560.0f, 640.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Widgets Gallery###WidgetsGallery", open))
        {
            ImGui::TextDisabled("Developer tool: every editor helper in every state. clicks: %d", clicks);
            ImGui::Spacing();

            ImGui::SeparatorText("BUTTONS");
            if (Button("Secondary", ICON_REFRESH_CW)) { ++clicks; }
            ImGui::SameLine();
            if (PrimaryButton("Primary", ICON_SAVE)) { ++clicks; }
            ImGui::SameLine();
            Button("Disabled", nullptr, false);
            ImGui::SameLine();
            if (DestructiveButton("Delete", ICON_TRASH_2)) { ++clicks; }

            ImGui::SeparatorText("ICON BUTTONS");
            IconButton("##i1", ICON_PLAY, "Play", false, true, style::kPlay, style::kToolButton, "F5", IconSize::Toolbar18);
            ImGui::SameLine();
            IconButton("##i2", ICON_SQUARE, "Stop", false, false, style::kStop, style::kToolButton, nullptr, IconSize::Toolbar18);
            ImGui::SameLine();
            IconButton("##i3", ICON_MOVE, "Translate", true, true, 0, style::kViewportButton, "W");
            ImGui::SameLine();
            IconButton("##i4", ICON_ROTATE_CW, "Rotate", false, true, 0, style::kViewportButton, "E");
            ImGui::SameLine();
            IconButton("##i5", ICON_SETTINGS, "Settings", false, true, 0, style::kPanelIconButton);
            ImGui::SameLine();
            ToolbarSeparator();
            ImGui::SameLine();
            const SplitButtonPart reload = SplitButton("##split", ICON_REFRESH_CW, "Reload Scripts", true, 0, false, "Reload Scripts", "F5");
            if (reload != SplitButtonPart::None) { ++clicks; }
            ImGui::SameLine();
            const SplitButtonPart add = SplitButton("##add", ICON_SQUARE_PLUS, "Add", true, 0, true, "Add");
            if (add != SplitButtonPart::None) { ++clicks; }

            ImGui::SeparatorText("TOGGLES AND FIELDS");
            static const char* kSegments[] = {"Console", "Errors (3)"};
            segment = Segmented("##seg", kSegments, 2, segment);
            ImGui::SameLine();
            static const char* kShapes[] = {"Sphere", "Cube"};
            shape = Segmented("##shape", kShapes, 2, shape);
            Checkbox("Keep script state on reload", &checkA);
            Checkbox("Show colliders", &checkB);
            Checkbox("Disabled checkbox", &checkA, false);
            SearchField("##search", &search, "Search models", 260.0f);

            ImGui::SeparatorText("CATEGORY AND PROPERTY GRID");
            if (BeginCategory("Transform", true, nullptr))
            {
                if (BeginPropertyGrid("##grid"))
                {
                    PropertyLabel("Position");
                    DragVector3("##pos", vec);
                    PropertyLabel("spawn_interval_with_a_long_name", true);
                    static float interval = 1.5f;
                    ImGui::DragFloat("##interval", &interval, 0.05f);
                    PropertyReset();
                    PropertyLabel("Mesh", false, style::kPickerRowHeight);
                    {
                        static std::string mesh = "assets/models/crate.obj";
                        const std::vector<std::string> meshes = {"assets/models/crate.obj", "assets/models/sphere.obj", "assets/models/pyramid.obj"};
                        AssetPickerOptions options;
                        options.kind = PickerKind::Mesh;
                        std::string chosen;
                        if (AssetPicker("##mesh", mesh, meshes, options, chosen))
                        {
                            mesh = chosen;
                        }
                    }
                    EndPropertyGrid();
                }
                EndCategory();
            }
            if (BeginCategory("Collider", false, "Box"))
            {
                EndCategory();
            }

            ImGui::SeparatorText("CHIPS AND BANNERS");
            Chip("Applied on Play", ChipKind::Gray);
            ImGui::SameLine();
            Chip("Active", ChipKind::Green, true);
            ImGui::SameLine();
            Chip("Faulted", ChipKind::Red, true);
            ImGui::SameLine();
            Chip("Unsaved", ChipKind::Amber, true);
            ImGui::SameLine();
            Chip("Current", ChipKind::Blue);
            Banner(BannerKind::Play, "Play mode: changes are not saved and fields are read-only.");
            Banner(BannerKind::Warning, "Claude Code CLI was not found in PATH. Set the path in the assistant settings.");
            Banner(BannerKind::Error, "Failed to read the prefab: unexpected token at line 12.");

            ImGui::SeparatorText("SELECTION AND DIALOG");
            PushSelectionColors(true);
            ImGui::Selectable(ICON_BOX "  Controlled_1", true);
            PopSelectionColors();
            PushSelectionColors(false);
            ImGui::Selectable(ICON_FILE_CODE "  EnemySpawner", true);
            PopSelectionColors();
            if (Button("Open dialog", ICON_FOLDER_OPEN))
            {
                showDialog = true;
                ImGui::OpenPopup("GalleryDialog");
            }

            ImGui::SeparatorText("EMPTY STATE");
            ImGui::BeginChild("##empty", ImVec2(0.0f, 130.0f), ImGuiChildFlags_None);
            EmptyState(ICON_SLIDERS_HORIZONTAL, "Nothing selected", "Select an entity in the Outliner or click it in the Viewport.", "Clear search");
            ImGui::EndChild();
        }

        if (BeginDialog("GalleryDialog", "Open Map", ICON_MAP, 440.0f, &showDialog))
        {
            ImGui::TextUnformatted("Dialog body text goes here.");
            ImGui::Dummy(ImVec2(0.0f, 24.0f));
            DialogFooter();
            DialogAlignRight(2 * 90.0f + 8.0f);
            if (PrimaryButton("Open", nullptr, true, 90.0f))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (Button("Cancel", nullptr, true, 90.0f))
            {
                ImGui::CloseCurrentPopup();
            }
            EndDialog();
        }
        ImGui::End();
    }

    // ---- combo (D1b) ----

    bool BeginCombo(const char* label, const char* preview, const ImGuiComboFlags flags)
    {
        const float width = std::max(ImGui::CalcItemWidth(), 40.0f);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + width, min.y + ImGui::GetFrameHeight());
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        ImGui::PushStyleColor(ImGuiCol_FrameBg, Vec4(style::kControl));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Vec4(style::kControlHover));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Vec4(style::kControlActive));
        const bool open = ImGui::BeginCombo(label, preview, flags | ImGuiComboFlags_NoArrowButton);
        ImGui::PopStyleColor(3);

        // The same chevron as the asset pickers instead of ImGui's arrow button
        DrawIcon(drawList, IconSize::Chevron12, ICON_CHEVRON_DOWN, ImVec2(max.x - 14.0f, (min.y + max.y) * 0.5f), style::kTextDim);
        return open;
    }

    void EndCombo()
    {
        ImGui::EndCombo();
    }
}
