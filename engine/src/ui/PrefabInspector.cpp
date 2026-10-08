// PrefabInspector.cpp

#include <myengine/ui/PrefabInspector.h>

#include <cstdint>
#include <utility>

#include <imgui/imgui.h>

#include <myengine/scripting/PrefabLibrary.h>
#include <myengine/ui/ScriptInspector.h>

namespace myengine::ui
{
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

        // Keep unsaved values when changing templates: explicitly Save or Reload first.
        ImGui::BeginDisabled(IsDirty());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::BeginCombo("##prefab", selectedName_.empty() ? "Select a template" : selectedName_.c_str()))
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

        if (selectedName_.empty())
        {
            ImGui::TextDisabled("No .prefab.json files in assets/prefabs.");
            return;
        }

        ImGui::TextWrapped("assets/prefabs/%s.prefab.json", selectedName_.c_str());
        ImGui::BeginDisabled(!IsDirty() || conflict_);
        if (ImGui::Button("Save"))
        {
            Save(library);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Reload"))
        {
            Reload(library);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Discard unsaved props and load the current template.");
        }
        ImGui::TextWrapped("%s", IsDirty() ? "Unsaved - Save or Reload before switching" : "No unsaved changes");
        ImGui::TextWrapped("Saved props apply to the next spawn, including in Play. Existing entities keep their values.");
        if (!lastError_.empty())
        {
            ImGui::TextWrapped("Error: %s", lastError_.c_str());
        }
        if (draft_.is_null())
        {
            return;
        }

        ImGui::Separator();
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
            const bool open = ImGui::CollapsingHeader((name + "##entity").c_str(), ImGuiTreeNodeFlags_DefaultOpen);
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
                    ImGui::Text("%s.%s", module.c_str(), className.c_str());
                    const auto fields = describeFields ? describeFields(module, className) : std::vector<scripting::ScriptFieldInfo>{};
                    if (fields.empty())
                    {
                        ImGui::TextDisabled("No editable fields, or the script failed to load (see the log).");
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
                    ImGui::PopID();
                }
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
