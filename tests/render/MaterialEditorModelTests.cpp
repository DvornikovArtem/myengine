// B3.1 (LR3): the rows of the Material Editor and the edits of a material, without ImGui and without a GPU.
// The layouts come from the reflection of real HLSL (tests/render/shaders, with the metadata of their comments).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <myengine/editor/MaterialEditorModel.h>
#include <myengine/render/MaterialParams.h>
#include <myengine/render/ShaderLayout.h>
#include <myengine/resource/ResourceManager.h>

#include "ShaderReflector.h"

namespace
{
    namespace fs = std::filesystem;
    using namespace myengine;
    using namespace myengine::editor;
    using namespace myengine::render;
    using resource::MaterialAsset;

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool Contains(const std::string& text, const std::string& part)
    {
        return text.find(part) != std::string::npos;
    }

    ShaderLayout LayoutOf(const fs::path& hlsl)
    {
        static dx12::ShaderReflector reflector;
        ShaderProgramData program;
        program.sourcePath = fs::u8path(MYENGINE_SOURCE_DIR) / hlsl;
        program.vertexEntry = "VSMain";
        program.pixelEntry = "PSMain";
        const dx12::ShaderCompileResult result = reflector.CompileProgram(program);
        std::string messages;
        for (const ShaderDiagnostic& diagnostic : result.diagnostics)
        {
            messages += "\n  " + FormatShaderDiagnostic(diagnostic);
        }
        Check(result.ok && result.program != nullptr, hlsl.u8string() + " must compile:" + messages);
        return result.program->layout;
    }

    ShaderLayout EditorLayout()
    {
        return LayoutOf("tests/render/shaders/editor_model.hlsl");
    }

    // A layout by hand: no texture and no gColor
    ShaderLayout PlainLayout()
    {
        ShaderLayout layout;
        ShaderConstantBuffer buffer;
        buffer.name = "PlainCB";
        buffer.sizeBytes = 16;
        ShaderVariable gain;
        gain.name = "gGain";
        gain.type = ShaderValueType::Float;
        gain.size = 4;
        buffer.variables.push_back(gain);
        layout.constantBuffers.push_back(buffer);
        return layout;
    }

    std::vector<std::string> Names(const std::vector<MaterialRow>& rows)
    {
        std::vector<std::string> names;
        for (const MaterialRow& row : rows)
        {
            names.push_back(row.name);
        }
        return names;
    }

    const MaterialRow& Row(const MaterialEditorModel& model, const std::string& name)
    {
        const MaterialRow* row = model.FindParameter(name);
        Check(row != nullptr, "no row '" + name + "'");
        return *row;
    }

    const MaterialRow& TextureRow(const MaterialEditorModel& model, const std::string& name)
    {
        const MaterialRow* row = model.FindTexture(name);
        Check(row != nullptr, "no texture row '" + name + "'");
        return *row;
    }

    bool Near(const double a, const double b)
    {
        return std::fabs(a - b) < 1e-6;
    }

    // ---- the rows ----

