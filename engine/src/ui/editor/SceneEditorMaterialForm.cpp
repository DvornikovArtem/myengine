// The form of the Material Editor and of the Material Viewer: Material (the shader), Textures and Parameters. The rows come from
// editor::MaterialEditorModel (built from the layout of the shader of the material, with the metadata of its comments), and every
// change goes through the edit functions of that model; this file only draws the widgets and records the history.

#include <climits>

#include <myengine/editor/MaterialEditorModel.h>
#include "SceneEditorInternal.h"

namespace myengine::ui
{
    using namespace detail;

    namespace
    {
        constexpr const char* kNotSetHint = "Default from the shader (@default). Edit to store a value in the material.";
        constexpr const char* kNotSetNoDefaultHint = "No @default in the shader: 0. Edit to store a value in the material.";
        constexpr const char* kNotSetNoDefaultColorHint = "No @default in the shader: white. Edit to store a value in the material.";

        // What the widgets of a row need from the panel: the material that the form edits and the two ways to put a change in the history
        struct RowContext
        {
            resource::MaterialAsset& asset;
            std::function<void(const std::string& label)> gesture;   // right after a widget that is dragged or typed (one entry per gesture)
            std::function<void(const std::string& label)> discrete;  // after a click: the whole change is in this frame
        };

        // The narrowest field that shows -123.45 and 1000.00 whole
        constexpr float kMinVectorFieldWidth = 56.0f;

