// MaterialParams.h
//
// The parameters of a material by the names of its shader: a shadow copy of every constant buffer (the bytes at the
// offsets of the ShaderLayout) and a table of the textures by the name of the resource. A setter checks the name and
// the type against the layout and answers with a message that says what is wrong, never with a silent skip.
// No DirectX types: built on ShaderLayout only. docs/render/architecture.md, section 3.5.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <myengine/render/RenderTypes.h>
#include <myengine/render/ShaderLayout.h>

namespace myengine::render
{
    // The raw value of a "params" entry of a .material.json, before it is matched with the layout of a shader
    struct MaterialValue
    {
        enum class Kind : std::uint8_t
        {
            Number, // 1 number: Float, Int or UInt, the layout decides
            Bool,   // 1 number, 0 or 1
            Vector, // 2, 3 or 4 numbers: Float2, Float3, Float4
            Matrix, // 16 numbers, row by row: Float4x4
        };

        Kind kind = Kind::Number;
        std::vector<double> numbers;

        static MaterialValue FromNumber(double value);
        static MaterialValue FromBool(bool value);
        static MaterialValue FromVector(std::vector<double> values); // 2..4 numbers
        static MaterialValue FromMatrix(std::vector<double> values); // 16 numbers

        bool operator==(const MaterialValue& other) const
        {
            return kind == other.kind && numbers == other.numbers;
        }
        bool operator!=(const MaterialValue& other) const
        {
            return !(*this == other);
        }
    };

    // "a number", "a bool", "Float3", "Float4x4": what a raw value is, for the messages
    std::string DescribeMaterialValue(const MaterialValue& value);

    enum class MaterialParamStatus : std::uint8_t
    {
        Ok,
        Warning, // nothing was wrong with the call, but the value is not used (an engine semantic)
        Error,   // the name or the type does not fit the layout: nothing was written
    };

    // Why a call was not Ok: a material is judged by it (an unknown name is a warning, a wrong type is an error)
    enum class MaterialParamReason : std::uint8_t
    {
        None,
        UnknownName,    // the shader has no such variable or texture
        TypeMismatch,   // the value does not fit the type of the variable (or the resource is not a texture)
        EngineSemantic, // gModel, gViewProjection, gLightDirection
        Invalid,        // anything else: an array variable, a file that could not be loaded...
    };

    struct MaterialParamResult
    {
        MaterialParamStatus status = MaterialParamStatus::Ok;
        MaterialParamReason reason = MaterialParamReason::None;
        std::string name;    // the variable or the texture that the call was about
        std::string message; // empty for Ok

        bool IsOk() const { return status == MaterialParamStatus::Ok; }
        bool IsWarning() const { return status == MaterialParamStatus::Warning; }
        bool IsError() const { return status == MaterialParamStatus::Error; }
    };

    // What a material says. The paths are turned into texture handles by the caller (the resource manager).
    // Both formats at once: the keys of the old one ("texture", "tint") and the new ones ("textures", "params").
    struct MaterialValues
    {
        std::map<std::string, MaterialValue> params;        // "params": name of a variable -> value
        std::map<std::string, std::string> textures;        // "textures": name of a texture -> path
        std::string texture;                                // "texture": the first texture of the shader
        std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};  // "tint": the variable gColor
    };

    class MaterialParams
    {
    public:
        // One texture of the shader and what the material put there (an invalid handle: nothing yet)
        struct TextureSlot
        {
            std::string name;                 // "gTexture"
            std::uint32_t registerIndex = 0;  // t0
            TextureHandle texture;
        };

        MaterialParams() = default;
        explicit MaterialParams(ShaderLayout layout);

        const ShaderLayout& Layout() const { return layout_; }
        std::uint64_t LayoutHash() const { return layoutHash_; }

        // gModel, gViewProjection, gLightDirection: the renderer writes them, a material cannot
        static bool IsEngineSemantic(const std::string& name);

        // Setters. The name and the type must fit the layout exactly (SetFloat: a Float variable and nothing else).
        // Matrices are given row by row, as XMFLOAT4X4; they are stored the way the renderer stores gModel
        // (transposed: HLSL packs matrices by columns).
        MaterialParamResult SetFloat(const std::string& name, float value);
        MaterialParamResult SetFloat2(const std::string& name, float x, float y);
        MaterialParamResult SetFloat3(const std::string& name, float x, float y, float z);
        MaterialParamResult SetFloat4(const std::string& name, float x, float y, float z, float w);
        MaterialParamResult SetMatrix(const std::string& name, const std::array<float, 16>& rows);
        MaterialParamResult SetInt(const std::string& name, std::int32_t value);
        MaterialParamResult SetUInt(const std::string& name, std::uint32_t value);
        MaterialParamResult SetBool(const std::string& name, bool value);
        MaterialParamResult SetTexture(const std::string& name, TextureHandle texture);

        // A raw value from a file: the layout decides what a number is (Float, Int or UInt)
        MaterialParamResult SetValue(const std::string& name, const MaterialValue& value);

        // For the draw: the bytes of every constant buffer (indexes of Layout().constantBuffers) and the textures
        std::size_t ConstantBufferCount() const { return constantBuffers_.size(); }
        const std::vector<std::uint8_t>& ConstantBufferBytes(std::size_t index) const { return constantBuffers_[index]; }
        const std::vector<TextureSlot>& Textures() const { return textures_; }
        const TextureSlot* FindTexture(const std::string& name) const;
        // The texture with the smallest t register: where the old "texture" key of a material goes
        const TextureSlot* FirstTexture() const;

        // "variables: gColor (Float4), ...; textures: gTexture": the names a material can use, for the messages
        std::string DescribeNames() const;

    private:
        MaterialParamResult SetVariable(const std::string& name, ShaderValueType type, const void* data, std::size_t bytes);
        MaterialParamResult NoSuchName(const std::string& name, const char* what) const;

        ShaderLayout layout_;
        std::uint64_t layoutHash_ = 0;
        std::vector<std::vector<std::uint8_t>> constantBuffers_; // parallel to layout_.constantBuffers
        std::vector<TextureSlot> textures_;                      // the Texture resources of the layout
    };

    // A number that changes when the constant buffers (names, sizes, variables with types and offsets) or the textures
    // (names, registers) of a layout change: the renderer uses the parameters of a material only for the layout they were built for
    std::uint64_t MaterialLayoutHash(const ShaderLayout& layout);

    using MaterialTextureResolver = std::function<TextureHandle(const std::string& path)>;

    // Puts everything a material says into `params`: the new keys by name, the old ones by the rules of the
    // compatibility (texture -> the first texture of the shader, tint -> gColor; a new key wins over the old one).
    // Returns the messages of everything that did not fit; the rest is applied. The rules for a material that is not right
    // (docs/render/b1-material-params.md): a name that the shader does not have is one warning ("'gX' is not in <program>; names: ..."),
    // a value of the wrong type is an error and the variable stays zero, a texture that cannot be loaded is an error (the caller
    // puts the fallback texture there). `programName`: what the messages call the shader.
    std::vector<MaterialParamResult> ApplyMaterialValues(MaterialParams& params, const MaterialValues& values, const MaterialTextureResolver& resolve, const std::string& programName = {});
}