    void TestRowsOfAnEmptyMaterial()
    {
        const ShaderLayout layout = EditorLayout();
        const MaterialAsset asset;
        const MaterialEditorModel model = BuildMaterialEditorModel(layout, asset);

        const std::vector<std::string> expected = {"gRoughness", "gColor", "gGlow", "gOffset", "gLevels", "gCount", "gEnabled", "gGain", "gLoose", "gExtra", "gCells", "gWeights"};
        Check(Names(model.parameters) == expected, "the rows are the declarations in the order of the HLSL, without the hidden ones");
        Check(Names(model.hidden) == std::vector<std::string>({"gModel", "gSecret", "gMask"}), "gModel (the renderer's), gSecret and gMask (@hidden) are left out");
        Check(model.hidden[0].state == MaterialRowState::Hidden && model.hidden[1].state == MaterialRowState::Hidden && model.hidden[2].state == MaterialRowState::Hidden, "and are Hidden");
        Check(Names(model.textures) == std::vector<std::string>({"gBase", "gAlbedo", "gDetail"}), "the textures in the order of the registers t0, t1, t2; gMask (@hidden, t3) is left out");
        Check(model.notInShader.empty(), "nothing is not in the shader");

        for (const char* name : {"gRoughness", "gColor", "gGlow", "gOffset", "gLevels", "gCount", "gEnabled", "gGain", "gLoose"})
        {
            Check(Row(model, name).state == MaterialRowState::NotSet && !Row(model, name).hasKey, std::string(name) + " is not set");
        }
        for (const char* name : {"gExtra", "gCells", "gWeights"})
        {
            Check(Row(model, name).state == MaterialRowState::Unsupported, std::string(name) + " is not supported");
        }
        Check(Row(model, "gExtra").widget == MaterialWidget::Matrix, "a Float4x4 is a Matrix row");
        Check(Row(model, "gCells").widget == MaterialWidget::Unsupported && Row(model, "gWeights").widget == MaterialWidget::Unsupported, "an Int3 and an array are grey rows");

        // The value of a row that the material does not set is the @default, zero without one (white for gColor without one)
        Check(Near(Row(model, "gRoughness").value[0], 0.5), "gRoughness shows its @default 0.5");
        Check(Row(model, "gColor").value.size() == 4 && Near(Row(model, "gColor").value[1], 0.75), "gColor shows its @default");
        Check(Near(Row(model, "gGlow").value[2], 0.6), "a Float3 colour shows its @default");
        Check(Near(Row(model, "gOffset").value[0], 0.0) && Row(model, "gOffset").value.size() == 2, "without a @default: zero");
        Check(Row(model, "gLevels").value == std::vector<double>({0.0}) && Row(model, "gEnabled").value == std::vector<double>({0.0}), "an int and a bool: zero / false");

        for (const MaterialRow& texture : model.textures)
        {
            Check(texture.state == MaterialRowState::NotSet && texture.texturePath.empty() && !texture.hasKey, texture.name + ": no texture, the shader gets a white 1x1");
        }
        Check(TextureRow(model, "gBase").firstTexture && !TextureRow(model, "gAlbedo").firstTexture && TextureRow(model, "gAlbedo").firstTextureName == "gBase", "the first texture is the one with the smallest register");
        Check(TextureRow(model, "gDetail").label == "Detail" && TextureRow(model, "gAlbedo").label == "gAlbedo", "@label or the name");
        Check(TextureRow(model, "gDetail").tooltip == "gDetail (t2)" && TextureRow(model, "gDetail").registerIndex == 2, "the tooltip of a texture");
    }

    void TestWidgetsAndLimits()
    {
        const MaterialEditorModel model = BuildMaterialEditorModel(EditorLayout(), MaterialAsset{});

        const MaterialRow& roughness = Row(model, "gRoughness");
        Check(roughness.widget == MaterialWidget::Float && roughness.slider && roughness.hasMin && roughness.hasMax && roughness.min == 0.0 && roughness.max == 1.0 && roughness.step == 0.01,
            "a Float with a range is a slider");
        Check(roughness.decimals == 2 && Near(roughness.dragSpeed, 0.01), "the digits and the speed come from the step");
        Check(roughness.label == "Roughness" && roughness.tooltip == "gRoughness (Float)" && roughness.detail == "Range 0 - 1, step 0.01, default 0.5", "the label, the tooltip, the detail: '" + roughness.detail + "'");
        Check(Row(model, "gGain").widget == MaterialWidget::Float && !Row(model, "gGain").slider && Row(model, "gGain").hasMin && !Row(model, "gGain").hasMax, "a Float with one bound is a drag");
        Check(Row(model, "gGain").detail == "Min 0" && Row(model, "gLoose").detail.empty() && Row(model, "gLoose").decimals == 3, "without a step: three digits");
        Check(Row(model, "gOffset").widget == MaterialWidget::Vector2 && Row(model, "gOffset").hasStep && Row(model, "gOffset").decimals == 2 && Row(model, "gOffset").min == -1.0, "a Float2 with a range and a step");

        const MaterialRow& colour = Row(model, "gColor");
        Check(colour.widget == MaterialWidget::Color4 && colour.isTint && colour.label == "Tint" && !colour.hasMin && !colour.hasMax, "gColor is the Tint row: a colour, no range");
        Check(Row(model, "gGlow").widget == MaterialWidget::Color3 && !Row(model, "gGlow").isTint, "a Float3 with @color is a colour; only gColor is the tint");

        const MaterialRow& levels = Row(model, "gLevels");
        Check(levels.widget == MaterialWidget::Int && levels.slider && levels.hasStep == false, "an Int with a range is a slider; a step does not apply to it");
        const MaterialRow& count = Row(model, "gCount");
        Check(count.widget == MaterialWidget::UInt && count.hasMin && count.min == 0.0 && count.hasMax && count.max == 100.0 && count.slider, "a UInt never goes below 0");
        Check(Row(model, "gEnabled").widget == MaterialWidget::Bool, "a Bool is a checkbox");
        Check(Row(model, "gGlow").hasDefault && Row(model, "gGlow").defaultValue.size() == 3 && !Row(model, "gLoose").hasDefault, "the default of the shader");
    }

