// RootSignatureBuilder.cpp

// d3dx12.h first: it brings its own d3d12.h, newer than the one of the Windows SDK
#include <directx-headers/directx/d3dx12.h>

#include <myengine/render/dx12/RootSignatureBuilder.h>

#include <algorithm>
#include <cstdio>

namespace myengine::render::dx12
{
    namespace
    {
        RootVisibility VisibilityOf(const std::uint8_t stages)
        {
            const bool vertex = (stages & StageMask(ShaderStage::Vertex)) != 0;
            const bool pixel = (stages & StageMask(ShaderStage::Pixel)) != 0;
            if (vertex && !pixel)
            {
                return RootVisibility::Vertex;
            }
            if (pixel && !vertex)
            {
                return RootVisibility::Pixel;
            }
            return RootVisibility::All;
        }

        D3D12_SHADER_VISIBILITY ToD3D(const RootVisibility visibility)
        {
            switch (visibility)
            {
                case RootVisibility::Vertex: return D3D12_SHADER_VISIBILITY_VERTEX;
                case RootVisibility::Pixel: return D3D12_SHADER_VISIBILITY_PIXEL;
                case RootVisibility::All: return D3D12_SHADER_VISIBILITY_ALL;
            }
            return D3D12_SHADER_VISIBILITY_ALL;
        }

        void MixHash(std::uint64_t& hash, const std::uint64_t value)
        {
            for (int shift = 0; shift < 64; shift += 8)
            {
                hash ^= (value >> shift) & 0xFFu;
                hash *= 1099511628211ull;
            }
        }

        // The structure of the signature without the names: two programs with the same structure share a root signature
        std::uint64_t HashShape(const RootSignatureDescription& description)
        {
            std::uint64_t hash = 14695981039346656037ull;
            MixHash(hash, description.allowInputAssembler ? 1 : 0);
            for (const RootParameterDesc& parameter : description.parameters)
            {
                MixHash(hash, 0x100u + static_cast<std::uint64_t>(parameter.kind));
                MixHash(hash, parameter.registerIndex);
                MixHash(hash, parameter.space);
                MixHash(hash, static_cast<std::uint64_t>(parameter.visibility));
                MixHash(hash, parameter.dwords);
            }
            for (const StaticSamplerDesc& sampler : description.samplers)
            {
                MixHash(hash, 0x200u + static_cast<std::uint64_t>(sampler.preset));
                MixHash(hash, sampler.registerIndex);
                MixHash(hash, sampler.space);
                MixHash(hash, static_cast<std::uint64_t>(sampler.visibility));
            }
            return hash;
        }

        void AddDiagnostic(RootSignatureBuild& build, const std::string& programName, std::string message, const bool warning = false)
        {
            ShaderDiagnostic diagnostic;
            diagnostic.file = programName;
            diagnostic.warning = warning;
            diagnostic.message = std::move(message);
            build.diagnostics.push_back(std::move(diagnostic));
            if (!warning)
            {
                build.ok = false;
            }
        }

        std::string RegisterName(const char prefix, const std::uint32_t index)
        {
            return std::string(1, prefix) + std::to_string(index);
        }

        D3D12_FILTER FilterOf(const SamplerPreset preset)
        {
            switch (preset)
            {
                case SamplerPreset::PointWrap:
                case SamplerPreset::PointClamp: return D3D12_FILTER_MIN_MAG_MIP_POINT;
                case SamplerPreset::AnisotropicWrap:
                case SamplerPreset::AnisotropicClamp: return D3D12_FILTER_ANISOTROPIC;
                case SamplerPreset::ShadowCompare: return D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
                case SamplerPreset::LinearWrap:
                case SamplerPreset::LinearClamp: return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            }
            return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        }

        D3D12_TEXTURE_ADDRESS_MODE AddressOf(const SamplerPreset preset)
        {
            switch (preset)
            {
                case SamplerPreset::LinearClamp:
                case SamplerPreset::PointClamp:
                case SamplerPreset::AnisotropicClamp: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                case SamplerPreset::ShadowCompare: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
                case SamplerPreset::LinearWrap:
                case SamplerPreset::PointWrap:
                case SamplerPreset::AnisotropicWrap: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            }
            return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        }
    }

