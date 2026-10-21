// ShaderLayout.h
//
// What a compiled shader program expects: bound resources, constant buffers with their variables, vertex inputs.
// Plain data, no DirectX types: the consumers of the reflection (root signature, material parameters, input layout)
// are built on it. Filled by the reflection of the DXBC blobs (engine/src/render/dx12/ShaderReflector.cpp),
// docs/render/architecture.md, section 3.2.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace myengine::render
{
    // A mask: the stages of a program that use something
    enum class ShaderStage : std::uint8_t
    {
        None = 0,
        Vertex = 1,
        Pixel = 2,
    };

    constexpr std::uint8_t StageMask(const ShaderStage stage)
    {
        return static_cast<std::uint8_t>(stage);
    }

    enum class ShaderResourceType : std::uint8_t
    {
        ConstantBuffer, // b#
        Texture,        // t#: Texture1D/2D/3D/Cube and their arrays
        Buffer,         // t#: StructuredBuffer, ByteAddressBuffer, Buffer<T>
        Sampler,        // s#
        RWResource,     // u#
    };

    // One bound resource: CBV, SRV, sampler or UAV
    struct ShaderResource
    {
        std::string name;                                     // "gTexture"
        ShaderResourceType type = ShaderResourceType::Texture;
        std::uint32_t registerIndex = 0;                      // b0 / t0 / s0 / u0
        std::uint32_t space = 0;                              // 0 on SM 5.0
        std::uint32_t bindCount = 1;                          // the size of a resource array
        std::uint8_t stages = 0;                              // mask of ShaderStage
    };

    enum class ShaderValueType : std::uint8_t
    {
        Unknown, // a struct, half, double...: the offset and the size are still right
        Bool,
        Int, Int2, Int3, Int4,
        UInt, UInt2, UInt3, UInt4,
        Float, Float2, Float3, Float4,
        Float3x3, Float4x4,
    };

    // A variable of a constant buffer
    struct ShaderVariable
    {
        std::string name;                                     // "gColor"
        ShaderValueType type = ShaderValueType::Unknown;
        std::uint32_t offset = 0;                             // in bytes from the start of the buffer
        std::uint32_t size = 0;                               // bytes, the whole array included, without the padding after it
        std::uint32_t arrayCount = 1;                         // 1: not an array
    };

    struct ShaderConstantBuffer
    {
        std::string name;                                     // "DrawCB"
        std::uint32_t registerIndex = 0;
        std::uint32_t space = 0;
        std::uint32_t sizeBytes = 0;                          // a multiple of 16
        std::uint8_t stages = 0;                              // mask of ShaderStage
        std::vector<ShaderVariable> variables;                // in the order of declaration (offsets grow)
    };

    // The format of a vertex shader input by the type of its components and the mask: float3 -> Float3
    enum class VertexFormat : std::uint8_t
    {
        Unknown,
        Float, Float2, Float3, Float4,
        Int, Int2, Int3, Int4,
        UInt, UInt2, UInt3, UInt4,
    };

    struct VertexInput
    {
        std::string semantic;                                 // "TEXCOORD"
        std::uint32_t semanticIndex = 0;                      // TEXCOORD1 -> 1
        VertexFormat format = VertexFormat::Unknown;
    };

    // A found variable of a constant buffer
    struct ShaderVariableRef
    {
        const ShaderConstantBuffer* buffer = nullptr;
        const ShaderVariable* variable = nullptr;
    };

    struct ShaderLayout
    {
        std::vector<ShaderResource> resources;                // CBV, SRV, samplers, UAV; by stage order of appearance
        std::vector<ShaderConstantBuffer> constantBuffers;    // the details of the CBV from `resources`
        std::vector<VertexInput> vertexInputs;                // of the vertex shader, in the order of declaration

        // (not FindResource: windows.h defines that name as a macro)
        const ShaderResource* FindResourceByName(const std::string& name) const;
        const ShaderConstantBuffer* FindConstantBuffer(const std::string& name) const;
        // By the name of the variable, in every constant buffer
        ShaderVariableRef FindVariable(const std::string& name) const;

        bool Empty() const
        {
            return resources.empty() && constantBuffers.empty() && vertexInputs.empty();
        }
    };

    // Which variant of a program the layout is asked for
    enum class ShaderVariant : std::uint8_t
    {
        Normal,
        Wireframe, // the same vertex shader with a constant pixel shader
    };

    // The programs that the renderer builds itself
    enum class BuiltinShader : std::uint8_t
    {
        DebugLines,
        Ui,
    };

    // A message of the shader compiler or of the layout check
    struct ShaderDiagnostic
    {
        std::string file;                                     // the file of the message (an include included), or of the program
        std::uint32_t line = 0;                               // 0: unknown
        std::uint32_t column = 0;                             // 0: unknown
        std::uint8_t stage = 0;                               // mask of ShaderStage, 0: the whole program
        bool warning = false;
        std::string code;                                     // "X3004"; empty for our own messages
        std::string message;
    };

    std::string ToString(ShaderStage stages);
    std::string ToString(ShaderResourceType type);
    std::string ToString(ShaderValueType type);
    std::string ToString(VertexFormat format);

    // "file(12,5): error X3004: undeclared identifier 'x'" - the form of the fxc messages
    std::string FormatShaderDiagnostic(const ShaderDiagnostic& diagnostic);

    // The vertex stage and the pixel stage of a program in one layout. A resource that both stages use under the same
    // name and register becomes one entry with both stages. A conflict (a name with different registers or sizes,
    // one register under different names, a constant buffer that differs) is reported in `diagnostics`, the result is then
    // not to be used.
    ShaderLayout MergeShaderLayouts(const ShaderLayout& vertex, const ShaderLayout& pixel, std::vector<ShaderDiagnostic>& diagnostics);

    // A table for the acceptance and for people who write materials: resources, constant buffers with offsets, inputs
    std::string FormatShaderLayout(const ShaderLayout& layout);
}