    void TestStates()
    {
        const ShaderLayout layout = EditorLayout();
        MaterialAsset asset;
        asset.params["gRoughness"] = MaterialValue::FromNumber(0.8);
        asset.params["gLevels"] = MaterialValue::FromBool(true);     // not an Int
        asset.params["gCells"] = MaterialValue::FromNumber(1);        // a type the panel cannot edit
        asset.params["gEnabled"] = MaterialValue::FromBool(true);
        const MaterialEditorModel model = BuildMaterialEditorModel(layout, asset);

        Check(Row(model, "gRoughness").state == MaterialRowState::Set && Row(model, "gRoughness").hasKey && Near(Row(model, "gRoughness").value[0], 0.8), "a value of the material: Set, shown");
        Check(Row(model, "gEnabled").state == MaterialRowState::Set && Row(model, "gEnabled").value == std::vector<double>({1.0}), "a bool that is set");
        const MaterialRow& wrong = Row(model, "gLevels");
        Check(wrong.state == MaterialRowState::WrongType && wrong.hasKey && Contains(wrong.message, "expected Int") && wrong.value == std::vector<double>({0.0}),
            "a value of the wrong type: WrongType, the message says why, the row shows the default: " + wrong.message);
        Check(Row(model, "gCells").state == MaterialRowState::Unsupported && Row(model, "gCells").hasKey, "a type the panel cannot edit keeps its key and can be reset");
        Check(Row(model, "gGain").state == MaterialRowState::NotSet, "the rest is not set");
        Check(!model.messages.empty(), "the messages of applying the material are kept");
    }

    // What a row shows is what the frame uses: the bytes of the MaterialParams of the same material
    void TestValuesAreTheBytesOfTheFrame()
    {
        const ShaderLayout layout = EditorLayout();
        MaterialAsset asset;
        asset.params["gRoughness"] = MaterialValue::FromNumber(0.25);
        asset.params["gGlow"] = MaterialValue::FromVector({0.9, 0.8, 0.7});
        asset.params["gOffset"] = MaterialValue::FromVector({0.5, -0.5});
        asset.params["gLevels"] = MaterialValue::FromNumber(7);
        asset.params["gCount"] = MaterialValue::FromNumber(42);
        asset.params["gEnabled"] = MaterialValue::FromBool(true);
        asset.params["gGain"] = MaterialValue::FromNumber(2.5);
        asset.tint = {0.1f, 0.2f, 0.3f, 0.4f};
        asset.tintSet = true;
        const MaterialEditorModel model = BuildMaterialEditorModel(layout, asset);

        MaterialParams params(layout);
        const auto messages = ApplyMaterialValues(params, resource::ToMaterialValues(asset), [](const std::string&) { return TextureHandle{1}; });
        Check(messages.empty(), "the material fits the shader");
        for (const MaterialRow& row : model.parameters)
        {
            if (row.widget == MaterialWidget::Matrix || row.widget == MaterialWidget::Unsupported)
            {
                continue;
            }
            const ShaderVariableRef found = layout.FindVariable(row.name);
            const std::size_t index = static_cast<std::size_t>(found.buffer - layout.constantBuffers.data());
            const std::uint8_t* bytes = params.ConstantBufferBytes(index).data() + found.variable->offset;
            for (std::size_t i = 0; i < row.value.size(); ++i)
            {
                if (row.type == ShaderValueType::Int)
                {
                    std::int32_t value = 0;
                    std::memcpy(&value, bytes, sizeof(value));
                    Check(row.value[i] == value, row.name + ": the same int as the frame");
                }
                else if (row.type == ShaderValueType::UInt || row.type == ShaderValueType::Bool)
                {
                    std::uint32_t value = 0;
                    std::memcpy(&value, bytes, sizeof(value));
                    Check(row.value[i] == (row.type == ShaderValueType::Bool ? (value != 0 ? 1.0 : 0.0) : static_cast<double>(value)), row.name + ": the same value as the frame");
                }
                else
                {
                    float value = 0.0f;
                    std::memcpy(&value, bytes + i * sizeof(float), sizeof(float));
                    Check(row.value[i] == static_cast<double>(value), row.name + ": the same float as the frame");
                }
            }
        }
        Check(Near(Row(model, "gColor").value[3], 0.4), "the tint of the material is gColor");
    }

    // ---- the edits ----