        // A slider is a fill: the grab is transparent and the field is painted by hand from the left edge to the value, so that
        // the number in the middle stays readable. Call before the slider, EndSliderFill after it.
        void BeginSliderFill(const double fraction, const bool grey)
        {
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float width = ImGui::CalcItemWidth();
            const ImVec2 max(min.x + width, min.y + ImGui::GetFrameHeight());
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg), style::kRounding);
            const float filled = min.x + width * static_cast<float>(std::clamp(fraction, 0.0, 1.0));
            if (filled > min.x)
            {
                drawList->AddRectFilled(
                    min,
                    ImVec2(filled, max.y),
                    ImGui::GetColorU32(grey ? style::kControlHover : style::kSelectUnfocused),
                    style::kRounding,
                    filled >= max.x - 0.5f ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft);
            }
            const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, clear);
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, clear);
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, clear);
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, clear);
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, clear);
        }

        void EndSliderFill()
        {
            ImGui::PopStyleColor(5);
        }

        double FractionOf(const editor::MaterialRow& row, const double value)
        {
            return row.max > row.min ? (value - row.min) / (row.max - row.min) : 0.0;
        }

        std::string FormatOf(const int decimals)
        {
            return "%." + std::to_string(std::clamp(decimals, 0, 6)) + "f";
        }

        float ToFloat(const double value)
        {
            return static_cast<float>(value);
        }

        int ToInt(const double value)
        {
            return static_cast<int>(std::clamp(std::llround(value), static_cast<long long>(INT_MIN), static_cast<long long>(INT_MAX)));
        }

        bool IsGrey(const editor::MaterialRow& row)
        {
            return row.state == editor::MaterialRowState::NotSet || row.state == editor::MaterialRowState::WrongType;
        }

        const char* NotSetHintOf(const editor::MaterialRow& row)
        {
            if (row.hasDefault)
            {
                return kNotSetHint;
            }
            return row.isTint ? kNotSetNoDefaultColorHint : kNotSetNoDefaultHint;
        }

        double ValueAt(const editor::MaterialRow& row, const std::size_t index)
        {
            return index < row.value.size() ? row.value[index] : 0.0;
        }

        // A float, as a slider with a range and as a drag without one
        void DrawFloatRow(const editor::MaterialRow& row, RowContext& context, const std::string& undoLabel)
        {
            float value = ToFloat(ValueAt(row, 0));
            const std::string format = FormatOf(row.decimals);
            bool changed = false;
            if (row.slider)
            {
                BeginSliderFill(FractionOf(row, ValueAt(row, 0)), row.state == editor::MaterialRowState::NotSet || row.state == editor::MaterialRowState::WrongType);
                changed = ImGui::SliderFloat("##value", &value, ToFloat(row.min), ToFloat(row.max), format.c_str());
                EndSliderFill();
            }
            else
            {
                changed = ImGui::DragFloat(
                    "##value",
                    &value,
                    ToFloat(row.dragSpeed),
                    row.hasMin ? ToFloat(row.min) : -FLT_MAX,
                    row.hasMax ? ToFloat(row.max) : FLT_MAX,
                    format.c_str());
            }
            FocusOutline();
            if (changed)
            {
                editor::EditMaterialParam(row, {static_cast<double>(value)}, context.asset);
            }
            context.gesture(undoLabel);
        }

        // Int and UInt: whole numbers
        void DrawIntRow(const editor::MaterialRow& row, RowContext& context, const std::string& undoLabel)
        {
            int value = ToInt(ValueAt(row, 0));
            const int minValue = row.hasMin ? ToInt(row.min) : INT_MIN;
            const int maxValue = row.hasMax ? ToInt(row.max) : INT_MAX;
            bool changed = false;
            if (row.slider)
            {
                BeginSliderFill(FractionOf(row, ValueAt(row, 0)), row.state == editor::MaterialRowState::NotSet || row.state == editor::MaterialRowState::WrongType);
                changed = ImGui::SliderInt("##value", &value, minValue, maxValue);
                EndSliderFill();
            }
            else
            {
                changed = ImGui::DragInt("##value", &value, ToFloat(row.dragSpeed), minValue, maxValue);
            }
            FocusOutline();
            if (changed)
            {
                editor::EditMaterialParam(row, {static_cast<double>(value)}, context.asset);
            }
            context.gesture(undoLabel);
        }

        void DrawBoolRow(const editor::MaterialRow& row, RowContext& context, const std::string& undoLabel)
        {
            bool value = ValueAt(row, 0) != 0.0;
            if (Checkbox("##value", &value))
            {
                editor::EditMaterialParam(row, {value ? 1.0 : 0.0}, context.asset);
                context.discrete(undoLabel);
            }
        }

        // Float2, Float3, Float4: the fields X Y (Z) (W) in one row; every field is a gesture of its own
        void DrawVectorRow(const editor::MaterialRow& row, RowContext& context, const std::string& undoLabel)
        {
            const int count = row.widget == editor::MaterialWidget::Vector2 ? 2 : row.widget == editor::MaterialWidget::Vector3 ? 3 : 4;
            float values[4]{};
            for (int axis = 0; axis < count; ++axis)
            {
                values[axis] = ToFloat(ValueAt(row, static_cast<std::size_t>(axis)));
            }
            // Four fields go in two rows when one row would be narrower than kMinVectorFieldWidth; the other vectors show one digit
            // less when their fields are that narrow (a step that the shader gives keeps its digits)
            const float minRowWidth = count == 4 ? kMinVectorFieldWidth : 0.0f;
            const bool narrow = DragVectorFieldWidth(count, minRowWidth) < kMinVectorFieldWidth;
            const std::string format = FormatOf(narrow && !row.hasStep ? std::min(row.decimals, 1) : row.decimals);
            const bool clamped = row.hasMin && row.hasMax;
            DragVector(
                "##value",
                values,
                count,
                ToFloat(row.dragSpeed),
                format.c_str(),
                [&](const int)
                {
                    if (ImGui::IsItemEdited())
                    {
                        std::vector<double> components;
                        for (int i = 0; i < count; ++i)
                        {
                            components.push_back(static_cast<double>(values[i]));
                        }
                        editor::EditMaterialParam(row, components, context.asset);
                    }
                    context.gesture(undoLabel);
                },
                clamped ? ToFloat(row.min) : 0.0f,
                clamped ? ToFloat(row.max) : 0.0f,
                minRowWidth);
        }

        // @color on a Float3 / Float4: a 40 x 24 swatch (the picker opens on click) and the 0-255 values in one row
        void DrawColorRow(const editor::MaterialRow& row, RowContext& context, const std::string& undoLabel)
        {
            const bool alpha = row.widget == editor::MaterialWidget::Color4;
            const int count = alpha ? 4 : 3;
            float colour[4]{
                ToFloat(ValueAt(row, 0)),
                ToFloat(ValueAt(row, 1)),
                ToFloat(ValueAt(row, 2)),
                alpha ? ToFloat(ValueAt(row, 3)) : 1.0f,
            };
            const auto apply = [&](const float (&channels)[4])
            {
                std::vector<double> components;
                for (int i = 0; i < count; ++i)
                {
                    components.push_back(static_cast<double>(channels[i]));
                }
                editor::EditMaterialParam(row, components, context.asset);
            };

            if (ImGui::ColorButton("##swatch", ImVec4(colour[0], colour[1], colour[2], colour[3]),
                    alpha ? ImGuiColorEditFlags_AlphaPreview : ImGuiColorEditFlags_NoAlpha, ImVec2(40.0f, style::kFrameHeight)))
            {
                ImGui::OpenPopup("##picker");
            }
            if (ImGui::BeginPopup("##picker"))
            {
                const bool picked = alpha
                    ? ImGui::ColorPicker4("##picker_colour", colour, ImGuiColorEditFlags_AlphaBar)
                    : ImGui::ColorPicker3("##picker_colour", colour);
                if (picked)
                {
                    apply(colour);
                }
                context.gesture(undoLabel);
                ImGui::EndPopup();
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            int channels[4]{};
            for (int i = 0; i < 4; ++i)
            {
                channels[i] = static_cast<int>(std::lround(colour[i] * 255.0f));
            }
            const bool changed = alpha
                ? ImGui::DragInt4("##channels", channels, 0.5f, 0, 255)
                : ImGui::DragInt3("##channels", channels, 0.5f, 0, 255);
            if (changed)
            {
                float edited[4]{1.0f, 1.0f, 1.0f, 1.0f};
                for (int i = 0; i < count; ++i)
                {
                    edited[i] = static_cast<float>(std::clamp(channels[i], 0, 255)) / 255.0f;
                }
                apply(edited);
            }
            FocusOutline();
            context.gesture(undoLabel);
        }

        // A matrix, a type the panel cannot edit yet: a grey line (the value, if the material has one, can be reset)
        void DrawUnsupportedRow(const editor::MaterialRow& row)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextDim));
            if (row.widget == editor::MaterialWidget::Matrix)
            {
                ImGui::TextUnformatted("Matrix 4x4 - edit in the .material.json");
            }
            else
            {
                const std::string text = row.typeName + " - not supported yet";
                ImGui::TextUnformatted(text.c_str());
            }
            ImGui::PopStyleColor();
        }

        // One row of the Textures grid: a picker with the three states of the slot (set, not set, the file is not found)
        void DrawTextureRow(
            const editor::MaterialRow& row,
            RowContext& context,
            resource::ResourceManager& resources,
            editor::ThumbnailService* thumbnails,
            const std::vector<std::string>& keys)
        {
            ImGui::PushID(row.name.c_str());

            const bool notSet = !row.hasKey;
            PropertyLabelOptions label;
            label.dim = notSet;
            label.rowHeight = style::kPickerRowHeight;
            label.tooltip = row.tooltip.c_str();
            label.tooltipDetail = row.texturePath.c_str();
            label.centered = true;
            PropertyLabel(row.label.c_str(), label);

            AssetPickerOptions options = MakePickerOptions(PickerKind::Texture, resources, thumbnails);
            options.missing = row.fileMissing;
            options.emptyIsWhite = true;
            ImGui::BeginGroup();
            std::string chosen;
            if (AssetPicker("##texture", row.texturePath, keys, options, chosen))
            {
                editor::SetMaterialTexture(row, chosen, context.asset);
                if (!chosen.empty())
                {
                    resources.Load<resource::TextureAsset>(chosen);
                }
                context.discrete("Change Material Texture " + row.label);
            }
            ImGui::EndGroup();
            if (notSet)
            {
                Tooltip("No texture: the shader gets a white 1x1.");
            }

            if (row.hasKey && PropertyReset("Remove the texture: the shader gets a white 1x1"))
            {
                editor::ResetMaterialTexture(row, context.asset);
                context.discrete("Reset Material Texture " + row.label);
            }

            ImGui::PopID();
        }

        // One row of the Parameters grid
        void DrawParameterRow(const editor::MaterialRow& row, RowContext& context)
        {
            using editor::MaterialRowState;
            using editor::MaterialWidget;

            ImGui::PushID(row.name.c_str());

            const bool wrongType = row.state == MaterialRowState::WrongType;
            const bool dimLabel = row.state == MaterialRowState::NotSet || row.state == MaterialRowState::Unsupported;
            const std::string tooltip = wrongType ? row.message : row.tooltip;
            std::string detail = wrongType ? row.tooltip : row.detail;
            PropertyLabelOptions label;
            label.dim = dimLabel;
            label.color = wrongType ? style::kErrorText : 0;
            label.tooltip = tooltip.c_str();
            label.tooltipDetail = detail.c_str();
            label.centered = true;
            PropertyLabel(row.label.c_str(), label);

            const std::string changeLabel = "Change Material Param " + row.label;
            const bool grey = IsGrey(row);
            if (grey)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextDim));
            }
            ImGui::BeginGroup();
            switch (row.widget)
            {
                case MaterialWidget::Float:
                    DrawFloatRow(row, context, changeLabel);
                    break;
                case MaterialWidget::Vector2:
                case MaterialWidget::Vector3:
                case MaterialWidget::Vector4:
                    DrawVectorRow(row, context, changeLabel);
                    break;
                case MaterialWidget::Color3:
                case MaterialWidget::Color4:
                    DrawColorRow(row, context, changeLabel);
                    break;
                case MaterialWidget::Int:
                case MaterialWidget::UInt:
                    DrawIntRow(row, context, changeLabel);
                    break;
                case MaterialWidget::Bool:
                    DrawBoolRow(row, context, changeLabel);
                    break;
                case MaterialWidget::Matrix:
                case MaterialWidget::Unsupported:
                case MaterialWidget::Texture:
                    DrawUnsupportedRow(row);
                    break;
            }
            ImGui::EndGroup();
            if (grey)
            {
                ImGui::PopStyleColor();
            }
            if (row.state == MaterialRowState::NotSet)
            {
                Tooltip(NotSetHintOf(row));
            }

            if (row.hasKey)
            {
                const char* resetTooltip = wrongType ? "Remove the value of the wrong type from the material" : "Reset to the value from the shader";
                if (PropertyReset(resetTooltip))
                {
                    editor::ResetMaterialParam(row, context.asset);
                    context.discrete("Reset Material Param " + row.label);
                }
            }

            ImGui::PopID();
        }
    }

    void SceneEditor::DrawMaterialForm(const std::string& materialPath, const editor::MaterialEditorModel& model, const bool editEnabled)
    {
        auto materialResource = services_.resourceManager->Load<resource::MaterialAsset>(materialPath);
        if (materialResource == nullptr)
        {
            return;
        }
        resource::MaterialAsset& asset = materialResource->asset;

        // The asset at the start of the frame: the "before" of the next entry of the history
        resource::MaterialAsset frameStart = CloneMaterialAsset(asset);
        RowContext context{
            asset,
            [&](const std::string& label) { CommitMaterialWidgetEdit(label.c_str(), materialPath, frameStart, asset); },
            [&](const std::string& label)
            {
                if (PushMaterialAssetCommand(label.c_str(), materialPath, frameStart, CloneMaterialAsset(asset)))
                {
                    frameStart = CloneMaterialAsset(asset);
                }
            },
        };

        // ---- Material: the shader ----
        if (BeginCategory("Material"))
        {
            ImGui::BeginDisabled(!editEnabled);
            if (BeginPropertyGrid("##material_asset"))
            {
                PropertyLabelOptions shaderLabel;
                shaderLabel.rowHeight = style::kPickerRowHeight;
                shaderLabel.centered = true;
                PropertyLabel("Shader", shaderLabel);
                const auto shaderKeys = services_.resourceManager->GetKnownShaderKeys();
                std::string chosen;
                if (AssetPicker(
                        "##shader",
                        asset.shaderPath,
                        shaderKeys,
                        MakePickerOptions(PickerKind::Shader, *services_.resourceManager, services_.thumbnails),
                        chosen))
                {
                    editor::ChangeMaterialShader(asset, chosen);
                    services_.resourceManager->Load<resource::ShaderAsset>(chosen);
                }
                CommitMaterialWidgetEdit("Change Material Shader", materialPath, frameStart, asset);
                EndPropertyGrid();
            }
            ImGui::EndDisabled();
            EndCategory();
        }

        // ---- Textures: a row for every Texture2D of the shader, in the order of the registers ----
        const std::string textureCount = std::to_string(model.textures.size());
        if (BeginCategory("Textures", true, model.textures.empty() ? nullptr : textureCount.c_str()))
        {
            ImGui::BeginDisabled(!editEnabled);
            if (model.textures.empty())
            {
                DrawInfoLine("The shader has no textures.");
            }
            else if (BeginPropertyGrid("##material_textures"))
            {
                const auto textureKeys = services_.resourceManager->GetKnownTextureKeys();
                for (const editor::MaterialRow& row : model.textures)
                {
                    DrawTextureRow(row, context, *services_.resourceManager, services_.thumbnails, textureKeys);
                }
                EndPropertyGrid();
            }
            ImGui::EndDisabled();
            EndCategory();
        }

        // ---- Parameters: the variables of the constant buffers ----
        const std::string count = std::to_string(model.parameters.size());
        if (BeginCategory("Parameters", true, model.parameters.empty() ? nullptr : count.c_str()))
        {
            ImGui::BeginDisabled(!editEnabled);
            if (model.parameters.empty())
            {
                DrawInfoLine("The shader has no editable parameters.");
            }
            else
            {
                if (BeginPropertyGrid("##material_params"))
                {
                    for (const editor::MaterialRow& row : model.parameters)
                    {
                        DrawParameterRow(row, context);
                    }
                    EndPropertyGrid();
                }

                const bool anySet = std::any_of(model.parameters.begin(), model.parameters.end(), [](const editor::MaterialRow& row) { return row.hasKey; });
                const float buttonWidth = 84.0f;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(ImGui::GetContentRegionAvail().x - buttonWidth - 6.0f, 0.0f));
                if (Button("Reset all", nullptr, anySet, buttonWidth))
                {
                    editor::ResetAllMaterialParams(model, asset);
                    context.discrete("Reset Material Params");
                }
                Tooltip("Remove every value of the parameters from the material", nullptr, "The textures stay");
            }
            ImGui::EndDisabled();
            EndCategory();
        }
    }
}
