// ScriptInspector.cpp

#include <myengine/ui/ScriptInspector.h>

#include <cfloat>
#include <cmath>

#include <imgui/imgui.h>
#include <imgui/misc/imgui_stdlib.h>

#include "editor/EditorWidgets.h"

namespace myengine::ui
{
    namespace
    {
        using FieldType = scripting::ScriptFieldInfo::Type;

        constexpr float kFieldIndent = 32.0f; // nested rows sit deeper than the rows of a category (20)
        constexpr float kHeaderIndent = 20.0f;
        constexpr float kHeaderHeight = 26.0f;

        // A stored value is used only if it has the field's type; otherwise the default is shown (the engine ignores
        // such a value at runtime too and logs a warning)
        bool HasValidValue(const scripting::ScriptFieldInfo& field, const nlohmann::json& props)
        {
            const auto it = props.find(field.name);
            if (it == props.end())
            {
                return false;
            }

            switch (field.type)
            {
            case FieldType::Bool:
                return it->is_boolean();
            case FieldType::Int:
                return it->is_number_integer();
            case FieldType::Float:
                return it->is_number();
            case FieldType::String:
                return it->is_string();
            }
            return false;
        }

        float ChipWidth(const char* text, const bool dot)
        {
            PushFontRole(FontRole::Tiny);
            const float width = ImGui::CalcTextSize(text).x + 16.0f + (dot ? 12.0f : 0.0f);
            PopFontRole();
            return width;
        }
    }

    bool DrawScriptHeader(const std::string& module, const std::string& className, const ScriptHeaderChip chip)
    {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const float available = ImGui::GetContentRegionAvail().x;
        const float centerY = min.y + kHeaderHeight * 0.5f;
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        DrawIcon(drawList, IconSize::Row14, ICON_FILE_CODE, ImVec2(min.x + kHeaderIndent + 7.0f, centerY), style::kTypeScript);

        // The class name is clipped before the chip
        const char* chipText = nullptr;
        ChipKind chipKind = ChipKind::Gray;
        bool chipDot = false;
        switch (chip)
        {
        case ScriptHeaderChip::AppliedOnPlay: chipText = "Applied on Play"; break;
        case ScriptHeaderChip::Active: chipText = "Active"; chipKind = ChipKind::Green; chipDot = true; break;
        case ScriptHeaderChip::Faulted: chipText = "Faulted"; chipKind = ChipKind::Red; chipDot = true; break;
        case ScriptHeaderChip::NotCreated: chipText = "Not created"; break;
        case ScriptHeaderChip::None: break;
        }
        const float chipWidth = chipText != nullptr ? ChipWidth(chipText, chipDot) : 0.0f;
        const char* linkText = "Open log";
        float linkWidth = 0.0f;
        if (chip == ScriptHeaderChip::Faulted)
        {
            PushFontRole(FontRole::Secondary);
            linkWidth = ImGui::CalcTextSize(linkText).x;
            PopFontRole();
        }
        const float rightReserved = (chipText != nullptr ? chipWidth + 12.0f : 0.0f) + (linkWidth > 0.0f ? linkWidth + 10.0f : 0.0f);

        const std::string title = module + "." + className;
        PushFontRole(FontRole::Strong);
        ImFont* titleFont = ImGui::GetFont();
        const float titleSize = ImGui::GetFontSize();
        PopFontRole();
        const float titleX = min.x + kHeaderIndent + 22.0f;
        drawList->PushClipRect(ImVec2(titleX, min.y), ImVec2(std::max(titleX + 24.0f, min.x + available - rightReserved), min.y + kHeaderHeight), true);
        drawList->AddText(titleFont, titleSize, ImVec2(titleX, std::floor(centerY - titleSize * 0.5f)), style::kTextStrong, title.c_str());
        drawList->PopClipRect();

        bool openLog = false;
        if (chipText != nullptr)
        {
            const float chipX = min.x + available - 12.0f - chipWidth;
            ImGui::SetCursorScreenPos(ImVec2(chipX, centerY - 9.0f));
            Chip(chipText, chipKind, chipDot);

            if (linkWidth > 0.0f)
            {
                const float linkX = chipX - 10.0f - linkWidth;
                ImGui::SetCursorScreenPos(ImVec2(linkX, centerY - 9.0f));
                ImGui::PushID("##openlog");
                openLog = ImGui::InvisibleButton("##link", ImVec2(linkWidth, 18.0f));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();

                PushFontRole(FontRole::Secondary);
                ImFont* linkFont = ImGui::GetFont();
                const float linkSize = ImGui::GetFontSize();
                PopFontRole();
                const ImVec2 position(linkX, std::floor(centerY - linkSize * 0.5f));
                drawList->AddText(linkFont, linkSize, position, hovered ? style::kPrimaryHover : style::kLink, linkText);
                if (hovered)
                {
                    drawList->AddLine(
                        ImVec2(position.x, position.y + linkSize + 1.0f),
                        ImVec2(position.x + linkWidth, position.y + linkSize + 1.0f),
                        style::kPrimaryHover,
                        1.0f);
                }
            }
        }

        ImGui::SetCursorScreenPos(min);
        ImGui::Dummy(ImVec2(available, kHeaderHeight));
        return openLog;
    }

