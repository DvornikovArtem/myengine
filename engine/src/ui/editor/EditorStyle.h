#pragma once

// Design tokens of the editor UI (spec section 2: colours, sizes). Panels use these constants instead of literals.
// Internal to engine/src/ui/editor.

#include <imgui/imgui.h>

namespace myengine::ui::style
{
    // ---- surfaces (dark to light) ----
    inline constexpr ImU32 kInput = IM_COL32(0x0F, 0x0F, 0x0F, 255);          // input fields, dock separators
    inline constexpr ImU32 kTitle = IM_COL32(0x15, 0x15, 0x15, 255);          // menu bar, toolbars, tab strips, status bar
    inline constexpr ImU32 kRecessed = IM_COL32(0x1A, 0x1A, 0x1A, 255);       // wells: trees, logs, popups, tooltips
    inline constexpr ImU32 kPanel = IM_COL32(0x24, 0x24, 0x24, 255);          // panel body, active tab, tile cards
    inline constexpr ImU32 kHeader = IM_COL32(0x2F, 0x2F, 0x2F, 255);         // category headers, column header, row hover
    inline constexpr ImU32 kControl = IM_COL32(0x38, 0x38, 0x38, 255);        // secondary buttons, combos, menu hover
    inline constexpr ImU32 kControlHover = IM_COL32(0x48, 0x48, 0x48, 255);
    inline constexpr ImU32 kControlActive = IM_COL32(0x57, 0x57, 0x57, 255);
    inline constexpr ImU32 kControlDisabled = IM_COL32(0x2B, 0x2B, 0x2B, 255);
    inline constexpr ImU32 kBorderLight = IM_COL32(0x38, 0x38, 0x38, 255);    // popups, dialogs, cards
    inline constexpr ImU32 kViewportBackdrop = IM_COL32(0x12, 0x12, 0x12, 255); // tile preview area

    // ---- text ----
    inline constexpr ImU32 kText = IM_COL32(0xC0, 0xC0, 0xC0, 255);
    inline constexpr ImU32 kTextStrong = IM_COL32(0xFF, 0xFF, 0xFF, 255);
    inline constexpr ImU32 kTextDim = IM_COL32(0x80, 0x80, 0x80, 255);
    inline constexpr ImU32 kTextDisabled = IM_COL32(0x5A, 0x5A, 0x5A, 255);
    inline constexpr ImU32 kLink = IM_COL32(0x4E, 0xA3, 0xFF, 255);

    // ---- accent and state ----
    inline constexpr ImU32 kPrimary = IM_COL32(0x00, 0x70, 0xE0, 255);        // selection (focused), active tool, primary button
    inline constexpr ImU32 kPrimaryHover = IM_COL32(0x1A, 0x85, 0xFF, 255);
    inline constexpr ImU32 kPrimaryActive = IM_COL32(0x00, 0x5B, 0xB8, 255);
    inline constexpr ImU32 kSelectUnfocused = IM_COL32(0x2A, 0x44, 0x66, 255);
    inline constexpr ImU32 kSelectTileFill = IM_COL32(0x23, 0x35, 0x4D, 255);
    inline constexpr ImU32 kPlay = IM_COL32(0x4C, 0xAF, 0x50, 255);
    inline constexpr ImU32 kPlayDisabled = IM_COL32(0x2C, 0x5A, 0x2E, 255);
    inline constexpr ImU32 kEditIndicator = IM_COL32(0x6B, 0x6B, 0x6B, 255);  // status dot in Edit
    inline constexpr ImU32 kStop = IM_COL32(0xE5, 0x48, 0x4D, 255);
    inline constexpr ImU32 kWarning = IM_COL32(0xFF, 0xB8, 0x00, 255);
    inline constexpr ImU32 kError = IM_COL32(0xE5, 0x48, 0x4D, 255);
    inline constexpr ImU32 kErrorText = IM_COL32(0xFF, 0x7B, 0x7F, 255);
    inline constexpr ImU32 kDestructiveText = IM_COL32(0xFF, 0x8A, 0x8D, 255);
    inline constexpr ImU32 kDestructiveBorder = IM_COL32(0x5A, 0x2A, 0x2C, 255);
    inline constexpr ImU32 kReset = IM_COL32(0xD6, 0xA3, 0x3A, 255);          // "reset to the value from the code" arrow
    inline constexpr ImU32 kStatsText = IM_COL32(0x9B, 0xE2, 0x7A, 255);      // FPS/ms overlay in the viewport
    inline constexpr ImU32 kCheckBorder = IM_COL32(0x4A, 0x4A, 0x4A, 255);
    inline constexpr ImU32 kEmptyStateIcon = IM_COL32(0x4A, 0x4A, 0x4A, 255);

