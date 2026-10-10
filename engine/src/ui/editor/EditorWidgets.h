#pragma once

// Shared widgets of the editor UI (spec section 4). They only draw and report "pressed / changed";
// undo, snapshots and any other editor logic stay with the caller.
// Internal to engine/src/ui/editor.

#include <functional>
#include <string>

#include <imgui/imgui.h>

#include "EditorIcons.h"
#include "EditorStyle.h"

namespace myengine::ui
{
    // ---- fonts ----
    // Every size is baked into the atlas once at start (the renderer does not stream glyphs), so a role always uses
    // the size it was loaded with. Text roles have the Lucide icons merged in; icon-only fonts are for DrawIcon.
    enum class FontRole
    {
        Body,       // Inter Regular 14
        Strong,     // Inter SemiBold 14
        Secondary,  // Inter Regular 13
        Tiny,       // Inter Regular 12
        Mono,       // JetBrains Mono 13
    };

    enum class IconSize
    {
        Chevron12,
        Row14,
        Button16,
        Toolbar18,
        Empty30,
        Tile40,
        Folder58,
    };

    struct EditorFonts
    {
        ImFont* body = nullptr;
        ImFont* strong = nullptr;
        ImFont* secondary = nullptr;
        ImFont* tiny = nullptr;
        ImFont* mono = nullptr;
        // Size the mono font was baked at. ImGui sizes a font by its line height (ascender - descender), not by the
        // em square, so the 13 px of the spec (em) is baked as 13 * lineHeight / em. 0: use style::kFontMono.
        float monoSize = 0.0f;
        ImFont* icon12 = nullptr;
        ImFont* icon14 = nullptr;
        ImFont* icon16 = nullptr;
        ImFont* icon18 = nullptr;
        ImFont* icon30 = nullptr;
        ImFont* icon40 = nullptr;
        ImFont* icon58 = nullptr;
    };

    // Registers the fonts of the current ImGui context (called by UiManager right after the atlas is built)
    void SetEditorFonts(const EditorFonts& fonts);
    void ClearEditorFonts();
    void PushFontRole(FontRole role);
    void PopFontRole();

    // Draws one Lucide glyph centred on `center`, at the baked size
    void DrawIcon(ImDrawList* drawList, IconSize size, const char* icon, ImVec2 center, ImU32 color);

    // ---- buttons ----
    // Secondary 24 px button. Returns true when clicked (never while disabled).
    bool Button(const char* label, const char* icon = nullptr, bool enabled = true, float width = 0.0f);
    // Primary (blue) button; one per view
    bool PrimaryButton(const char* label, const char* icon = nullptr, bool enabled = true, float width = 0.0f);
    // Transparent button with red text and border (menus and dialogs only)
    bool DestructiveButton(const char* label, const char* icon = nullptr, bool enabled = true);
    // Square icon button: `size` is kToolButton (32) in the main toolbar, kPanelIconButton / kViewportButton in panels.
    // `tint` 0 = default text colour. Always shows `tooltip` (with an optional shortcut).
    bool IconButton(const char* id, const char* icon, const char* tooltip, bool selected = false, bool enabled = true,
                    ImU32 tint = 0, float size = style::kPanelIconButton, const char* shortcut = nullptr,
                    IconSize iconSize = IconSize::Button16);

    enum class SplitButtonPart
    {
        None,
        Main,     // the main part (icon + label) was pressed
        Chevron,  // the chevron zone was pressed
    };
    // One visual button: [icon label | chevron]. With `wholeOpensPopup` the chevron zone is part of the main one
    // and a click anywhere reports Chevron (the "+ Add" button).
    SplitButtonPart SplitButton(const char* id, const char* icon, const char* label, bool enabled = true,
                                ImU32 iconTint = 0, bool wholeOpensPopup = false, const char* tooltip = nullptr,
                                const char* shortcut = nullptr);
    void ToolbarSeparator();

    // ---- toggles and fields ----
    // Segmented control; returns the (possibly new) selected index
    int Segmented(const char* id, const char* const* labels, int count, int current, float segmentWidth = 0.0f);
    // 16x16 box, blue when on. Returns true when the value changed.
    bool Checkbox(const char* label, bool* value, bool enabled = true);
    // Search field with a magnifier icon and a hint. `width` <= 0 fills the available width.
    bool SearchField(const char* id, std::string* text, const char* hint, float width = 0.0f);
    // 1 px primary outline around the last item while it is active (text fields)
    void FocusOutline();