    const ResourceBinding* ProgramBinding::Find(const std::string& name) const
    {
        for (const ResourceBinding& binding : bindings)
        {
            if (binding.name == name)
            {
                return &binding;
            }
        }
        return nullptr;
    }

    bool ParseSamplerPreset(const std::string& name, SamplerPreset& outPreset)
    {
        static const std::pair<const char*, SamplerPreset> kPresets[] = {
            {"linear_wrap", SamplerPreset::LinearWrap},
            {"linear_clamp", SamplerPreset::LinearClamp},
            {"point_wrap", SamplerPreset::PointWrap},
            {"point_clamp", SamplerPreset::PointClamp},
            {"anisotropic_wrap", SamplerPreset::AnisotropicWrap},
            {"anisotropic_clamp", SamplerPreset::AnisotropicClamp},
            {"shadow_compare", SamplerPreset::ShadowCompare},
        };
        for (const auto& preset : kPresets)
        {
            if (name == preset.first)
            {
                outPreset = preset.second;
                return true;
            }
        }
        return false;
    }

    std::string ToString(const SamplerPreset preset)
    {
        switch (preset)
        {
            case SamplerPreset::LinearWrap: return "linear_wrap";
            case SamplerPreset::LinearClamp: return "linear_clamp";
            case SamplerPreset::PointWrap: return "point_wrap";
            case SamplerPreset::PointClamp: return "point_clamp";
            case SamplerPreset::AnisotropicWrap: return "anisotropic_wrap";
            case SamplerPreset::AnisotropicClamp: return "anisotropic_clamp";
            case SamplerPreset::ShadowCompare: return "shadow_compare";
        }
        return "?";
    }

    std::string ToString(const RootParameterKind kind)
    {
        switch (kind)
        {
            case RootParameterKind::Constants: return "Constants";
            case RootParameterKind::ConstantBufferView: return "RootCBV";
            case RootParameterKind::TextureTable: return "TextureTable";
        }
        return "?";
    }

    std::string ToString(const RootVisibility visibility)
    {
        switch (visibility)
        {
            case RootVisibility::All: return "VS+PS";
            case RootVisibility::Vertex: return "VS";
            case RootVisibility::Pixel: return "PS";
        }
        return "?";
    }

