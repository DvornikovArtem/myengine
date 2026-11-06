#include "PrankTab.h"

#include <algorithm>
#include <cmath>

#include <imgui/imgui_internal.h>

#include "PrankStrings.h"
#include "PrankUi.h"

namespace myengine::ui::prank
{
    namespace
    {
        constexpr float kTabPulsePeriod = 1.2f;
        constexpr float kBadgeHideAfter = 2.0f;
    }

    PrankTab::PrankTab(PrankSceneServices scene, AgeCheckFace face) : generator_(std::move(scene))
    {
        gags_.SetAgeCheckFace(std::move(face));
        // The editor opens on the Viewport: the tab sits next to it and pulses, but is not in front (the editor focuses the
        // Viewport for the first frames, see StartupFrame)
    }

    PrankTab::~PrankTab() = default;

    const char* PrankTab::WindowName()
    {
        return "       ПОРНО       ###Prank";
    }

    void PrankTab::Shutdown()
    {
        generator_.Shutdown();
    }

    void PrankTab::Update(const float dt)
    {
        generator_.Update(dt);
    }

    void PrankTab::SwitchTo(const Mode mode)
    {
        if (mode_ != mode)
        {
            mode_ = mode;
            switchAge_ = 0.0f;
        }
    }

    // ---- the tab ----