    void TestEdits()
    {
        const MaterialEditorModel model = BuildMaterialEditorModel(EditorLayout(), MaterialAsset{});
        MaterialAsset asset;

        // Float: snapped to the step, inside the range, exactly a float
        Check(EditMaterialParam(Row(model, "gRoughness"), {0.337}, asset), "an edit creates the key");
        Check(asset.params.at("gRoughness").kind == MaterialValue::Kind::Number && asset.params.at("gRoughness").numbers == std::vector<double>({static_cast<double>(0.34f)}), "snapped to 0.34, a float");
        Check(!EditMaterialParam(Row(model, "gRoughness"), {0.337}, asset), "the same value again changes nothing");
        EditMaterialParam(Row(model, "gRoughness"), {1.7}, asset);
        Check(asset.params.at("gRoughness").numbers[0] == 1.0, "inside the range");
        EditMaterialParam(Row(model, "gRoughness"), {0.3}, asset);
        Check(asset.params.at("gRoughness").numbers[0] == static_cast<double>(0.3f), "0.3 is not 0.30000000000000004");

        // Int and UInt: whole numbers
        EditMaterialParam(Row(model, "gLevels"), {3.6}, asset);
        Check(asset.params.at("gLevels").kind == MaterialValue::Kind::Number && asset.params.at("gLevels").numbers[0] == 4.0, "an int is whole");
        EditMaterialParam(Row(model, "gLevels"), {99.0}, asset);
        Check(asset.params.at("gLevels").numbers[0] == 10.0, "an int stays in its range");
        EditMaterialParam(Row(model, "gCount"), {-5.0}, asset);
        Check(asset.params.at("gCount").numbers[0] == 0.0, "a UInt is never below 0");

        // Bool, vectors, a colour
        EditMaterialParam(Row(model, "gEnabled"), {1.0}, asset);
        Check(asset.params.at("gEnabled").kind == MaterialValue::Kind::Bool && asset.params.at("gEnabled").numbers[0] == 1.0, "a bool is a bool");
        EditMaterialParam(Row(model, "gOffset"), {0.3, -1.4}, asset);
        Check(asset.params.at("gOffset").kind == MaterialValue::Kind::Vector && asset.params.at("gOffset").numbers == std::vector<double>({0.25, -1.0}),
            "each component to the step from @min (-1), inside the range");
        EditMaterialParam(Row(model, "gGlow"), {0.1, 0.2, 0.3}, asset);
        Check(asset.params.at("gGlow").kind == MaterialValue::Kind::Vector && asset.params.at("gGlow").numbers.size() == 3, "a colour of three channels is a vector of three");

        // The tint: the old key, and tintSet
        Check(EditMaterialParam(Row(model, "gColor"), {0.5, 0.25, 0.125, 1.0}, asset), "an edit of gColor");
        Check(asset.tintSet && asset.tint.r == 0.5f && asset.tint.g == 0.25f && asset.tint.b == 0.125f && asset.tint.a == 1.0f && asset.params.count("gColor") == 0,
            "gColor is asset.tint and tintSet, not a key of params");
        asset.params["gColor"] = MaterialValue::FromVector({0.0, 0.0, 0.0, 1.0});
        EditMaterialParam(Row(model, "gColor"), {0.75, 0.5, 0.25, 1.0}, asset);
        Check(asset.params.at("gColor").numbers == std::vector<double>({0.75, 0.5, 0.25, 1.0}) && asset.tint.r == 0.75f, "when params has gColor too, both follow (the new key would win over the tint)");

        // What cannot be edited
        Check(!EditMaterialParam(Row(model, "gExtra"), {1.0}, asset) && !EditMaterialParam(Row(model, "gCells"), {1.0, 2.0, 3.0}, asset), "a matrix and an unsupported type are not edited");
        Check(!EditMaterialParam(Row(model, "gOffset"), {1.0}, asset), "a wrong number of components is refused");
    }

