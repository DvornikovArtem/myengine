// Asset viewers (AV): Texture, Static Mesh, Material and Prefab tabs docked next to the Viewport.
// Each one is a canvas on the left (a live view of the preview service, or ImGui drawing for textures) and an
// info panel on the right. They only read the assets, except the material properties, which go through the same
// undo commands as the Material Editor.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <set>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "SceneEditorInternal.h"

namespace myengine::ui
{
    using namespace detail;

    namespace
    {
        constexpr float kPanelWidth = 344.0f;
        constexpr float kSeparatorWidth = 2.0f;
        constexpr float kOverlayButton = 26.0f;
        constexpr float kFovYRadians = 0.5585f; // 32 degrees
        constexpr ImU32 kCanvasColor = IM_COL32(0x14, 0x14, 0x14, 255);
        constexpr ImU32 kOverlayColor = IM_COL32(0x15, 0x15, 0x15, 220);

        const char* ViewerIcon(const AssetViewerType type)
        {
            switch (type)
            {
                case AssetViewerType::Texture: return ICON_IMAGE;
                case AssetViewerType::Mesh: return ICON_BOX;
                case AssetViewerType::Material: return ICON_PALETTE;
                case AssetViewerType::Prefab: break;
            }
            return ICON_PACKAGE;
        }

        ImU32 ViewerColor(const AssetViewerType type)
        {
            switch (type)
            {
                case AssetViewerType::Texture: return style::kTypeTexture;
                case AssetViewerType::Mesh: return style::kTypeMesh;
                case AssetViewerType::Material: return style::kTypeMaterial;
                case AssetViewerType::Prefab: break;
            }
            return style::kTypePrefab;
        }

        const char* ViewerKindName(const AssetViewerType type)
        {
            switch (type)
            {
                case AssetViewerType::Texture: return "Texture";
                case AssetViewerType::Mesh: return "Mesh";
                case AssetViewerType::Material: return "Material";
                case AssetViewerType::Prefab: break;
            }
            return "Prefab";
        }

        // The overlay panel behind a row of buttons: draw the buttons between Begin and End
        struct OverlayGroup
        {
            ImDrawList* drawList = nullptr;
            ImVec2 min{};
            float width = 0.0f;
        };

        OverlayGroup BeginOverlay(const ImVec2 position)
        {
            OverlayGroup group;
            group.drawList = ImGui::GetWindowDrawList();
            group.min = position;
            group.drawList->ChannelsSplit(2);
            group.drawList->ChannelsSetCurrent(1);
            ImGui::SetCursorScreenPos(ImVec2(position.x + 2.0f, position.y + 2.0f));
            ImGui::BeginGroup();
            return group;
        }

        // Returns the width of the group so that the next one can be placed to the right
        float EndOverlay(OverlayGroup& group)
        {
            ImGui::EndGroup();
            const ImVec2 max = ImGui::GetItemRectMax();
            group.drawList->ChannelsSetCurrent(0);
            group.drawList->AddRectFilled(group.min, ImVec2(max.x + 2.0f, max.y + 2.0f), kOverlayColor, 4.0f);
            group.drawList->ChannelsMerge();
            return max.x + 2.0f - group.min.x;
        }

        // A letter button of the channel switches: coloured, dimmed when the channel is off
        bool ChannelButton(const char* id, const char* letter, const ImU32 color, bool& value)
        {
            constexpr float kWidth = 24.0f;
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(kWidth, kOverlayButton));
            if (pressed)
            {
                value = !value;
            }
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            if (ImGui::IsItemHovered())
            {
                drawList->AddRectFilled(min, ImVec2(min.x + kWidth, min.y + kOverlayButton), IM_COL32(255, 255, 255, 24), 3.0f);
            }
            PushFontRole(FontRole::Strong);
            const ImVec2 size = ImGui::CalcTextSize(letter);
            drawList->AddText(
                ImVec2(std::floor(min.x + (kWidth - size.x) * 0.5f), std::floor(min.y + (kOverlayButton - size.y) * 0.5f)),
                value ? color : style::WithAlpha(color, 0.35f), letter);
            PopFontRole();
            return pressed;
        }

