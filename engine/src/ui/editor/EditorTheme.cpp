#include "EditorTheme.h"

#include <imgui/imgui.h>

namespace myengine::ui::detail
{
    void ApplyEditorTheme()
    {
        ImGuiStyle& style = ImGui::GetStyle();

        // Shape: soft 2-4 px corners, dense spacing
        style.WindowRounding = 3.0f;
        style.ChildRounding = 3.0f;
        style.FrameRounding = 3.0f;
        style.PopupRounding = 3.0f;
        style.ScrollbarRounding = 3.0f;
        style.GrabRounding = 3.0f;
        style.TabRounding = 3.0f;
        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.TabBorderSize = 0.0f;
        style.WindowPadding = ImVec2(8.0f, 6.0f);
        style.FramePadding = ImVec2(6.0f, 3.0f);
        style.CellPadding = ImVec2(6.0f, 3.0f);
        style.ItemSpacing = ImVec2(6.0f, 4.0f);
        style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
        style.IndentSpacing = 14.0f;
        style.ScrollbarSize = 12.0f;
        style.GrabMinSize = 10.0f;
        style.WindowMenuButtonPosition = ImGuiDir_Left;

        const ImVec4 text(0.82f, 0.82f, 0.83f, 1.0f);
        const ImVec4 textDisabled(0.46f, 0.46f, 0.48f, 1.0f);
        const ImVec4 panel(0.135f, 0.135f, 0.142f, 1.0f);       // window and panel background
        const ImVec4 panelDark(0.098f, 0.098f, 0.104f, 1.0f);   // inputs, tab strip, menu bar
        const ImVec4 panelDeep(0.075f, 0.075f, 0.080f, 1.0f);   // behind the panels
        const ImVec4 border(0.045f, 0.045f, 0.050f, 1.0f);
        const ImVec4 control(0.200f, 0.200f, 0.212f, 1.0f);
        const ImVec4 controlHovered(0.275f, 0.275f, 0.292f, 1.0f);
        const ImVec4 controlActive(0.340f, 0.340f, 0.360f, 1.0f);
        const ImVec4 accent(0.000f, 0.447f, 0.890f, 1.0f);        // the single accent colour
        const ImVec4 accentHovered(0.110f, 0.530f, 0.950f, 1.0f);
        const ImVec4 accentDim(0.050f, 0.290f, 0.540f, 1.0f);

        ImVec4* c = style.Colors;
        c[ImGuiCol_Text] = text;
        c[ImGuiCol_TextDisabled] = textDisabled;
        c[ImGuiCol_WindowBg] = panel;
        c[ImGuiCol_ChildBg] = panelDark;
        c[ImGuiCol_PopupBg] = ImVec4(0.118f, 0.118f, 0.125f, 0.98f);
        c[ImGuiCol_Border] = border;
        c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_FrameBg] = panelDark;
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.165f, 0.165f, 0.176f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.205f, 0.205f, 0.220f, 1.0f);
        c[ImGuiCol_TitleBg] = panelDeep;
        c[ImGuiCol_TitleBgActive] = panelDark;
        c[ImGuiCol_TitleBgCollapsed] = panelDeep;
        c[ImGuiCol_MenuBarBg] = panelDark;
        c[ImGuiCol_ScrollbarBg] = panelDark;
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0.260f, 0.260f, 0.275f, 1.0f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.340f, 0.340f, 0.360f, 1.0f);
        c[ImGuiCol_ScrollbarGrabActive] = accent;
        c[ImGuiCol_CheckMark] = accentHovered;
        c[ImGuiCol_SliderGrab] = accent;
        c[ImGuiCol_SliderGrabActive] = accentHovered;
        c[ImGuiCol_Button] = control;
        c[ImGuiCol_ButtonHovered] = controlHovered;
        c[ImGuiCol_ButtonActive] = controlActive;
        c[ImGuiCol_Header] = accentDim;
        c[ImGuiCol_HeaderHovered] = ImVec4(0.215f, 0.215f, 0.230f, 1.0f);
        c[ImGuiCol_HeaderActive] = accent;
        c[ImGuiCol_Separator] = border;
        c[ImGuiCol_SeparatorHovered] = accentHovered;
        c[ImGuiCol_SeparatorActive] = accent;
        c[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_ResizeGripHovered] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
        c[ImGuiCol_ResizeGripActive] = accent;
        c[ImGuiCol_Tab] = panelDark;
        c[ImGuiCol_TabHovered] = ImVec4(0.215f, 0.215f, 0.230f, 1.0f);
        c[ImGuiCol_TabSelected] = panel;
        c[ImGuiCol_TabSelectedOverline] = accent;
        c[ImGuiCol_TabDimmed] = panelDeep;
        c[ImGuiCol_TabDimmedSelected] = panelDark;
        c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(accent.x, accent.y, accent.z, 0.0f);
        c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
        c[ImGuiCol_DockingEmptyBg] = panelDeep;
        c[ImGuiCol_TableHeaderBg] = panelDark;
        c[ImGuiCol_TableBorderStrong] = border;
        c[ImGuiCol_TableBorderLight] = ImVec4(0.070f, 0.070f, 0.076f, 1.0f);
        c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
        c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
        c[ImGuiCol_DragDropTarget] = accentHovered;
        c[ImGuiCol_NavCursor] = accentHovered;
        c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.70f);
        c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);
        c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
    }
}
