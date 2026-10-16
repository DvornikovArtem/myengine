// RootSignatureBuilder.h
//
// The consumer No.1 of the shader reflection: a root signature built from a ShaderLayout by fixed rules, not written
// by hand. The rules and the result are plain data (no device needed), so they are tested without a GPU:
//   cbuffers  -> root constants (one parameter per cbuffer) while all of them together fit into 64 DWORD, else root CBVs
//   textures  -> a descriptor table with one SRV range per texture, visibility from the stages of the layout
//   samplers  -> static samplers; the state comes from "samplers" of the .shader.json (default: linear, wrap)
//   inputs    -> ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT when the vertex shader has inputs
// docs/render/architecture.md, section 3.4.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <myengine/render/ShaderLayout.h>

namespace myengine::render::dx12
{
    // The limit of a root signature: 64 DWORD of root arguments
    constexpr std::uint32_t kRootSignatureLimitDwords = 64;
    constexpr std::uint32_t kNoRootParameter = 0xFFFFFFFFu;

    enum class RootParameterKind : std::uint8_t
    {
        Constants,           // 32-bit constants: the data of a cbuffer, written with SetGraphicsRoot32BitConstants
        ConstantBufferView,  // a root CBV: the data is in a buffer (the frame upload buffer)
        TextureTable,        // a descriptor table with one SRV
    };

    enum class RootVisibility : std::uint8_t
    {
        All,
        Vertex,
        Pixel,
    };

    struct RootParameterDesc
    {
        RootParameterKind kind = RootParameterKind::Constants;
        std::string name;                       // the resource that the parameter carries
        std::uint32_t registerIndex = 0;
        std::uint32_t space = 0;
        RootVisibility visibility = RootVisibility::All;
        std::uint32_t dwords = 0;               // the cost in the 64 DWORD budget (constants: their count, CBV: 2, table: 1)
        std::uint32_t sizeBytes = 0;            // cbuffers: the size of the data
    };

    // The states of a static sampler that a .shader.json can name
    enum class SamplerPreset : std::uint8_t
    {
        LinearWrap,
        LinearClamp,
        PointWrap,
        PointClamp,
        AnisotropicWrap,
    };

    struct StaticSamplerDesc
    {
        std::string name;
        std::uint32_t registerIndex = 0;
        std::uint32_t space = 0;
        SamplerPreset preset = SamplerPreset::LinearWrap;
        RootVisibility visibility = RootVisibility::All;
    };

    struct RootSignatureDescription
    {
        std::vector<RootParameterDesc> parameters;
        std::vector<StaticSamplerDesc> samplers;
        bool allowInputAssembler = false;
        std::uint32_t totalDwords = 0;
        std::uint64_t shapeHash = 0;            // the same structure, the same hash: such programs share one root signature
    };

    enum class BindingKind : std::uint8_t
    {
        ConstantBuffer,
        Texture,
        Sampler,
    };

    // Where a resource of the shader lives in the root signature
    struct ResourceBinding
    {
        std::string name;
        BindingKind kind = BindingKind::ConstantBuffer;
        std::uint32_t rootParameter = kNoRootParameter;  // static samplers have none
        bool rootConstants = false;                      // ConstantBuffer: root constants (true) or a root CBV (false)
        std::uint32_t sizeBytes = 0;                     // ConstantBuffer: the size of the data
        std::uint32_t sizeDwords = 0;                    // ConstantBuffer: the same in DWORD
    };

    // name -> root parameter, computed once when the program is created
    struct ProgramBinding
    {
        std::vector<ResourceBinding> bindings;

        const ResourceBinding* Find(const std::string& name) const;
    };

    struct RootSignatureBuild
    {
        RootSignatureDescription description;
        ProgramBinding binding;
        std::vector<ShaderDiagnostic> diagnostics;  // errors (the program is not created) and warnings
        bool ok = false;
    };

    // Options of the rules; the defaults are the engine's
    struct RootSignatureOptions
    {
        // cbuffers are root constants while their total size does not exceed this many DWORD (0: always root CBVs)
        std::uint32_t rootConstantsLimitDwords = kRootSignatureLimitDwords;
    };

    // `samplers`: the "samplers" of the .shader.json, resource name -> preset name ("linear_wrap")
    RootSignatureBuild BuildRootSignatureDescription(
        const ShaderLayout& layout,
        const std::vector<std::pair<std::string, std::string>>& samplers,
        const std::string& programName,
        const RootSignatureOptions& options = {});

    bool ParseSamplerPreset(const std::string& name, SamplerPreset& outPreset);
    std::string ToString(SamplerPreset preset);
    std::string ToString(RootParameterKind kind);
    std::string ToString(RootVisibility visibility);

    // D3D12SerializeRootSignature of the description (version 1.0); no device is needed
    bool SerializeRootSignature(const RootSignatureDescription& description, Microsoft::WRL::ComPtr<ID3DBlob>& outBlob, std::string& outError);

    // Root parameters, static samplers and the hash of the shape as text, for the dump and the Shader Layouts window
    std::string FormatRootSignature(const RootSignatureDescription& description);
}