    // ---- axes ----
    inline constexpr ImU32 kAxisX = IM_COL32(0xD9, 0x4A, 0x4A, 255);
    inline constexpr ImU32 kAxisY = IM_COL32(0x6A, 0xAE, 0x3E, 255);
    inline constexpr ImU32 kAxisZ = IM_COL32(0x3F, 0x7F, 0xE0, 255);

    // ---- asset types (icon and 3 px stripe under a tile) ----
    inline constexpr ImU32 kTypeFolder = IM_COL32(0xC9, 0xA6, 0x6B, 255);
    inline constexpr ImU32 kTypeMesh = IM_COL32(0x3E, 0xC5, 0xC8, 255);
    inline constexpr ImU32 kTypeMaterial = IM_COL32(0x6C, 0xBF, 0x4A, 255);
    inline constexpr ImU32 kTypeTexture = IM_COL32(0xD9, 0x53, 0x4F, 255);
    inline constexpr ImU32 kTypePrefab = IM_COL32(0x3B, 0x8E, 0xEA, 255);
    inline constexpr ImU32 kTypeScene = IM_COL32(0xF0, 0x93, 0x2B, 255);
    inline constexpr ImU32 kTypeScript = IM_COL32(0xB5, 0x7E, 0xDC, 255);
    inline constexpr ImU32 kTypeShader = IM_COL32(0xE0, 0x7A, 0xB8, 255);
    inline constexpr ImU32 kTypeOther = IM_COL32(0x8A, 0x8A, 0x8A, 255);

    // ---- sizes (pixels at UI scale 1.0) ----
    inline constexpr float kMenuBarHeight = 28.0f;
    inline constexpr float kToolbarHeight = 44.0f;
    inline constexpr float kToolButton = 32.0f;            // main toolbar icon button (icon 18)
    inline constexpr float kTabHeight = 30.0f;
    inline constexpr float kPanelToolsHeight = 36.0f;      // tools row inside a panel
    inline constexpr float kFrameHeight = 24.0f;           // fields, combos, buttons in panels
    inline constexpr float kRowHeight = 24.0f;             // tree / list row
    inline constexpr float kPropRowHeight = 26.0f;         // property grid row
    inline constexpr float kCategoryHeight = 28.0f;
    inline constexpr float kStatusBarHeight = 28.0f;
    inline constexpr float kViewportButton = 26.0f;        // viewport toolbar button (icon 16)
    inline constexpr float kViewportInset = 8.0f;
    inline constexpr float kSplitter = 2.0f;
    inline constexpr float kIndent = 14.0f;
    inline constexpr float kPanelIconButton = 26.0f;       // icon button inside panels

    inline constexpr float kTileWidth = 104.0f;
    inline constexpr float kTilePreview = 92.0f;
    inline constexpr float kTileStripe = 3.0f;
    inline constexpr float kTileHeight = kTilePreview + kTileStripe + 52.0f;

    inline constexpr float kIcon = 14.0f;                  // rows, tabs, menus
    inline constexpr float kIconToolbar = 18.0f;
    inline constexpr float kIconSmall = 12.0f;             // chevrons

    // ---- font sizes ----
    // The em of the mockups (CSS px). UiManager bakes a font at size * lineHeight / em, because ImGui sizes by line height;
    // read the baked size of a role from FontRoleSize(), not from here.
    inline constexpr float kFontBody = 14.0f;
    inline constexpr float kFontStrong = 14.0f;
    inline constexpr float kFontSecondary = 13.0f;
    inline constexpr float kFontTiny = 12.0f;
    inline constexpr float kFontMono = 13.0f;

    // ---- rounding ----
    inline constexpr float kRounding = 4.0f;               // fields, buttons, tiles, popups, tooltips
    inline constexpr float kRoundingSmall = 3.0f;          // buttons inside a toolbar group
    inline constexpr float kRoundingDialog = 6.0f;         // dialogs, cards, message bubbles

    // ---- helpers ----
    inline ImU32 WithAlpha(const ImU32 color, const float alpha)
    {
        const ImU32 a = static_cast<ImU32>(static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha + 0.5f);
        return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }

    inline ImVec4 ToVec4(const ImU32 color)
    {
        return ImGui::ColorConvertU32ToFloat4(color);
    }
}