    void TestResets()
    {
        const ShaderLayout layout = EditorLayout();
        MaterialAsset asset;
        asset.params["gRoughness"] = MaterialValue::FromNumber(0.8);
        asset.params["gGain"] = MaterialValue::FromNumber(2.0);
        asset.params["gOldGain"] = MaterialValue::FromNumber(1.0);
        asset.params["gCells"] = MaterialValue::FromNumber(3);
        asset.tint = {0.1f, 0.2f, 0.3f, 1.0f};
        asset.tintSet = true;
        asset.textures["gAlbedo"] = "a.bmp";
        const MaterialEditorModel model = BuildMaterialEditorModel(layout, asset);

        MaterialAsset one = asset;
        Check(ResetMaterialParam(Row(model, "gRoughness"), one) && one.params.count("gRoughness") == 0 && one.params.count("gGain") == 1, "Reset removes the key of this row only");
        Check(!ResetMaterialParam(Row(model, "gRoughness"), one), "a second Reset changes nothing");
        Check(ResetMaterialParam(Row(model, "gCells"), one) && one.params.count("gCells") == 0, "an unsupported row with a key can be reset");

        MaterialAsset tint = asset;
        Check(ResetMaterialParam(Row(model, "gColor"), tint), "Reset of the tint");
        Check(!tint.tintSet && tint.params.count("gColor") == 0 && tint.tint.r == 1.0f && tint.tint.g == 0.75f && tint.tint.b == 0.12f, "gColor: tintSet false and the tint is the @default of the shader");
        tint.params["gColor"] = MaterialValue::FromVector({0.0, 0.0, 0.0, 1.0});
        ResetMaterialParam(Row(model, "gColor"), tint);
        Check(tint.params.count("gColor") == 0, "and params.gColor is gone");
        const MaterialEditorModel afterReset = BuildMaterialEditorModel(layout, tint);
        Check(Row(afterReset, "gColor").state == MaterialRowState::NotSet && Near(Row(afterReset, "gColor").value[1], 0.75), "the row is grey again and shows the @default");

        // A shader without a @default for gColor: the tint goes back to white
        const MaterialEditorModel probe = BuildMaterialEditorModel(LayoutOf("tests/render/shaders/material_probe.hlsl"), asset);
        MaterialAsset white = asset;
        ResetMaterialParam(Row(probe, "gColor"), white);
        Check(white.tint.r == 1.0f && white.tint.g == 1.0f && white.tint.b == 1.0f && white.tint.a == 1.0f && !white.tintSet, "no @default: white");

        // Reset all: every parameter of the shader that the material sets; the textures and the entries "Not in the shader" stay
        MaterialAsset all = asset;
        Check(ResetAllMaterialParams(model, all), "Reset all");
        Check(all.params.size() == 1 && all.params.count("gOldGain") == 1 && !all.tintSet && all.textures.size() == 1, "gOldGain (not in the shader) and the texture stay, the rest is gone");
    }

    void TestNotInTheShader()
    {
        const ShaderLayout layout = EditorLayout();
        MaterialAsset asset;
        asset.params["gOldGain"] = MaterialValue::FromNumber(0.4);
        asset.params["gModel"] = MaterialValue::FromMatrix(std::vector<double>(16, 1.0));
        asset.params["gRoughness"] = MaterialValue::FromNumber(0.3);
        asset.textures["gGone"] = "gone.bmp";
        asset.textures["gAlbedo"] = "a.bmp";
        const MaterialEditorModel model = BuildMaterialEditorModel(layout, asset);

        Check(model.notInShader.size() == 3, "three entries are not in the shader, " + std::to_string(model.notInShader.size()) + " found");
        const auto find = [&model](const std::string& name) -> const MaterialEntry*
        {
            for (const MaterialEntry& entry : model.notInShader)
            {
                if (entry.name == name)
                {
                    return &entry;
                }
            }
            return nullptr;
        };
        Check(find("gOldGain") != nullptr && find("gOldGain")->kind == MaterialEntryKind::Param && find("gOldGain")->text == "0.4", "a name of params that the shader lacks, with its value");
        Check(find("gModel") != nullptr && find("gModel")->kind == MaterialEntryKind::EngineSemantic, "an engine semantic in params");
        Check(find("gGone") != nullptr && find("gGone")->kind == MaterialEntryKind::Texture && find("gGone")->text == "gone.bmp", "a name of textures that the shader lacks");
        Check(model.FindParameter("gOldGain") == nullptr && model.FindTexture("gGone") == nullptr, "and no row for them");
        Check(Row(model, "gRoughness").state == MaterialRowState::Set && TextureRow(model, "gAlbedo").texturePath == "a.bmp", "what the shader has is a row");

        MaterialAsset removed = asset;
        for (const MaterialEntry& entry : model.notInShader)
        {
            Check(RemoveMaterialEntry(entry, removed), "Remove changes the material: " + entry.name);
        }
        Check(removed.params.size() == 1 && removed.params.count("gRoughness") == 1 && removed.textures.size() == 1 && removed.textures.count("gAlbedo") == 1, "only the entries that were not in the shader are gone");
        MaterialAsset allRemoved = asset;
        Check(RemoveAllMaterialEntries(model, allRemoved) && BuildMaterialEditorModel(layout, allRemoved).notInShader.empty(), "Remove all");

        // The old keys when the shader has no gColor and no texture
        const ShaderLayout plain = PlainLayout();
        MaterialAsset old;
        old.tint = {0.1f, 0.2f, 0.3f, 1.0f};
        old.tintSet = true;
        old.texturePath = "t.bmp";
        const MaterialEditorModel oldModel = BuildMaterialEditorModel(plain, old);
        Check(oldModel.notInShader.size() == 2 && oldModel.notInShader[0].kind == MaterialEntryKind::OldTint && oldModel.notInShader[1].kind == MaterialEntryKind::OldTexture, "the old tint and the old texture of a shader that has neither");
        Check(oldModel.textures.empty(), "the shader has no texture rows");
        MaterialAsset cleaned = old;
        RemoveAllMaterialEntries(oldModel, cleaned);
        Check(!cleaned.tintSet && cleaned.tint.r == 1.0f && cleaned.tint.g == 1.0f && cleaned.texturePath.empty(), "Remove: the tint is white and not set, the texture is gone");

        // A white tint is not an entry
        MaterialAsset whiteTint;
        Check(BuildMaterialEditorModel(plain, whiteTint).notInShader.empty(), "an untouched material has no entries");

        // A shader change: nothing is lost, what the new shader lacks is listed
        MaterialAsset changed = asset;
        Check(ChangeMaterialShader(changed, "other.shader.json") && changed.shaderPath == "other.shader.json" && changed.params == asset.params, "the shader changes, the values stay");
        Check(!ChangeMaterialShader(changed, "other.shader.json"), "the same shader again changes nothing");
        Check(BuildMaterialEditorModel(PlainLayout(), changed).notInShader.size() == 5, "on a shader without these names every entry is listed: 3 of params (one is an engine semantic) and 2 textures");
    }

