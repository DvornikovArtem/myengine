// PrefabInspector.cpp

#include <myengine/ui/PrefabInspector.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include <imgui/imgui.h>

#include <myengine/editor/ThumbnailService.h>
#include <myengine/scripting/PrefabLibrary.h>
#include <myengine/ui/ScriptInspector.h>

#include "editor/EditorWidgets.h"

namespace myengine::ui
{
    namespace
    {
        // Spec 4.8 "No prefabs"
        void DrawNoPrefabs()
        {
            EmptyState(ICON_PACKAGE, "No prefabs", "Add .prefab.json files to assets/prefabs.");
        }

        // A square with the thumbnail of a template (or the package icon while it is not ready)
        void DrawPrefabThumbnail(ImDrawList* drawList, const ImVec2 min, const float size, editor::ThumbnailService* thumbnails, const std::string& path)
        {
            drawList->AddRectFilled(min, ImVec2(min.x + size, min.y + size), IM_COL32(0x12, 0x12, 0x12, 255), 4.0f);
            if (thumbnails != nullptr)
            {
                const editor::Thumbnail thumbnail = thumbnails->Request(path, size > 40.0f ? 128 : 64);
                if (thumbnail.ready)
                {
                    drawList->AddImage(static_cast<ImTextureID>(thumbnail.ImGuiTextureId()), min, ImVec2(min.x + size, min.y + size));
                    return;
                }
            }
            DrawIcon(
                drawList,
                size >= 40.0f ? IconSize::Tile40 : IconSize::Row14,
                ICON_PACKAGE,
                ImVec2(min.x + size * 0.5f, min.y + size * 0.5f),
                IM_COL32(0x3D, 0x3D, 0x3D, 255));
        }
    }

    void PrefabInspector::Clear()
    {
        selectedName_.clear();
        original_ = nullptr;
        draft_ = nullptr;
        lastError_.clear();
        conflict_ = false;
    }

    bool PrefabInspector::Select(scene::PrefabLibrary& library, const std::string& name)
    {
        if (name == selectedName_)
        {
            return !draft_.is_null();
        }
        if (IsDirty())
        {
            lastError_ = "Save or Reload the current template before selecting another one.";
            return false;
        }
        selectedName_ = name;
        original_ = nullptr;
        draft_ = nullptr;
        return Reload(library);
    }

    bool PrefabInspector::Reload(scene::PrefabLibrary& library)
    {
        if (selectedName_.empty())
        {
            return false;
        }
        const auto* prefab = library.ReloadPrefab(selectedName_);
        if (prefab == nullptr)
        {
            lastError_ = library.GetLastError();
            conflict_ = true;
            return false;
        }
        original_ = *prefab;
        draft_ = original_;
        conflict_ = false;
        lastError_.clear();
        return true;
    }

    void PrefabInspector::Refresh(scene::PrefabLibrary& library)
    {
        if (selectedName_.empty() || draft_.is_null() || conflict_)
        {
            return;
        }
        const auto* prefab = library.GetPrefabJson(selectedName_);
        if (prefab == nullptr)
        {
            lastError_ = library.GetLastError();
            conflict_ = true;
        }
        else if (*prefab != original_)
        {
            if (IsDirty())
            {
                lastError_ = "The file changed outside the panel. Reload to discard the draft and read the new version.";
                conflict_ = true;
            }
            else
            {
                original_ = *prefab;
                draft_ = original_;
                lastError_.clear();
            }
        }
    }

    bool PrefabInspector::Save(scene::PrefabLibrary& library)
    {
        if (draft_.is_null() || conflict_ || !IsDirty())
        {
            return false;
        }
        // Check disk even if the Streaming watcher has not delivered the change yet.
        if (library.ReloadPrefab(selectedName_) == nullptr)
        {
            lastError_ = library.GetLastError();
            conflict_ = true;
            return false;
        }
        Refresh(library);
        if (conflict_)
        {
            return false;
        }
        if (!library.SavePrefab(selectedName_, draft_))
        {
            lastError_ = library.GetLastError();
            return false;
        }
        original_ = draft_;
        lastError_.clear();
        return true;
    }