    void PrankTab::DrawTabDecoration(const ImVec2 tabMin, const ImVec2 tabMax, const bool active)
    {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float pulse = Pulse(ImGui::GetTime(), kTabPulsePeriod);
        Glow(fg, tabMin, tabMax, WithAlpha(kHot, 0.75f * pulse), 4.0f, 3, 3.0f);
        // The sign 18 on the left of the label
        Sign18(fg, ImVec2(tabMin.x + 17.0f, (tabMin.y + tabMax.y) * 0.5f), 14.0f, kWhite, 0, kWhite, 1.6f);
        if (!badgeGone_)
        {
            // The white pill with the number on the right, left of the cross of the tab (the cross takes the last 24 px)
            const ImVec2 a(tabMax.x - 52.0f, (tabMin.y + tabMax.y) * 0.5f - 9.0f);
            const ImVec2 b(a.x + 20.0f, a.y + 18.0f);
            fg->AddRectFilled(a, b, kWhite, 9.0f);
            TextCentered(fg, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), 12.0f, kRed, str::kBadgeText);
        }
        (void)active;
    }

    bool PrankTab::DrawWindow(const ImGuiID dockId)
    {
        ImGuiIO& io = ImGui::GetIO();
        const double time = ImGui::GetTime();
        const float dt = io.DeltaTime;
        switchAge_ += dt;
        ClearHitRecords();

        // The colours of the tab (the docking branch takes them into the style of the tab): the pulse between two pinks
        const float pulse = Pulse(time, kTabPulsePeriod);
        const ImU32 base = Lerp(kHot, kHot2, pulse);
        const ImU32 dim = Scaled(base, 0.82f);
        ImGui::PushStyleColor(ImGuiCol_Tab, ImGui::ColorConvertU32ToFloat4(dim));
        ImGui::PushStyleColor(ImGuiCol_TabHovered, ImGui::ColorConvertU32ToFloat4(kHotHover));
        ImGui::PushStyleColor(ImGuiCol_TabSelected, ImGui::ColorConvertU32ToFloat4(base));
        ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, ImGui::ColorConvertU32ToFloat4(kWhite));
        ImGui::PushStyleColor(ImGuiCol_TabDimmed, ImGui::ColorConvertU32ToFloat4(dim));
        ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, ImGui::ColorConvertU32ToFloat4(dim));
        ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, ImGui::ColorConvertU32ToFloat4(kWhite));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kWhite));

        if (dockId != 0)
        {
            ImGui::SetNextWindowDockID(dockId, ImGuiCond_FirstUseEver);
        }
        ImGui::SetNextWindowSize(ImVec2(900.0f, 560.0f), ImGuiCond_FirstUseEver);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        if (focusFrames_ > 0)
        {
            --focusFrames_;
            ImGui::SetNextWindowFocus();
        }
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNavInputs;
        // p_open puts the cross on the tab: a click on it closes the window, and the editor takes that as "the check mark is off"
        bool open = true;
        const bool shown = ImGui::Begin(WindowName(), &open, flags);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(8);

        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const bool active = window != nullptr && window->DockNode != nullptr ? window->DockIsActive : shown;
        const ImRect tabRect = window != nullptr ? window->DC.DockTabItemRect : ImRect();
        const bool docked = window != nullptr && window->DockNode != nullptr;
        visible_ = shown;
        if (shown)
        {
            activeSeconds_ += dt;
            if (activeSeconds_ >= kBadgeHideAfter)
            {
                badgeGone_ = true;
            }
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImVec2 size = ImGui::GetContentRegionAvail();
            size.x = std::max(size.x, 200.0f);
            size.y = std::max(size.y, 120.0f);
            DrawContent(ImGui::GetWindowDrawList(), p, size, time, dt);
            // The window takes the whole area: the items drawn by hand must not leave the child area scrolling
            ImGui::SetCursorScreenPos(p);
            ImGui::Dummy(ImVec2(1.0f, 1.0f));
        }
        else
        {
            activeSeconds_ = 0.0f;
        }
        ImGui::End();

        // The glow, the sign and the badge on the tab itself: drawn once the tab rectangle is known (the tab is there whether it is active or not)
        if (docked && tabRect.GetWidth() > 1.0f)
        {
            DrawTabDecoration(tabRect.Min, tabRect.Max, active);
        }
        return !open;
    }

    void PrankTab::DrawContent(ImDrawList* list, const ImVec2 min, const ImVec2 size, const double time, const float dt)
    {
        const ImVec2 max(min.x + size.x, min.y + size.y);
        if (mode_ == Mode::Generator)
        {
            // The explorer fades out in 200 ms, the generator appears in 250 ms
            const int first = list->VtxBuffer.Size;
            const bool back = generator_.Draw(list, min, max, time, dt);
            FadeVertices(list, first, std::min(1.0f, switchAge_ / 0.25f));
            if (back)
            {
                SwitchTo(Mode::Explorer);
            }
            return;
        }

        ExplorerRequest request;
        const bool modal = gags_.Active();
        const int first = list->VtxBuffer.Size;
        explorer_.Draw(list, min, size, time, dt, !modal, request);
        if (switchAge_ < 0.2f)
        {
            FadeVertices(list, first, 0.6f + 0.4f * switchAge_ / 0.2f);
        }

        if (modal)
        {
            const GagKind kind = gags_.Kind();
            explorer_.SetWorking(gags_.Working());
            if (gags_.Draw(list, min, max, time, dt))
            {
                explorer_.SetAddressFill(-1.0f);
                explorer_.SetWorking(false);
                if (gags_.Passed())
                {
                    explorer_.SetPassed(kind);
                    if (!gags_.ToastAfter().empty())
                    {
                        explorer_.Toast(gags_.ToastAfter());
                    }
                    if (kind == GagKind::Uac)
                    {
                        SwitchTo(Mode::Generator);
                    }
                    else if (!gags_.WasVideo() && pendingTarget_ >= 0)
                    {
                        explorer_.GoTo(pendingTarget_);
                    }
                }
                pendingTarget_ = -1;
            }
            else
            {
                explorer_.SetAddressFill(gags_.AddressFill());
            }
            return;
        }

        if (request.gag != GagKind::None)
        {
            if (request.gag == GagKind::Uac && request.targetLevel == -2)
            {
                SwitchTo(Mode::Generator);   // the UAC was passed once already
            }
            else
            {
                pendingTarget_ = request.targetLevel;
                lastGag_ = request.gag;
                gags_.Open(request.gag, request.video);
            }
        }
    }
}