    // ---- the textures ----

    void TestTextures()
    {
        const ShaderLayout layout = EditorLayout();
        const auto rows = [&layout](const MaterialAsset& asset, const MaterialEditorOptions& options = {}) { return BuildMaterialEditorModel(layout, asset, options); };
        const MaterialEditorModel empty = rows(MaterialAsset{});

        // An old material: the old key means the first texture
        MaterialAsset asset;
        asset.texturePath = "a.bmp";
        MaterialEditorModel model = rows(asset);
        Check(TextureRow(model, "gBase").texturePath == "a.bmp" && TextureRow(model, "gBase").state == MaterialRowState::Set && TextureRow(model, "gAlbedo").state == MaterialRowState::NotSet, "the old 'texture' is gBase");

        // Another file for the first texture: only the old key changes, the file stays an old file
        Check(SetMaterialTexture(TextureRow(empty, "gBase"), "b.bmp", asset) && asset.texturePath == "b.bmp" && asset.textures.empty(), "the first texture of an old material: only the old key");

        // The second texture comes: the old key moves to "textures", the file is a new file
        Check(SetMaterialTexture(TextureRow(empty, "gAlbedo"), "c.bmp", asset), "the second texture");
        Check(asset.textures.size() == 2 && asset.textures.at("gBase") == "b.bmp" && asset.textures.at("gAlbedo") == "c.bmp", "the old key moved to textures by name");
        model = rows(asset);
        Check(TextureRow(model, "gBase").texturePath == "b.bmp" && TextureRow(model, "gAlbedo").texturePath == "c.bmp" && TextureRow(model, "gDetail").state == MaterialRowState::NotSet, "both are rows now");

        // The first texture of a material that has it in textures: textures and the mirror follow
        Check(SetMaterialTexture(TextureRow(empty, "gBase"), "d.bmp", asset) && asset.textures.at("gBase") == "d.bmp", "the first texture in textures");
        model = rows(asset);
        Check(TextureRow(model, "gBase").texturePath == "d.bmp" && TextureRow(model, "gAlbedo").texturePath == "c.bmp", "and the mirror does not turn into a second source for another slot");

        // The same file for two slots
        MaterialAsset same;
        same.texturePath = "x.bmp";
        SetMaterialTexture(TextureRow(empty, "gDetail"), "x.bmp", same);
        model = rows(same);
        Check(TextureRow(model, "gBase").texturePath == "x.bmp" && TextureRow(model, "gDetail").texturePath == "x.bmp", "the same file in the old key and in a slot: both slots have it");

        // Reset: the slot is empty, not another slot
        Check(ResetMaterialTexture(TextureRow(empty, "gBase"), asset), "Reset of the first texture");
        model = rows(asset);
        Check(TextureRow(model, "gBase").state == MaterialRowState::NotSet && TextureRow(model, "gAlbedo").texturePath == "c.bmp", "gBase is empty, gAlbedo keeps its file");
        Check(ResetMaterialTexture(TextureRow(empty, "gAlbedo"), asset), "Reset of the last texture is allowed");
        Check(asset.textures.empty() && asset.texturePath.empty(), "a material without any texture");
        model = rows(asset);
        for (const MaterialRow& texture : model.textures)
        {
            Check(texture.state == MaterialRowState::NotSet, texture.name + " is empty: the shader gets a white 1x1");
        }

        // A mirror that is the only texture
        MaterialAsset mirrored;
        mirrored.textures["gAlbedo"] = "m.bmp";
        mirrored.texturePath = "m.bmp"; // what the loader puts when the file has only "textures"
        model = rows(mirrored);
        Check(TextureRow(model, "gAlbedo").texturePath == "m.bmp" && TextureRow(model, "gBase").state == MaterialRowState::NotSet, "the mirror of a new file is not the first texture");
        SetMaterialTexture(TextureRow(empty, "gAlbedo"), "n.bmp", mirrored);
        model = rows(mirrored);
        Check(TextureRow(model, "gAlbedo").texturePath == "n.bmp" && TextureRow(model, "gBase").state == MaterialRowState::NotSet, "after another file the old mirror does not move to gBase");
        ResetMaterialTexture(TextureRow(empty, "gAlbedo"), mirrored);
        Check(mirrored.texturePath.empty() && mirrored.textures.empty(), "a reset takes the mirror away too");

        // The file that cannot be loaded
        MaterialAsset missing;
        missing.textures["gDetail"] = "gone.bmp";
        MaterialEditorOptions options;
        options.fileMissing = [](const std::string& key) { return key == "gone.bmp"; };
        model = rows(missing, options);
        Check(TextureRow(model, "gDetail").fileMissing && TextureRow(model, "gDetail").state == MaterialRowState::Set, "a file that is not found: the row is Set and says it is missing");
        Check(!rows(missing).textures[2].fileMissing, "without the callback nothing is missing");
    }

