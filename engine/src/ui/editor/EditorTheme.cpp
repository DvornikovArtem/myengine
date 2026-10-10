#include "EditorTheme.h"

#include "EditorStyle.h"

#include <imgui/imgui.h>

namespace myengine::ui::detail
{
    void ApplyEditorTheme()
    {
        using namespace style;

        ImGuiStyle& style = ImGui::GetStyle();

        // Metrics (spec 2.7)
        style.WindowPadding = ImVec2(8.0f, 8.0f);
        style.FramePadding = ImVec2(8.0f, 5.0f);
        style.ItemSpacing = ImVec2(6.0f, 4.0f);
        style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
        style.CellPadding = ImVec2(8.0f, 4.0f);
        style.IndentSpacing = kIndent;
        style.ScrollbarSize = 10.0f;
        style.GrabMinSize = 10.0f;

        style.WindowRounding = 0.0f;   // docked panels are square
        style.ChildRounding = 0.0f;
        style.PopupRounding = kRounding;
        style.FrameRounding = kRounding;
        style.TabRounding = kRounding;
        style.ScrollbarRounding = 5.0f;
        style.GrabRounding = 3.0f;

        style.WindowBorderSize = 0.0f;
        style.ChildBorderSize = 0.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.TabBorderSize = 0.0f;
        style.TabBarBorderSize = 0.0f;
        style.TabBarOverlineSize = 2.0f;
        style.DockingSeparatorSize = kSplitter;
        style.DisabledAlpha = 0.45f;
        style.WindowMenuButtonPosition = ImGuiDir_None;       // no window menu arrow on tabs
        style.TabCloseButtonMinWidthSelected = 0.0f;          // the close button shows on hover / on the active tab only
        style.TabCloseButtonMinWidthUnselected = 0.0f;
        style.DockingNodeHasCloseButton = false;              // no group close button at the right of the tab strip
        style.SeparatorTextBorderSize = 1.0f;
        style.SeparatorTextPadding = ImVec2(10.0f, 4.0f);

        // Colours (spec 2.7). The global Header colour is neutral: blue selection comes only from PushSelectionColors.
        ImVec4* c = style.Colors;
        c[ImGuiCol_Text] = ToVec4(kText);
        c[ImGuiCol_TextDisabled] = ToVec4(kTextDim);
        c[ImGuiCol_WindowBg] = ToVec4(kPanel);
        c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);   // children paint kRecessed themselves
        c[ImGuiCol_PopupBg] = ToVec4(kRecessed);
        c[ImGuiCol_Border] = ToVec4(kBorderLight);              // only popups, menus and tooltips draw a border (2.4)
        c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_FrameBg] = ToVec4(kInput);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0x16 / 255.0f, 0x16 / 255.0f, 0x16 / 255.0f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0x0B / 255.0f, 0x0B / 255.0f, 0x0B / 255.0f, 1.0f);
        c[ImGuiCol_TitleBg] = ToVec4(kTitle);
        c[ImGuiCol_TitleBgActive] = ToVec4(kTitle);
        c[ImGuiCol_TitleBgCollapsed] = ToVec4(kTitle);
        c[ImGuiCol_MenuBarBg] = ToVec4(kTitle);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0x3A / 255.0f, 0x3A / 255.0f, 0x3A / 255.0f, 1.0f);
        c[ImGuiCol_ScrollbarGrabHovered] = ToVec4(kControlActive);
        c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0x6A / 255.0f, 0x6A / 255.0f, 0x6A / 255.0f, 1.0f);
        c[ImGuiCol_CheckMark] = ToVec4(kTextStrong);
        c[ImGuiCol_SliderGrab] = ToVec4(kPrimary);
        c[ImGuiCol_SliderGrabActive] = ToVec4(kPrimaryHover);
        c[ImGuiCol_Button] = ToVec4(kControl);
        c[ImGuiCol_ButtonHovered] = ToVec4(kControlHover);
        c[ImGuiCol_ButtonActive] = ToVec4(kControlActive);
        c[ImGuiCol_Header] = ToVec4(kHeader);
        c[ImGuiCol_HeaderHovered] = ToVec4(kControl);
        c[ImGuiCol_HeaderActive] = ToVec4(kControlActive);
        c[ImGuiCol_Separator] = ToVec4(kInput);
        c[ImGuiCol_SeparatorHovered] = ToVec4(kPrimary);
        c[ImGuiCol_SeparatorActive] = ToVec4(kPrimary);
        c[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_ResizeGripHovered] = ToVec4(WithAlpha(kPrimary, 0.55f));
        c[ImGuiCol_ResizeGripActive] = ToVec4(kPrimary);
        c[ImGuiCol_Tab] = ToVec4(kTitle);
        c[ImGuiCol_TabHovered] = ImVec4(0x1E / 255.0f, 0x1E / 255.0f, 0x1E / 255.0f, 1.0f);
        c[ImGuiCol_TabSelected] = ToVec4(kPanel);
        c[ImGuiCol_TabSelectedOverline] = ToVec4(kPrimary);
        c[ImGuiCol_TabDimmed] = ToVec4(kTitle);
        c[ImGuiCol_TabDimmedSelected] = ToVec4(kPanel);
        c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_DockingPreview] = ToVec4(WithAlpha(kPrimary, 0.5f));
        c[ImGuiCol_DockingEmptyBg] = ToVec4(kTitle);
        c[ImGuiCol_TableHeaderBg] = ToVec4(kHeader);
        c[ImGuiCol_TableBorderStrong] = ToVec4(kInput);
        c[ImGuiCol_TableBorderLight] = ToVec4(kInput);
        c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.018f);
        c[ImGuiCol_TextLink] = ToVec4(kLink);
        c[ImGuiCol_TextSelectedBg] = ToVec4(WithAlpha(kPrimary, 0.35f));
        c[ImGuiCol_DragDropTarget] = ToVec4(kPrimary);
        c[ImGuiCol_NavCursor] = ToVec4(kPrimary);
        c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.70f);
        c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);
        c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
    }
}
