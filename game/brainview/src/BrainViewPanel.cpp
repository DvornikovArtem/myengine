#include <brainview/BrainViewPanel.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace brainview
{
    namespace
    {
        constexpr double kRateWindowSeconds = 0.2;
        constexpr float kBarFullHz = 60.0f;      // the bar is full at this rate (a square root scale: small rates still show)
        constexpr int kRasterBinGroup = 2;       // 10 ms bins folded by 2: 100 columns
        constexpr float kRasterBandHeight = 13.0f;

        ImU32 Rgba(const float r, const float g, const float b, const float a)
        {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f), std::clamp(a, 0.0f, 1.0f)));
        }

        // the colour of a group for the HUD: its palette row, brought into 0..1
        ImVec4 GroupColor(const uint8_t colorIndex)
        {
            float palette[kPaletteSize][4];
            FillPalette(palette);
            const float* c = palette[colorIndex & 15];
            const float peak = std::max({c[0], c[1], c[2], 1.0f});
            return ImVec4(c[0] / peak, c[1] / peak, c[2] / peak, 1.0f);
        }
    }

    void DrawBrainViewPanel(BrainView& view, const std::uint64_t textureId, const float width, const float height, const BrainPanelInfo& info)
    {
        const ImVec2 size(width, height);
        const ImTextureID texture = static_cast<ImTextureID>(textureId);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 end(origin.x + size.x, origin.y + size.y);
        ImDrawList* draw = ImGui::GetWindowDrawList();

        // the picture and the mouse
        ImGui::InvisibleButton("##brainview", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        if (info.allowMouse)
        {
            if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)))
            {
                const ImVec2 delta = ImGui::GetIO().MouseDelta;
                view.Camera().Orbit(delta.x, delta.y);
            }
            if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
            {
                view.Camera().Zoom(ImGui::GetIO().MouseWheel);
            }
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                view.Camera().autoRotate = !view.Camera().autoRotate;
            }
        }
        if (texture != 0)
        {
            draw->AddImage(texture, origin, end);
        }
        else
        {
            draw->AddRectFilled(origin, end, Rgba(0.02f, 0.03f, 0.05f, 1.0f));
        }
        DrawBrainHud(draw, origin.x, origin.y, end.x, end.y, view, info);
    }

    void DrawBrainHud(ImDrawList* draw, const float left0, const float top0, const float right0, const float bottom0, BrainView& view, const BrainPanelInfo& info)
    {
        if (!info.showHud)
        {
            return;
        }
        const ImVec2 origin(left0, top0);
        const ImVec2 end(right0, bottom0);
        draw->PushClipRect(origin, end, true);
        const float margin = 12.0f;
        char line[160];

        // the title
        draw->AddText(ImVec2(origin.x + margin, origin.y + margin), Rgba(0.75f, 0.88f, 1.0f, 0.95f), info.title.c_str());
        std::snprintf(line, sizeof(line), "%u neurons   %.2f M spikes/s   t = %.1f s", view.NeuronCount(), info.spikesPerSecond / 1.0e6, info.simSeconds);
        draw->AddText(ImVec2(origin.x + margin, origin.y + margin + 16.0f), Rgba(0.6f, 0.7f, 0.85f, 0.85f), line);
        if (info.realtimeRatio > 0.0)
        {
            std::snprintf(line, sizeof(line), "simulation %.2fx real time", info.realtimeRatio);
            draw->AddText(ImVec2(origin.x + margin, origin.y + margin + 32.0f), Rgba(0.6f, 0.7f, 0.85f, 0.7f), line);
        }
        if (!info.subtitle.empty())
        {
            draw->AddText(ImVec2(origin.x + margin, origin.y + margin + 48.0f), Rgba(0.5f, 0.6f, 0.75f, 0.6f), info.subtitle.c_str());
        }

        // the bars, on the right
        const GroupActivity& activity = view.Activity();
        const auto& groups = view.Groups();
        const float barLeft = end.x - margin - kHudBarWidth - 62.0f;   // the label goes left of the bar, the rate right of it, all inside the picture
        float y = origin.y + margin;
        for (std::size_t g = 0; g < groups.size() && g < activity.GroupCount(); ++g)
        {
            const float hz = activity.RateHz(g, kRateWindowSeconds);
            const float fill = std::clamp(std::sqrt(hz / kBarFullHz), 0.0f, 1.0f);
            const ImVec4 color = GroupColor(groups[g].colorIndex);
            const ImU32 bright = Rgba(color.x, color.y, color.z, 1.0f);
            draw->AddText(ImVec2(barLeft - 8.0f - ImGui::CalcTextSize(groups[g].label.c_str()).x, y), Rgba(color.x, color.y, color.z, 0.95f), groups[g].label.c_str());
            draw->AddRectFilled(ImVec2(barLeft, y + 3.0f), ImVec2(barLeft + kHudBarWidth, y + 13.0f), Rgba(1.0f, 1.0f, 1.0f, 0.08f), 2.0f);
            if (fill > 0.0f)
            {
                draw->AddRectFilled(ImVec2(barLeft, y + 3.0f), ImVec2(barLeft + kHudBarWidth * fill, y + 13.0f), bright, 2.0f);
            }
            std::snprintf(line, sizeof(line), "%.1f Hz", hz);
            draw->AddText(ImVec2(barLeft + kHudBarWidth + 6.0f, y), Rgba(0.8f, 0.85f, 0.95f, 0.7f), line);
            y += 18.0f;
        }

        // the raster, along the bottom: a band per group, time from left (2 s ago) to right (now)
        if (info.showRaster && !groups.empty())
        {
            const float rasterHeight = kRasterBandHeight * static_cast<float>(groups.size());
            const float left = origin.x + margin;
            const float right = end.x - margin - 40.0f;
            const float top = end.y - margin - rasterHeight;
            draw->AddRectFilled(ImVec2(left - 4.0f, top - 4.0f), ImVec2(right + 4.0f, end.y - margin + 4.0f), Rgba(0.0f, 0.0f, 0.0f, 0.35f), 4.0f);
            const int columns = GroupActivity::kBinCount / kRasterBinGroup;
            const float cellWidth = (right - left) / static_cast<float>(columns);
            std::vector<uint16_t> grid;
            for (std::size_t g = 0; g < groups.size() && g < activity.GroupCount(); ++g)
            {
                const float bandTop = top + kRasterBandHeight * static_cast<float>(g);
                const ImVec4 color = GroupColor(groups[g].colorIndex);
                draw->AddLine(ImVec2(left, bandTop + kRasterBandHeight - 0.5f), ImVec2(right, bandTop + kRasterBandHeight - 0.5f), Rgba(1.0f, 1.0f, 1.0f, 0.06f));
                activity.Raster(g, grid);
                const uint32_t rows = activity.Rows(g);
                if (grid.empty() || rows == 0)
                {
                    continue;
                }
                const float rowHeight = (kRasterBandHeight - 1.0f) / static_cast<float>(rows);
                // a neuron has a row of its own when there are few; many neurons share the band and the cell is lighter or darker by how many spiked
                const float full = std::max(1.0f, static_cast<float>(activity.GroupSize(g)) / static_cast<float>(rows) * 0.9f);
                for (uint32_t row = 0; row < rows; ++row)
                {
                    for (int column = 0; column < columns; ++column)
                    {
                        uint32_t count = 0;
                        for (int k = 0; k < kRasterBinGroup; ++k)
                        {
                            count += grid[static_cast<std::size_t>(row) * GroupActivity::kBinCount + static_cast<std::size_t>(column * kRasterBinGroup + k)];
                        }
                        if (count == 0)
                        {
                            continue;
                        }
                        const float alpha = std::clamp(0.35f + 0.65f * static_cast<float>(count) / full, 0.0f, 1.0f);
                        const float x = left + cellWidth * static_cast<float>(column);
                        const float cellTop = bandTop + rowHeight * static_cast<float>(row);
                        draw->AddRectFilled(ImVec2(x, cellTop), ImVec2(x + std::max(cellWidth, 1.0f), cellTop + std::max(rowHeight, 1.0f)), Rgba(color.x, color.y, color.z, alpha));
                    }
                }
            }
            draw->AddText(ImVec2(right + 8.0f, top - 1.0f), Rgba(0.6f, 0.7f, 0.85f, 0.6f), "now");
            draw->AddText(ImVec2(left, top - 18.0f), Rgba(0.6f, 0.7f, 0.85f, 0.6f), "spikes, last 2 s");
        }
        draw->PopClipRect();
    }
}