    // ---- categories and the property grid ----
    // Neutral full-width header with a chevron. Use like TreeNode: `if (BeginCategory(...)) { ...; EndCategory(); }`.
    bool BeginCategory(const char* title, bool defaultOpen = true, const char* rightText = nullptr, const char* icon = nullptr);
    void EndCategory();

    // Three columns: label (38% of the width, 110..200), value (stretch), reset (22).
    // `indent` is the left offset under the category chevron (20) or deeper for nested rows (32).
    bool BeginPropertyGrid(const char* id, float indent = 20.0f);
    // Starts a new row and draws the label in the first column; the value column is current afterwards
    void PropertyLabel(const char* text, bool dim = false);
    // Draws the reset arrow in the third column and returns true when it was clicked
    bool PropertyReset(const char* tooltip = "Reset to the value from the script code");
    void EndPropertyGrid();

    // Three drag fields X/Y/Z with coloured axis stripes. `afterField(axis)` is called right after each field is
    // submitted, so `IsItemActivated()` / `IsItemDeactivatedAfterEdit()` refer to that field (undo recording).
    // Returns true when any value changed.
    bool DragVector3(const char* id, float values[3], float speed = 0.05f, const char* format = "%.3f",
                     const std::function<void(int axis)>& afterField = {});

    // Combo-like picker with a type stripe, an icon and the item name; the full path is the tooltip.
    // Returns true while the popup is open: `if (BeginAssetPicker(...)) { items; EndAssetPicker(); }`
    bool BeginAssetPicker(const char* id, const char* label, const char* icon, ImU32 stripeColor, const char* tooltipPath = nullptr);
    void EndAssetPicker();

    // ---- chips, banners, empty states ----
    enum class ChipKind
    {
        Gray,
        Green,
        Red,
        Amber,
        Blue,
    };
    // Pill, 18 px high. `dot` draws a leading status dot. Returns the width used.
    float Chip(const char* text, ChipKind kind = ChipKind::Gray, bool dot = false);

    enum class BannerKind
    {
        Play,
        Warning,
        Error,
        Info,
    };
    // Full-width block with a coloured stripe on the left, an icon and wrapped text
    void Banner(BannerKind kind, const char* text, const char* icon = nullptr);

    // Centred icon, title, text and an optional link. Returns true when the link was clicked.
    bool EmptyState(const char* icon, const char* title, const char* text, const char* linkLabel = nullptr);

    // ---- selection colours ----
    // Blue (focused panel) or muted blue (unfocused) for Selectable / TreeNode rows drawn between the calls
    void PushSelectionColors(bool panelFocused);
    void PopSelectionColors();

    // ---- dialogs ----
    // Modal dialog frame: header with icon and title, body, footer band with buttons.
    // The caller does ImGui::OpenPopup(popupName) and:
    //   if (BeginDialog(name, "Title", ICON_FOLDER, 460)) { ...body...; DialogFooter(); buttons; EndDialog(); }
    bool BeginDialog(const char* popupName, const char* title, const char* icon = nullptr, float width = 460.0f, bool* open = nullptr);
    // Ends the body and starts the footer band; the cursor is inside it afterwards
    void DialogFooter();
    // Moves the cursor so that `buttonsWidth` of buttons ends at the right edge of the footer
    void DialogAlignRight(float buttonsWidth);
    void EndDialog();

    // ---- tooltips ----
    // Call right after an item: text with an optional shortcut (right, dim) and a second dim detail line
    void Tooltip(const char* text, const char* shortcut = nullptr, const char* detail = nullptr);

    // ---- menus (D2) ----
    // Menu entry with an icon column (dim), a check column when `showCheck`, and a shortcut at the right.
    // Returns true when clicked. `width` is the content width of the menu (all entries of one menu use the same).
    bool MenuItemIcon(const char* icon, const char* label, const char* shortcut = nullptr, bool checked = false,
                      bool enabled = true, bool showCheck = false, float width = 248.0f);
    // Small upper-case dim section title inside a menu
    void MenuSection(const char* title);
    // Popup with the editor menu look (4 px padding, light border); use like BeginPopup / EndPopup
    bool BeginMenuPopup(const char* id);

    // ---- combo ----
    // The editor's drop-down: like ImGui::BeginCombo, with the chevron icon of the asset pickers instead of the arrow
    // button. if (BeginCombo(...)) { Selectable(...); EndCombo(); }; the width comes from SetNextItemWidth.
    bool BeginCombo(const char* label, const char* preview, ImGuiComboFlags flags = 0);
    void EndCombo();

    // ---- developer ----
    // Help > Developer > Widgets gallery: every helper in every state (acceptance tool, off by default)
    void DrawWidgetsGallery(bool* open);
}
