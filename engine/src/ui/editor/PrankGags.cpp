#include "PrankGags.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "PrankStrings.h"
#include "PrankUi.h"

namespace myengine::ui::prank
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;

        float Ease(const float t)
        {
            const float k = std::clamp(t, 0.0f, 1.0f);
            return k * k * (3.0f - 2.0f * k);
        }

        void DrawTick(ImDrawList* list, const ImVec2 c, const float r, const ImU32 color, const float width)
        {
            list->AddLine(ImVec2(c.x - r * 0.55f, c.y), ImVec2(c.x - r * 0.1f, c.y + r * 0.45f), color, width);
            list->AddLine(ImVec2(c.x - r * 0.1f, c.y + r * 0.45f), ImVec2(c.x + r * 0.6f, c.y - r * 0.4f), color, width);
        }

        void DrawCross(ImDrawList* list, const ImVec2 c, const float r, const ImU32 color, const float width)
        {
            list->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), color, width);
            list->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), color, width);
        }

        // The wireframe shapes of the captcha: 0 cube, 1 sphere, 2 teapot, 3 cone, 4 torus, 5 cylinder, 6 pyramid
        void DrawShape(ImDrawList* list, const ImVec2 c, const float r, const int type)
        {
            const ImU32 fill = Rgb(0xC9D2DB);
            const ImU32 line = Rgb(0x4A5A6A);
            const float w = 1.5f;
            switch (type)
            {
                case 0:
                {
                    const ImVec2 top[] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r * 0.87f, c.y - r * 0.5f), ImVec2(c.x, c.y), ImVec2(c.x - r * 0.87f, c.y - r * 0.5f)};
                    const ImVec2 left[] = {ImVec2(c.x - r * 0.87f, c.y - r * 0.5f), ImVec2(c.x, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r * 0.87f, c.y + r * 0.5f)};
                    const ImVec2 right[] = {ImVec2(c.x, c.y), ImVec2(c.x + r * 0.87f, c.y - r * 0.5f), ImVec2(c.x + r * 0.87f, c.y + r * 0.5f), ImVec2(c.x, c.y + r)};
                    list->AddConvexPolyFilled(top, 4, fill);
                    list->AddConvexPolyFilled(left, 4, Scaled(fill, 0.93f));
                    list->AddConvexPolyFilled(right, 4, Scaled(fill, 0.86f));
                    list->AddPolyline(top, 4, line, ImDrawFlags_Closed, w);
                    list->AddPolyline(left, 4, line, ImDrawFlags_Closed, w);
                    list->AddPolyline(right, 4, line, ImDrawFlags_Closed, w);
                    break;
                }
                case 1:
                    list->AddCircleFilled(c, r, fill, 32);
                    list->AddCircle(c, r, line, 32, w);
                    list->AddEllipse(c, ImVec2(r, r * 0.35f), line, 0.0f, 32, w);
                    list->AddEllipse(c, ImVec2(r * 0.35f, r), line, 0.0f, 32, w);
                    break;
                case 2:
                    list->AddEllipseFilled(c, ImVec2(r * 0.8f, r * 0.55f), fill, 0.0f, 32);
                    list->AddEllipse(c, ImVec2(r * 0.8f, r * 0.55f), line, 0.0f, 32, w);
                    list->AddRect(ImVec2(c.x - r * 0.35f, c.y - r * 0.8f), ImVec2(c.x + r * 0.35f, c.y - r * 0.5f), line, 4.0f, 0, w);
                    list->AddBezierCubic(ImVec2(c.x + r * 0.7f, c.y - r * 0.2f), ImVec2(c.x + r * 1.2f, c.y - r * 0.7f), ImVec2(c.x + r * 1.1f, c.y - r * 0.6f), ImVec2(c.x + r * 0.85f, c.y + r * 0.1f), line, w);
                    list->AddBezierCubic(ImVec2(c.x - r * 0.75f, c.y - r * 0.1f), ImVec2(c.x - r * 1.2f, c.y - r * 0.5f), ImVec2(c.x - r * 1.2f, c.y + r * 0.2f), ImVec2(c.x - r * 0.7f, c.y + r * 0.2f), line, w);
                    break;
                case 3:
                {
                    const ImVec2 tri[] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r * 0.75f, c.y + r * 0.7f), ImVec2(c.x - r * 0.75f, c.y + r * 0.7f)};
                    list->AddConvexPolyFilled(tri, 3, fill);
                    list->AddEllipse(ImVec2(c.x, c.y + r * 0.7f), ImVec2(r * 0.75f, r * 0.25f), line, 0.0f, 24, w);
                    list->AddLine(tri[0], tri[1], line, w);
                    list->AddLine(tri[0], tri[2], line, w);
                    break;
                }
                case 4:
                    list->AddEllipse(c, ImVec2(r, r * 0.65f), line, 0.0f, 32, w);
                    list->AddEllipse(c, ImVec2(r * 0.42f, r * 0.2f), line, 0.0f, 24, w);
                    list->AddEllipse(c, ImVec2(r * 0.7f, r * 0.42f), Rgb(0x8FA0B0), 0.0f, 28, w);
                    break;
                case 5:
                    list->AddRectFilled(ImVec2(c.x - r * 0.6f, c.y - r * 0.7f), ImVec2(c.x + r * 0.6f, c.y + r * 0.7f), fill);
                    list->AddLine(ImVec2(c.x - r * 0.6f, c.y - r * 0.7f), ImVec2(c.x - r * 0.6f, c.y + r * 0.7f), line, w);
                    list->AddLine(ImVec2(c.x + r * 0.6f, c.y - r * 0.7f), ImVec2(c.x + r * 0.6f, c.y + r * 0.7f), line, w);
                    list->AddEllipse(ImVec2(c.x, c.y - r * 0.7f), ImVec2(r * 0.6f, r * 0.22f), line, 0.0f, 24, w);
                    list->AddEllipse(ImVec2(c.x, c.y + r * 0.7f), ImVec2(r * 0.6f, r * 0.22f), line, 0.0f, 24, w);
                    break;
                default:
                {
                    const ImVec2 tri[] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r * 0.9f, c.y + r * 0.6f), ImVec2(c.x, c.y + r * 0.9f), ImVec2(c.x - r * 0.9f, c.y + r * 0.6f)};
                    list->AddConvexPolyFilled(tri, 4, fill);
                    list->AddPolyline(tri, 4, line, ImDrawFlags_Closed, w);
                    list->AddLine(tri[0], tri[2], line, w);
                    break;
                }
            }
        }

        // The Windows 11 dialog (light): the white title bar, the body, the grey strip of the buttons. Returns the position of the body below the title bar
        void DrawWindowsDialog(ImDrawList* list, const ImVec2 a, const ImVec2 b, const char* title, const float footer)
        {
            list->AddRectFilled(ImVec2(a.x + 2.0f, a.y + 6.0f), ImVec2(b.x + 2.0f, b.y + 10.0f), Rgb(0x000000, 70), 8.0f);
            list->AddRectFilled(a, b, kWhite, 8.0f);
            list->AddRect(a, b, Rgb(0xC8C8C8), 8.0f);
            list->AddRectFilled(ImVec2(a.x, b.y - footer), b, Rgb(0xF3F3F3), 8.0f, ImDrawFlags_RoundCornersBottom);
            Text(list, ImVec2(a.x + 14.0f, a.y + 9.0f), 13.0f, Rgb(0x1B1B1B), title);
            DrawCross(list, ImVec2(b.x - 24.0f, a.y + 17.0f), 5.0f, Rgb(0x1B1B1B), 1.2f);
        }
    }

    void PrankGags::Open(const GagKind kind, const bool video)
    {
        kind_ = kind;
        video_ = video;
        passed_ = false;
        finished_ = false;
        age_ = 0.0f;
        toastAfter_.clear();
        addressFill_ = -1.0f;
        for (int i = 0; i < 9; ++i)
        {
            selected_[i] = false;
            shrink_[i] = 0.0f;
        }
        paying_ = -1.0f;
        ivanCaught_ = false;
        runInit_ = false;
        runAge_ = 1.0f;
        faceStarted_ = false;
        faceDoneAge_ = -1.0f;
        faceWaitAge_ = 0.0f;
        faceState_ = AgeCheckFace::State();
        errorAge_ = -1.0f;
        closingAge_ = -1.0f;
    }

    void PrankGags::Finish(const bool passed, const char* toast)
    {
        if (faceStarted_ && face_.stop)
        {
            face_.stop();   // the camera goes off at once
        }
        faceStarted_ = false;
        passed_ = passed;
        toastAfter_ = toast != nullptr ? toast : "";
        finished_ = true;
        kind_ = GagKind::None;
        addressFill_ = -1.0f;
    }

    void PrankGags::DrawScrim(ImDrawList* list, const ImVec2 min, const ImVec2 max, const unsigned rgb, const float alpha, const float eased)
    {
        list->AddRectFilled(min, max, Rgb(rgb, static_cast<unsigned>(255.0f * alpha * eased)));
    }

    bool PrankGags::Draw(ImDrawList* list, const ImVec2 areaMin, const ImVec2 areaMax, const double time, const float dt)
    {
        finished_ = false;
        if (kind_ == GagKind::None)
        {
            return false;
        }
        age_ += dt;
        const float eased = Ease(age_ / 0.16f);
        const ImVec2 center((areaMin.x + areaMax.x) * 0.5f, (areaMin.y + areaMax.y) * 0.5f);
        list->PushClipRect(areaMin, areaMax, true);

        // The load gag draws over the explorer with its own light scrim (the explorer is visible behind it)
        switch (kind_)
        {
            case GagKind::Captcha: DrawScrim(list, areaMin, areaMax, 0x28000A, 0.45f, eased); break;
            case GagKind::Paywall: DrawScrim(list, areaMin, areaMax, 0x1E000A, 0.62f, eased); break;
            case GagKind::Ivan: DrawScrim(list, areaMin, areaMax, 0x000000, 0.35f, eased); break;
            case GagKind::Age: DrawScrim(list, areaMin, areaMax, 0x140005, 0.78f, eased); break;
            case GagKind::Uac: DrawScrim(list, areaMin, areaMax, 0x000000, 0.55f, eased); break;
            default: break;
        }
        const int first = list->VtxBuffer.Size;
        switch (kind_)
        {
            case GagKind::Captcha: DrawCaptcha(list, center, time); break;
            case GagKind::Paywall: DrawPaywall(list, center, time); break;
            case GagKind::Ivan: DrawIvan(list, center); break;
            case GagKind::Age: DrawAge(list, areaMin, areaMax, center, time, dt); break;
            case GagKind::Load99: DrawLoad(list, areaMin, areaMax, center, time); break;
            case GagKind::Uac: DrawUac(list, center); break;
            default: break;
        }
        if (kind_ != GagKind::None)
        {
            ScaleRotate(list, first, center, 0.96f + 0.04f * eased, 0.0f);
            FadeVertices(list, first, eased);
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && kind_ != GagKind::Load99)
            {
                Finish(false, nullptr);
            }
        }
        list->PopClipRect();
        return finished_;
    }

    // ---- 5.1 the captcha ----
    void PrankGags::DrawCaptcha(ImDrawList* list, const ImVec2 center, const double time)
    {
        (void)time;
        const ImVec2 a(center.x - 210.0f, center.y - 309.0f);
        const ImVec2 b(a.x + 420.0f, a.y + 618.0f);
        list->AddRectFilled(ImVec2(a.x + 3.0f, a.y + 8.0f), ImVec2(b.x + 3.0f, b.y + 12.0f), Rgb(0x000000, 90), 6.0f);
        list->AddRectFilled(a, b, kWhite, 4.0f);
        list->AddRectFilled(a, ImVec2(b.x, a.y + 130.0f), Rgb(0x1A73E8), 4.0f, ImDrawFlags_RoundCornersTop);
        Text(list, ImVec2(a.x + 20.0f, a.y + 18.0f), 15.0f, kWhite, str::kCaptchaLine1);
        TextFit(list, ImVec2(a.x + 20.0f, a.y + 38.0f), 380.0f, 26.0f, kWhite, str::kCaptchaLine2);
        Text(list, ImVec2(a.x + 20.0f, a.y + 84.0f), 14.0f, kWhite, str::kCaptchaLine3);

        const float cell = 126.0f;
        const float gap = 4.0f;
        const ImVec2 grid(a.x + 17.0f, a.y + 146.0f);
        static const int shapes[9] = {0, 1, 2, 3, 4, 5, 6, 0, 1};
        for (int i = 0; i < 9; ++i)
        {
            const ImVec2 cMin(grid.x + static_cast<float>(i % 3) * (cell + gap), grid.y + static_cast<float>(i / 3) * (cell + gap));
            const ImVec2 cMax(cMin.x + cell, cMin.y + cell);
            char id[16];
            std::snprintf(id, sizeof(id), "##pcap%d", i);
            if (Hit(id, cMin, cMax))
            {
                selected_[i] = !selected_[i];
            }
            shrink_[i] = Approach(shrink_[i], selected_[i] ? 1.0f : 0.0f, ImGui::GetIO().DeltaTime, 0.12f);
            const float scale = 1.0f - 0.12f * shrink_[i];
            const ImVec2 cc((cMin.x + cMax.x) * 0.5f, (cMin.y + cMax.y) * 0.5f);
            const ImVec2 half(cell * 0.5f * scale, cell * 0.5f * scale);
            list->AddRectFilled(ImVec2(cc.x - half.x, cc.y - half.y), ImVec2(cc.x + half.x, cc.y + half.y), Rgb(0xE8EDF2));
            DrawShape(list, cc, 36.0f * scale, shapes[i]);
            if (shrink_[i] > 0.05f)
            {
                const ImVec2 badge(cc.x - half.x + 14.0f, cc.y - half.y + 14.0f);
                list->AddCircleFilled(badge, 12.0f, Rgb(0x1A73E8), 20);
                DrawTick(list, badge, 10.0f, kWhite, 2.2f);
            }
        }

        // The footer: icons and the button
        const float fy = grid.y + 3.0f * cell + 2.0f * gap + 36.0f;
        list->AddCircle(ImVec2(a.x + 30.0f, fy), 8.0f, Rgb(0x6B7580), 16, 1.6f);
        list->AddCircle(ImVec2(a.x + 64.0f, fy), 8.0f, Rgb(0x6B7580), 16, 1.6f);
        list->AddCircle(ImVec2(a.x + 98.0f, fy), 8.0f, Rgb(0x6B7580), 16, 1.6f);
        Text(list, ImVec2(a.x + 95.0f, fy - 8.0f), 13.0f, Rgb(0x6B7580), "i");
        bool any = false;
        for (const bool selected : selected_)
        {
            any = any || selected;
        }
        const char* label = any ? str::kCaptchaConfirm : str::kCaptchaSkip;
        if (ColorButton(list, "##pcapok", ImVec2(b.x - 20.0f - 150.0f, fy - 22.0f), ImVec2(b.x - 20.0f, fy + 22.0f), label, 14.0f, Rgb(0x1A73E8), Rgb(0x3B87F0), kWhite, 0, 3.0f))
        {
            Finish(true, str::kToastCaptchaOk);
        }
    }

    // ---- 5.2 the paywall ----
    void PrankGags::DrawPaywall(ImDrawList* list, const ImVec2 center, const double time)
    {
        const ImVec2 a(center.x - 260.0f, center.y - 250.0f);
        const ImVec2 b(a.x + 520.0f, a.y + 500.0f);
        Glow(list, a, b, WithAlpha(kHot, 0.35f), 22.0f, 5, 16.0f);
        GradientV(list, a, b, Rgb(0x2A0618), Rgb(0x14040D), 16.0f);
        list->AddRect(a, b, Rgb(0x6A1745), 16.0f, 0, 1.5f);
        // The strip on top: three colours
        list->AddRectFilledMultiColor(a, ImVec2(a.x + 260.0f, a.y + 6.0f), kHot, Rgb(0xFFCC4D), Rgb(0xFFCC4D), kHot);
        list->AddRectFilledMultiColor(ImVec2(a.x + 260.0f, a.y), ImVec2(b.x, a.y + 6.0f), Rgb(0xFFCC4D), kHot2, kHot2, Rgb(0xFFCC4D));

        list->AddRectFilled(ImVec2(a.x + 28.0f, a.y + 28.0f), ImVec2(a.x + 108.0f, a.y + 50.0f), kGold, 11.0f);
        TextCentered(list, ImVec2(a.x + 68.0f, a.y + 39.0f), 12.0f, Rgb(0x3A2300), str::kPaywallChip);
        TextFit(list, ImVec2(a.x + 120.0f, a.y + 31.0f), 372.0f, 13.0f, Rgb(0xE8C5D6), str::kPaywallTitle);

        Text(list, ImVec2(a.x + 28.0f, a.y + 66.0f), 56.0f, kWhite, str::kPaywallPrice);
        Text(list, ImVec2(a.x + 28.0f + TextWidth(56.0f, str::kPaywallPrice) + 10.0f, a.y + 108.0f), 18.0f, Rgb(0xE8C5D6), str::kPaywallPer);

        float y = a.y + 178.0f;
        for (const char* line : str::kPaywallGood)
        {
            DrawTick(list, ImVec2(a.x + 38.0f, y + 10.0f), 14.0f, Rgb(0x6BE38A), 2.4f);
            Text(list, ImVec2(a.x + 60.0f, y), 16.0f, Rgb(0xF4E3EB), line);
            y += 32.0f;
        }
        DrawCross(list, ImVec2(a.x + 38.0f, y + 10.0f), 6.0f, Rgb(0xFF6A8A), 2.4f);
        Text(list, ImVec2(a.x + 60.0f, y), 16.0f, Rgb(0xFF9DB0), str::kPaywallBad);

        const ImVec2 bMin(a.x + 28.0f, a.y + 340.0f);
        const ImVec2 bMax(b.x - 28.0f, bMin.y + 56.0f);
        const bool working = paying_ >= 0.0f;
        if (Button(list, "##ppay", bMin, bMax, working ? str::kPaywallWorking : str::kPaywallButton, ButtonKind::Primary, 17.0f, !working))
        {
            paying_ = 0.0f;
        }
        if (working)
        {
            Spinner(list, ImVec2(bMin.x + 34.0f, (bMin.y + bMax.y) * 0.5f), 10.0f, 2.6f, kWhite, time, 0.8f);
            paying_ += ImGui::GetIO().DeltaTime;
            if (paying_ >= 0.4f)
            {
                Finish(true, str::kToastPaid);
                return;
            }
        }
        TextWrapped(list, ImVec2(a.x + 28.0f, a.y + 414.0f), 464.0f, 12.0f, Rgb(0x9A6F86), str::kPaywallFine, false, 17.0f, 3);
    }

    // ---- 5.3 Ivan ----
    void PrankGags::DrawIvan(ImDrawList* list, const ImVec2 center)
    {
        const ImVec2 a(center.x - 210.0f, center.y - 95.0f);
        const ImVec2 b(a.x + 420.0f, a.y + 190.0f);
        DrawWindowsDialog(list, a, b, str::kIvanTitle, 56.0f);
        list->AddCircleFilled(ImVec2(a.x + 44.0f, a.y + 74.0f), 17.0f, Rgb(0x0067C0), 24);
        TextCentered(list, ImVec2(a.x + 44.0f, a.y + 74.0f), 20.0f, kWhite, "?");
        TextWrapped(list, ImVec2(a.x + 78.0f, a.y + 58.0f), 320.0f, 15.0f, Rgb(0x1B1B1B), ivanCaught_ ? str::kIvanCaught : str::kIvanQuestion, false, 20.0f, 3);
        const float by = b.y - 46.0f;
        if (!ivanCaught_)
        {
            if (ColorButton(list, "##pivno", ImVec2(b.x - 20.0f - 110.0f - 120.0f, by), ImVec2(b.x - 20.0f - 120.0f, by + 34.0f), str::kIvanNo, 13.0f, Rgb(0x0067C0), Rgb(0x1A78CC), kWhite, 0, 4.0f))
            {
                Finish(true, nullptr);
                return;
            }
            if (ColorButton(list, "##pivyes", ImVec2(b.x - 20.0f - 110.0f, by), ImVec2(b.x - 20.0f, by + 34.0f), str::kIvanYes, 13.0f, Rgb(0xFBFBFB), Rgb(0xF0F0F0), Rgb(0x1B1B1B), Rgb(0xD0D0D0), 4.0f))
            {
                ivanCaught_ = true;
            }
        }
        else if (ColorButton(list, "##pivnobody", ImVec2(b.x - 20.0f - 110.0f, by), ImVec2(b.x - 20.0f, by + 34.0f), str::kIvanNobody, 13.0f, Rgb(0x0067C0), Rgb(0x1A78CC), kWhite, 0, 4.0f))
        {
            Finish(true, nullptr);
        }
    }

    // ---- 5.4 the age check ----
    void PrankGags::DrawAge(ImDrawList* list, const ImVec2 areaMin, const ImVec2 areaMax, const ImVec2 center, const double time, const float dt)
    {
        (void)areaMin;
        (void)areaMax;
        const bool faceAvailable = face_.available && face_.available();
        const float h = faceAvailable ? 600.0f : 430.0f;
        const ImVec2 a(center.x - 280.0f, center.y - h * 0.5f);
        const ImVec2 b(a.x + 560.0f, a.y + h);
        const float pulse = 0.8f + 0.2f * Pulse(time, 1.0f);
        Glow(list, a, b, WithAlpha(kRed, 0.55f * pulse), 60.0f, 6, 14.0f);
        list->AddRectFilled(a, b, Rgb(0x1A0408), 14.0f);
        list->AddRect(a, b, kRed, 14.0f, 0, 2.0f);

        const float cx = (a.x + b.x) * 0.5f;
        Sign18(list, ImVec2(cx, a.y + 86.0f), 110.0f, kRed, Rgb(0x1A0408), kWhite, 8.0f, true);
        TextCentered(list, ImVec2(cx, a.y + 168.0f), 30.0f, kWhite, str::kAgeWarning);

        if (faceAvailable)
        {
            if (!faceStarted_)
            {
                faceStarted_ = true;
                if (face_.start)
                {
                    face_.start();
                }
                faceWaitAge_ = 0.0f;
            }
            faceWaitAge_ += dt;
            if (face_.poll && faceDoneAge_ < 0.0f)
            {
                faceState_ = face_.poll();
                if (faceState_.phase != AgeCheckFace::Phase::Scanning)
                {
                    faceDoneAge_ = 0.0f;
                    if (face_.stop)
                    {
                        face_.stop();
                        faceStarted_ = false;
                    }
                }
                else if (faceWaitAge_ > 6.0f)
                {
                    faceState_.phase = AgeCheckFace::Phase::Unknown;
                    faceDoneAge_ = 0.0f;
                    if (face_.stop)
                    {
                        face_.stop();
                        faceStarted_ = false;
                    }
                }
            }
            const ImVec2 cc(cx, a.y + 330.0f);
            const float r = 120.0f;
            list->AddCircleFilled(cc, r, Rgb(0x0E0306), 48);
            if (faceState_.picture != 0)
            {
                // mirrored, and cut to a circle: the longer side of the frame is cropped
                const float aspect = std::max(faceState_.aspect, 0.1f);
                const float halfU = aspect > 1.0f ? 0.5f / aspect : 0.5f;
                const float halfV = aspect > 1.0f ? 0.5f : 0.5f * aspect;
                list->AddImageRounded(faceState_.picture, ImVec2(cc.x - r, cc.y - r), ImVec2(cc.x + r, cc.y + r), ImVec2(0.5f + halfU, 0.5f - halfV),
                                      ImVec2(0.5f - halfU, 0.5f + halfV), kWhite, r);
            }
            const float sweep = static_cast<float>(std::fmod(time, 1.2) / 1.2) * 2.0f * kPi;
            list->PathClear();
            for (int i = 0; i <= 20; ++i)
            {
                const float t = sweep + static_cast<float>(i) / 20.0f * 1.2f;
                list->PathLineTo(ImVec2(cc.x + std::cos(t) * (r + 6.0f), cc.y + std::sin(t) * (r + 6.0f)));
            }
            list->PathStroke(kHot, 0, 4.0f);
            list->AddCircle(cc, r, faceDoneAge_ >= 0.0f ? Rgb(0x6BE38A) : Rgb(0x6A2350), 48, 2.0f);
            char line[160];
            const char* text = str::kAgeUnknown;
            if (faceDoneAge_ >= 0.0f)
            {
                if (faceState_.phase == AgeCheckFace::Phase::Recognized)
                {
                    std::snprintf(line, sizeof(line), str::kAgeRecognized, faceState_.name.c_str());
                    text = line;
                }
                else if (faceState_.phase == AgeCheckFace::Phase::Other)
                {
                    std::snprintf(line, sizeof(line), str::kAgeOther, faceState_.name.c_str());
                    text = line;
                }
                TextCentered(list, ImVec2(cx, a.y + 480.0f), 20.0f, kWhite, text);
                faceDoneAge_ += dt;
                if (faceDoneAge_ > 1.2f)
                {
                    Finish(true, nullptr);
                    return;
                }
            }
            TextCentered(list, ImVec2(cx, b.y - 36.0f), 13.0f, Rgb(0xFF9DB0), str::kAgeFaceIdFine);
            return;
        }

        TextCentered(list, ImVec2(cx, a.y + 216.0f), 16.0f, Rgb(0xFFC2CF), str::kAgeConfirm);
        const ImVec2 yesMin(cx - 180.0f - 10.0f, a.y + 300.0f);
        const ImVec2 yesMax(yesMin.x + 180.0f, yesMin.y + 40.0f);
        if (Button(list, "##pageyes", yesMin, yesMax, str::kAgeYes, ButtonKind::Primary, 16.0f))
        {
            Finish(true, nullptr);
            return;
        }

        // "I am not 18" runs away from the cursor
        const ImVec2 noSize(150.0f, 40.0f);
        if (!runInit_)
        {
            runInit_ = true;
            runPosition_ = ImVec2(cx + 20.0f, a.y + 300.0f);
        }
        runAge_ += dt;
        ImVec2 position = runPosition_;
        if (runAge_ < 0.12f)
        {
            const float t = Ease(runAge_ / 0.12f);
            position = ImVec2(runFrom_.x + (runTo_.x - runFrom_.x) * t, runFrom_.y + (runTo_.y - runFrom_.y) * t);
        }
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const ImVec2 noCenter(position.x + noSize.x * 0.5f, position.y + noSize.y * 0.5f);
        const float dx = std::max({position.x - mouse.x, 0.0f, mouse.x - (position.x + noSize.x)});
        const float dy = std::max({position.y - mouse.y, 0.0f, mouse.y - (position.y + noSize.y)});
        if (runAge_ >= 0.12f && std::sqrt(dx * dx + dy * dy) < 70.0f)
        {
            // A new place inside the window, 120 px from the cursor and not over the first button
            for (int attempt = 0; attempt < 24; ++attempt)
            {
                random_ = random_ * 1664525u + 1013904223u;
                const float rx = static_cast<float>((random_ >> 8) & 0xFFFF) / 65535.0f;
                random_ = random_ * 1664525u + 1013904223u;
                const float ry = static_cast<float>((random_ >> 8) & 0xFFFF) / 65535.0f;
                const ImVec2 candidate(a.x + 24.0f + rx * (560.0f - 48.0f - noSize.x), a.y + 240.0f + ry * (h - 240.0f - 60.0f - noSize.y));
                const ImVec2 cc(candidate.x + noSize.x * 0.5f, candidate.y + noSize.y * 0.5f);
                const bool overYes = candidate.x < yesMax.x + 10.0f && candidate.x + noSize.x > yesMin.x - 10.0f && candidate.y < yesMax.y + 10.0f && candidate.y + noSize.y > yesMin.y - 10.0f;
                const float md = std::sqrt((cc.x - mouse.x) * (cc.x - mouse.x) + (cc.y - mouse.y) * (cc.y - mouse.y));
                if (!overYes && md >= 120.0f)
                {
                    runFrom_ = position;
                    runTo_ = candidate;
                    runPosition_ = candidate;
                    runAge_ = 0.0f;
                    break;
                }
            }
        }
        const int start = list->VtxBuffer.Size;
        const ImVec2 noMax(position.x + noSize.x, position.y + noSize.y);
        const bool noClicked = Button(list, "##pageno", position, noMax, str::kAgeNo, ButtonKind::Ghost, 15.0f);
        ScaleRotate(list, start, noCenter, 1.0f, -6.0f);
        if (noClicked)
        {
            toastAfter_ = str::kAgeNoPressed;
            Finish(true, str::kAgeNoPressed);
        }
    }

    // ---- 5.5 the video that stops at 99 % ----
    void PrankGags::DrawLoad(ImDrawList* list, const ImVec2 areaMin, const ImVec2 areaMax, const ImVec2 center, const double time)
    {
        (void)areaMin;
        (void)areaMax;
        // The address bar: 0..90 % in 1.2 s, then 99 % in 1.5 s more, then it stands. After the OK it runs to the end
        float fill = 0.0f;
        if (age_ < 1.2f)
        {
            fill = 0.9f * age_ / 1.2f;
        }
        else if (age_ < 2.7f)
        {
            fill = 0.9f + 0.09f * (age_ - 1.2f) / 1.5f;
        }
        else
        {
            fill = 0.99f;
        }
        if (closingAge_ >= 0.0f)
        {
            closingAge_ += ImGui::GetIO().DeltaTime;
            fill = 0.99f + 0.01f * std::min(1.0f, closingAge_ / 0.25f);
            addressFill_ = closingAge_ > 0.5f ? -1.0f : fill;
            if (closingAge_ > 0.5f)
            {
                Finish(true, video_ ? str::kToastAgain99 : nullptr);
                return;
            }
        }
        else
        {
            addressFill_ = fill;
        }

        // The player
        const ImVec2 a(center.x - 360.0f, center.y - 220.0f);
        const ImVec2 b(a.x + 640.0f, a.y + 400.0f);
        list->AddRectFilled(ImVec2(a.x + 3.0f, a.y + 8.0f), ImVec2(b.x + 3.0f, b.y + 12.0f), Rgb(0x000000, 90), 8.0f);
        list->AddRectFilled(a, b, Rgb(0x000000), 8.0f);
        list->AddRectFilled(a, ImVec2(b.x, a.y + 32.0f), Rgb(0x202020), 8.0f, ImDrawFlags_RoundCornersTop);
        list->AddRectFilled(ImVec2(a.x + 12.0f, a.y + 10.0f), ImVec2(a.x + 26.0f, a.y + 24.0f), Rgb(0x9B7FE8), 3.0f);
        Text(list, ImVec2(a.x + 36.0f, a.y + 8.0f), 13.0f, Rgb(0xE8E8E8), str::kLoadTitle);
        const ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f - 20.0f);
        list->AddCircle(mid, 30.0f, Rgb(0x333333), 40, 5.0f);
        Spinner(list, mid, 30.0f, 5.0f, Rgb(0xDDDDDD), time, 1.1f);
        TextCentered(list, ImVec2(mid.x, mid.y + 70.0f), 18.0f, kWhite, str::kLoadLoading);
        Text(list, ImVec2(a.x + 30.0f, b.y - 56.0f), 12.0f, Rgb(0x999999), str::kLoadLeft);
        list->AddRectFilled(ImVec2(a.x + 30.0f, b.y - 36.0f), ImVec2(b.x - 30.0f, b.y - 31.0f), Rgb(0x333333), 3.0f);
        list->AddRectFilled(ImVec2(a.x + 30.0f, b.y - 36.0f), ImVec2(a.x + 30.0f + (b.x - a.x - 60.0f) * 0.99f, b.y - 31.0f), kHot, 3.0f);

        // The error, 3 s after the 99 %
        if (age_ >= 5.7f && closingAge_ < 0.0f)
        {
            if (errorAge_ < 0.0f)
            {
                errorAge_ = 0.0f;
            }
            errorAge_ += ImGui::GetIO().DeltaTime;
            const ImVec2 ea(center.x + 20.0f, center.y + 40.0f);
            const ImVec2 eb(ea.x + 470.0f, ea.y + 212.0f);
            const int start = list->VtxBuffer.Size;
            DrawWindowsDialog(list, ea, eb, "видео.mp4", 64.0f);
            list->AddCircleFilled(ImVec2(ea.x + 46.0f, ea.y + 82.0f), 18.0f, Rgb(0xC42B1C), 24);
            DrawCross(list, ImVec2(ea.x + 46.0f, ea.y + 82.0f), 7.0f, kWhite, 3.0f);
            Text(list, ImVec2(ea.x + 82.0f, ea.y + 56.0f), 15.0f, Rgb(0x1B1B1B), str::kLoadErrorTitle);
            TextWrapped(list, ImVec2(ea.x + 82.0f, ea.y + 80.0f), 370.0f, 14.0f, Rgb(0x1B1B1B), str::kLoadErrorBody, false, 19.0f, 3);
            const float by = eb.y - 50.0f;
            ColorButton(list, "##pbuy", ImVec2(eb.x - 20.0f - 130.0f - 148.0f - 8.0f, by), ImVec2(eb.x - 20.0f - 148.0f - 8.0f, by + 34.0f), str::kLoadBuy, 13.0f, Rgb(0xFBFBFB), Rgb(0xFBFBFB), Rgb(0xA0A0A0), Rgb(0xDADADA), 4.0f);
            if (ColorButton(list, "##pbuyok", ImVec2(eb.x - 20.0f - 148.0f, by), ImVec2(eb.x - 20.0f, by + 34.0f), str::kLoadOk, 13.0f, Rgb(0x0067C0), Rgb(0x1A78CC), kWhite, 0, 4.0f))
            {
                closingAge_ = 0.0f;
            }
            FadeVertices(list, start, Ease(errorAge_ / 0.16f));
        }
    }

    // ---- 5.6 the UAC ----
    void PrankGags::DrawUac(ImDrawList* list, const ImVec2 center)
    {
        const ImVec2 a(center.x - 235.0f, center.y - 165.0f);
        const ImVec2 b(a.x + 470.0f, a.y + 330.0f);
        list->AddRectFilled(ImVec2(a.x + 3.0f, a.y + 8.0f), ImVec2(b.x + 3.0f, b.y + 12.0f), Rgb(0x000000, 110), 8.0f);
        list->AddRectFilled(a, b, Rgb(0x1F1F1F), 8.0f);
        list->AddRectFilled(a, ImVec2(b.x, a.y + 32.0f), kGold, 8.0f, ImDrawFlags_RoundCornersTop);
        Text(list, ImVec2(a.x + 14.0f, a.y + 8.0f), 13.0f, Rgb(0x2B2B2B), str::kUacTitle);
        TextWrapped(list, ImVec2(a.x + 24.0f, a.y + 52.0f), 422.0f, 19.0f, kWhite, str::kUacQuestion, false, 25.0f, 3);
        Sign18(list, ImVec2(a.x + 46.0f, a.y + 160.0f), 44.0f, kRed, Rgb(0x2A0710), kWhite, 3.0f, true);
        Text(list, ImVec2(a.x + 80.0f, a.y + 142.0f), 17.0f, kWhite, str::kUacFile);
        Text(list, ImVec2(a.x + 80.0f, a.y + 168.0f), 13.0f, Rgb(0xC4C4C4), str::kUacPublisher);
        Text(list, ImVec2(a.x + 80.0f, a.y + 188.0f), 13.0f, Rgb(0xC4C4C4), str::kUacSource);
        const float by = b.y - 58.0f;
        const float half = (b.x - a.x - 24.0f * 2.0f - 12.0f) * 0.5f;
        if (ColorButton(list, "##puacyes", ImVec2(a.x + 24.0f, by), ImVec2(a.x + 24.0f + half, by + 38.0f), str::kUacYes, 15.0f, Rgb(0x0067C0), Rgb(0x1A78CC), kWhite, 0, 4.0f))
        {
            Finish(true, nullptr);
            return;
        }
        if (ColorButton(list, "##puacno", ImVec2(a.x + 24.0f + half + 12.0f, by), ImVec2(b.x - 24.0f, by + 38.0f), str::kUacNo, 15.0f, Rgb(0x2B2B2B), Rgb(0x3A3A3A), kWhite, Rgb(0x555555), 4.0f))
        {
            Finish(true, nullptr);
        }
    }
}