    void PrefabInspector::Draw(scene::PrefabLibrary& library, const DescribeFields& describeFields)
    {
        const auto names = library.ListPrefabs();
        if (selectedName_.empty() && !names.empty())
        {
            Select(library, names.front());
        }
        Refresh(library);

        if (names.empty() && selectedName_.empty())
        {
            DrawNoPrefabs();
            return;
        }

        // ---- Header: the template thumbnail (64) on the left; the combo and, under it, the path with the
        // Saved / Unsaved chip on the right
        constexpr float kThumb = 64.0f;
        const float headerLeft = kThumb + 8.0f;
        {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float available = ImGui::GetContentRegionAvail().x;
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const std::string path = "assets/prefabs/" + selectedName_ + ".prefab.json";

            DrawPrefabThumbnail(drawList, start, kThumb, selectedName_.empty() ? nullptr : thumbnails_, path);
            ImGui::InvisibleButton("##prefab_thumbnail", ImVec2(kThumb, kThumb));
            if (!selectedName_.empty() && thumbnails_ != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            {
                ImGui::BeginTooltip();
                const ImVec2 imageMin = ImGui::GetCursorScreenPos();
                DrawPrefabThumbnail(ImGui::GetWindowDrawList(), imageMin, 256.0f, thumbnails_, path);
                ImGui::Dummy(ImVec2(256.0f, 256.0f));
                PushFontRole(FontRole::Strong);
                ImGui::TextUnformatted(selectedName_.c_str());
                PopFontRole();
                PushFontRole(FontRole::Secondary);
                ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextDim));
                ImGui::TextUnformatted(path.c_str());
                ImGui::PopStyleColor();
                PopFontRole();
                ImGui::EndTooltip();
            }

            // Keep unsaved values when changing templates: explicitly Save or Reload first.
            ImGui::SetCursorScreenPos(ImVec2(start.x + headerLeft, start.y + 4.0f));
            ImGui::BeginDisabled(IsDirty());
            const bool hasViewerButton = openViewer_ != nullptr && !selectedName_.empty();
            ImGui::SetNextItemWidth(std::max(available - headerLeft - (hasViewerButton ? style::kPanelIconButton + 4.0f : 0.0f), 40.0f));
            if (BeginCombo("##prefab", selectedName_.empty() ? "Select a template" : selectedName_.c_str()))
            {
                for (const auto& name : names)
                {
                    // A row with a 20 px preview of the template
                    ImGui::PushID(name.c_str());
                    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                    const float rowWidth = std::max(ImGui::GetContentRegionAvail().x, 60.0f);
                    const bool picked = ImGui::Selectable("##prefab_item", name == selectedName_, 0, ImVec2(rowWidth, 26.0f));
                    ImDrawList* rowDraw = ImGui::GetWindowDrawList();
                    DrawPrefabThumbnail(rowDraw, ImVec2(rowMin.x + 4.0f, rowMin.y + 3.0f), 20.0f, thumbnails_,
                        "assets/prefabs/" + name + ".prefab.json");
                    PushFontRole(FontRole::Body);
                    rowDraw->AddText(ImVec2(rowMin.x + 32.0f, rowMin.y + (26.0f - ImGui::GetFontSize()) * 0.5f), style::kTextStrong, name.c_str());
                    PopFontRole();
                    ImGui::PopID();
                    if (picked)
                    {
                        Select(library, name);
                    }
                }
                EndCombo();
            }
            ImGui::EndDisabled();

            if (hasViewerButton)
            {
                ImGui::SetCursorScreenPos(ImVec2(start.x + available - style::kPanelIconButton, start.y + 3.0f));
                if (IconButton("##open_prefab_viewer", ICON_EXTERNAL_LINK, "Open in Prefab Viewer", false, true, 0,
                        style::kPanelIconButton, nullptr, IconSize::Row14))
                {
                    openViewer_(path);
                }
            }

            if (!selectedName_.empty())
            {
                const bool dirty = IsDirty();
                const float lineY = start.y + 4.0f + style::kFrameHeight + 8.0f;

                PushFontRole(FontRole::Secondary);
                ImFont* font = ImGui::GetFont();
                const float size = ImGui::GetFontSize();
                PopFontRole();
                PushFontRole(FontRole::Tiny);
                const float chipWidth = ImGui::CalcTextSize(dirty ? "Unsaved" : "Saved").x + 16.0f + (dirty ? 12.0f : 0.0f);
                PopFontRole();

                drawList->PushClipRect(
                    ImVec2(start.x + headerLeft, lineY),
                    ImVec2(start.x + std::max(available - chipWidth - 8.0f, headerLeft + 20.0f), lineY + 20.0f),
                    true);
                drawList->AddText(font, size, ImVec2(start.x + headerLeft + 2.0f, lineY + 2.0f), style::kTextDim, path.c_str());
                drawList->PopClipRect();

                ImGui::SetCursorScreenPos(ImVec2(start.x + available - chipWidth, lineY + 1.0f));
                Chip(dirty ? "Unsaved" : "Saved", dirty ? ChipKind::Amber : ChipKind::Gray, dirty);
            }

            ImGui::SetCursorScreenPos(start);
            ImGui::Dummy(ImVec2(available, kThumb + 8.0f));
        }