    RootSignatureBuild BuildRootSignatureDescription(
        const ShaderLayout& layout,
        const std::vector<std::pair<std::string, std::string>>& samplers,
        const std::string& programName,
        const RootSignatureOptions& options)
    {
        RootSignatureBuild build;
        build.ok = true;
        RootSignatureDescription& description = build.description;

        // What the rules do not cover yet is an error with the name of the resource, not a silent omission
        for (const ShaderResource& resource : layout.resources)
        {
            if (resource.type == ShaderResourceType::RWResource)
            {
                AddDiagnostic(build, programName, "resource '" + resource.name + "' is a UAV (" + RegisterName('u', resource.registerIndex) +
                    "): the root signature builder does not support UAVs");
            }
            else if (resource.type == ShaderResourceType::Buffer)
            {
                AddDiagnostic(build, programName, "resource '" + resource.name + "' is a buffer SRV (" + RegisterName('t', resource.registerIndex) +
                    "): the root signature builder does not support buffers");
            }
            else if (resource.bindCount != 1)
            {
                AddDiagnostic(build, programName, "resource '" + resource.name + "' is an array of " + std::to_string(resource.bindCount) +
                    ": the root signature builder does not support arrays of resources");
            }
        }
        if (!build.ok)
        {
            return build;
        }

        description.allowInputAssembler = !layout.vertexInputs.empty();

        // cbuffers: all root constants while they fit together, else all root CBVs
        std::uint32_t constantDwords = 0;
        for (const ShaderConstantBuffer& buffer : layout.constantBuffers)
        {
            constantDwords += (buffer.sizeBytes + 3) / 4;
        }
        const bool rootConstants = constantDwords <= options.rootConstantsLimitDwords;
        for (const ShaderConstantBuffer& buffer : layout.constantBuffers)
        {
            const std::uint32_t dwords = (buffer.sizeBytes + 3) / 4;
            RootParameterDesc parameter;
            parameter.kind = rootConstants ? RootParameterKind::Constants : RootParameterKind::ConstantBufferView;
            parameter.name = buffer.name;
            parameter.registerIndex = buffer.registerIndex;
            parameter.space = buffer.space;
            parameter.visibility = VisibilityOf(buffer.stages);
            parameter.dwords = rootConstants ? dwords : 2;
            parameter.sizeBytes = buffer.sizeBytes;

            ResourceBinding binding;
            binding.name = buffer.name;
            binding.kind = BindingKind::ConstantBuffer;
            binding.rootParameter = static_cast<std::uint32_t>(description.parameters.size());
            binding.rootConstants = rootConstants;
            binding.sizeBytes = buffer.sizeBytes;
            binding.sizeDwords = dwords;
            build.binding.bindings.push_back(std::move(binding));
            description.parameters.push_back(std::move(parameter));
        }

        for (const ShaderResource& resource : layout.resources)
        {
            if (resource.type != ShaderResourceType::Texture)
            {
                continue;
            }
            RootParameterDesc parameter;
            parameter.kind = RootParameterKind::TextureTable;
            parameter.name = resource.name;
            parameter.registerIndex = resource.registerIndex;
            parameter.space = resource.space;
            parameter.visibility = VisibilityOf(resource.stages);
            parameter.dwords = 1;

            ResourceBinding binding;
            binding.name = resource.name;
            binding.kind = BindingKind::Texture;
            binding.rootParameter = static_cast<std::uint32_t>(description.parameters.size());
            build.binding.bindings.push_back(std::move(binding));
            description.parameters.push_back(std::move(parameter));
        }

        // Static samplers: the state is data of the .shader.json, linear wrap when it says nothing
        for (const ShaderResource& resource : layout.resources)
        {
            if (resource.type != ShaderResourceType::Sampler)
            {
                continue;
            }
            StaticSamplerDesc sampler;
            sampler.name = resource.name;
            sampler.registerIndex = resource.registerIndex;
            sampler.space = resource.space;
            sampler.visibility = VisibilityOf(resource.stages);
            for (const auto& entry : samplers)
            {
                if (entry.first != resource.name)
                {
                    continue;
                }
                if (!ParseSamplerPreset(entry.second, sampler.preset))
                {
                    AddDiagnostic(build, programName, "sampler '" + resource.name + "': unknown state '" + entry.second +
                        "' (linear_wrap, linear_clamp, point_wrap, point_clamp, anisotropic_wrap, anisotropic_clamp, shadow_compare)");
                }
            }

            ResourceBinding binding;
            binding.name = resource.name;
            binding.kind = BindingKind::Sampler;
            build.binding.bindings.push_back(std::move(binding));
            description.samplers.push_back(std::move(sampler));
        }
        for (const auto& entry : samplers)
        {
            const ShaderResource* resource = nullptr;
            for (const ShaderResource& candidate : layout.resources)
            {
                if (candidate.type == ShaderResourceType::Sampler && candidate.name == entry.first)
                {
                    resource = &candidate;
                }
            }
            if (resource == nullptr)
            {
                AddDiagnostic(build, programName, "\"samplers\" names '" + entry.first + "', which is not a sampler of the shader", true);
            }
        }

        for (const RootParameterDesc& parameter : description.parameters)
        {
            description.totalDwords += parameter.dwords;
        }
        if (description.totalDwords > kRootSignatureLimitDwords)
        {
            AddDiagnostic(build, programName, "the root signature needs " + std::to_string(description.totalDwords) +
                " DWORD, the limit is " + std::to_string(kRootSignatureLimitDwords));
        }

        description.shapeHash = HashShape(description);
        return build;
    }

