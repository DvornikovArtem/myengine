// PrefabInspector.cpp

#include <myengine/ui/PrefabInspector.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include <imgui/imgui.h>

#include <myengine/scripting/PrefabLibrary.h>
#include <myengine/ui/ScriptInspector.h>

#include "editor/EditorWidgets.h"

namespace myengine::ui
{
    namespace
    {
        // Spec 4.8 "No prefabs": icon, title and text centred in the panel. Drawn with plain ImGui calls, so it also
        // works in a context that has no editor fonts (headless tests).
        void DrawNoPrefabs()
        {
            const ImVec2 region = ImGui::GetContentRegionAvail();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float centerX = origin.x + region.x * 0.5f;
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            const float topPadding = std::max((region.y - 90.0f) * 0.5f, 8.0f);
            DrawIcon(drawList, IconSize::Empty30, ICON_PACKAGE, ImVec2(centerX, origin.y + topPadding + 15.0f), style::kEmptyStateIcon);

            const char* title = "No prefabs";
            const char* text = "Add .prefab.json files to assets/prefabs.";
            const float lineHeight = ImGui::GetTextLineHeight();
            float y = origin.y + topPadding + 40.0f;
            drawList->AddText(ImVec2(std::floor(centerX - ImGui::CalcTextSize(title).x * 0.5f), y), style::kText, title);
            y += lineHeight + 4.0f;
            drawList->AddText(ImVec2(std::floor(centerX - ImGui::CalcTextSize(text).x * 0.5f), y), style::kTextDim, text);
            ImGui::Dummy(ImVec2(region.x, topPadding + 40.0f + lineHeight * 2.0f + 8.0f));
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

        // ---- Header: a package icon and the template combo; the path and the Saved / Unsaved chip below
        {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float squareSize = 28.0f;
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(start, ImVec2(start.x + squareSize, start.y + squareSize), style::kRecessed, style::kRounding);
            DrawIcon(drawList, IconSize::Row14, ICON_PACKAGE, ImVec2(start.x + squareSize * 0.5f, start.y + squareSize * 0.5f), style::kTypePrefab);

            // Keep unsaved values when changing templates: explicitly Save or Reload first.
            ImGui::SetCursorScreenPos(ImVec2(start.x + squareSize + 8.0f, start.y + (squareSize - style::kFrameHeight) * 0.5f));
            ImGui::BeginDisabled(IsDirty());
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool comboOpen = ImGui::BeginCombo(
                "##prefab",
                selectedName_.empty() ? "Select a template" : selectedName_.c_str(),
                ImGuiComboFlags_NoArrowButton);
            {
                // The same chevron as the asset pickers instead of ImGui's triangle button
                const ImVec2 comboMin = ImGui::GetItemRectMin();
                const ImVec2 comboMax = ImGui::GetItemRectMax();
                DrawIcon(
                    drawList,
                    IconSize::Chevron12,
                    ICON_CHEVRON_DOWN,
                    ImVec2(comboMax.x - 14.0f, (comboMin.y + comboMax.y) * 0.5f),
                    style::kTextDim);
            }
            if (comboOpen)
            {
                for (const auto& name : names)
                {
                    if (ImGui::Selectable(name.c_str(), name == selectedName_))
                    {
                        Select(library, name);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + squareSize + 6.0f));
        }

        if (selectedName_.empty())
        {
            DrawNoPrefabs();
            return;
        }

        // Path (dim) and the state chip on the right
        {
            const std::string path = "assets/prefabs/" + selectedName_ + ".prefab.json";
            const bool dirty = IsDirty();
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float available = ImGui::GetContentRegionAvail().x;

            PushFontRole(FontRole::Secondary);
            ImFont* font = ImGui::GetFont();
            const float size = ImGui::GetFontSize();
            PopFontRole();
            PushFontRole(FontRole::Tiny);
            const float chipWidth = ImGui::CalcTextSize(dirty ? "Unsaved" : "Saved").x + 16.0f + (dirty ? 12.0f : 0.0f);
            PopFontRole();

            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->PushClipRect(start, ImVec2(start.x + std::max(available - chipWidth - 8.0f, 20.0f), start.y + 20.0f), true);
            drawList->AddText(font, size, ImVec2(start.x + 2.0f, start.y + 2.0f), style::kTextDim, path.c_str());
            drawList->PopClipRect();

            ImGui::SetCursorScreenPos(ImVec2(start.x + available - chipWidth, start.y + 1.0f));
            Chip(dirty ? "Unsaved" : "Saved", dirty ? ChipKind::Amber : ChipKind::Gray, dirty);
            ImGui::SetCursorScreenPos(start);
            ImGui::Dummy(ImVec2(available, 22.0f));
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