    // ---- the cache ----

    // ---- B3.4: the banner, the filter and the texts of the entries ----
    void TestBannerFilterAndTexts()
    {
        // the banner of the shader
        const ShaderBanner none = DescribeShaderBanner(false, "");
        Check(none.kind == ShaderBannerKind::None && none.text.empty(), "a shader that works says nothing");
        const ShaderBanner error = DescribeShaderBanner(true, "textured_lit.hlsl(27,14): undeclared identifier 'gRoughnes'.\n  second line\nthird");
        Check(error.kind == ShaderBannerKind::Error, "the first compile failed: an error");
        Check(error.text == "Shader error: textured_lit.hlsl(27,14): undeclared identifier 'gRoughnes'. The fallback shader is drawn. Fix the HLSL: this panel updates after the save.",
            "the banner says the first error only, with one full stop: " + error.text);
        Check(DescribeShaderBanner(true, "").kind == ShaderBannerKind::Error && Contains(DescribeShaderBanner(true, "").text, "did not compile"), "an error without a reason still says that it did not compile");
        const ShaderBanner warning = DescribeShaderBanner(false, "lit.hlsl(5,2): bad token");
        Check(warning.kind == ShaderBannerKind::Warning && warning.text == "The last change of the shader did not compile, the previous version is used: lit.hlsl(5,2): bad token",
            "a reload failed while the old version works: a warning: " + warning.text);

        // the filter shows up above the threshold and looks at the label and the name
        const ShaderLayout layout = EditorLayout();
        const MaterialEditorModel model = BuildMaterialEditorModel(layout, MaterialAsset{});
        Check(!MaterialFilterIsShown(model), "a few rows: no filter");
        MaterialEditorModel many = model;
        while (many.parameters.size() <= kMaterialFilterThreshold)
        {
            many.parameters.push_back(model.parameters.front());
        }
        Check(MaterialFilterIsShown(many), "more than the threshold: the filter is shown");
        MaterialEditorModel exact = model;
        while (exact.parameters.size() < kMaterialFilterThreshold)
        {
            exact.parameters.push_back(model.parameters.front());
        }
        Check(!MaterialFilterIsShown(exact), "exactly the threshold: no filter");

        MaterialRow row;
        row.name = "gRoughness";
        row.label = "Roughness";
        Check(MaterialRowMatchesFilter(row, ""), "an empty filter matches");
        Check(MaterialRowMatchesFilter(row, "rough") && MaterialRowMatchesFilter(row, "ROUGH"), "the label, without the case");
        Check(MaterialRowMatchesFilter(row, "gro") && !MaterialRowMatchesFilter(row, "metal"), "the name; what is in neither does not match");
        row.label = "\xD0\xA8\xD0\xB5\xD1\x80\xD0\xBE\xD1\x85"; // a Cyrillic label is found by its bytes
        Check(MaterialRowMatchesFilter(row, "\xD0\xB5\xD1\x80") && MaterialRowMatchesFilter(row, "gRough"), "a label in another script, and the name behind it");

        // the texts of the entries
        MaterialEntry param{MaterialEntryKind::Param, "gOldGain", "0.4"};
        Check(MaterialEntryName(param) == "gOldGain" && MaterialEntryValueText(param) == "0.4", "a value that the shader does not have");
        Check(MaterialEntryTooltip(param) == "The shader has no variable 'gOldGain'. The value stays in the file and is ignored. Put the variable back or remove it.", "its tooltip");
        MaterialEntry texture{MaterialEntryKind::Texture, "gDetail2", "a/b.bmp"};
        Check(Contains(MaterialEntryValueText(texture), "texture") && Contains(MaterialEntryValueText(texture), "a/b.bmp"), "a texture says its path");
        MaterialEntry semantic{MaterialEntryKind::EngineSemantic, "gModel", "set by the renderer, the value is ignored"};
        Check(MaterialEntryValueText(semantic) == "set by the renderer, the value is ignored" && Contains(MaterialEntryTooltip(semantic), "gModel"), "an engine semantic");
        MaterialEntry oldTint{MaterialEntryKind::OldTint, "tint", "1, 0.5, 0, 1"};
        MaterialEntry oldTexture{MaterialEntryKind::OldTexture, "texture", "a.bmp"};
        Check(MaterialEntryName(oldTint) == "tint (old key)" && MaterialEntryName(oldTexture) == "texture (old key)", "the old keys are called so");
        Check(!MaterialEntryTooltip(oldTint).empty() && !MaterialEntryTooltip(oldTexture).empty(), "and have a tooltip");
    }

