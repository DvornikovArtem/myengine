// MaterialEditorModel.h
//
// The rows of the Material Editor without ImGui: what the panel shows and what its widgets do to the material. The rows are
// built from the ShaderLayout of the shader of the material (with the metadata of its comments, docs/render/b2-shader-metadata.md)
// and from the MaterialAsset; the edit functions change a MaterialAsset the way the panel needs (a key of the right type, a reset
// that removes the key, a texture that goes to the right key). Nothing here knows about the screen, so it is tested without a GPU.
// The values of the rows are not computed here: they are read from the MaterialParams that the same material gives, so what the
// panel shows is what the frame uses.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <myengine/render/MaterialParams.h>
#include <myengine/render/ShaderLayout.h>
#include <myengine/resource/ResourceManager.h>

namespace myengine::editor
{
    // What a row is drawn with (the panel maps it to a widget)
    enum class MaterialWidget : std::uint8_t
    {
        Float,        // one float: a slider when `slider` (it has a range), else a drag
        Vector2,      // Float2, Float3, Float4 in one row
        Vector3,
        Vector4,
        Color3,       // @color on a Float3 / Float4: a colour button and 0..255 fields
        Color4,
        Int,
        UInt,
        Bool,
        Matrix,       // Float4x4: a grey row, edited in the file
        Unsupported,  // Int2..4, UInt2..4, Float3x3, a type that is not known, arrays: a grey row
        Texture,
    };

    enum class MaterialRowState : std::uint8_t
    {
        Set,         // the material has the value (the key is in the file)
        NotSet,      // it has not: the row shows the @default of the shader, or zero (white for gColor)
        WrongType,   // the key is there, but the value does not fit the type (the frame does not use it)
        Unsupported, // the panel cannot edit this type; it only shows it (and can remove the key)
        Hidden,      // @hidden, or a value that the renderer writes
    };

    struct MaterialRow
    {
        std::string name;                       // the name in the HLSL: what the .material.json calls it
        std::string label;                      // @label, or the name
        std::string tooltip;                    // "gRoughness (Float)"; for a texture "gDetail (t1)"
        std::string detail;                     // "Range 0 - 1, step 0.01, default 0.5": only what the shader says; may be empty
        std::string typeName;                   // "Float", "Float4", "Float4x4"...; "Texture2D" for a texture
        render::ShaderValueType type = render::ShaderValueType::Unknown;
        MaterialWidget widget = MaterialWidget::Unsupported;
        MaterialRowState state = MaterialRowState::NotSet;
        bool hasKey = false;                    // the material has this entry: a Reset removes it
        bool isTint = false;                    // gColor of a Float4: the value lives in asset.tint (the old key), and tintSet says it is set
        std::string message;                    // WrongType: why ("'gRoughness': expected Float, got Float2")

        bool hasMin = false;                    // the range, the step and the default of the shader (the colours have no range)
        bool hasMax = false;
        bool hasStep = false;
        double min = 0.0;
        double max = 0.0;
        double step = 0.0;
        bool slider = false;                    // Float, Int, UInt with both @min and @max
        int decimals = 3;                       // digits after the point in the number format
        double dragSpeed = 0.01;
        bool hasDefault = false;
        std::vector<double> defaultValue;

        std::vector<double> value;              // what to show: the components as the frame uses them (colours 0..1, a bool 0 or 1)

        // Textures only
        std::uint32_t registerIndex = 0;        // t#
        bool firstTexture = false;              // the texture with the smallest register: where the old "texture" key goes
        std::string firstTextureName;           // the name of that texture (the same for all the texture rows)
        std::string texturePath;                // the key the material gives this texture; empty: none (the shader gets a white 1x1)
        bool fileMissing = false;               // the file of that texture could not be loaded
    };

    enum class MaterialEntryKind : std::uint8_t
    {
        Param,           // a name in "params" that the shader does not have
        Texture,         // a name in "textures" that the shader does not have
        EngineSemantic,  // gModel, gViewProjection, gLightDirection in "params": the renderer writes them
        OldTint,         // the old "tint" when the shader has no gColor
        OldTexture,      // the old "texture" when the shader has no texture
    };

