// ScriptInspector.cpp

#include <myengine/ui/ScriptInspector.h>

#include <imgui/imgui.h>
#include <imgui/misc/imgui_stdlib.h>

namespace myengine::ui
{
    namespace
    {
        using FieldType = scripting::ScriptFieldInfo::Type;

        // Leaves room for the field name and the Reset button in a narrow inspector
        void SetFieldWidth()
        {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        }

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
    }

    bool DrawScriptFields(const std::vector<scripting::ScriptFieldInfo>& fields, nlohmann::json& props, const ScriptFieldsUndo& undo)
    {
        if (!props.is_object())
        {
            props = nlohmann::json::object();
        }

        bool changed = false;
        for (const auto& field : fields)
        {
            ImGui::PushID(field.name.c_str());

            const bool stored = HasValidValue(field, props);
            const nlohmann::json& value = stored ? props[field.name] : field.defaultValue;
            const std::string label = field.name + "##value";
            const std::string undoLabel = "Edit Script Field " + field.name;

            // Values that come from the script code are greyed out: they are not written to the scene
            if (!stored)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }

            bool edited = false;
            SetFieldWidth();
            switch (field.type)
            {
            case FieldType::Float:
            {
                float current = value.is_number() ? value.get<float>() : 0.0f;
                if (ImGui::DragFloat(label.c_str(), &current, 0.05f))
                {
                    props[field.name] = current;
                    edited = true;
                }
                break;
            }
            case FieldType::Int:
            {
                int current = value.is_number_integer() ? value.get<int>() : 0;
                if (ImGui::DragInt(label.c_str(), &current, 0.2f))
                {
                    props[field.name] = current;
                    edited = true;
                }
                break;
            }
            case FieldType::Bool:
            {
                bool current = value.is_boolean() && value.get<bool>();
                if (ImGui::Checkbox(label.c_str(), &current))
                {
                    props[field.name] = current;
                    edited = true;
                }
                break;
            }
            case FieldType::String:
            {
                std::string current = value.is_string() ? value.get<std::string>() : std::string();
                if (ImGui::InputText(label.c_str(), &current))
                {
                    props[field.name] = current;
                    edited = true;
                }
                break;
            }
            }

            if (undo.recordFromItem)
            {
                undo.recordFromItem(undoLabel.c_str());
            }

            if (!stored)
            {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Default from the script code. Edit to store a value in the scene or prefab");
                }
            }
            else
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset"))
                {
                    const std::string before = undo.captureBefore ? undo.captureBefore() : std::string();
                    props.erase(field.name);
                    edited = true;
                    if (undo.recordImmediate)
                    {
                        undo.recordImmediate(("Reset Script Field " + field.name).c_str(), before);
                    }
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Remove the stored value: the default from the script code applies");
                }
            }

            changed = changed || edited;
            ImGui::PopID();
        }

        return changed;
    }

    void DrawScriptFieldValues(const std::vector<scripting::ScriptFieldInfo>& fields, const nlohmann::json& values)
    {
        ImGui::BeginDisabled();
        for (const auto& field : fields)
        {
            ImGui::PushID(field.name.c_str());
            const auto it = values.find(field.name);
            const nlohmann::json& value = it != values.end() ? *it : field.defaultValue;
            const std::string label = field.name + "##live";

            SetFieldWidth();
            switch (field.type)
            {
            case FieldType::Float:
            {
                float current = value.is_number() ? value.get<float>() : 0.0f;
                ImGui::DragFloat(label.c_str(), &current);
                break;
            }
            case FieldType::Int:
            {
                int current = value.is_number_integer() ? value.get<int>() : 0;
                ImGui::DragInt(label.c_str(), &current);
                break;
            }
            case FieldType::Bool:
            {
                bool current = value.is_boolean() && value.get<bool>();
                ImGui::Checkbox(label.c_str(), &current);
                break;
            }
            case FieldType::String:
            {
                std::string current = value.is_string() ? value.get<std::string>() : std::string();
                ImGui::InputText(label.c_str(), &current);
                break;
            }
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
    }
}