    bool DrawScriptFields(const std::vector<scripting::ScriptFieldInfo>& fields, nlohmann::json& props, const ScriptFieldsUndo& undo)
    {
        if (!props.is_object())
        {
            props = nlohmann::json::object();
        }

        if (!BeginPropertyGrid("##script_fields", kFieldIndent))
        {
            return false;
        }

        bool changed = false;
        for (const auto& field : fields)
        {
            ImGui::PushID(field.name.c_str());

            const bool stored = HasValidValue(field, props);
            const nlohmann::json& value = stored ? props[field.name] : field.defaultValue;
            const std::string undoLabel = "Edit Script Field " + field.name;

            // Values that come from the script code are dim: they are not written to the scene
            PropertyLabel(field.name.c_str(), !stored);

            if (!stored)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextDim));
            }

            bool edited = false;
            switch (field.type)
            {
            case FieldType::Float:
            {
                float current = value.is_number() ? value.get<float>() : 0.0f;
                if (ImGui::DragFloat("##value", &current, 0.05f))
                {
                    props[field.name] = current;
                    edited = true;
                }
                FocusOutline();
                if (undo.recordFromItem)
                {
                    undo.recordFromItem(undoLabel.c_str());
                }
                break;
            }
            case FieldType::Int:
            {
                int current = value.is_number_integer() ? value.get<int>() : 0;
                if (ImGui::DragInt("##value", &current, 0.2f))
                {
                    props[field.name] = current;
                    edited = true;
                }
                FocusOutline();
                if (undo.recordFromItem)
                {
                    undo.recordFromItem(undoLabel.c_str());
                }
                break;
            }
            case FieldType::Bool:
            {
                // The shared checkbox is a plain button, so there is no "edited" state to record from:
                // the snapshot is taken before the stored value changes, like for Reset
                bool current = value.is_boolean() && value.get<bool>();
                if (Checkbox("##value", &current))
                {
                    const std::string before = undo.captureBefore ? undo.captureBefore() : std::string();
                    props[field.name] = current;
                    edited = true;
                    if (undo.recordImmediate)
                    {
                        undo.recordImmediate(undoLabel.c_str(), before);
                    }
                }
                break;
            }
            case FieldType::String:
            {
                std::string current = value.is_string() ? value.get<std::string>() : std::string();
                if (ImGui::InputText("##value", &current))
                {
                    props[field.name] = current;
                    edited = true;
                }
                FocusOutline();
                if (undo.recordFromItem)
                {
                    undo.recordFromItem(undoLabel.c_str());
                }
                break;
            }
            }

            if (!stored)
            {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Default from the script code. Edit to store a value in the scene or prefab");
                }
            }
            else if (PropertyReset("Reset to the value from the script code"))
            {
                const std::string before = undo.captureBefore ? undo.captureBefore() : std::string();
                props.erase(field.name);
                edited = true;
                if (undo.recordImmediate)
                {
                    undo.recordImmediate(("Reset Script Field " + field.name).c_str(), before);
                }
            }

            changed = changed || edited;
            ImGui::PopID();
        }

        EndPropertyGrid();
        return changed;
    }

    void DrawScriptFieldValues(const std::vector<scripting::ScriptFieldInfo>& fields, const nlohmann::json& values)
    {
        if (!BeginPropertyGrid("##script_values", kFieldIndent))
        {
            return;
        }

        for (const auto& field : fields)
        {
            ImGui::PushID(field.name.c_str());
            const auto it = values.find(field.name);
            const nlohmann::json& value = it != values.end() ? *it : field.defaultValue;

            PropertyLabel(field.name.c_str());
            ImGui::BeginDisabled();
            switch (field.type)
            {
            case FieldType::Float:
            {
                float current = value.is_number() ? value.get<float>() : 0.0f;
                ImGui::DragFloat("##live", &current);
                break;
            }
            case FieldType::Int:
            {
                int current = value.is_number_integer() ? value.get<int>() : 0;
                ImGui::DragInt("##live", &current);
                break;
            }
            case FieldType::Bool:
            {
                bool current = value.is_boolean() && value.get<bool>();
                Checkbox("##live", &current, false);
                break;
            }
            case FieldType::String:
            {
                std::string current = value.is_string() ? value.get<std::string>() : std::string();
                ImGui::InputText("##live", &current);
                break;
            }
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }

        EndPropertyGrid();
    }
}