    bool SerializeRootSignature(const RootSignatureDescription& description, Microsoft::WRL::ComPtr<ID3DBlob>& outBlob, std::string& outError)
    {
        std::vector<CD3DX12_DESCRIPTOR_RANGE> ranges(description.parameters.size());
        std::vector<CD3DX12_ROOT_PARAMETER> parameters(description.parameters.size());
        for (std::size_t i = 0; i < description.parameters.size(); ++i)
        {
            const RootParameterDesc& source = description.parameters[i];
            const D3D12_SHADER_VISIBILITY visibility = ToD3D(source.visibility);
            switch (source.kind)
            {
                case RootParameterKind::Constants:
                    parameters[i].InitAsConstants(source.dwords, source.registerIndex, source.space, visibility);
                    break;
                case RootParameterKind::ConstantBufferView:
                    parameters[i].InitAsConstantBufferView(source.registerIndex, source.space, visibility);
                    break;
                case RootParameterKind::TextureTable:
                    ranges[i].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, source.registerIndex, source.space);
                    parameters[i].InitAsDescriptorTable(1, &ranges[i], visibility);
                    break;
            }
        }

        std::vector<CD3DX12_STATIC_SAMPLER_DESC> samplers;
        samplers.reserve(description.samplers.size());
        for (const StaticSamplerDesc& source : description.samplers)
        {
            const D3D12_TEXTURE_ADDRESS_MODE address = AddressOf(source.preset);
            samplers.emplace_back(
                source.registerIndex,
                FilterOf(source.preset),
                address,
                address,
                address,
                0.0f,
                8u,
                D3D12_COMPARISON_FUNC_LESS_EQUAL,
                D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
                0.0f,
                D3D12_FLOAT32_MAX,
                ToD3D(source.visibility),
                source.space);
        }

        CD3DX12_ROOT_SIGNATURE_DESC rootDesc;
        rootDesc.Init(
            static_cast<UINT>(parameters.size()),
            parameters.empty() ? nullptr : parameters.data(),
            static_cast<UINT>(samplers.size()),
            samplers.empty() ? nullptr : samplers.data(),
            description.allowInputAssembler ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT : D3D12_ROOT_SIGNATURE_FLAG_NONE);

        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        const HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, outBlob.ReleaseAndGetAddressOf(), errorBlob.GetAddressOf());
        if (FAILED(hr))
        {
            outBlob.Reset();
            outError = errorBlob != nullptr
                ? std::string(static_cast<const char*>(errorBlob->GetBufferPointer()), errorBlob->GetBufferSize())
                : "D3D12SerializeRootSignature failed";
            return false;
        }
        return true;
    }

    std::string FormatRootSignature(const RootSignatureDescription& description)
    {
        char hash[32];
        std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(description.shapeHash));
        std::string text = std::string("Root signature  shape ") + hash + "  " + std::to_string(description.totalDwords) + " of " +
            std::to_string(kRootSignatureLimitDwords) + " DWORD" + (description.allowInputAssembler ? "  input assembler" : "") + "\n";

        const auto pad = [](std::string value, const std::size_t width)
        {
            if (value.size() < width)
            {
                value.append(width - value.size(), ' ');
            }
            return value;
        };
        for (std::size_t i = 0; i < description.parameters.size(); ++i)
        {
            const RootParameterDesc& parameter = description.parameters[i];
            const char registerPrefix = parameter.kind == RootParameterKind::TextureTable ? 't' : 'b';
            text += "  [" + std::to_string(i) + "] " + pad(ToString(parameter.kind), 14) + pad(RegisterName(registerPrefix, parameter.registerIndex), 6) +
                pad(std::to_string(parameter.dwords) + " DWORD", 10) + pad(ToString(parameter.visibility), 7) + parameter.name + "\n";
        }
        for (const StaticSamplerDesc& sampler : description.samplers)
        {
            text += "  static   " + pad("Sampler", 14) + pad(RegisterName('s', sampler.registerIndex), 6) + pad(ToString(sampler.preset), 18) +
                pad(ToString(sampler.visibility), 7) + sampler.name + "\n";
        }
        return text;
    }
}