    // An entry of the material that the shader does not use (the "Not in the shader" section)
    struct MaterialEntry
    {
        MaterialEntryKind kind = MaterialEntryKind::Param;
        std::string name;    // the key; for the old keys "tint" / "texture"
        std::string text;    // the value as text: "0.4", "1, 0.75, 0.12, 1", the path
    };

    struct MaterialEditorModel
    {
        std::vector<MaterialRow> textures;      // in the order of the t registers; the hidden ones are in `hidden`
        std::vector<MaterialRow> parameters;    // in the order of the declarations; the hidden ones are in `hidden`
        std::vector<MaterialRow> hidden;        // @hidden and the engine semantics: the panel does not show them
        std::vector<MaterialEntry> notInShader;
        std::vector<render::MaterialParamResult> messages; // what applying the material to the shader says (errors and warnings)

        const MaterialRow* FindParameter(const std::string& name) const;
        const MaterialRow* FindTexture(const std::string& name) const;
    };

    struct MaterialEditorOptions
    {
        // true when the file of a texture (a key) could not be loaded; without it no texture is missing
        std::function<bool(const std::string& textureKey)> fileMissing;
    };

    // The rows for the material on this layout. The shader must be a real one (not the fallback program of a shader that does
    // not compile): the layout of a fallback says nothing about the names of the material.
    MaterialEditorModel BuildMaterialEditorModel(const render::ShaderLayout& layout, const resource::MaterialAsset& asset, const MaterialEditorOptions& options = {});

    // The model, kept until the material, the shader layout (with its metadata) or the files change: the rows are built by applying
    // the material to a temporary MaterialParams, which is not for every frame.
    class MaterialEditorModelCache
    {
    public:
        const MaterialEditorModel& Get(const render::ShaderLayout& layout, const resource::MaterialAsset& asset, const MaterialEditorOptions& options = {});
        std::uint64_t Builds() const { return builds_; }

    private:
        MaterialEditorModel model_;
        std::uint64_t contentHash_ = 0;
        std::uint64_t layoutHash_ = 0;
        std::uint64_t metadataHash_ = 0;
        std::uint64_t missingHash_ = 0;
        bool valid_ = false;
        std::uint64_t builds_ = 0;
    };

    // ---- the edits: every function changes the asset the way the panel needs and says whether it changed ----

    // A value to the nearest step (from @min, or from 0), inside [min, max], whole for Int and UInt, exactly a float for Float
    double SnapMaterialValue(double value, const MaterialRow& row);

    // The widget gave `components` (as many as the row shows; colours 0..1): the key of the right type is made or changed.
    // Int and UInt are whole, Float / Float2..4 are snapped to the step. gColor of a Float4 goes to asset.tint (and tintSet), and
    // to params.gColor too when the material has it there.
    bool EditMaterialParam(const MaterialRow& row, const std::vector<double>& components, resource::MaterialAsset& asset);

    // Removes the key, so that the value is the @default again. For gColor: tintSet is false, params.gColor is gone and the tint is the
    // @default of the shader (white without one).
    bool ResetMaterialParam(const MaterialRow& row, resource::MaterialAsset& asset);

    // Resets every parameter that the material has and that the panel shows (the textures and the entries "Not in the shader" stay)
    bool ResetAllMaterialParams(const MaterialEditorModel& model, resource::MaterialAsset& asset);

    // A texture for this slot: the first texture goes to "textures" when the material has it there and to the old "texture" key
    // when it does not; the other textures go to "textures" (an old "texture" moves there when the second texture comes)
    bool SetMaterialTexture(const MaterialRow& row, const std::string& textureKey, resource::MaterialAsset& asset);

    // Removes the texture of the slot: the shader gets a white 1x1
    bool ResetMaterialTexture(const MaterialRow& row, resource::MaterialAsset& asset);

    // Removes an entry "Not in the shader" from the material (an old tint becomes white, tintSet false)
    bool RemoveMaterialEntry(const MaterialEntry& entry, resource::MaterialAsset& asset);
    bool RemoveAllMaterialEntries(const MaterialEditorModel& model, resource::MaterialAsset& asset);

    // The shader of the material. What the new shader does not have shows up in "Not in the shader", nothing is lost.
    bool ChangeMaterialShader(resource::MaterialAsset& asset, const std::string& shaderKey);
}