        // One overlay segment: [icon label], 26 high. Used alone and inside OverlaySegmented.
        bool OverlaySegment(const char* id, const char* icon, const char* label, const bool active, const char* tooltip, const char* shortcut)
        {
            PushFontRole(FontRole::Body);
            const float textWidth = ImGui::CalcTextSize(label).x;
            const float textHeight = ImGui::GetFontSize();
            PopFontRole();
            const float iconWidth = icon != nullptr ? 14.0f + 6.0f : 0.0f;
            const float width = 10.0f + iconWidth + textWidth + 10.0f;
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, kOverlayButton));
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            if (active)
            {
                drawList->AddRectFilled(min, ImVec2(min.x + width, min.y + kOverlayButton), hovered ? style::kPrimaryHover : style::kPrimary, 3.0f);
            }
            else if (hovered)
            {
                drawList->AddRectFilled(min, ImVec2(min.x + width, min.y + kOverlayButton), IM_COL32(255, 255, 255, 24), 3.0f);
            }
            const ImU32 color = active ? style::kTextStrong : style::kText;
            float x = min.x + 10.0f;
            if (icon != nullptr)
            {
                DrawIcon(drawList, IconSize::Row14, icon, ImVec2(x + 7.0f, min.y + kOverlayButton * 0.5f), color);
                x += iconWidth;
            }
            PushFontRole(FontRole::Body);
            drawList->AddText(ImVec2(x, std::floor(min.y + (kOverlayButton - textHeight) * 0.5f)), color, label);
            PopFontRole();
            if (hovered && tooltip != nullptr)
            {
                Tooltip(tooltip, shortcut);
            }
            return pressed;
        }

        // Segments with an icon and a label; the active one is blue. Returns the (possibly new) index.
        int OverlaySegmented(const char* id, const char* const* labels, const char* const* icons, const int count, const int current)
        {
            int result = current;
            ImGui::PushID(id);
            for (int index = 0; index < count; ++index)
            {
                if (index > 0)
                {
                    ImGui::SameLine(0.0f, 2.0f);
                }
                ImGui::PushID(index);
                if (OverlaySegment("##segment", icons != nullptr ? icons[index] : nullptr, labels[index], index == current, nullptr, nullptr))
                {
                    result = index;
                }
                ImGui::PopID();
            }
            ImGui::PopID();
            return result;
        }

        // A dim hint on a plate at the bottom left of the canvas
        void DrawHint(const ImVec2 canvasMin, const ImVec2 canvasMax, const char* text)
        {
            PushFontRole(FontRole::Mono);
            const ImVec2 size = ImGui::CalcTextSize(text);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 min(canvasMin.x + 8.0f, canvasMax.y - 8.0f - size.y - 8.0f);
            drawList->AddRectFilled(min, ImVec2(min.x + size.x + 16.0f, min.y + size.y + 8.0f), kOverlayColor, 3.0f);
            drawList->AddText(ImVec2(min.x + 8.0f, min.y + 4.0f), style::kTextDim, text);
            PopFontRole();
        }

        // Info panel pieces ------------------------------------------------------------------------------------

        // The tab strip of the panel (one tab) and a line under it
        void DrawPanelTab(const char* icon, const char* label)
        {
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(min, ImVec2(min.x + width, min.y + 30.0f), style::kRecessed);
            PushFontRole(FontRole::Body);
            const ImVec2 size = ImGui::CalcTextSize(label);
            DrawIcon(drawList, IconSize::Row14, icon, ImVec2(min.x + 13.0f + 7.0f, min.y + 15.0f), style::kTextStrong);
            drawList->AddText(ImVec2(min.x + 13.0f + 20.0f, min.y + (30.0f - size.y) * 0.5f), style::kTextStrong, label);
            PopFontRole();
            drawList->AddRectFilled(ImVec2(min.x + 6.0f, min.y + 28.0f), ImVec2(min.x + 20.0f + 20.0f + size.x, min.y + 30.0f), style::kPrimary);
            ImGui::Dummy(ImVec2(width, 30.0f));
        }

        void DrawPanelHeader(const AssetViewerType type, const std::string& name, const std::string& subtitle, const char* chip)
        {
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 square(min.x + 10.0f, min.y + 8.0f);
            drawList->AddRectFilled(square, ImVec2(square.x + 28.0f, square.y + 28.0f), style::kRecessed, style::kRounding);
            DrawIcon(drawList, IconSize::Row14, ViewerIcon(type), ImVec2(square.x + 14.0f, square.y + 14.0f), ViewerColor(type));
            const float textX = square.x + 28.0f + 8.0f;
            drawList->PushClipRect(ImVec2(textX, min.y), ImVec2(min.x + width - (chip != nullptr ? 90.0f : 8.0f), min.y + 44.0f), true);
            PushFontRole(FontRole::Strong);
            drawList->AddText(ImVec2(textX, min.y + 6.0f), style::kTextStrong, name.c_str());
            PopFontRole();
            PushFontRole(FontRole::Tiny);
            drawList->AddText(ImVec2(textX, min.y + 24.0f), style::kTextDim, subtitle.c_str());
            PopFontRole();
            drawList->PopClipRect();
            if (chip != nullptr)
            {
                ImGui::SetCursorScreenPos(ImVec2(min.x + width - 82.0f, min.y + 13.0f));
                Chip(chip, ChipKind::Gray);
            }
            ImGui::SetCursorScreenPos(ImVec2(min.x, min.y + 44.0f));
            ImGui::Dummy(ImVec2(width, 0.0f));
        }

        // `text` cut in the middle with "..." so that it is `width` wide at most (measured with the body font)
        std::string FitMiddle(const std::string& text, const float width)
        {
            PushFontRole(FontRole::Body);
            std::string result = text;
            if (ImGui::CalcTextSize(text.c_str()).x > width)
            {
                std::size_t keep = text.size();
                while (keep > 4)
                {
                    --keep;
                    std::size_t head = (keep + 1) / 2;
                    std::size_t tailStart = text.size() - (keep - head);
                    // Do not cut a UTF-8 sequence in the middle
                    while (head > 0 && (static_cast<unsigned char>(text[head]) & 0xC0) == 0x80)
                    {
                        --head;
                    }
                    while (tailStart < text.size() && (static_cast<unsigned char>(text[tailStart]) & 0xC0) == 0x80)
                    {
                        ++tailStart;
                    }
                    result = text.substr(0, head) + "..." + text.substr(tailStart);
                    if (ImGui::CalcTextSize(result.c_str()).x <= width)
                    {
                        break;
                    }
                }
            }
            PopFontRole();
            return result;
        }

        // A property row: dim label, white value
        void InfoRow(const char* label, const std::string& value)
        {
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            PushFontRole(FontRole::Body);
            const float textY = std::floor(min.y + (style::kPropRowHeight - ImGui::GetFontSize()) * 0.5f);
            drawList->AddText(ImVec2(min.x + 18.0f, textY), style::kText, label);
            const float valueWidth = std::max(width - 128.0f - 8.0f, 20.0f);
            const std::string shown = FitMiddle(value, valueWidth);
            drawList->AddText(ImVec2(min.x + 128.0f, textY), style::kTextStrong, shown.c_str());
            PopFontRole();
            ImGui::Dummy(ImVec2(width, style::kPropRowHeight));
            if (ImGui::IsItemHovered() && shown != value)
            {
                Tooltip(value.c_str());
            }
        }

        std::string FormatModified(const std::filesystem::path& path)
        {
            std::error_code error;
            const auto fileTime = std::filesystem::last_write_time(path, error);
            if (error)
            {
                return "-";
            }
            const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            const std::time_t time = std::chrono::system_clock::to_time_t(system);
            std::tm local{};
            localtime_s(&local, &time);
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%02d.%02d.%04d %02d:%02d", local.tm_mday, local.tm_mon + 1, local.tm_year + 1900,
                local.tm_hour, local.tm_min);
            return buffer;
        }

        // File category: size, modified, path and the two buttons
        void DrawFileCategory(const std::filesystem::path& absolute, const std::string& assetPath, const bool buttons)
        {
            if (!BeginCategory("File"))
            {
                return;
            }
            std::error_code error;
            const std::uint64_t size = std::filesystem::is_regular_file(absolute, error)
                ? static_cast<std::uint64_t>(std::filesystem::file_size(absolute, error))
                : 0;
            InfoRow("File size", error ? std::string("-") : FormatBytes(size));
            InfoRow("Modified", FormatModified(absolute));
            InfoRow("Path", assetPath);
            if (buttons)
            {
                ImGui::Dummy(ImVec2(0.0f, 4.0f));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f);
                if (Button("Show in Explorer", ICON_FOLDER_OPEN))
                {
                    const std::wstring parameters = L"/select,\"" + absolute.wstring() + L"\"";
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
                }
                ImGui::SameLine(0.0f, 6.0f);
                if (Button("Copy Path", ICON_COPY))
                {
                    ImGui::SetClipboardText(assetPath.c_str());
                }
                ImGui::Dummy(ImVec2(0.0f, 6.0f));
            }
            EndCategory();
        }

        // The 3/4 camera of the 3D viewers around `target`
        void BuildOrbitCamera(
            const AssetViewerTab& tab,
            const float aspect,
            render::Matrix4& view,
            render::Matrix4& projection)
        {
            const float cosPitch = std::cos(tab.pitch);
            const DirectX::XMVECTOR target = DirectX::XMVectorSet(tab.target[0], tab.target[1], tab.target[2], 1.0f);
            const DirectX::XMVECTOR offset = DirectX::XMVectorSet(
                tab.distance * std::sin(tab.yaw) * cosPitch,
                tab.distance * std::sin(tab.pitch),
                -tab.distance * std::cos(tab.yaw) * cosPitch,
                0.0f);
            const DirectX::XMVECTOR eye = DirectX::XMVectorAdd(target, offset);
            view = scene::ToRenderMatrix(DirectX::XMMatrixLookAtLH(eye, target, DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
            projection = scene::ToRenderMatrix(DirectX::XMMatrixPerspectiveFovLH(
                kFovYRadians, std::max(aspect, 0.1f), std::max(tab.distance * 0.02f, 0.01f), tab.distance * 40.0f + 100.0f));
        }

        // Mouse of the 3D canvases: LMB orbit, RMB/MMB pan, wheel zoom, F frame. Returns true on F.
        bool HandleOrbitInput(AssetViewerTab& tab, const bool hovered, const bool active)
        {
            ImGuiIO& io = ImGui::GetIO();
            if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
            {
                tab.yaw -= io.MouseDelta.x * 0.012f;
                tab.pitch = std::clamp(tab.pitch + io.MouseDelta.y * 0.012f, -1.45f, 1.45f);
            }
            if (hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)))
            {
                // Pan in the screen plane: right and up of the camera
                const float scale = tab.distance * 0.0018f;
                const float cosYaw = std::cos(tab.yaw);
                const float sinYaw = std::sin(tab.yaw);
                tab.target[0] += (-io.MouseDelta.x * cosYaw) * scale;
                tab.target[2] += (-io.MouseDelta.x * sinYaw) * scale;
                tab.target[1] += io.MouseDelta.y * scale;
            }
            if (hovered && io.MouseWheel != 0.0f)
            {
                tab.distance = std::clamp(tab.distance * (1.0f - io.MouseWheel * 0.1f), 0.05f, 4000.0f);
            }
            return hovered && ImGui::IsKeyPressed(ImGuiKey_F, false);
        }

        // Frames a box: the camera looks at its centre from far enough to fit the bounding sphere
        void FrameBounds(AssetViewerTab& tab, const editor::BoundsBox& bounds)
        {
            if (!bounds.IsValid())
            {
                tab.target[0] = tab.target[1] = tab.target[2] = 0.0f;
                tab.distance = 3.0f;
                return;
            }
            tab.target[0] = (bounds.min.x + bounds.max.x) * 0.5f;
            tab.target[1] = (bounds.min.y + bounds.max.y) * 0.5f;
            tab.target[2] = (bounds.min.z + bounds.max.z) * 0.5f;
            const float dx = bounds.max.x - bounds.min.x;
            const float dy = bounds.max.y - bounds.min.y;
            const float dz = bounds.max.z - bounds.min.z;
            const float radius = std::max(0.5f * std::sqrt(dx * dx + dy * dy + dz * dz), 0.01f);
            tab.distance = radius / std::sin(kFovYRadians * 0.5f) * 1.15f;
        }

        // The 12 edges of a box; `dashed` cuts each of them into pieces (the bounds of the viewers)
        void AppendBox(std::vector<render::DebugLine>& lines, const editor::BoundsBox& box, const core::Color& color, const bool dashed)
        {
            const float maxSide = std::max({box.max.x - box.min.x, box.max.y - box.min.y, box.max.z - box.min.z, 0.001f});
            const float dashLength = maxSide * 0.035f;
            const render::Float3 corners[8] = {
                {box.min.x, box.min.y, box.min.z}, {box.max.x, box.min.y, box.min.z}, {box.max.x, box.max.y, box.min.z},
                {box.min.x, box.max.y, box.min.z}, {box.min.x, box.min.y, box.max.z}, {box.max.x, box.min.y, box.max.z},
                {box.max.x, box.max.y, box.max.z}, {box.min.x, box.max.y, box.max.z},
            };
            static const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& edge : edges)
            {
                const render::Float3 a = corners[edge[0]];
                const render::Float3 b = corners[edge[1]];
                const float length = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) + (b.z - a.z) * (b.z - a.z));
                // An odd number of pieces so that both corners of the edge are drawn
                int pieces = dashed ? std::clamp(static_cast<int>(length / dashLength), 1, 61) : 1;
                pieces |= 1;
                for (int piece = 0; piece < pieces; piece += 2)
                {
                    const float t0 = static_cast<float>(piece) / static_cast<float>(pieces);
                    const float t1 = static_cast<float>(piece + 1) / static_cast<float>(pieces);
                    render::DebugLine line;
                    line.start = {a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0, a.z + (b.z - a.z) * t0};
                    line.end = {a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1, a.z + (b.z - a.z) * t1};
                    line.color = color;
                    lines.push_back(line);
                }
            }
        }

        // The floor: a square grid of 1 m cells (10 m, 100 m ... for large things) around the origin
        void AppendFloorGrid(std::vector<render::DebugLine>& lines, const float extent)
        {
            float step = 1.0f;
            while (extent / step > 12.0f)
            {
                step *= 10.0f;
            }
            const int count = static_cast<int>(std::ceil(extent / step)) + 2;
            const float half = count * step;
            const core::Color color{0.18f, 0.19f, 0.21f, 1.0f}; // about #FFF at 0.07 over the background
            for (int index = -count; index <= count; ++index)
            {
                render::DebugLine along;
                along.start = {-half, 0.0f, index * step};
                along.end = {half, 0.0f, index * step};
                along.color = color;
                lines.push_back(along);
                render::DebugLine across;
                across.start = {index * step, 0.0f, -half};
                across.end = {index * step, 0.0f, half};
                across.color = color;
                lines.push_back(across);
            }
        }

        // The dark gradient behind a transparent live view
        void DrawViewerBackground(ImDrawList* drawList, const ImVec2 min, const ImVec2 max)
        {
            drawList->AddRectFilled(min, max, IM_COL32(0x17, 0x19, 0x1D, 255));
            drawList->AddRectFilledMultiColor(
                min, ImVec2(max.x, min.y + (max.y - min.y) * 0.6f),
                IM_COL32(0x2B, 0x2F, 0x36, 255), IM_COL32(0x2B, 0x2F, 0x36, 255), IM_COL32(0x17, 0x19, 0x1D, 255), IM_COL32(0x17, 0x19, 0x1D, 255));
        }
    }

    // ---- opening and the windows -------------------------------------------------------------------------

    void SceneEditor::OpenAssetViewer(const std::string& path, const AssetViewerType type)
    {
        if (path.empty())
        {
            return;
        }
        for (auto& tab : viewers_)
        {
            if (tab->type == type && tab->path == path)
            {
                tab->focusRequested = true;
                return;
            }
        }

        auto tab = std::make_unique<AssetViewerTab>();
        tab->type = type;
        tab->path = path;
        std::string name = std::filesystem::path(path).filename().string();
        if (type == AssetViewerType::Material || type == AssetViewerType::Prefab)
        {
            name = AssetNameWithoutExtensions(name);
        }
        tab->name = name;
        tab->title = std::string(ViewerIcon(type)) + "  " + name + "###viewer:" + std::string(ViewerKindName(type)) + ":" + path;
        tab->focusRequested = true;
        tab->nearest = true;
        viewers_.push_back(std::move(tab));
    }

    void SceneEditor::CloseAssetViewers()
    {
        for (auto& tab : viewers_)
        {
            if (tab->maskedTexture.IsValid() && services_.thumbnails != nullptr)
            {
                services_.thumbnails->DestroyViewTexture(tab->maskedTexture);
            }
        }
        viewers_.clear();
    }

    void SceneEditor::BuildAssetViewers(const SceneEditorWindowContext& windowContext)
    {
        (void)windowContext;
        for (std::size_t index = 0; index < viewers_.size();)
        {
            AssetViewerTab& tab = *viewers_[index];

            if (!tab.docked)
            {
                // Next to the Viewport, in its dock node: the tab strip appears when the node has two windows
                if (const ImGuiWindow* viewport = ImGui::FindWindowByName(kViewportWindowName); viewport != nullptr && viewport->DockId != 0)
                {
                    ImGui::SetNextWindowDockID(viewport->DockId, ImGuiCond_Appearing);
                }
                ImGui::SetNextWindowSize(ImVec2(900.0f, 600.0f), ImGuiCond_FirstUseEver);
                tab.docked = true;
            }
            if (tab.focusRequested)
            {
                ImGui::SetNextWindowFocus();
                tab.focusRequested = false;
            }

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            const bool visible = ImGui::Begin(tab.title.c_str(), &tab.open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar();
            if (visible)
            {
                const ImVec2 available = ImGui::GetContentRegionAvail();
                const float panelWidth = std::clamp(kPanelWidth, 220.0f, std::max(available.x * 0.5f, 220.0f));
                const float canvasWidth = std::max(available.x - panelWidth - kSeparatorWidth, 40.0f);

                ImGui::PushStyleColor(ImGuiCol_ChildBg, style::ToVec4(kCanvasColor));
                ImGui::BeginChild(
                    "##viewer_canvas", ImVec2(canvasWidth, available.y), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                ImGui::PopStyleColor();
                const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
                const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
                switch (tab.type)
                {
                    case AssetViewerType::Texture: DrawTextureCanvas(tab, canvasMin, canvasSize); break;
                    case AssetViewerType::Mesh: DrawMeshCanvas(tab, canvasMin, canvasSize); break;
                    case AssetViewerType::Material: DrawMaterialCanvas(tab, canvasMin, canvasSize); break;
                    case AssetViewerType::Prefab: DrawPrefabCanvas(tab, canvasMin, canvasSize); break;
                }
                ImGui::EndChild();

                ImGui::SameLine(0.0f, 0.0f);
                const ImVec2 separatorMin = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    separatorMin, ImVec2(separatorMin.x + kSeparatorWidth, separatorMin.y + available.y), style::kInput);
                ImGui::Dummy(ImVec2(kSeparatorWidth, available.y));
                ImGui::SameLine(0.0f, 0.0f);

                ImGui::PushStyleColor(ImGuiCol_ChildBg, style::ToVec4(style::kPanel));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                ImGui::BeginChild("##viewer_panel", ImVec2(panelWidth, available.y), ImGuiChildFlags_None);
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                switch (tab.type)
                {
                    case AssetViewerType::Texture: DrawTexturePanel(tab); break;
                    case AssetViewerType::Mesh: DrawMeshPanel(tab); break;
                    case AssetViewerType::Material: DrawMaterialPanel(tab); break;
                    case AssetViewerType::Prefab: DrawPrefabPanel(tab); break;
                }
                ImGui::EndChild();
            }
            ImGui::End();

            if (!tab.open)
            {
                if (tab.maskedTexture.IsValid() && services_.thumbnails != nullptr)
                {
                    services_.thumbnails->DestroyViewTexture(tab.maskedTexture);
                }
                viewers_.erase(viewers_.begin() + static_cast<std::ptrdiff_t>(index));
            }
            else
            {
                ++index;
            }
        }
    }

    // ---- Texture --------------------------------------------------------------------------------------------

    namespace
    {
        struct ChannelMask
        {
            bool r = true;
            bool g = true;
            bool b = true;
            bool a = true;
        };

        // One pixel through the channel switches: off channels are black, alpha off makes it opaque, and alpha alone
        // is shown as grey
        void MaskPixel(const std::uint8_t* pixel, const ChannelMask& mask, std::uint8_t* out)
        {
            if (mask.a && !mask.r && !mask.g && !mask.b)
            {
                out[0] = out[1] = out[2] = pixel[3];
                out[3] = 255;
                return;
            }
            out[0] = mask.r ? pixel[0] : 0;
            out[1] = mask.g ? pixel[1] : 0;
            out[2] = mask.b ? pixel[2] : 0;
            out[3] = mask.a ? pixel[3] : 255;
        }

        // The data of the texture the viewer shows: loaded, still loading, or not openable
        enum class TextureState
        {
            Ready,
            Loading,
            Failed,
        };

        TextureState ResolveTexture(
            resource::ResourceManager& resources,
            const std::string& path,
            resource::ResourceHandle<resource::TextureAsset>& out)
        {
            out = resources.Load<resource::TextureAsset>(path);
            if (out == nullptr)
            {
                return TextureState::Failed;
            }
            if (resources.IsLoadPending(path))
            {
                return TextureState::Loading;
            }
            if (resources.LoadFailed(path))
            {
                return TextureState::Failed;
            }
            if (out->asset.data.width == 0 || out->asset.data.height == 0 ||
                out->asset.data.pixelsRgba8.size() != static_cast<std::size_t>(out->asset.data.width) * out->asset.data.height * 4)
            {
                return TextureState::Failed;
            }
            return TextureState::Ready;
        }

        void DrawCannotOpen(const ImVec2 min, const ImVec2 size, const std::string& path)
        {
            ImGui::SetCursorScreenPos(min);
            ImGui::BeginChild("##cannot_open", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
            EmptyState(ICON_CIRCLE_X, "Cannot open this file", (path + "\nSee the Output Log for the reason.").c_str());
            ImGui::EndChild();
        }

        void FillChecker(ImDrawList* drawList, const ImVec2 min, const ImVec2 max, const float cell)
        {
            drawList->AddRectFilled(min, max, IM_COL32(0x2A, 0x2A, 0x2A, 255));
            const int columns = static_cast<int>(std::ceil((max.x - min.x) / cell));
            const int rows = static_cast<int>(std::ceil((max.y - min.y) / cell));
            if (columns * rows > 60000)
            {
                return;
            }
            for (int row = 0; row < rows; ++row)
            {
                for (int column = 0; column < columns; ++column)
                {
                    if (((row + column) & 1) == 0)
                    {
                        continue;
                    }
                    const ImVec2 a(min.x + column * cell, min.y + row * cell);
                    drawList->AddRectFilled(a, ImVec2(std::min(a.x + cell, max.x), std::min(a.y + cell, max.y)), IM_COL32(0x3A, 0x3A, 0x3A, 255));
                }
            }
        }
    }

    void SceneEditor::DrawTextureCanvas(AssetViewerTab& tab, const ImVec2& canvasMin, const ImVec2& canvasSize)
    {
        resource::ResourceHandle<resource::TextureAsset> texture;
        const TextureState state = ResolveTexture(*services_.resourceManager, tab.path, texture);
        if (state != TextureState::Ready)
        {
            if (state == TextureState::Failed)
            {
                DrawCannotOpen(canvasMin, canvasSize, tab.path);
            }
            else
            {
                PushFontRole(FontRole::Secondary);
                ImGui::SetCursorScreenPos(ImVec2(canvasMin.x + 16.0f, canvasMin.y + 16.0f));
                ImGui::TextDisabled("Loading...");
                PopFontRole();
            }
            return;
        }

        const render::TextureData& data = texture->asset.data;
        const float textureWidth = static_cast<float>(data.width);
        const float textureHeight = static_cast<float>(data.height);
        const ImVec2 canvasMax(canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Alpha, scanned once per loaded texture
        if (tab.infoHandle != texture->asset.gpuHandle.value)
        {
            tab.infoHandle = texture->asset.gpuHandle.value;
            tab.hasAlpha = false;
            for (std::size_t offset = 3; offset < data.pixelsRgba8.size(); offset += 4)
            {
                if (data.pixelsRgba8[offset] < 255)
                {
                    tab.hasAlpha = true;
                    break;
                }
            }
            if (!tab.nearestChosen)
            {
                tab.nearest = std::max(data.width, data.height) < 64;
            }
            tab.maskedKey = ~0u; // a reloaded texture needs its masked copy again
        }

        // Fit leaves room for the overlay at the top: the picture sits in the area below it
        constexpr float kOverlayBand = 44.0f;
        const float fitZoom = std::max(
            std::min((canvasSize.x - 32.0f) / textureWidth, (canvasSize.y - 32.0f - kOverlayBand) / textureHeight), 0.01f);
        if (tab.fit)
        {
            tab.zoom = fitZoom;
            tab.pan[0] = 0.0f;
            tab.pan[1] = kOverlayBand * 0.5f;
        }

        // Input: the canvas below the overlay
        ImGui::SetCursorScreenPos(canvasMin);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##texture_canvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 center(canvasMin.x + canvasSize.x * 0.5f, canvasMin.y + canvasSize.y * 0.5f);
        if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)))
        {
            tab.pan[0] += io.MouseDelta.x;
            tab.pan[1] += io.MouseDelta.y;
            tab.fit = false;
        }
        if (hovered && io.MouseWheel != 0.0f)
        {
            const float newZoom = std::clamp(tab.zoom * std::pow(1.25f, io.MouseWheel), 0.01f, 400.0f);
            // Keep the pixel under the cursor where it is
            const ImVec2 mouse = io.MousePos;
            const float imageX = (mouse.x - (center.x + tab.pan[0])) / tab.zoom;
            const float imageY = (mouse.y - (center.y + tab.pan[1])) / tab.zoom;
            tab.zoom = newZoom;
            tab.pan[0] = mouse.x - center.x - imageX * newZoom;
            tab.pan[1] = mouse.y - center.y - imageY * newZoom;
            tab.fit = false;
        }
        if (hovered && ImGui::IsKeyPressed(ImGuiKey_F, false))
        {
            tab.fit = true;
        }
        if (hovered && ImGui::IsKeyPressed(ImGuiKey_1, false))
        {
            tab.fit = false;
            tab.zoom = 1.0f;
            tab.pan[0] = tab.pan[1] = 0.0f;
        }

        const float zoom = tab.zoom;
        const ImVec2 imageMin(center.x + tab.pan[0] - textureWidth * zoom * 0.5f, center.y + tab.pan[1] - textureHeight * zoom * 0.5f);
        const ImVec2 imageMax(imageMin.x + textureWidth * zoom, imageMin.y + textureHeight * zoom);

        drawList->PushClipRect(canvasMin, canvasMax, true);
        const bool allChannels = tab.channelR && tab.channelG && tab.channelB && tab.channelA;
        const ChannelMask mask{tab.channelR, tab.channelG, tab.channelB, tab.channelA};
        if (tab.checker)
        {
            const ImVec2 visibleMin(std::max(imageMin.x, canvasMin.x), std::max(imageMin.y, canvasMin.y));
            const ImVec2 visibleMax(std::min(imageMax.x, canvasMax.x), std::min(imageMax.y, canvasMax.y));
            if (visibleMax.x > visibleMin.x && visibleMax.y > visibleMin.y)
            {
                FillChecker(drawList, visibleMin, visibleMax, 8.0f);
            }
        }

        // Pixels as squares when each of them is big (crisp edges: ImGui samples with a linear filter), the
        // texture otherwise. Channels: the masks are applied to the pixel colours or to a masked copy.
        const auto channelColor = [&](const std::size_t offset) -> ImU32
        {
            std::uint8_t out[4];
            MaskPixel(&data.pixelsRgba8[offset], mask, out);
            return IM_COL32(out[0], out[1], out[2], out[3]);
        };

        const int firstColumn = std::max(0, static_cast<int>(std::floor((canvasMin.x - imageMin.x) / zoom)));
        const int lastColumn = std::min(static_cast<int>(data.width) - 1, static_cast<int>(std::floor((canvasMax.x - imageMin.x) / zoom)));
        const int firstRow = std::max(0, static_cast<int>(std::floor((canvasMin.y - imageMin.y) / zoom)));
        const int lastRow = std::min(static_cast<int>(data.height) - 1, static_cast<int>(std::floor((canvasMax.y - imageMin.y) / zoom)));
        const long long visiblePixels = static_cast<long long>(std::max(lastColumn - firstColumn + 1, 0)) * std::max(lastRow - firstRow + 1, 0);
        if (tab.nearest && zoom >= 2.0f && visiblePixels > 0 && visiblePixels <= 40000)
        {
            for (int row = firstRow; row <= lastRow; ++row)
            {
                for (int column = firstColumn; column <= lastColumn; ++column)
                {
                    const std::size_t offset = (static_cast<std::size_t>(row) * data.width + column) * 4;
                    const ImVec2 a(std::floor(imageMin.x + column * zoom), std::floor(imageMin.y + row * zoom));
                    const ImVec2 b(std::ceil(imageMin.x + (column + 1) * zoom), std::ceil(imageMin.y + (row + 1) * zoom));
                    drawList->AddRectFilled(a, b, channelColor(offset));
                }
            }
        }
        else
        {
            render::TextureHandle handle = texture->asset.gpuHandle;
            const std::uint32_t key = (tab.channelR ? 1u : 0u) | (tab.channelG ? 2u : 0u) | (tab.channelB ? 4u : 0u) | (tab.channelA ? 8u : 0u);
            if (!allChannels && services_.thumbnails != nullptr)
            {
                if (tab.maskedKey != key || !tab.maskedTexture.IsValid())
                {
                    if (tab.maskedTexture.IsValid())
                    {
                        services_.thumbnails->DestroyViewTexture(tab.maskedTexture);
                    }
                    render::TextureData masked;
                    masked.width = data.width;
                    masked.height = data.height;
                    masked.channels = 4;
                    masked.srgb = data.srgb; // the same encoding as the source: it looks the same as the unmasked image
                    masked.pixelsRgba8.resize(data.pixelsRgba8.size());
                    for (std::size_t offset = 0; offset < data.pixelsRgba8.size(); offset += 4)
                    {
                        MaskPixel(&data.pixelsRgba8[offset], mask, &masked.pixelsRgba8[offset]);
                    }
                    tab.maskedTexture = services_.thumbnails->CreateViewTexture(masked);
                    tab.maskedKey = key;
                }
                if (tab.maskedTexture.IsValid())
                {
                    handle = tab.maskedTexture;
                }
            }
            drawList->AddImage(static_cast<ImTextureID>(handle.value), imageMin, imageMax);
        }
        drawList->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 30));
        drawList->PopClipRect();

        // Overlay groups
        ImVec2 cursor(canvasMin.x + 8.0f, canvasMin.y + 8.0f);
        {
            OverlayGroup group = BeginOverlay(cursor);
            if (OverlaySegment("##tex_fit", ICON_MAXIMIZE, "Fit", false, "Fit to the window", "F"))
            {
                tab.fit = true;
            }
            ImGui::SameLine(0.0f, 2.0f);
            if (OverlaySegment("##tex_actual", ICON_SCAN, "1:1", false, "Actual size", "1"))
            {
                tab.fit = false;
                tab.zoom = 1.0f;
                tab.pan[0] = tab.pan[1] = 0.0f;
            }
            ImGui::SameLine(0.0f, 6.0f);
            char zoomText[24];
            std::snprintf(zoomText, sizeof(zoomText), "%d%%", static_cast<int>(std::lround(tab.zoom * 100.0f)));
            PushFontRole(FontRole::Mono);
            const ImVec2 zoomSize = ImGui::CalcTextSize(zoomText);
            const ImVec2 zoomMin = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(std::max(zoomSize.x, 52.0f), kOverlayButton));
            ImGui::GetWindowDrawList()->AddText(ImVec2(zoomMin.x, zoomMin.y + (kOverlayButton - zoomSize.y) * 0.5f), style::kTextDim, zoomText);
            PopFontRole();
            cursor.x += EndOverlay(group) + 6.0f;
        }
        {
            OverlayGroup group = BeginOverlay(cursor);
            ChannelButton("##tex_r", "R", IM_COL32(0xFF, 0x6B, 0x6B, 255), tab.channelR);
            ImGui::SameLine(0.0f, 2.0f);
            ChannelButton("##tex_g", "G", IM_COL32(0x6A, 0xD4, 0x6A, 255), tab.channelG);
            ImGui::SameLine(0.0f, 2.0f);
            ChannelButton("##tex_b", "B", IM_COL32(0x6A, 0xA8, 0xFF, 255), tab.channelB);
            ImGui::SameLine(0.0f, 2.0f);
            ChannelButton("##tex_a", "A", IM_COL32(0xDD, 0xDD, 0xDD, 255), tab.channelA);
            cursor.x += EndOverlay(group) + 6.0f;
        }
        {
            OverlayGroup group = BeginOverlay(cursor);
            if (IconButton("##tex_checker", ICON_GRID_3X3, "Checkerboard under transparency", tab.checker, true, 0, kOverlayButton, nullptr, IconSize::Row14))
            {
                tab.checker = !tab.checker;
            }
            ImGui::SameLine(0.0f, 2.0f);
            static const char* const kFilters[] = {"Nearest", "Linear"};
            const int filter = OverlaySegmented("##tex_filter", kFilters, nullptr, 2, tab.nearest ? 0 : 1);
            if ((filter == 0) != tab.nearest)
            {
                tab.nearest = filter == 0;
                tab.nearestChosen = true;
            }
            EndOverlay(group);
        }
        DrawHint(canvasMin, canvasMax, "Drag: pan \xC2\xB7 Wheel: zoom \xC2\xB7 F: fit \xC2\xB7 1: actual size");
    }

    void SceneEditor::DrawTexturePanel(AssetViewerTab& tab)
    {
        const std::filesystem::path absolute = services_.resourceManager->ResolvePath(std::filesystem::path(tab.path));
        DrawPanelTab(ICON_INFO, "Info");
        DrawPanelHeader(tab.type, std::filesystem::path(tab.path).filename().string(), "Texture", nullptr);

        resource::ResourceHandle<resource::TextureAsset> texture;
        const TextureState state = ResolveTexture(*services_.resourceManager, tab.path, texture);
        if (state == TextureState::Ready)
        {
            const render::TextureData& data = texture->asset.data;
            std::string extension = std::filesystem::path(tab.path).extension().string();
            for (char& character : extension)
            {
                character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
            }
            if (!extension.empty() && extension.front() == '.')
            {
                extension.erase(0, 1);
            }
            if (BeginCategory("Texture"))
            {
                InfoRow("Size", std::to_string(data.width) + " \xC3\x97 " + std::to_string(data.height));
                InfoRow("Channels", std::to_string(data.channels));
                InfoRow("sRGB", data.srgb ? "Yes" : "No");
                InfoRow("Alpha", tab.hasAlpha ? "Yes" : "No");
                InfoRow("Source format", extension.empty() ? std::string("-") : extension);
                EndCategory();
            }
        }
        DrawFileCategory(absolute, tab.path, true);
    }

    // ---- Static mesh ----------------------------------------------------------------------------------------

    void SceneEditor::DrawMeshCanvas(AssetViewerTab& tab, const ImVec2& canvasMin, const ImVec2& canvasSize)
    {
        resource::ResourceManager& resources = *services_.resourceManager;
        const auto mesh = resources.Load<resource::MeshAsset>(tab.path);
        if (mesh == nullptr || resources.LoadFailed(tab.path) ||
            (!resources.IsLoadPending(tab.path) && mesh->asset.data.vertices.empty()))
        {
            DrawCannotOpen(canvasMin, canvasSize, tab.path);
            return;
        }
        if (resources.IsLoadPending(tab.path))
        {
            PushFontRole(FontRole::Secondary);
            ImGui::SetCursorScreenPos(ImVec2(canvasMin.x + 16.0f, canvasMin.y + 16.0f));
            ImGui::TextDisabled("Loading...");
            PopFontRole();
            return;
        }

        const ImVec2 canvasMax(canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        DrawViewerBackground(drawList, canvasMin, canvasMax);

        // The bounds of the mesh (local), cached per loaded mesh
        if (tab.infoHandle != mesh->asset.gpuHandle.value)
        {
            const bool firstLoad = tab.infoHandle == 0; // a reload keeps the camera
            tab.infoHandle = mesh->asset.gpuHandle.value;
            editor::BoundsBox box;
            for (const auto& vertex : mesh->asset.data.vertices)
            {
                box.Add(render::Float3{vertex.position.x, vertex.position.y, vertex.position.z});
            }
            tab.localBounds = box;
            if (firstLoad)
            {
                tab.needsFrame = true;
            }
        }

        // The mesh stands on the floor, centred over the origin
        const editor::BoundsBox& local = tab.localBounds;
        const float centerX = (local.min.x + local.max.x) * 0.5f;
        const float centerZ = (local.min.z + local.max.z) * 0.5f;
        const DirectX::XMMATRIX model = DirectX::XMMatrixTranslation(-centerX, -local.min.y, -centerZ);
        editor::BoundsBox world;
        world.min = {local.min.x - centerX, 0.0f, local.min.z - centerZ};
        world.max = {local.max.x - centerX, local.max.y - local.min.y, local.max.z - centerZ};
        if (tab.needsFrame)
        {
            FrameBounds(tab, world);
            tab.needsFrame = false;
        }

        ImGui::SetCursorScreenPos(canvasMin);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##mesh_canvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        if (HandleOrbitInput(tab, ImGui::IsItemHovered(), ImGui::IsItemActive()))
        {
            tab.needsFrame = true;
        }

        // The material of the preview: chosen in the panel, or the suggested one
        const std::string materialPath = tab.previewMaterial.empty() ? ResolveSuggestedMaterialForMesh(tab.path) : tab.previewMaterial;
        editor::LiveViewRequest request;
        request.id = "viewer:mesh:" + tab.path;
        request.width = static_cast<std::uint32_t>(std::max(canvasSize.x, 8.0f));
        request.height = static_cast<std::uint32_t>(std::max(canvasSize.y, 8.0f));
        request.wireframe = tab.wireframe;
        render::DrawItem item;
        if (services_.thumbnails != nullptr &&
            services_.thumbnails->BuildDrawItem(tab.path, materialPath, scene::ToRenderMatrix(model), item) == editor::DrawItemStatus::Ready)
        {
            request.items.push_back(item);
        }
        if (tab.showGrid)
        {
            const float extent = std::max({world.max.x - world.min.x, world.max.z - world.min.z, world.max.y - world.min.y, 1.0f});
            AppendFloorGrid(request.lines, extent);
        }
        if (tab.showBounds)
        {
            AppendBox(request.lines, world, core::Color{0.94f, 0.78f, 0.45f, 1.0f}, true);
        }
        BuildOrbitCamera(tab, canvasSize.x / std::max(canvasSize.y, 1.0f), request.view, request.projection);
        if (services_.thumbnails != nullptr)
        {
            const render::TextureHandle texture = services_.thumbnails->SubmitLiveView(std::move(request));
            if (texture.IsValid())
            {
                drawList->AddImage(static_cast<ImTextureID>(texture.value), canvasMin, canvasMax);
            }
        }

        // Overlay: Lit | Wireframe, the floor and the bounds on the left, Reset camera on the right
        ImVec2 cursor(canvasMin.x + 8.0f, canvasMin.y + 8.0f);
        {
            OverlayGroup group = BeginOverlay(cursor);
            static const char* const kModes[] = {"Lit", "Wireframe"};
            static const char* const kModeIcons[] = {ICON_SUN, ICON_BOX};
            const int mode = OverlaySegmented("##mesh_mode", kModes, kModeIcons, 2, tab.wireframe ? 1 : 0);
            tab.wireframe = mode == 1;
            cursor.x += EndOverlay(group) + 6.0f;
        }
        {
            OverlayGroup group = BeginOverlay(cursor);
            if (IconButton("##mesh_grid", ICON_GRID_3X3, "Floor grid", tab.showGrid, true, 0, kOverlayButton, nullptr, IconSize::Row14))
            {
                tab.showGrid = !tab.showGrid;
            }
            ImGui::SameLine(0.0f, 2.0f);
            if (IconButton("##mesh_bounds", ICON_SQUARE_DASHED, "Bounds", tab.showBounds, true, 0, kOverlayButton, nullptr, IconSize::Row14))
            {
                tab.showBounds = !tab.showBounds;
            }
            EndOverlay(group);
        }
        {
            OverlayGroup group = BeginOverlay(ImVec2(canvasMax.x - 8.0f - kOverlayButton - 4.0f, canvasMin.y + 8.0f));
            if (IconButton("##mesh_frame", ICON_ROTATE_3D, "Frame the mesh", false, true, 0, kOverlayButton, "F", IconSize::Row14))
            {
                tab.needsFrame = true;
                tab.yaw = 0.55f;
                tab.pitch = 0.32f;
            }
            EndOverlay(group);
        }
        DrawHint(canvasMin, canvasMax, "LMB: orbit \xC2\xB7 RMB/MMB: pan \xC2\xB7 Wheel: zoom \xC2\xB7 F: frame");
    }

    void SceneEditor::DrawMeshPanel(AssetViewerTab& tab)
    {
        const std::filesystem::path absolute = services_.resourceManager->ResolvePath(std::filesystem::path(tab.path));
        DrawPanelTab(ICON_INFO, "Info");
        DrawPanelHeader(tab.type, std::filesystem::path(tab.path).filename().string(), "Mesh", nullptr);

        const auto mesh = services_.resourceManager->Load<resource::MeshAsset>(tab.path);
        if (mesh != nullptr && !services_.resourceManager->IsLoadPending(tab.path) && !services_.resourceManager->LoadFailed(tab.path) &&
            !mesh->asset.data.vertices.empty())
        {
            if (BeginCategory("Mesh"))
            {
                InfoRow("Vertices", std::to_string(mesh->asset.data.vertices.size()));
                InfoRow("Triangles", std::to_string(mesh->asset.data.indices.size() / 3));
                char bounds[96];
                const editor::BoundsBox& box = tab.localBounds;
                std::snprintf(bounds, sizeof(bounds), "%.2f \xC3\x97 %.2f \xC3\x97 %.2f", box.max.x - box.min.x, box.max.y - box.min.y, box.max.z - box.min.z);
                InfoRow("Bounds", bounds);
                EndCategory();
            }
            if (BeginCategory("Preview"))
            {
                const std::string suggested = ResolveSuggestedMaterialForMesh(tab.path);
                const std::string current = tab.previewMaterial.empty() ? suggested : tab.previewMaterial;
                const auto materialKeys = services_.resourceManager->GetKnownMaterialKeys();
                if (BeginPropertyGrid("##mesh_preview"))
                {
                    PropertyLabel("Material", false, style::kPickerRowHeight);
                    AssetPickerOptions options = MakePickerOptions(PickerKind::Material, *services_.resourceManager, services_.thumbnails);
                    const auto baseMeta = options.meta;
                    const bool isSuggested = tab.previewMaterial.empty();
                    options.meta = [baseMeta, isSuggested](const std::string& key)
                    {
                        return isSuggested ? std::string("Suggested") : baseMeta(key);
                    };
                    std::string chosen;
                    if (AssetPicker("##preview_material", current, materialKeys, options, chosen))
                    {
                        tab.previewMaterial = chosen; // the preview only: no asset is changed
                    }
                    EndPropertyGrid();
                }
                EndCategory();
            }
        }
        DrawFileCategory(absolute, tab.path, false);
    }

    // ---- Material -------------------------------------------------------------------------------------------

    void SceneEditor::DrawMaterialCanvas(AssetViewerTab& tab, const ImVec2& canvasMin, const ImVec2& canvasSize)
    {
        if (services_.resourceManager->Load<resource::MaterialAsset>(tab.path) == nullptr || services_.resourceManager->LoadFailed(tab.path))
        {
            DrawCannotOpen(canvasMin, canvasSize, tab.path);
            return;
        }

        ImGui::SetCursorScreenPos(canvasMin);
        DrawMaterialPreviewView(
            services_.thumbnails, ("viewer:material:" + tab.path).c_str(), tab.path, canvasSize, 0.0f, tab.materialView, nullptr, false);

        // Overlay: Sphere | Cube, Lit | Wireframe, Reset camera on the right
        const ImVec2 canvasMax(canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y);
        ImVec2 cursor(canvasMin.x + 8.0f, canvasMin.y + 8.0f);
        {
            OverlayGroup group = BeginOverlay(cursor);
            static const char* const kShapes[] = {"Sphere", "Cube"};
            static const char* const kShapeIcons[] = {ICON_CIRCLE, ICON_BOX};
            tab.materialView.cube = OverlaySegmented("##material_shape", kShapes, kShapeIcons, 2, tab.materialView.cube ? 1 : 0) == 1;
            cursor.x += EndOverlay(group) + 6.0f;
        }
        {
            OverlayGroup group = BeginOverlay(cursor);
            static const char* const kModes[] = {"Lit", "Wireframe"};
            static const char* const kModeIcons[] = {ICON_SUN, ICON_BOX};
            tab.materialView.wireframe = OverlaySegmented("##material_mode", kModes, kModeIcons, 2, tab.materialView.wireframe ? 1 : 0) == 1;
            EndOverlay(group);
        }
        {
            OverlayGroup group = BeginOverlay(ImVec2(canvasMax.x - 8.0f - kOverlayButton - 4.0f, canvasMin.y + 8.0f));
            if (IconButton("##material_reset", ICON_ROTATE_3D, "Reset camera", false, true, 0, kOverlayButton, nullptr, IconSize::Row14))
            {
                tab.materialView.yaw = 0.61f;
                tab.materialView.pitch = 0.44f;
                tab.materialView.distance = 3.0f;
            }
            EndOverlay(group);
        }
        DrawHint(canvasMin, canvasMax, "LMB: orbit \xC2\xB7 Wheel: zoom");
    }

    void SceneEditor::DrawMaterialPanel(AssetViewerTab& tab)
    {
        DrawPanelTab(ICON_SLIDERS_HORIZONTAL, "Details");
        DrawPanelHeader(tab.type, tab.name, tab.path, nullptr);

        auto materialResource = services_.resourceManager->Load<resource::MaterialAsset>(tab.path);
        if (materialResource == nullptr || services_.resourceManager->LoadFailed(tab.path))
        {
            DrawFileCategory(services_.resourceManager->ResolvePath(std::filesystem::path(tab.path)), tab.path, true);
            return;
        }
        auto& state = core::ServiceLocator::GetEditorRuntimeState();
        const bool editEnabled = state.mode == editor::RuntimeMode::Edit;
        if (!editEnabled)
        {
            Banner(BannerKind::Play, "Playing - edit the material after Stop.");
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
        PushFontRole(FontRole::Secondary);
        ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextDim));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 8.0f);
        ImGui::TextUnformatted(ICON_INFO "  Edits apply live to every entity using this material.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        PopFontRole();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        DrawMaterialCategory(tab.path, editEnabled);

        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f);
        if (Button("Show in Material Editor", ICON_PALETTE))
        {
            state.showMaterialEditor = true;
            pinnedMaterialPath_ = tab.path;
            pinnedMaterialEntity_ = state.selectedEntity;
            ImGui::SetWindowFocus(kMaterialEditorWindowName);
        }
    }

    // ---- Prefab ---------------------------------------------------------------------------------------------

    void SceneEditor::DrawPrefabCanvas(AssetViewerTab& tab, const ImVec2& canvasMin, const ImVec2& canvasSize)
    {
        if (services_.thumbnails == nullptr)
        {
            DrawCannotOpen(canvasMin, canvasSize, tab.path);
            return;
        }
        // Read on open and again when the file changes on disk (the camera stays where it is)
        const double now = ImGui::GetTime();
        if (!tab.prefabRead || now - tab.prefabCheckTime > 0.5)
        {
            tab.prefabCheckTime = now;
            std::error_code error;
            const auto stamp = std::filesystem::last_write_time(services_.resourceManager->ResolvePath(std::filesystem::path(tab.path)), error);
            if (!tab.prefabRead || (!error && stamp != tab.prefabWriteTime))
            {
                tab.prefabRead = true;
                tab.prefabWriteTime = error ? std::filesystem::file_time_type{} : stamp;
                services_.thumbnails->ReadPrefab(tab.path, tab.prefabEntities);
                if (tab.selectedEntity >= static_cast<int>(tab.prefabEntities.size()))
                {
                    tab.selectedEntity = -1;
                }
            }
        }
        if (tab.prefabEntities.empty())
        {
            DrawCannotOpen(canvasMin, canvasSize, tab.path);
            return;
        }

        const ImVec2 canvasMax(canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        DrawViewerBackground(drawList, canvasMin, canvasMax);

        // All the meshes, with the box of each (for the selection) and the common one
        editor::LiveViewRequest request;
        request.id = "viewer:prefab:" + tab.path;
        request.width = static_cast<std::uint32_t>(std::max(canvasSize.x, 8.0f));
        request.height = static_cast<std::uint32_t>(std::max(canvasSize.y, 8.0f));
        request.wireframe = tab.wireframe;
        std::vector<editor::BoundsBox> entityBounds(tab.prefabEntities.size());
        std::vector<render::DrawItem> items;
        editor::BoundsBox common;
        for (std::size_t index = 0; index < tab.prefabEntities.size(); ++index)
        {
            const editor::PrefabEntityInfo& entity = tab.prefabEntities[index];
            if (!entity.hasMesh || !entity.visible || entity.materialPath.empty())
            {
                continue;
            }
            render::DrawItem item;
            if (services_.thumbnails->BuildDrawItem(entity.meshPath, entity.materialPath, entity.world, item, &entityBounds[index]) ==
                editor::DrawItemStatus::Ready)
            {
                items.push_back(item);
                common.Add(entityBounds[index]);
            }
        }

        // On the floor: everything is lifted so that the lowest point is at y = 0
        const float lift = common.IsValid() ? -common.min.y : 0.0f;
        const DirectX::XMMATRIX liftMatrix = DirectX::XMMatrixTranslation(0.0f, lift, 0.0f);
        for (render::DrawItem& item : items)
        {
            const DirectX::XMMATRIX moved = DirectX::XMMatrixMultiply(scene::ToDirectXMatrix(item.model), liftMatrix);
            item.model = scene::ToRenderMatrix(moved);
            request.items.push_back(item);
        }
        editor::BoundsBox lifted;
        if (common.IsValid())
        {
            lifted.min = {common.min.x, 0.0f, common.min.z};
            lifted.max = {common.max.x, common.max.y + lift, common.max.z};
        }
        if (tab.needsFrame && lifted.IsValid())
        {
            FrameBounds(tab, lifted);
            tab.needsFrame = false;
        }

        ImGui::SetCursorScreenPos(canvasMin);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##prefab_canvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        if (HandleOrbitInput(tab, ImGui::IsItemHovered(), ImGui::IsItemActive()))
        {
            tab.needsFrame = true;
        }

        if (tab.showGrid)
        {
            const float extent = lifted.IsValid()
                ? std::max({lifted.max.x - lifted.min.x, lifted.max.z - lifted.min.z, lifted.max.y - lifted.min.y, 1.0f})
                : 4.0f;
            AppendFloorGrid(request.lines, extent);
        }
        if (tab.showBounds && lifted.IsValid())
        {
            AppendBox(request.lines, lifted, core::Color{0.94f, 0.78f, 0.45f, 1.0f}, true);
        }
        // The selected entity: its box, always
        if (tab.selectedEntity >= 0 && tab.selectedEntity < static_cast<int>(entityBounds.size()) &&
            entityBounds[static_cast<std::size_t>(tab.selectedEntity)].IsValid())
        {
            editor::BoundsBox box = entityBounds[static_cast<std::size_t>(tab.selectedEntity)];
            box.min.y += lift;
            box.max.y += lift;
            AppendBox(request.lines, box, core::Color{0.30f, 0.64f, 1.0f, 1.0f}, false);
        }
        BuildOrbitCamera(tab, canvasSize.x / std::max(canvasSize.y, 1.0f), request.view, request.projection);
        const render::TextureHandle texture = services_.thumbnails->SubmitLiveView(std::move(request));
        if (texture.IsValid())
        {
            drawList->AddImage(static_cast<ImTextureID>(texture.value), canvasMin, canvasMax);
        }

        ImVec2 cursor(canvasMin.x + 8.0f, canvasMin.y + 8.0f);
        {
            OverlayGroup group = BeginOverlay(cursor);
            static const char* const kModes[] = {"Lit", "Wireframe"};
            static const char* const kModeIcons[] = {ICON_SUN, ICON_BOX};
            const int mode = OverlaySegmented("##prefab_mode", kModes, kModeIcons, 2, tab.wireframe ? 1 : 0);
            tab.wireframe = mode == 1;
            cursor.x += EndOverlay(group) + 6.0f;
        }
        {
            OverlayGroup group = BeginOverlay(cursor);
            if (IconButton("##prefab_grid", ICON_GRID_3X3, "Floor grid", tab.showGrid, true, 0, kOverlayButton, nullptr, IconSize::Row14))
            {
                tab.showGrid = !tab.showGrid;
            }
            ImGui::SameLine(0.0f, 2.0f);
            if (IconButton("##prefab_bounds", ICON_SQUARE_DASHED, "Bounds", tab.showBounds, true, 0, kOverlayButton, nullptr, IconSize::Row14))
            {
                tab.showBounds = !tab.showBounds;
            }
            EndOverlay(group);
        }
        {
            OverlayGroup group = BeginOverlay(ImVec2(canvasMax.x - 8.0f - kOverlayButton - 4.0f, canvasMin.y + 8.0f));
            if (IconButton("##prefab_frame", ICON_ROTATE_3D, "Frame the prefab", false, true, 0, kOverlayButton, "F", IconSize::Row14))
            {
                tab.needsFrame = true;
                tab.yaw = 0.55f;
                tab.pitch = 0.32f;
            }
            EndOverlay(group);
        }
        DrawHint(canvasMin, canvasMax, "LMB: orbit \xC2\xB7 RMB/MMB: pan \xC2\xB7 Wheel: zoom \xC2\xB7 F: frame");
    }

    void SceneEditor::DrawPrefabPanel(AssetViewerTab& tab)
    {
        DrawPanelTab(ICON_LIST_TREE, "Entities");
        const std::size_t count = tab.prefabEntities.size();
        if (count == 0)
        {
            DrawPanelHeader(tab.type, tab.name, "Prefab", nullptr);
        }
        else
        {
            DrawPanelHeader(
                tab.type, tab.name, "Prefab \xC2\xB7 " + std::to_string(count) + (count == 1 ? " entity" : " entities"), "Read-only");
        }

        if (tab.prefabEntities.empty())
        {
            DrawFileCategory(services_.resourceManager->ResolvePath(std::filesystem::path(tab.path)), tab.path, true);
            return;
        }

        // The tree: roots first, children below their parent
        const float treeHeight = std::max(ImGui::GetContentRegionAvail().y * 0.5f - 40.0f, 80.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, style::ToVec4(style::kRecessed));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 4.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##prefab_tree", ImVec2(0.0f, treeHeight), ImGuiChildFlags_None);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        {
            std::set<long long> ids;
            for (const auto& entity : tab.prefabEntities)
            {
                ids.insert(entity.id);
            }

            const bool panelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            const auto drawRow = [&](const std::size_t index, const int depth)
            {
                const editor::PrefabEntityInfo& entity = tab.prefabEntities[index];
                ImGui::PushID(static_cast<int>(index));
                const float width = ImGui::GetContentRegionAvail().x;
                const ImVec2 min = ImGui::GetCursorScreenPos();
                const bool clicked = ImGui::InvisibleButton("##entity", ImVec2(width, style::kRowHeight));
                const bool hovered = ImGui::IsItemHovered();
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                const bool selected = tab.selectedEntity == static_cast<int>(index);
                if (selected)
                {
                    drawList->AddRectFilled(
                        min, ImVec2(min.x + width, min.y + style::kRowHeight), panelFocused ? style::kPrimary : style::kSelectUnfocused);
                }
                else if (hovered)
                {
                    drawList->AddRectFilled(min, ImVec2(min.x + width, min.y + style::kRowHeight), style::kHeader);
                }
                const float indent = 10.0f + depth * 16.0f;
                DrawIcon(drawList, IconSize::Row14, entity.hasMesh ? ICON_BOX : ICON_CIRCLE_DASHED,
                    ImVec2(min.x + indent + 7.0f, min.y + style::kRowHeight * 0.5f), entity.hasMesh ? style::kTypeMesh : style::kTextDim);
                PushFontRole(FontRole::Body);
                drawList->AddText(ImVec2(min.x + indent + 22.0f, std::floor(min.y + (style::kRowHeight - ImGui::GetFontSize()) * 0.5f)),
                    style::kTextStrong, entity.name.empty() ? "Entity" : entity.name.c_str());
                PopFontRole();
                ImGui::PopID();
                if (clicked)
                {
                    tab.selectedEntity = static_cast<int>(index);
                }
            };

            std::function<void(long long, int)> drawChildren = [&](const long long parent, const int depth)
            {
                for (std::size_t index = 0; index < tab.prefabEntities.size(); ++index)
                {
                    if (tab.prefabEntities[index].parent != parent)
                    {
                        continue;
                    }
                    drawRow(index, depth);
                    drawChildren(tab.prefabEntities[index].id, depth + 1);
                }
            };

            // Roots: no parent, or a parent that is not in the file
            for (std::size_t index = 0; index < tab.prefabEntities.size(); ++index)
            {
                const editor::PrefabEntityInfo& entity = tab.prefabEntities[index];
                if (entity.parent >= 0 && ids.count(entity.parent) != 0)
                {
                    continue;
                }
                drawRow(index, 0);
                drawChildren(entity.id, 1);
            }
        }
        ImGui::EndChild();

        // The selected entity: a read-only summary of its components
        if (tab.selectedEntity >= 0 && tab.selectedEntity < static_cast<int>(tab.prefabEntities.size()))
        {
            const editor::PrefabEntityInfo& entity = tab.prefabEntities[static_cast<std::size_t>(tab.selectedEntity)];
            if (BeginCategory(entity.name.empty() ? "Entity" : entity.name.c_str()))
            {
                if (entity.hasMesh)
                {
                    InfoRow("Mesh", std::filesystem::path(entity.meshPath).filename().string());
                    InfoRow("Material", AssetNameWithoutExtensions(entity.materialPath));
                }
                if (entity.hasRigidbody)
                {
                    InfoRow("Rigidbody", entity.rigidbody.empty() ? std::string("Yes") : entity.rigidbody);
                }
                if (!entity.collider.empty())
                {
                    InfoRow("Collider", entity.collider);
                }
                if (!entity.script.empty())
                {
                    InfoRow("Script", entity.script);
                }
                if (!entity.hasMesh && !entity.hasRigidbody && entity.collider.empty() && entity.script.empty())
                {
                    InfoRow("Components", "Transform only");
                }
                EndCategory();
            }
        }

        // Edit props: in the Prefabs panel, as decided
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f);
        if (Button("Edit Props in Prefabs Panel", ICON_PACKAGE, true, ImGui::GetContentRegionAvail().x - 24.0f))
        {
            const std::string name = AssetNameWithoutExtensions(std::filesystem::path(tab.path).filename().string());
            auto& state = core::ServiceLocator::GetEditorRuntimeState();
            state.showPrefabs = true;
            if (services_.prefabLibrary != nullptr && prefabInspector_ != nullptr)
            {
                prefabInspector_->Select(*services_.prefabLibrary, name);
            }
            ImGui::SetWindowFocus(kPrefabsWindowName);
        }
    }
}