    void TestCache()
    {
        ShaderLayout layout = EditorLayout();
        MaterialAsset asset;
        MaterialEditorModelCache cache;
        cache.Get(layout, asset);
        cache.Get(layout, asset);
        Check(cache.Builds() == 1, "the same material and layout: built once");
        asset.params["gRoughness"] = MaterialValue::FromNumber(0.9);
        cache.Get(layout, asset);
        Check(cache.Builds() == 2, "a changed material: built again");
        Check(Near(cache.Get(layout, asset).parameters.front().value[0], 0.9), "and the rows are the new ones");

        // The metadata changes with the layout kept (a comment edit): the rows are built again
        for (ShaderConstantBuffer& buffer : layout.constantBuffers)
        {
            for (ShaderVariable& variable : buffer.variables)
            {
                if (variable.name == "gLoose")
                {
                    variable.metadata.label = "Loose";
                }
            }
        }
        const MaterialEditorModel& relabelled = cache.Get(layout, asset);
        Check(cache.Builds() == 3 && Row(relabelled, "gLoose").label == "Loose", "a changed label: built again");

        // Another layout
        const ShaderLayout other = LayoutOf("tests/render/shaders/material_probe.hlsl");
        cache.Get(other, asset);
        Check(cache.Builds() == 4, "another layout: built again");

        // A file that turns out to be missing
        asset.textures["gTexture"] = "gone.bmp";
        bool missing = false;
        MaterialEditorOptions options;
        options.fileMissing = [&missing](const std::string&) { return missing; };
        cache.Get(other, asset, options);
        const std::uint64_t before = cache.Builds();
        cache.Get(other, asset, options);
        Check(cache.Builds() == before, "nothing changed");
        missing = true;
        cache.Get(other, asset, options);
        Check(cache.Builds() == before + 1, "a texture that failed to load: built again");
    }
}

int main()
{
    int result = 0;
    try
    {
        const auto run = [](const char* name, void (*test)())
        {
            std::cout << "  " << name << std::endl;
            test();
        };
        run("rows of an empty material", TestRowsOfAnEmptyMaterial);
        run("widgets and limits", TestWidgetsAndLimits);
        run("states", TestStates);
        run("values are the bytes of the frame", TestValuesAreTheBytesOfTheFrame);
        run("edits", TestEdits);
        run("resets", TestResets);
        run("not in the shader", TestNotInTheShader);
        run("textures", TestTextures);
        run("banner, filter and texts", TestBannerFilterAndTexts);
        run("cache", TestCache);
        std::cout << "OK: material editor model: rows, states, values of the frame, edits, resets, entries not in the shader, textures, cache\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        result = 1;
    }
    return result;
}
