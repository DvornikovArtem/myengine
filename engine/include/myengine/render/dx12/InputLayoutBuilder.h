// InputLayoutBuilder.h
//
// The consumer No.3 of the shader reflection: the input layout of a pipeline built from the inputs of the vertex
// shader. The shader says which semantics it reads and with how many components; what the vertex buffer really holds
// (format and offset of each attribute) is data about the buffer, kept in a small table per vertex stream.
// Plain data and a pure function, so it is tested without a GPU. docs/render/architecture.md, section 3.6.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <d3d12.h>

#include <myengine/render/ShaderLayout.h>

namespace myengine::render::dx12
{
    // The size of one vertex of the streams that the renderer writes itself (checked against its structs in the adapter)
    constexpr std::uint32_t kMeshVertexStride = 32;      // float3 position, float3 normal, float2 uv
    constexpr std::uint32_t kDebugLineVertexStride = 32; // float4 position (clip space), float4 colour
    constexpr std::uint32_t kUiVertexStride = 20;        // float2 position, rgba8 colour, float2 uv
    constexpr std::uint32_t kInstanceStride = 64;        // slot 1 of the "mesh_instanced" stream: the model matrix, four float4 rows

    // The format of an attribute in the vertex buffer
    enum class StreamFormat : std::uint8_t
    {
        Float, Float2, Float3, Float4,
        Int, Int2, Int3, Int4,
        UInt, UInt2, UInt3, UInt4,
        Unorm8x4,   // four bytes read by the shader as float4 in 0..1
    };

    struct StreamAttribute
    {
        std::string semantic;                   // "TEXCOORD"
        std::uint32_t semanticIndex = 0;
        StreamFormat format = StreamFormat::Float;
        std::uint32_t offset = 0;               // in bytes from the start of the vertex
        std::uint32_t slot = 0;                 // the vertex buffer slot: 0 the mesh, 1 the per-instance data
    };

    // One vertex stream: what the vertex buffer holds
    struct VertexStreamDesc
    {
        std::string name;                       // "mesh"
        std::uint32_t stride = 0;
        std::vector<StreamAttribute> attributes;
        std::uint32_t instanceStride = 0;       // > 0: the attributes of slot 1 are per instance data of this size
    };

    // One element of the input layout (slot 0: per vertex data; slot 1: per instance data)
    struct InputElement
    {
        std::string semantic;                   // as the shader writes it
        std::uint32_t semanticIndex = 0;
        StreamFormat format = StreamFormat::Float;
        std::uint32_t offset = 0;
        std::uint32_t slot = 0;
    };

    struct InputLayoutBuild
    {
        std::string stream;                     // the name of the vertex stream
        std::uint32_t stride = 0;
        std::uint32_t instanceStride = 0;       // > 0: the program reads per instance data from slot 1
        std::vector<InputElement> elements;     // in the order of the vertex shader inputs
        std::vector<ShaderDiagnostic> diagnostics;
        bool ok = false;
    };

    // The streams of the renderer: "mesh", "mesh_instanced" (the mesh plus the model matrix per instance as INSTANCE0..3),
    // "debug_lines", "ui". nullptr: no such stream
    const VertexStreamDesc* FindVertexStream(const std::string& name);

    // `layout.vertexInputs` against the stream. A semantic that the stream does not have, or an input that the format of the
    // stream cannot feed (an int input from a float attribute, more components than the attribute has) is an error that
    // names the program, the input and the stream. Attributes that the shader does not read are ignored.
    InputLayoutBuild BuildInputLayout(const ShaderLayout& layout, const VertexStreamDesc& stream, const std::string& programName);

    DXGI_FORMAT ToDxgiFormat(StreamFormat format);
    std::string ToString(StreamFormat format);

    // The elements for D3D12_GRAPHICS_PIPELINE_STATE_DESC::InputLayout. The semantic names point into `build`,
    // which must outlive the call that uses them.
    std::vector<D3D12_INPUT_ELEMENT_DESC> ToD3D12Elements(const InputLayoutBuild& build);

    // The layout as text for the dump and the Shader Layouts window: stream, stride, and every element with format and offset
    std::string FormatInputLayout(const InputLayoutBuild& build);
}