        if (selectedName_.empty())
        {
            DrawNoPrefabs();
            return;
        }

        // Reload (secondary) and Save (primary), right aligned
        {
            const bool canSave = IsDirty() && !conflict_;
            const float reloadWidth = 84.0f;
            const float saveWidth = 74.0f;
            const float available = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(available - reloadWidth - saveWidth - 8.0f, 0.0f));
            if (Button("Reload", ICON_ROTATE_CCW, true, reloadWidth))
            {
                Reload(library);
            }
            if (ImGui::IsItemHovered())
            {
                Tooltip("Discard unsaved props and load the current template.");
            }
            ImGui::SameLine(0.0f, 8.0f);
            if (PrimaryButton("Save", ICON_SAVE, canSave, saveWidth))
            {
                Save(library);
            }
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
        }

        // A conflict (the file changed on disk) is a warning with the text the panel already has; any other
        // problem (a failed save, a template that does not load) is an error
        if (!lastError_.empty())
        {
            Banner(conflict_ ? BannerKind::Warning : BannerKind::Error, lastError_.c_str());
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
        }
        if (draft_.is_null())
        {
            return;
        }

        // One explanatory line, not a paragraph
        {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float available = ImGui::GetContentRegionAvail().x;
            PushFontRole(FontRole::Secondary);
            ImFont* font = ImGui::GetFont();
            const float size = ImGui::GetFontSize();
            const float wrapWidth = std::max(available - 24.0f, 40.0f);
            const ImVec2 textSize = ImGui::CalcTextSize("Saved props apply to the next spawn, also in Play.", nullptr, false, wrapWidth);
            PopFontRole();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            DrawIcon(drawList, IconSize::Row14, ICON_INFO, ImVec2(start.x + 7.0f, start.y + 9.0f), style::kTextDim);
            drawList->AddText(font, size, ImVec2(start.x + 22.0f, start.y + 2.0f), style::kTextDim,
                "Saved props apply to the next spawn, also in Play.", nullptr, wrapWidth);
            ImGui::Dummy(ImVec2(available, std::max(textSize.y + 8.0f, 22.0f)));
        }

        bool foundScripts = false;
        ImGui::BeginDisabled(conflict_);
        for (auto& entity : draft_.at("entities"))
        {
            if (!entity.contains("Script"))
            {
                continue;
            }
            auto& scripts = entity.at("Script").at("scripts");
            if (scripts.empty())
            {
                continue;
            }
            foundScripts = true;
            const auto id = entity.at("id").get<std::uint32_t>();
            const auto tag = entity.value("Tag", nlohmann::json::object());
            const auto name = tag.value("name", std::string("Entity"));
            ImGui::PushID(static_cast<int>(id));
            const bool open = BeginCategory(name.c_str());
            if (open)
            {
                for (std::size_t index = 0; index < scripts.size(); ++index)
                {
                    auto& entry = scripts[index];
                    const auto module = entry.value("module", std::string());
                    const auto className = entry.value("class", std::string());
                    ImGui::PushID(static_cast<int>(index));
                    if (module.empty() || className.empty())
                    {
                        ImGui::TextDisabled("Script requires a module and class name in the template.");
                        ImGui::PopID();
                        continue;
                    }
                    DrawScriptHeader(module, className);
                    const auto fields = describeFields ? describeFields(module, className) : std::vector<scripting::ScriptFieldInfo>{};
                    if (fields.empty())
                    {
                        ImGui::Indent(20.0f);
                        ImGui::TextDisabled("No editable fields, or the script failed to load (see the log).");
                        ImGui::Unindent(20.0f);
                    }
                    else
                    {
                        // Looking at defaults must not add an empty props key or mark the draft dirty.
                        auto props = entry.value("props", nlohmann::json::object());
                        if (DrawScriptFields(fields, props))
                        {
                            entry["props"] = std::move(props);
                        }
                    }
                    ImGui::Dummy(ImVec2(0.0f, 4.0f));
                    ImGui::PopID();
                }
                EndCategory();
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        if (!foundScripts)
        {
            ImGui::TextDisabled("This template has no script behaviours.");
        }
    }

    const std::string& PrefabInspector::GetSelectedName() const { return selectedName_; }
    const std::string& PrefabInspector::GetLastError() const { return lastError_; }
    bool PrefabInspector::IsDirty() const { return !draft_.is_null() && draft_ != original_; }
    bool PrefabInspector::HasConflict() const { return conflict_; }
    nlohmann::json& PrefabInspector::GetDraft() { return draft_; }
}
