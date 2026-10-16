// Dx12RenderAdapter.cpp

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

#include <directx-headers/directx/d3dx12.h>
#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <myengine/core/Logger.h>
#include <myengine/render/ShaderDescriptor.h>
#include <myengine/render/dx12/Dx12RenderAdapter.h>

#include "Dx12GpuProfiler.h"
#include "ShaderCompiler.h"
#include "ShaderReflector.h"

#include <tracy/Tracy.hpp>

namespace myengine::render::dx12
{
    namespace
    {
        bool IsEnvironmentEnabled(const char* variableName)
        {
            char* value = nullptr;
            std::size_t valueLength = 0;
            if (_dupenv_s(&value, &valueLength, variableName) != 0 || value == nullptr || value[0] == '\0')
            {
                if (value != nullptr)
                {
                    std::free(value);
                }
                return false;
            }

            const std::string normalized = value;
            std::free(value);
            return normalized == "1" || normalized == "true" || normalized == "TRUE" || normalized == "on" || normalized == "ON";
        }

        std::string HrToString(const HRESULT hr)
        {
            std::ostringstream stream;
            stream << "0x" << std::hex << static_cast<unsigned long>(hr);
            return stream.str();
        }

        std::string WideToUtf8(const std::wstring& text)
        {
            if (text.empty())
            {
                return std::string();
            }

            const int requiredSize = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (requiredSize <= 0)
            {
                return std::string();
            }

            std::string result(static_cast<std::size_t>(requiredSize - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), requiredSize, nullptr, nullptr);
            return result;
        }

        DirectX::XMMATRIX MatrixToXm(const Matrix4& matrix)
        {
            DirectX::XMFLOAT4X4 value{};
            std::memcpy(&value, matrix.data.data(), sizeof(float) * 16);
            return DirectX::XMLoadFloat4x4(&value);
        }

        Matrix4 XmToMatrix(const DirectX::XMMATRIX& matrix)
        {
            DirectX::XMFLOAT4X4 value{};
            DirectX::XMStoreFloat4x4(&value, matrix);

            Matrix4 result{};
            std::memcpy(result.data.data(), &value, sizeof(float) * 16);
            return result;
        }

        std::filesystem::path ResolveSourcePath(const char* relativePath)
        {
            std::error_code ec;
            const std::filesystem::path relative = relativePath;
            if (std::filesystem::exists(relative, ec))
            {
                return std::filesystem::weakly_canonical(relative, ec);
            }

            wchar_t modulePath[MAX_PATH]{};
            const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            if (length > 0 && length < MAX_PATH)
            {
                const auto executableRelative = std::filesystem::path(modulePath).parent_path() / relative;
                if (std::filesystem::exists(executableRelative, ec))
                {
                    return std::filesystem::weakly_canonical(executableRelative, ec);
                }
            }

            return std::filesystem::u8path(MYENGINE_SOURCE_DIR) / relative;
        }

        UINT64 AlignUp(const UINT64 value, const UINT64 alignment)
        {
            if (alignment <= 1)
            {
                return value;
            }

            return ((value + alignment - 1) / alignment) * alignment;
        }

    }

    Dx12RenderAdapter::Dx12RenderAdapter(core::Logger& logger)
        : logger_(logger)
        , shaderReflector_(std::make_unique<ShaderReflector>())
        , profiler_(std::make_unique<Dx12GpuProfiler>())
    {
    }

    Dx12RenderAdapter::~Dx12RenderAdapter()
    {
        Shutdown();
    }

    bool Dx12RenderAdapter::Initialize()
    {
        logger_.Info("Initializing DX12 render adapter");

        if (!CreateDevice())
        {
            return false;
        }
        if (!CreateCommandObjects())
        {
            return false;
        }
        if (!CreateFence())
        {
            return false;
        }
        if (profiler_->Initialize(context_.device.Get(), context_.commandQueue.Get(), kBackBufferCount))
        {
            logger_.Info("GPU timestamps are available: frame statistics have GPU times");
        }
        else
        {
            logger_.Warning("GPU timestamps are not available: frame statistics have no GPU times");
        }
        forceRootCbv_ = IsEnvironmentEnabled("MYENGINE_DX12_ROOT_CBV");
        if (forceRootCbv_)
        {
            logger_.Info("MYENGINE_DX12_ROOT_CBV: the constant buffers of every program are root CBVs");
        }
        if (!BuildDebugLinePipeline())
        {
            return false;
        }
        if (!BuildTextureDescriptorHeap())
        {
            return false;
        }
        if (!BuildRenderTargetHeaps())
        {
            return false;
        }
        if (!BuildUiPipeline())
        {
            return false;
        }
        TextureData defaultWhiteTexture;
        defaultWhiteTexture.width = 1;
        defaultWhiteTexture.height = 1;
        defaultWhiteTexture.channels = 4;
        defaultWhiteTexture.pixelsRgba8 = {255, 255, 255, 255};
        defaultWhiteTexture_ = CreateTexture(defaultWhiteTexture);

        logger_.Info("DX12 render adapter initialized");
        return true;
    }

    MeshHandle Dx12RenderAdapter::UploadMesh(const MeshData& meshData)
    {
        ZoneScoped;

        if (meshData.vertices.empty() || meshData.indices.empty())
        {
            logger_.Warning("UploadMesh failed: mesh is empty");
            return {};
        }

        const auto invalidIndex = std::find_if(
            meshData.indices.begin(),
            meshData.indices.end(),
            [&meshData](const std::uint32_t index)
            {
                return static_cast<std::size_t>(index) >= meshData.vertices.size();
            });
        if (invalidIndex != meshData.indices.end())
        {
            logger_.Warning("UploadMesh failed: mesh index is outside the vertex buffer");
            return {};
        }

        std::vector<DxVertex> vertices;
        vertices.reserve(meshData.vertices.size());
        for (const auto& vertex : meshData.vertices)
        {
            vertices.push_back(DxVertex{
                {vertex.position.x, vertex.position.y, vertex.position.z},
                {vertex.normal.x, vertex.normal.y, vertex.normal.z},
                {vertex.uv.x, vertex.uv.y}
                });
        }

        MeshRecord meshRecord;
        const UINT64 vertexDataSize = static_cast<UINT64>(vertices.size() * sizeof(DxVertex));
        const UINT64 indexDataSize = static_cast<UINT64>(meshData.indices.size() * sizeof(std::uint32_t));

        if (!CreateBuffer(vertices.data(), vertexDataSize, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, meshRecord.vertexBuffer))
        {
            return {};
        }

        if (!CreateBuffer(meshData.indices.data(), indexDataSize, D3D12_RESOURCE_STATE_INDEX_BUFFER, meshRecord.indexBuffer))
        {
            return {};
        }

        meshRecord.vertexBufferView.BufferLocation = meshRecord.vertexBuffer->GetGPUVirtualAddress();
        meshRecord.vertexBufferView.StrideInBytes = sizeof(DxVertex);
        meshRecord.vertexBufferView.SizeInBytes = static_cast<UINT>(vertexDataSize);

        meshRecord.indexBufferView.BufferLocation = meshRecord.indexBuffer->GetGPUVirtualAddress();
        meshRecord.indexBufferView.SizeInBytes = static_cast<UINT>(indexDataSize);
        meshRecord.indexBufferView.Format = DXGI_FORMAT_R32_UINT;
        meshRecord.indexCount = static_cast<UINT>(meshData.indices.size());

        const std::uint32_t meshId = nextMeshId_++;
        meshes_.insert_or_assign(meshId, std::move(meshRecord));
        return MeshHandle{meshId};
    }

    TextureHandle Dx12RenderAdapter::CreateTexture(const TextureData& textureData)
    {
        ZoneScoped;

        if (textureSrvHeap_ == nullptr)
        {
            logger_.Warning("CreateTexture failed: SRV heap is not initialized");
            return {};
        }
        constexpr std::size_t kRgba8Channels = 4;
        const std::size_t maxSize = std::numeric_limits<std::size_t>::max();
        if (textureData.width == 0 ||
            textureData.height == 0 ||
            textureData.channels != kRgba8Channels ||
            static_cast<std::uint64_t>(textureData.width) > maxSize / kRgba8Channels)
        {
            logger_.Warning("CreateTexture failed: invalid texture data");
            return {};
        }

        const std::size_t rowBytes = static_cast<std::size_t>(textureData.width) * kRgba8Channels;
        if (static_cast<std::uint64_t>(textureData.height) > maxSize / rowBytes)
        {
            logger_.Warning("CreateTexture failed: texture dimensions overflow pixel buffer size");
            return {};
        }

        const std::size_t expectedPixelBytes = rowBytes * static_cast<std::size_t>(textureData.height);
        if (textureData.pixelsRgba8.size() != expectedPixelBytes)
        {
            logger_.Warning("CreateTexture failed: pixel buffer size does not match texture dimensions");
            return {};
        }
        CollectRetiredResources();
        UINT descriptorIndex = 0;
        if (!AllocateTextureDescriptor(descriptorIndex))
        {
            logger_.Warning("CreateTexture failed: descriptor heap is full");
            return {};
        }

        TextureRecord textureRecord;
        if (!CreateTextureResource(textureData, textureRecord.textureResource))
        {
            freeTextureDescriptors_.push_back(descriptorIndex);
            return {};
        }

        textureRecord.descriptorIndex = descriptorIndex;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = textureSrvHeap_->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += static_cast<UINT64>(textureRecord.descriptorIndex) * static_cast<UINT64>(textureSrvDescriptorSize_);
        context_.device->CreateShaderResourceView(textureRecord.textureResource.Get(), &srvDesc, cpuHandle);

        const std::uint32_t textureId = nextTextureId_++;
        textures_.insert_or_assign(textureId, std::move(textureRecord));
        return TextureHandle{textureId};
    }

    void Dx12RenderAdapter::DestroyTexture(const TextureHandle texture)
    {
        if (!texture.IsValid() || texture.value == defaultWhiteTexture_.value)
        {
            return;
        }

        const auto it = textures_.find(texture.value);
        if (it == textures_.end() || it->second.ownedByRenderTarget)
        {
            return;
        }

        RetiredResource retired;
        retired.resource = std::move(it->second.textureResource);
        retired.descriptorIndex = it->second.descriptorIndex;
        retired.hasDescriptor = true;
        textures_.erase(it);
        RetireResources(std::move(retired));
    }

    ShaderHandle Dx12RenderAdapter::CreateShaderProgram(const ShaderProgramData& shaderProgram)
    {
        const ShaderCompileResult compiled = shaderReflector_->CompileProgram(shaderProgram);
        lastShaderDiagnostics_ = compiled.diagnostics;
        LogShaderDiagnostics(compiled.diagnostics);
        if (!compiled.ok)
        {
            return {};
        }

        ShaderRecord record;
        if (!BuildProgram(*compiled.program, shaderProgram, ProgramKind::Scene, record))
        {
            return {};
        }

        const std::uint32_t shaderId = nextShaderId_++;
        shaders_.insert_or_assign(shaderId, std::move(record));
        return ShaderHandle{shaderId};
    }

    bool Dx12RenderAdapter::GetRootSignature(const RootSignatureDescription& description, Microsoft::WRL::ComPtr<ID3D12RootSignature>& outRootSignature)
    {
        const auto cached = rootSignatures_.find(description.shapeHash);
        if (cached != rootSignatures_.end())
        {
            outRootSignature = cached->second;
            return true;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        std::string error;
        if (!SerializeRootSignature(description, serialized, error))
        {
            logger_.Error("D3D12SerializeRootSignature failed: " + error);
            return false;
        }
        const HRESULT hr = context_.device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(outRootSignature.ReleaseAndGetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateRootSignature failed: " + HrToString(hr));
            return false;
        }
        rootSignatures_.emplace(description.shapeHash, outRootSignature);
        return true;
    }

    void Dx12RenderAdapter::SetRootSignature(ID3D12RootSignature* rootSignature)
    {
        if (rootSignature != boundRootSignature_)
        {
            context_.commandList->SetGraphicsRootSignature(rootSignature);
            boundRootSignature_ = rootSignature;
        }
    }

    // The root signature, the pipeline states and the slots of the values that the renderer writes: all from the layout
    bool Dx12RenderAdapter::BuildProgram(const CompiledShaderProgram& compiled, const ShaderProgramData& data, const ProgramKind kind, ShaderRecord& record)
    {
        ZoneScopedN("Shader::BuildPipeline");

        RootSignatureOptions options;
        if (forceRootCbv_)
        {
            options.rootConstantsLimitDwords = 0;
        }
        const RootSignatureBuild build = BuildRootSignatureDescription(compiled.layout, data.samplers, compiled.sourceName, options);
        lastShaderDiagnostics_.insert(lastShaderDiagnostics_.end(), build.diagnostics.begin(), build.diagnostics.end());
        LogShaderDiagnostics(build.diagnostics);
        if (!build.ok)
        {
            return false;
        }
        if (!GetRootSignature(build.description, record.rootSignature))
        {
            return false;
        }
        record.rootSignatureShape = build.description.shapeHash;
        record.binding = build.binding;
        record.layout = compiled.layout;

        // The values that the renderer writes into the constant buffers are found by name, once
        const auto findVariable = [&](const ShaderConstantBuffer& buffer, const char* name, const ShaderValueType type) -> std::uint32_t
        {
            for (const ShaderVariable& variable : buffer.variables)
            {
                if (variable.name != name)
                {
                    continue;
                }
                if (variable.type != type || variable.arrayCount != 1)
                {
                    logger_.Warning("Shader " + compiled.sourceName + ": variable '" + name + "' is " + ToString(variable.type) + ", the renderer writes " +
                        ToString(type) + ": not written");
                    return kNoOffset;
                }
                return variable.offset;
            }
            return kNoOffset;
        };
        for (const ResourceBinding& binding : build.binding.bindings)
        {
            if (binding.kind == BindingKind::ConstantBuffer)
            {
                const ShaderConstantBuffer* buffer = compiled.layout.FindConstantBuffer(binding.name);
                if (buffer == nullptr)
                {
                    continue;
                }
                ConstantBufferSlot slot;
                slot.rootParameter = binding.rootParameter;
                slot.rootConstants = binding.rootConstants;
                slot.sizeBytes = binding.sizeBytes;
                slot.sizeDwords = binding.sizeDwords;
                slot.model = findVariable(*buffer, "gModel", ShaderValueType::Float4x4);
                slot.viewProjection = findVariable(*buffer, "gViewProjection", ShaderValueType::Float4x4);
                slot.color = findVariable(*buffer, "gColor", ShaderValueType::Float4);
                slot.lightDirection = findVariable(*buffer, "gLightDirection", ShaderValueType::Float4);
                slot.screenSize = findVariable(*buffer, "screenSize", ShaderValueType::Float2);
                slot.translation = findVariable(*buffer, "translation", ShaderValueType::Float2);
                record.slots.constantBuffers.push_back(slot);
            }
            else if (binding.kind == BindingKind::Texture && record.slots.textureParameter == kNoRootParameter)
            {
                record.slots.textureParameter = binding.rootParameter;
            }
        }

        // The input layout: the inputs of the vertex shader against the table of the vertex stream the program reads
        const VertexStreamDesc* stream = FindVertexStream(data.vertexStream);
        if (stream == nullptr)
        {
            ShaderDiagnostic diagnostic;
            diagnostic.file = compiled.sourceName;
            diagnostic.message = "unknown vertex stream '" + data.vertexStream + "' (mesh, debug_lines, ui)";
            lastShaderDiagnostics_.push_back(diagnostic);
            LogShaderDiagnostics({diagnostic});
            return false;
        }
        const InputLayoutBuild inputLayout = BuildInputLayout(compiled.layout, *stream, compiled.sourceName);
        lastShaderDiagnostics_.insert(lastShaderDiagnostics_.end(), inputLayout.diagnostics.begin(), inputLayout.diagnostics.end());
        LogShaderDiagnostics(inputLayout.diagnostics);
        if (!inputLayout.ok)
        {
            return false;
        }
        const std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements = ToD3D12Elements(inputLayout);

        const Microsoft::WRL::ComPtr<ID3DBlob>& vertexShader = compiled.vertexBlob;
        const Microsoft::WRL::ComPtr<ID3DBlob>& pixelShader = compiled.pixelBlob;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
        pipelineDesc.pRootSignature = record.rootSignature.Get();
        pipelineDesc.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
        pipelineDesc.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
        pipelineDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pipelineDesc.SampleMask = UINT_MAX;
        pipelineDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pipelineDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pipelineDesc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipelineDesc.NumRenderTargets = 1;
        pipelineDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipelineDesc.DSVFormat = kDepthFormat;
        pipelineDesc.SampleDesc.Count = 1;
        pipelineDesc.InputLayout = {inputElements.data(), static_cast<UINT>(inputElements.size())};

        switch (kind)
        {
            case ProgramKind::Scene:
                break;
            case ProgramKind::DebugLines:
                pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
                break;
            case ProgramKind::Ui:
            {
                // Alpha blending, no culling, no depth: the UI is drawn over the frame
                D3D12_BLEND_DESC blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
                blendDesc.RenderTarget[0].BlendEnable = TRUE;
                blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
                blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
                blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
                blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
                blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                pipelineDesc.BlendState = blendDesc;
                pipelineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pipelineDesc.DepthStencilState.DepthEnable = FALSE;
                pipelineDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
                break;
            }
        }

        const HRESULT hr = context_.device->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(record.pipelineState.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateGraphicsPipelineState failed for " + compiled.sourceName + ": " + HrToString(hr));
            return false;
        }

        if (kind != ProgramKind::Scene)
        {
            return true;
        }

        // The wireframe view mode: the same vertex shader, a constant light pixel shader (lines of the texture colour
        // vanish on a dark background), wireframe fill. Without this variant the mode is simply not drawn.
        if (wireframePixelShader_ == nullptr)
        {
            static const std::string kWireframePixelShader = "float4 main() : SV_Target { return float4(0.753, 0.753, 0.753, 1.0); }";
            ShaderStageSource wireframeSource;
            wireframeSource.sourceName = "wireframe_ps";
            wireframeSource.source = &kWireframePixelShader;
            wireframeSource.entry = "main";
            wireframeSource.profile = "ps_5_0";
            wireframeSource.stage = ShaderStage::Pixel;
            std::vector<ShaderDiagnostic> wireframeDiagnostics;
            if (!CompileShaderStage(wireframeSource, wireframePixelShader_, wireframeDiagnostics) ||
                !ReflectShader(wireframePixelShader_->GetBufferPointer(), wireframePixelShader_->GetBufferSize(), ShaderStage::Pixel, wireframePixelLayout_, wireframeDiagnostics))
            {
                wireframePixelShader_.Reset();
                LogShaderDiagnostics(wireframeDiagnostics);
            }
        }
        if (wireframePixelShader_ != nullptr)
        {
            pipelineDesc.PS = {wireframePixelShader_->GetBufferPointer(), wireframePixelShader_->GetBufferSize()};
        }
        pipelineDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
        pipelineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        if (FAILED(context_.device->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(record.wireframePipelineState.GetAddressOf()))))
        {
            logger_.Warning("CreateGraphicsPipelineState (wireframe) failed; the shader has no wireframe variant");
        }
        if (wireframePixelShader_ != nullptr)
        {
            std::vector<ShaderDiagnostic> wireframeConflicts;
            record.wireframeLayout = MergeShaderLayouts(compiled.vertexLayout, wireframePixelLayout_, wireframeConflicts);
        }
        return true;
    }

    bool Dx12RenderAdapter::BuildBuiltinProgram(const char* relativeDescriptorPath, const ProgramKind kind, ShaderRecord& record)
    {
        const std::filesystem::path descriptorPath = ResolveSourcePath(relativeDescriptorPath);
        ShaderProgramData program;
        std::string error;
        if (!LoadShaderDescriptor(descriptorPath, program, error))
        {
            logger_.Error("Shader error: " + descriptorPath.string() + ": " + error);
            return false;
        }

        const ShaderCompileResult compiled = shaderReflector_->CompileProgram(program);
        LogShaderDiagnostics(compiled.diagnostics);
        if (!compiled.ok)
        {
            return false;
        }
        return BuildProgram(*compiled.program, program, kind, record);
    }

    bool Dx12RenderAdapter::GetShaderLayout(const ShaderHandle shader, ShaderLayout& outLayout, const ShaderVariant variant) const
    {
        outLayout = {};
        const auto it = shaders_.find(shader.value);
        if (it == shaders_.end())
        {
            return false;
        }
        if (variant == ShaderVariant::Wireframe)
        {
            if (it->second.wireframePipelineState == nullptr)
            {
                return false;
            }
            outLayout = it->second.wireframeLayout;
            return true;
        }
        outLayout = it->second.layout;
        return true;
    }

    bool Dx12RenderAdapter::GetBuiltinShaderLayout(const BuiltinShader shader, ShaderLayout& outLayout) const
    {
        const ShaderLayout& layout = shader == BuiltinShader::DebugLines ? debugProgram_.layout : uiProgram_.layout;
        outLayout = layout;
        return !layout.Empty();
    }

    void Dx12RenderAdapter::LogShaderDiagnostics(const std::vector<ShaderDiagnostic>& diagnostics) const
    {
        for (const ShaderDiagnostic& diagnostic : diagnostics)
        {
            const std::string text = "Shader " + std::string(diagnostic.warning ? "warning: " : "error: ") + FormatShaderDiagnostic(diagnostic);
            if (diagnostic.warning)
            {
                logger_.Warning(text);
            }
            else
            {
                logger_.Error(text);
            }
        }
    }

    RenderSurfaceHandle Dx12RenderAdapter::CreateSurface(HWND hwnd, std::uint32_t width, std::uint32_t height)
    {
        if (hwnd == nullptr || context_.factory == nullptr || context_.commandQueue == nullptr)
        {
            logger_.Error("CreateSurface failed: render adapter is not initialized");
            return {};
        }

        SurfaceData surface;
        surface.hwnd = hwnd;
        surface.width = std::max<std::uint32_t>(width, 1);
        surface.height = std::max<std::uint32_t>(height, 1);

        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = surface.width;
        swapDesc.Height = surface.height;
        swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = kBackBufferCount;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDesc.Flags = allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
        HRESULT hr = context_.factory->CreateSwapChainForHwnd(context_.commandQueue.Get(), hwnd, &swapDesc, nullptr, nullptr, swapChain1.GetAddressOf());
        if (FAILED(hr))
        {
            logger_.Error("CreateSwapChainForHwnd failed: " + HrToString(hr));
            return {};
        }

        hr = context_.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(hr))
        {
            logger_.Warning("MakeWindowAssociation failed: " + HrToString(hr));
        }

        hr = swapChain1.As(&surface.swapChain);
        if (FAILED(hr))
        {
            logger_.Error("Swap chain cast failed: " + HrToString(hr));
            return {};
        }

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.NumDescriptors = kBackBufferCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

        hr = context_.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(surface.rtvHeap.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateDescriptorHeap failed: " + HrToString(hr));
            return {};
        }

        for (UINT backBufferIndex = 0; backBufferIndex < kBackBufferCount; ++backBufferIndex)
        {
            hr = context_.device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(surface.frameAllocators[backBufferIndex].GetAddressOf()));
            if (FAILED(hr))
            {
                logger_.Error("CreateCommandAllocator(frame) failed: " + HrToString(hr));
                return {};
            }
        }

        surface.rtvDescriptorSize = context_.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        RebuildSurfaceBuffers(surface);

        const std::uint32_t surfaceId = nextSurfaceId_++;
        surfaces_.insert_or_assign(surfaceId, std::move(surface));
        return RenderSurfaceHandle{surfaceId};
    }

    void Dx12RenderAdapter::ResizeSurface(const RenderSurfaceHandle handle, const std::uint32_t width, const std::uint32_t height)
    {
        if (!handle.IsValid() || width == 0 || height == 0)
        {
            return;
        }

        auto* surface = FindSurface(handle);
        if (surface == nullptr || surface->swapChain == nullptr)
        {
            return;
        }

        WaitForGpu();

        for (auto& backBuffer : surface->backBuffers)
        {
            backBuffer.Reset();
        }
        for (auto& transientResources : surface->frameTransientResources)
        {
            transientResources.clear();
        }

        const HRESULT hr = surface->swapChain->ResizeBuffers(
            kBackBufferCount,
            width,
            height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        if (FAILED(hr))
        {
            logger_.Error("ResizeBuffers failed: " + HrToString(hr));
            return;
        }

        surface->width = width;
        surface->height = height;
        RebuildSurfaceBuffers(*surface);
    }

    bool Dx12RenderAdapter::BeginFrame(const RenderSurfaceHandle handle, const core::Color& clearColor)
    {
        ZoneScoped;

        auto* surface = FindSurface(handle);
        if (surface == nullptr || surface->dsvHeap == nullptr || surface->depthStencil == nullptr)
        {
            return false;
        }

        CollectRetiredResources();

        surface->currentBackBuffer = surface->swapChain->GetCurrentBackBufferIndex();
        const UINT backBufferIndex = surface->currentBackBuffer;

        if (surface->frameFenceValues[backBufferIndex] != 0 &&
            context_.fence->GetCompletedValue() < surface->frameFenceValues[backBufferIndex])
        {
            WaitForFenceValue(surface->frameFenceValues[backBufferIndex]);
        }

        // That frame's GPU work is done: its timings become the last frame statistics
        if (surface->frameFenceValues[backBufferIndex] != 0 &&
            context_.fence->GetCompletedValue() >= surface->frameFenceValues[backBufferIndex])
        {
            profiler_->CollectSlot(backBufferIndex);
        }

        ++frameCounter_;
        if (context_.device->GetDeviceRemovedReason() != S_OK)
        {
            LogDeviceRemoved("BeginFrame");
            return false;
        }

        if (FAILED(surface->frameAllocators[backBufferIndex]->Reset()))
        {
            logger_.Error("Frame command allocator reset failed");
            return false;
        }

        if (FAILED(context_.commandList->Reset(surface->frameAllocators[backBufferIndex].Get(), nullptr)))
        {
            logger_.Error("Frame command list reset failed");
            return false;
        }

        surface->frameTransientResources[backBufferIndex].clear();
        surface->uiVertexUploadBuffers[backBufferIndex].used = 0;
        surface->uiIndexUploadBuffers[backBufferIndex].used = 0;
        surface->debugVertexUploadBuffers[backBufferIndex].used = 0;
        surface->constantUploadBuffers[backBufferIndex].used = 0;
        boundRootSignature_ = nullptr; // a new command list has no root signature

        ID3D12Resource* backBuffer = surface->backBuffers[backBufferIndex].Get();

        profiler_->BeginFrame(context_.commandList.Get(), backBufferIndex, frameCounter_);
        autoGroup_ = AutoGroup::None;
        profiler_->BeginPass(context_.commandList.Get(), "Clear Back Buffer");

        const auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        context_.commandList->ResourceBarrier(1, &toRenderTarget);
        profiler_->NoteBarriers(1);

        const CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(surface->rtvHeap->GetCPUDescriptorHandleForHeapStart(), static_cast<INT>(surface->currentBackBuffer), static_cast<INT>(surface->rtvDescriptorSize));
        const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = surface->dsvHeap->GetCPUDescriptorHandleForHeapStart();

        const float color[4] = {clearColor.r, clearColor.g, clearColor.b, clearColor.a};

        context_.commandList->RSSetViewports(1, &surface->viewport);
        context_.commandList->RSSetScissorRects(1, &surface->scissorRect);
        context_.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
        context_.commandList->ClearRenderTargetView(rtvHandle, color, 0, nullptr);
        context_.commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        profiler_->EndPass(context_.commandList.Get());

        activeSurface_ = surface;
        activeFrameTransientResources_ = &surface->frameTransientResources[backBufferIndex];
        return true;
    }

    void Dx12RenderAdapter::SetRenderRegion(const RenderSurfaceHandle handle, const IntRect* region)
    {
        auto* surface = FindSurface(handle);
        if (surface == nullptr)
        {
            return;
        }

        if (region == nullptr)
        {
            surface->viewport = {
                0.0f,
                0.0f,
                static_cast<float>(surface->width),
                static_cast<float>(surface->height),
                0.0f,
                1.0f,
            };
            surface->scissorRect = {
                0,
                0,
                static_cast<LONG>(surface->width),
                static_cast<LONG>(surface->height),
            };
        }
        else
        {
            const int left = std::clamp(region->left, 0, static_cast<int>(surface->width));
            const int top = std::clamp(region->top, 0, static_cast<int>(surface->height));
            const int right = std::clamp(region->right, left + 1, static_cast<int>(surface->width));
            const int bottom = std::clamp(region->bottom, top + 1, static_cast<int>(surface->height));

            surface->viewport = {
                static_cast<float>(left),
                static_cast<float>(top),
                static_cast<float>(right - left),
                static_cast<float>(bottom - top),
                0.0f,
                1.0f,
            };
            surface->scissorRect = {left, top, right, bottom};
        }

        if (activeSurface_ == nullptr || surface != activeSurface_ || activeTarget_ != nullptr)
        {
            return;
        }

        context_.commandList->RSSetViewports(1, &surface->viewport);
        context_.commandList->RSSetScissorRects(1, &surface->scissorRect);
    }

    void Dx12RenderAdapter::SetViewProjection(const RenderSurfaceHandle handle, const Matrix4& view, const Matrix4& projection)
    {
        auto* surface = FindSurface(handle);
        if (surface == nullptr)
        {
            return;
        }

        const Matrix4 viewProjection = XmToMatrix(DirectX::XMMatrixMultiply(MatrixToXm(view), MatrixToXm(projection)));
        if (activeTarget_ != nullptr && surface == activeSurface_)
        {
            activeTarget_->viewProjection = viewProjection;
            return;
        }
        surface->viewProjection = viewProjection;
    }

    void Dx12RenderAdapter::Draw(const RenderSurfaceHandle handle, const DrawItem& drawItem)
    {
        ZoneScoped;

        if (activeSurface_ == nullptr || !handle.IsValid())
        {
            return;
        }

        auto* surface = FindSurface(handle);
        if (surface == nullptr || surface != activeSurface_)
        {
            return;
        }

        const auto meshIt = meshes_.find(drawItem.mesh.value);
        const auto textureIt = textures_.find(drawItem.texture.value);
        const auto shaderIt = shaders_.find(drawItem.shader.value);
        if (meshIt == meshes_.end() || textureIt == textures_.end() || shaderIt == shaders_.end())
        {
            return;
        }

        const MeshRecord& mesh = meshIt->second;
        const TextureRecord& texture = textureIt->second;
        const ShaderRecord& shader = shaderIt->second;

        EnterAutoGroup(AutoGroup::Scene);
        ID3D12PipelineState* pipelineState =
            wireframe_ && shader.wireframePipelineState != nullptr ? shader.wireframePipelineState.Get() : shader.pipelineState.Get();
        profiler_->NotePipeline(pipelineState);
        profiler_->NoteTexture(texture.descriptorIndex);
        SetRootSignature(shader.rootSignature.Get());
        context_.commandList->SetPipelineState(pipelineState);
        context_.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_.commandList->IASetVertexBuffers(0, 1, &mesh.vertexBufferView);
        context_.commandList->IASetIndexBuffer(&mesh.indexBufferView);

        ID3D12DescriptorHeap* descriptorHeaps[] = {textureSrvHeap_.Get()};
        context_.commandList->SetDescriptorHeaps(1, descriptorHeaps);

        // The texture of the draw item goes to the first texture of the shader; a shader without one ignores it
        if (shader.slots.textureParameter != kNoRootParameter)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = textureSrvHeap_->GetGPUDescriptorHandleForHeapStart();
            textureHandle.ptr += static_cast<UINT64>(texture.descriptorIndex) * static_cast<UINT64>(textureSrvDescriptorSize_);
            context_.commandList->SetGraphicsRootDescriptorTable(shader.slots.textureParameter, textureHandle);
        }

        if (!shader.slots.constantBuffers.empty())
        {
            // The engine semantics: gModel, gViewProjection, gColor, gLightDirection are written by name where the
            // shader has them; everything else in the constant buffers stays zero
            DirectX::XMFLOAT4X4 model{};
            DirectX::XMFLOAT4X4 viewProjection{};
            DirectX::XMStoreFloat4x4(&model, DirectX::XMMatrixTranspose(MatrixToXm(drawItem.model)));
            DirectX::XMStoreFloat4x4(&viewProjection, DirectX::XMMatrixTranspose(MatrixToXm(CurrentViewProjection(*surface))));
            const float color[4] = {drawItem.color.r, drawItem.color.g, drawItem.color.b, drawItem.color.a};
            float light[4] = {0.35f, -1.0f, 0.25f, 0.25f};
            if (activeTarget_ != nullptr)
            {
                light[0] = targetLightDirection_.x;
                light[1] = targetLightDirection_.y;
                light[2] = targetLightDirection_.z;
                light[3] = targetAmbient_;
            }

            for (const ConstantBufferSlot& slot : shader.slots.constantBuffers)
            {
                constantScratch_.assign(slot.sizeBytes, 0);
                const auto put = [this](const std::uint32_t offset, const void* value, const std::size_t size)
                {
                    if (offset != kNoOffset)
                    {
                        std::memcpy(constantScratch_.data() + offset, value, size);
                    }
                };
                put(slot.model, &model, sizeof(model));
                put(slot.viewProjection, &viewProjection, sizeof(viewProjection));
                put(slot.color, color, sizeof(color));
                put(slot.lightDirection, light, sizeof(light));
                SetConstants(slot, *surface);
            }
        }
        context_.commandList->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
        profiler_->NoteDraw(mesh.indexCount / 3);
    }

    // Sends the bytes of constantScratch_ to the cbuffer: root constants, or a root CBV with the data in the frame upload buffer
    void Dx12RenderAdapter::SetConstants(const ConstantBufferSlot& slot, SurfaceData& surface)
    {
        if (slot.rootConstants)
        {
            context_.commandList->SetGraphicsRoot32BitConstants(slot.rootParameter, slot.sizeDwords, constantScratch_.data(), 0);
            return;
        }

        D3D12_GPU_VIRTUAL_ADDRESS address = 0;
        if (AllocateFrameUploadData(surface.constantUploadBuffers[surface.currentBackBuffer], constantScratch_.data(), slot.sizeBytes, 256, address))
        {
            context_.commandList->SetGraphicsRootConstantBufferView(slot.rootParameter, address);
        }
    }

    void Dx12RenderAdapter::DrawDebugLines(const RenderSurfaceHandle handle, const std::vector<DebugLine>& lines)
    {
        if (activeSurface_ == nullptr || debugProgram_.pipelineState == nullptr || lines.empty())
        {
            return;
        }

        auto* surface = FindSurface(handle);
        if (surface == nullptr || surface != activeSurface_)
        {
            return;
        }

        std::vector<DxDebugVertex> vertices;
        vertices.reserve(lines.size() * 2);

        const DirectX::XMMATRIX viewProjectionMatrix = MatrixToXm(CurrentViewProjection(*surface));

        for (const auto& line : lines)
        {
            const auto appendVertex = [&](const Float3& position)
            {
                const DirectX::XMVECTOR clipPosition = DirectX::XMVector4Transform(
                    DirectX::XMVectorSet(position.x, position.y, position.z, 1.0f),
                    viewProjectionMatrix);

                DirectX::XMFLOAT4 clip{};
                DirectX::XMStoreFloat4(&clip, clipPosition);

                vertices.push_back(DxDebugVertex{
                    {clip.x, clip.y, clip.z, clip.w},
                    {line.color.r, line.color.g, line.color.b, line.color.a},
                });
            };

            appendVertex(line.start);
            appendVertex(line.end);
        }

        const UINT64 vertexBufferSize = static_cast<UINT64>(vertices.size() * sizeof(DxDebugVertex));
        FrameUploadBuffer& uploadBuffer = surface->debugVertexUploadBuffers[surface->currentBackBuffer];
        D3D12_GPU_VIRTUAL_ADDRESS vertexBufferAddress = 0;
        if (!AllocateFrameUploadData(uploadBuffer, vertices.data(), vertexBufferSize, 16, vertexBufferAddress))
        {
            return;
        }

        D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
        vertexBufferView.BufferLocation = vertexBufferAddress;
        vertexBufferView.StrideInBytes = sizeof(DxDebugVertex);
        vertexBufferView.SizeInBytes = static_cast<UINT>(vertexBufferSize);

        EnterAutoGroup(AutoGroup::DebugLines);
        profiler_->NotePipeline(debugProgram_.pipelineState.Get());
        SetRootSignature(debugProgram_.rootSignature.Get());
        context_.commandList->SetPipelineState(debugProgram_.pipelineState.Get());
        context_.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        context_.commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
        context_.commandList->DrawInstanced(static_cast<UINT>(vertices.size()), 1, 0, 0);
        profiler_->NoteDraw(0, lines.size());
    }

    void Dx12RenderAdapter::DrawUiGeometry(const RenderSurfaceHandle handle, const UiDrawData& drawData)
    {
        ZoneScoped;

        if (activeSurface_ == nullptr || uiProgram_.pipelineState == nullptr || activeTarget_ != nullptr)
        {
            return;
        }

        auto* surface = FindSurface(handle);
        if (surface == nullptr ||
            surface != activeSurface_ ||
            drawData.vertices == nullptr ||
            drawData.indices == nullptr ||
            drawData.vertexCount == 0 ||
            drawData.indexCount == 0)
        {
            return;
        }

        const TextureHandle textureHandle = drawData.texture.IsValid() ? drawData.texture : defaultWhiteTexture_;
        const auto textureIt = textures_.find(textureHandle.value);
        if (textureIt == textures_.end())
        {
            return;
        }

        static_assert(sizeof(DxUiVertex) == sizeof(UiVertex), "UI vertex layout mismatch");

        const UINT64 vertexBufferSize = static_cast<UINT64>(drawData.vertexCount * sizeof(UiVertex));
        const UINT64 indexBufferSize = static_cast<UINT64>(drawData.indexCount * sizeof(std::uint32_t));
        FrameUploadBuffer& vertexUploadBuffer = surface->uiVertexUploadBuffers[surface->currentBackBuffer];
        FrameUploadBuffer& indexUploadBuffer = surface->uiIndexUploadBuffers[surface->currentBackBuffer];
        D3D12_GPU_VIRTUAL_ADDRESS vertexBufferAddress = 0;
        D3D12_GPU_VIRTUAL_ADDRESS indexBufferAddress = 0;
        if (!AllocateFrameUploadData(vertexUploadBuffer, drawData.vertices, vertexBufferSize, 16, vertexBufferAddress) ||
            !AllocateFrameUploadData(indexUploadBuffer, drawData.indices, indexBufferSize, sizeof(std::uint32_t), indexBufferAddress))
        {
            return;
        }

        D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
        vertexBufferView.BufferLocation = vertexBufferAddress;
        vertexBufferView.StrideInBytes = sizeof(DxUiVertex);
        vertexBufferView.SizeInBytes = static_cast<UINT>(vertexBufferSize);

        D3D12_INDEX_BUFFER_VIEW indexBufferView{};
        indexBufferView.BufferLocation = indexBufferAddress;
        indexBufferView.SizeInBytes = static_cast<UINT>(indexBufferSize);
        indexBufferView.Format = DXGI_FORMAT_R32_UINT;

        const D3D12_RECT fullScissorRect = surface->scissorRect;
        D3D12_RECT scissorRect = fullScissorRect;
        if (drawData.scissorEnabled)
        {
            scissorRect.left = std::clamp(drawData.scissor.left, 0, static_cast<int>(surface->width));
            scissorRect.top = std::clamp(drawData.scissor.top, 0, static_cast<int>(surface->height));
            scissorRect.right = std::clamp(drawData.scissor.right, 0, static_cast<int>(surface->width));
            scissorRect.bottom = std::clamp(drawData.scissor.bottom, 0, static_cast<int>(surface->height));
        }

        EnterAutoGroup(AutoGroup::Ui);
        profiler_->NotePipeline(uiProgram_.pipelineState.Get());
        profiler_->NoteTexture(textureIt->second.descriptorIndex);
        ID3D12DescriptorHeap* descriptorHeaps[] = {textureSrvHeap_.Get()};
        context_.commandList->SetDescriptorHeaps(1, descriptorHeaps);
        SetRootSignature(uiProgram_.rootSignature.Get());
        context_.commandList->SetPipelineState(uiProgram_.pipelineState.Get());
        context_.commandList->RSSetScissorRects(1, &scissorRect);
        context_.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_.commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
        context_.commandList->IASetIndexBuffer(&indexBufferView);

        // The engine semantics of the UI: screenSize and translation by name, the texture into the first texture slot
        const float screenSize[2] =
        {
            std::max(1.0f, static_cast<float>(surface->width)),
            std::max(1.0f, static_cast<float>(surface->height)),
        };
        const float translation[2] = {drawData.translationX, drawData.translationY};
        for (const ConstantBufferSlot& slot : uiProgram_.slots.constantBuffers)
        {
            constantScratch_.assign(slot.sizeBytes, 0);
            const auto put = [this](const std::uint32_t offset, const void* value, const std::size_t size)
            {
                if (offset != kNoOffset)
                {
                    std::memcpy(constantScratch_.data() + offset, value, size);
                }
            };
            put(slot.screenSize, screenSize, sizeof(screenSize));
            put(slot.translation, translation, sizeof(translation));
            SetConstants(slot, *surface);
        }

        if (uiProgram_.slots.textureParameter != kNoRootParameter)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE gpuTextureHandle = textureSrvHeap_->GetGPUDescriptorHandleForHeapStart();
            gpuTextureHandle.ptr += static_cast<UINT64>(textureIt->second.descriptorIndex) * static_cast<UINT64>(textureSrvDescriptorSize_);
            context_.commandList->SetGraphicsRootDescriptorTable(uiProgram_.slots.textureParameter, gpuTextureHandle);
        }
        context_.commandList->DrawIndexedInstanced(static_cast<UINT>(drawData.indexCount), 1, 0, 0, 0);
        profiler_->NoteDraw(drawData.indexCount / 3);

        context_.commandList->RSSetScissorRects(1, &fullScissorRect);
    }

    void Dx12RenderAdapter::EndFrame(const RenderSurfaceHandle handle)
    {
        ZoneScoped;

        auto* surface = FindSurface(handle);
        if (surface == nullptr || surface != activeSurface_)
        {
            return;
        }

        CloseAutoGroup();
        profiler_->BeginPass(context_.commandList.Get(), "Present Transition");
        ID3D12Resource* backBuffer = surface->backBuffers[surface->currentBackBuffer].Get();
        const auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        context_.commandList->ResourceBarrier(1, &toPresent);
        profiler_->NoteBarriers(1);
        profiler_->EndPass(context_.commandList.Get());
        profiler_->EndFrame(context_.commandList.Get());

        if (FAILED(context_.commandList->Close()))
        {
            logger_.Error("Command list close failed");
            return;
        }

        ID3D12CommandList* commandLists[] = {context_.commandList.Get()};
        context_.commandQueue->ExecuteCommandLists(1, commandLists);

        // MYENGINE_DX12_TEST_REMOVE_DEVICE=<frame>: a device removal on purpose, with this frame's commands submitted,
        // to see what DRED logs (docs/dev/gpu-debugging.md)
        if (testRemoveDeviceFrame_ != 0 && frameCounter_ == testRemoveDeviceFrame_)
        {
            Microsoft::WRL::ComPtr<ID3D12Device5> device5;
            if (SUCCEEDED(context_.device.As(&device5)))
            {
                logger_.Warning("MYENGINE_DX12_TEST_REMOVE_DEVICE: removing the device after frame " + std::to_string(frameCounter_));
                device5->RemoveDevice();
            }
        }

        {
            ZoneScopedN("Present");
            const UINT syncInterval = vsyncEnabled_ ? 1u : 0u;
            const UINT presentFlags = (!vsyncEnabled_ && allowTearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
            const HRESULT presentResult = surface->swapChain->Present(syncInterval, presentFlags);
            if (FAILED(presentResult))
            {
                logger_.Error("Present failed: " + HrToString(presentResult));
                if (presentResult == DXGI_ERROR_DEVICE_REMOVED || presentResult == DXGI_ERROR_DEVICE_RESET)
                {
                    logger_.Error("DX12 device removed reason: " + HrToString(context_.device->GetDeviceRemovedReason()));
                    LogDeviceRemoved("Present");
                }

                activeFrameTransientResources_ = nullptr;
                activeSurface_ = nullptr;
                return;
            }
        }

        ++context_.fenceValue;
        const HRESULT signalResult = context_.commandQueue->Signal(context_.fence.Get(), context_.fenceValue);
        if (FAILED(signalResult))
        {
            logger_.Error("Fence signal failed at EndFrame: " + HrToString(signalResult));
        }
        else
        {
            surface->frameFenceValues[surface->currentBackBuffer] = context_.fenceValue;
        }

        activeFrameTransientResources_ = nullptr;
        activeSurface_ = nullptr;
    }

    RenderTargetHandle Dx12RenderAdapter::CreateRenderTarget(const std::uint32_t width, const std::uint32_t height)
    {
        ZoneScoped;

        constexpr std::uint32_t kMaxTargetSize = 4096;
        if (context_.device == nullptr || targetRtvHeap_ == nullptr || targetDsvHeap_ == nullptr || textureSrvHeap_ == nullptr)
        {
            logger_.Warning("CreateRenderTarget failed: the adapter is not initialized");
            return {};
        }
        if (width == 0 || height == 0 || width > kMaxTargetSize || height > kMaxTargetSize)
        {
            logger_.Warning("CreateRenderTarget failed: invalid size " + std::to_string(width) + "x" + std::to_string(height));
            return {};
        }

        CollectRetiredResources();
        if (freeTargetSlots_.empty())
        {
            logger_.Warning("CreateRenderTarget failed: the render target limit is reached");
            return {};
        }
        UINT descriptorIndex = 0;
        if (!AllocateTextureDescriptor(descriptorIndex))
        {
            logger_.Warning("CreateRenderTarget failed: descriptor heap is full");
            return {};
        }

        RenderTargetRecord record;
        record.width = width;
        record.height = height;

        const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);

        const auto colorDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        D3D12_CLEAR_VALUE colorClear{};
        colorClear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        HRESULT hr = context_.device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &colorDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &colorClear,
            IID_PPV_ARGS(record.color.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommittedResource for the render target failed: " + HrToString(hr));
            freeTextureDescriptors_.push_back(descriptorIndex);
            return {};
        }

        const auto depthDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            kDepthFormat, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = kDepthFormat;
        depthClear.DepthStencil.Depth = 1.0f;
        hr = context_.device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depthClear,
            IID_PPV_ARGS(record.depth.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommittedResource for the render target depth failed: " + HrToString(hr));
            freeTextureDescriptors_.push_back(descriptorIndex);
            return {};
        }

        record.slot = freeTargetSlots_.back();
        freeTargetSlots_.pop_back();

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = targetRtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += static_cast<UINT64>(record.slot) * static_cast<UINT64>(targetRtvDescriptorSize_);
        context_.device->CreateRenderTargetView(record.color.Get(), nullptr, rtvHandle);

        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = targetDsvHeap_->GetCPUDescriptorHandleForHeapStart();
        dsvHandle.ptr += static_cast<UINT64>(record.slot) * static_cast<UINT64>(targetDsvDescriptorSize_);
        context_.device->CreateDepthStencilView(record.depth.Get(), nullptr, dsvHandle);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = textureSrvHeap_->GetCPUDescriptorHandleForHeapStart();
        srvHandle.ptr += static_cast<UINT64>(descriptorIndex) * static_cast<UINT64>(textureSrvDescriptorSize_);
        context_.device->CreateShaderResourceView(record.color.Get(), &srvDesc, srvHandle);

        // The target is a texture like any other, so the UI pass (ImGui::Image) and DrawItem::texture can use it
        TextureRecord textureRecord;
        textureRecord.textureResource = record.color;
        textureRecord.descriptorIndex = descriptorIndex;
        textureRecord.ownedByRenderTarget = true;
        const std::uint32_t textureId = nextTextureId_++;
        textures_.insert_or_assign(textureId, std::move(textureRecord));
        record.texture = TextureHandle{textureId};

        const std::uint32_t targetId = nextRenderTargetId_++;
        renderTargets_.insert_or_assign(targetId, std::move(record));
        return RenderTargetHandle{targetId};
    }

    void Dx12RenderAdapter::DestroyRenderTarget(const RenderTargetHandle target)
    {
        const auto it = renderTargets_.find(target.value);
        if (it == renderTargets_.end())
        {
            return;
        }
        if (activeTarget_ == &it->second)
        {
            logger_.Warning("DestroyRenderTarget ignored: the target is being rendered");
            return;
        }

        RetiredResource retired;
        retired.resource = std::move(it->second.color);
        retired.secondResource = std::move(it->second.depth);
        retired.hasSlot = true;
        retired.slot = it->second.slot;
        if (const auto textureIt = textures_.find(it->second.texture.value); textureIt != textures_.end())
        {
            retired.descriptorIndex = textureIt->second.descriptorIndex;
            retired.hasDescriptor = true;
            textures_.erase(textureIt);
        }
        renderTargets_.erase(it);
        RetireResources(std::move(retired));
    }

    TextureHandle Dx12RenderAdapter::GetRenderTargetTexture(const RenderTargetHandle target) const
    {
        const auto it = renderTargets_.find(target.value);
        return it != renderTargets_.end() ? it->second.texture : TextureHandle{};
    }

    bool Dx12RenderAdapter::GetRenderTargetSize(const RenderTargetHandle target, std::uint32_t& outWidth, std::uint32_t& outHeight) const
    {
        const auto it = renderTargets_.find(target.value);
        if (it == renderTargets_.end())
        {
            outWidth = 0;
            outHeight = 0;
            return false;
        }
        outWidth = it->second.width;
        outHeight = it->second.height;
        return true;
    }

    bool Dx12RenderAdapter::BeginRenderTarget(const RenderTargetHandle target, const core::Color& clearColor)
    {
        ZoneScoped;

        if (activeSurface_ == nullptr || activeTarget_ != nullptr)
        {
            return false;
        }
        const auto it = renderTargets_.find(target.value);
        if (it == renderTargets_.end() || it->second.color == nullptr)
        {
            return false;
        }

        RenderTargetRecord& record = it->second;
        CloseAutoGroup();
        {
            std::string label = pendingTargetLabel_.empty() ? "Render Target #" + std::to_string(target.value) : pendingTargetLabel_;
            pendingTargetLabel_.clear();
            profiler_->BeginPass(context_.commandList.Get(), label, true, record.width, record.height);
        }
        const auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
            record.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        context_.commandList->ResourceBarrier(1, &toRenderTarget);
        profiler_->NoteBarriers(1);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = targetRtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += static_cast<UINT64>(record.slot) * static_cast<UINT64>(targetRtvDescriptorSize_);
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = targetDsvHeap_->GetCPUDescriptorHandleForHeapStart();
        dsvHandle.ptr += static_cast<UINT64>(record.slot) * static_cast<UINT64>(targetDsvDescriptorSize_);

        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(record.width), static_cast<float>(record.height), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(record.width), static_cast<LONG>(record.height)};
        const float color[4] = {clearColor.r, clearColor.g, clearColor.b, clearColor.a};

        context_.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
        context_.commandList->RSSetViewports(1, &viewport);
        context_.commandList->RSSetScissorRects(1, &scissor);
        context_.commandList->ClearRenderTargetView(rtvHandle, color, 0, nullptr);
        context_.commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        record.viewProjection = Matrix4::Identity();
        activeTarget_ = &record;
        wireframeBeforeTarget_ = wireframe_;
        wireframe_ = false;
        return true;
    }

    void Dx12RenderAdapter::EndRenderTarget(const RenderTargetHandle target)
    {
        ZoneScoped;

        const auto it = renderTargets_.find(target.value);
        if (it == renderTargets_.end() || activeTarget_ != &it->second || activeSurface_ == nullptr)
        {
            return;
        }

        const auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
            it->second.color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        context_.commandList->ResourceBarrier(1, &toShaderResource);
        profiler_->NoteBarriers(1);
        profiler_->EndPass(context_.commandList.Get());

        // Back to the surface that the frame draws into
        SurfaceData& surface = *activeSurface_;
        const CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
            surface.rtvHeap->GetCPUDescriptorHandleForHeapStart(), static_cast<INT>(surface.currentBackBuffer), static_cast<INT>(surface.rtvDescriptorSize));
        const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = surface.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        context_.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
        context_.commandList->RSSetViewports(1, &surface.viewport);
        context_.commandList->RSSetScissorRects(1, &surface.scissorRect);

        activeTarget_ = nullptr;
        wireframe_ = wireframeBeforeTarget_;
    }

    bool Dx12RenderAdapter::ReadRenderTargetPixels(const RenderTargetHandle target, std::vector<std::uint8_t>& outRgba8)
    {
        ZoneScoped;

        outRgba8.clear();
        const auto it = renderTargets_.find(target.value);
        if (it == renderTargets_.end() || it->second.color == nullptr || activeSurface_ != nullptr)
        {
            return false;
        }

        const RenderTargetRecord& record = it->second;
        const D3D12_RESOURCE_DESC colorDesc = record.color->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rowCount = 0;
        UINT64 rowSize = 0;
        UINT64 totalSize = 0;
        context_.device->GetCopyableFootprints(&colorDesc, 0, 1, 0, &footprint, &rowCount, &rowSize, &totalSize);

        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        const CD3DX12_HEAP_PROPERTIES readbackHeap(D3D12_HEAP_TYPE_READBACK);
        const auto readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(totalSize);
        HRESULT hr = context_.device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(readback.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommittedResource for the readback buffer failed: " + HrToString(hr));
            return false;
        }

        if (!ResetCommandList())
        {
            return false;
        }

        const auto toCopySource = CD3DX12_RESOURCE_BARRIER::Transition(
            record.color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        context_.commandList->ResourceBarrier(1, &toCopySource);

        const CD3DX12_TEXTURE_COPY_LOCATION destination(readback.Get(), footprint);
        const CD3DX12_TEXTURE_COPY_LOCATION source(record.color.Get(), 0);
        context_.commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

        const auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
            record.color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        context_.commandList->ResourceBarrier(1, &toShaderResource);

        if (!ExecuteCommandListAndWait("render target readback"))
        {
            return false;
        }

        const D3D12_RANGE readRange{0, static_cast<SIZE_T>(totalSize)};
        void* mapped = nullptr;
        if (FAILED(readback->Map(0, &readRange, &mapped)) || mapped == nullptr)
        {
            logger_.Error("Mapping the readback buffer failed");
            return false;
        }

        const std::size_t rowBytes = static_cast<std::size_t>(record.width) * 4;
        outRgba8.resize(rowBytes * static_cast<std::size_t>(record.height));
        const auto* sourceBytes = static_cast<const std::uint8_t*>(mapped) + footprint.Offset;
        for (std::uint32_t row = 0; row < record.height; ++row)
        {
            std::memcpy(outRgba8.data() + static_cast<std::size_t>(row) * rowBytes, sourceBytes + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch, rowBytes);
        }
        const D3D12_RANGE writtenRange{0, 0};
        readback->Unmap(0, &writtenRange);
        return true;
    }

    bool Dx12RenderAdapter::BuildRenderTargetHeaps()
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.NumDescriptors = kMaxRenderTargets;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HRESULT hr = context_.device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(targetRtvHeap_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateDescriptorHeap for the render target RTVs failed: " + HrToString(hr));
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
        dsvDesc.NumDescriptors = kMaxRenderTargets;
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hr = context_.device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(targetDsvHeap_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateDescriptorHeap for the render target DSVs failed: " + HrToString(hr));
            return false;
        }

        targetRtvDescriptorSize_ = context_.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        targetDsvDescriptorSize_ = context_.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        freeTargetSlots_.clear();
        for (UINT slot = kMaxRenderTargets; slot > 0; --slot)
        {
            freeTargetSlots_.push_back(slot - 1);
        }
        return true;
    }

    bool Dx12RenderAdapter::AllocateTextureDescriptor(UINT& outIndex)
    {
        if (!freeTextureDescriptors_.empty())
        {
            outIndex = freeTextureDescriptors_.back();
            freeTextureDescriptors_.pop_back();
            return true;
        }
        if (nextTextureDescriptorIndex_ >= kMaxTextureDescriptors)
        {
            return false;
        }
        outIndex = nextTextureDescriptorIndex_++;
        return true;
    }

    // The GPU may still be reading a destroyed texture or target: keep it until the next submitted work is done
    void Dx12RenderAdapter::RetireResources(RetiredResource&& retired)
    {
        retired.fenceValue = context_.fenceValue + 1;
        retiredResources_.push_back(std::move(retired));
    }

    void Dx12RenderAdapter::CollectRetiredResources()
    {
        if (retiredResources_.empty() || context_.fence == nullptr)
        {
            return;
        }

        const UINT64 completed = context_.fence->GetCompletedValue();
        for (auto it = retiredResources_.begin(); it != retiredResources_.end();)
        {
            if (it->fenceValue > completed)
            {
                ++it;
                continue;
            }
            if (it->hasDescriptor)
            {
                freeTextureDescriptors_.push_back(it->descriptorIndex);
            }
            if (it->hasSlot)
            {
                freeTargetSlots_.push_back(it->slot);
            }
            it = retiredResources_.erase(it);
        }
    }

    const Matrix4& Dx12RenderAdapter::CurrentViewProjection(const SurfaceData& surface) const
    {
        return activeTarget_ != nullptr ? activeTarget_->viewProjection : surface.viewProjection;
    }

    void Dx12RenderAdapter::Shutdown()
    {
        if (context_.device == nullptr)
        {
            return;
        }

        WaitForGpu();
        profiler_->Shutdown();

        for (auto& [_, surface] : surfaces_)
        {
            for (auto& uploadBuffer : surface.uiVertexUploadBuffers)
            {
                ReleaseFrameUploadBuffer(uploadBuffer);
            }
            for (auto& uploadBuffer : surface.uiIndexUploadBuffers)
            {
                ReleaseFrameUploadBuffer(uploadBuffer);
            }
            for (auto& uploadBuffer : surface.debugVertexUploadBuffers)
            {
                ReleaseFrameUploadBuffer(uploadBuffer);
            }
            for (auto& uploadBuffer : surface.constantUploadBuffers)
            {
                ReleaseFrameUploadBuffer(uploadBuffer);
            }
        }

        surfaces_.clear();
        meshes_.clear();
        renderTargets_.clear();
        retiredResources_.clear();
        freeTextureDescriptors_.clear();
        freeTargetSlots_.clear();
        activeTarget_ = nullptr;
        targetRtvHeap_.Reset();
        targetDsvHeap_.Reset();
        textures_.clear();
        shaders_.clear();

        uiProgram_ = {};
        debugProgram_ = {};
        boundRootSignature_ = nullptr;
        rootSignatures_.clear();
        textureSrvHeap_.Reset();

        context_.commandList.Reset();
        context_.commandAllocator.Reset();
        context_.commandQueue.Reset();
        context_.fence.Reset();
        context_.device.Reset();
        context_.factory.Reset();

        if (fenceEvent_ != nullptr)
        {
            CloseHandle(fenceEvent_);
            fenceEvent_ = nullptr;
        }

        textureSrvDescriptorSize_ = 0;
        nextTextureDescriptorIndex_ = 0;
        activeSurface_ = nullptr;
        activeFrameTransientResources_ = nullptr;
        defaultWhiteTexture_ = {};

        logger_.Info("DX12 render adapter shutdown complete");
    }

    ID3D12Device* Dx12RenderAdapter::GetDevice() const
    {
        return context_.device.Get();
    }

    ID3D12CommandQueue* Dx12RenderAdapter::GetCommandQueue() const
    {
        return context_.commandQueue.Get();
    }

    ID3D12GraphicsCommandList* Dx12RenderAdapter::GetCommandList() const
    {
        return context_.commandList.Get();
    }

    DXGI_FORMAT Dx12RenderAdapter::GetBackBufferFormat() const
    {
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    DXGI_FORMAT Dx12RenderAdapter::GetDepthStencilFormat() const
    {
        return kDepthFormat;
    }

    UINT Dx12RenderAdapter::GetFramesInFlight() const
    {
        return kBackBufferCount;
    }

    bool Dx12RenderAdapter::CreateDevice()
    {
        UINT factoryFlags = 0;
        // --dx12-debug / MYENGINE_DX12_DEBUG: the debug layer and DRED. --dx12-gbv / MYENGINE_DX12_GBV: GPU-based
        // validation on top. Any configuration (the debug layer needs the "Graphics Tools" Windows feature).
        const bool requestGbv = IsEnvironmentEnabled("MYENGINE_DX12_GBV");
        const bool enableDebugLayer = IsEnvironmentEnabled("MYENGINE_DX12_DEBUG") || requestGbv;
        if (enableDebugLayer)
        {
            Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debugController.GetAddressOf()))))
            {
                debugController->EnableDebugLayer();
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                logger_.Info("DX12 debug layer enabled (--dx12-debug / MYENGINE_DX12_DEBUG)");

                if (requestGbv)
                {
                    Microsoft::WRL::ComPtr<ID3D12Debug1> debugController1;
                    if (SUCCEEDED(debugController.As(&debugController1)))
                    {
                        debugController1->SetEnableGPUBasedValidation(TRUE);
                        logger_.Info("DX12 GPU-based validation enabled (--dx12-gbv): expect a much slower frame");
                    }
                    else
                    {
                        logger_.Warning("GPU-based validation is not available on this system");
                    }
                }
            }
            else
            {
                logger_.Warning(
                    "DX12 debug layer was requested but is not available: install the Windows optional feature "
                    "\"Graphics Tools\" (Settings > System > Optional features)");
            }
        }
#if defined(_DEBUG)
        else
        {
            logger_.Info("DX12 debug layer disabled. Set MYENGINE_DX12_DEBUG=1 (or --dx12-debug) to enable it.");
        }
#endif

        // DRED: breadcrumbs and page-fault data of a removed device. It costs little: on with the debug layer, or alone
        // with MYENGINE_DX12_DRED=1 / --dx12-dred.
        if (enableDebugLayer || IsEnvironmentEnabled("MYENGINE_DX12_DRED"))
        {
            Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(dredSettings.GetAddressOf()))))
            {
                dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dredEnabled_ = true;
                logger_.Info("DX12 DRED enabled: a removed device logs its breadcrumbs");
            }
            else
            {
                logger_.Warning("DX12 DRED is not available on this system");
            }
        }

        {
            char* testFrame = nullptr;
            std::size_t testFrameLength = 0;
            if (_dupenv_s(&testFrame, &testFrameLength, "MYENGINE_DX12_TEST_REMOVE_DEVICE") == 0 && testFrame != nullptr)
            {
                testRemoveDeviceFrame_ = static_cast<std::uint64_t>(std::strtoull(testFrame, nullptr, 10));
                std::free(testFrame);
            }
        }

        HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(context_.factory.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateDXGIFactory2 failed: " + HrToString(hr));
            return false;
        }

        allowTearing_ = false;
        Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(context_.factory.As(&factory5)))
        {
            BOOL allowTearing = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                &allowTearing,
                sizeof(allowTearing))))
            {
                allowTearing_ = (allowTearing == TRUE);
            }
        }

#if defined(_DEBUG)
        vsyncEnabled_ = IsEnvironmentEnabled("MYENGINE_VSYNC");
        if (vsyncEnabled_)
        {
            logger_.Info("DX12 present vsync enabled via MYENGINE_VSYNC.");
        }
        else
        {
            logger_.Info("DX12 present vsync disabled in Debug. Set MYENGINE_VSYNC=1 to enable it.");
        }
#else
        vsyncEnabled_ = IsEnvironmentEnabled("MYENGINE_VSYNC");
#endif

        struct AdapterCandidate
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            DXGI_ADAPTER_DESC1 desc{};
        };

        std::vector<AdapterCandidate> candidates;

        const auto registerCandidate =
            [&](IDXGIAdapter1* adapter)
        {
            if (adapter == nullptr)
            {
                return;
            }

            DXGI_ADAPTER_DESC1 adapterDesc{};
            adapter->GetDesc1(&adapterDesc);
            if ((adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            {
                return;
            }

            if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
            {
                return;
            }

            const bool alreadyRegistered = std::any_of(
                candidates.begin(),
                candidates.end(),
                [&](const AdapterCandidate& candidate)
                {
                    return candidate.desc.AdapterLuid.LowPart == adapterDesc.AdapterLuid.LowPart &&
                        candidate.desc.AdapterLuid.HighPart == adapterDesc.AdapterLuid.HighPart;
                });
            if (alreadyRegistered)
            {
                return;
            }

            candidates.push_back(AdapterCandidate{adapter, adapterDesc});
        };

        Microsoft::WRL::ComPtr<IDXGIFactory6> factory6;
        if (SUCCEEDED(context_.factory.As(&factory6)))
        {
            for (UINT adapterIndex = 0;; ++adapterIndex)
            {
                Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
                hr = factory6->EnumAdapterByGpuPreference(
                    adapterIndex,
                    DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    IID_PPV_ARGS(adapter.GetAddressOf()));
                if (hr == DXGI_ERROR_NOT_FOUND)
                {
                    break;
                }

                if (FAILED(hr))
                {
                    logger_.Warning("EnumAdapterByGpuPreference failed: " + HrToString(hr));
                    break;
                }

                registerCandidate(adapter.Get());
            }
        }

        for (UINT adapterIndex = 0;; ++adapterIndex)
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            hr = context_.factory->EnumAdapters1(adapterIndex, adapter.GetAddressOf());
            if (hr == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }

            if (FAILED(hr))
            {
                logger_.Warning("EnumAdapters1 failed: " + HrToString(hr));
                break;
            }

            registerCandidate(adapter.Get());
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const AdapterCandidate& left, const AdapterCandidate& right)
            {
                if (left.desc.DedicatedVideoMemory != right.desc.DedicatedVideoMemory)
                {
                    return left.desc.DedicatedVideoMemory > right.desc.DedicatedVideoMemory;
                }

                return left.desc.SharedSystemMemory > right.desc.SharedSystemMemory;
            });

        for (const AdapterCandidate& candidate : candidates)
        {
            logger_.Info(
                "DX12 adapter candidate: " +
                WideToUtf8(candidate.desc.Description) +
                " | dedicated_vram_mb=" +
                std::to_string(static_cast<unsigned long long>(candidate.desc.DedicatedVideoMemory / (1024ull * 1024ull))));

            hr = D3D12CreateDevice(candidate.adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(context_.device.GetAddressOf()));
            if (SUCCEEDED(hr))
            {
                adapterName_ = WideToUtf8(candidate.desc.Description);
                logger_.Info(
                    "DX12 hardware adapter selected: " +
                    WideToUtf8(candidate.desc.Description) +
                    " | dedicated_vram_mb=" +
                    std::to_string(static_cast<unsigned long long>(candidate.desc.DedicatedVideoMemory / (1024ull * 1024ull))));
                return true;
            }
        }

        if (!candidates.empty())
        {
            logger_.Warning("All hardware adapter device creation attempts failed. Falling back to WARP.");
        }
        else
        {
            logger_.Warning("No supported hardware adapters found. Falling back to WARP.");
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter> warpAdapter;
        hr = context_.factory->EnumWarpAdapter(IID_PPV_ARGS(warpAdapter.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("EnumWarpAdapter failed: " + HrToString(hr));
            return false;
        }

        hr = D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(context_.device.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("WARP device creation failed: " + HrToString(hr));
            return false;
        }

        adapterName_ = "WARP software rasterizer";
        logger_.Warning("DX12 is running on WARP software rasterizer.");
        return true;
    }

    bool Dx12RenderAdapter::CreateCommandObjects()
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        HRESULT hr = context_.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(context_.commandQueue.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommandQueue failed: " + HrToString(hr));
            return false;
        }

        hr = context_.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(context_.commandAllocator.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommandAllocator failed: " + HrToString(hr));
            return false;
        }

        hr = context_.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, context_.commandAllocator.Get(), nullptr, IID_PPV_ARGS(context_.commandList.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommandList failed: " + HrToString(hr));
            return false;
        }

        context_.commandList->Close();
        return true;
    }

    bool Dx12RenderAdapter::CreateFence()
    {
        const HRESULT hr = context_.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(context_.fence.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateFence failed: " + HrToString(hr));
            return false;
        }

        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fenceEvent_ == nullptr)
        {
            logger_.Error("CreateEventW for fence failed");
            return false;
        }

        return true;
    }

    bool Dx12RenderAdapter::BuildDebugLinePipeline()
    {
        return BuildBuiltinProgram("assets/shaders/debug_line.shader.json", ProgramKind::DebugLines, debugProgram_);
    }

    bool Dx12RenderAdapter::BuildTextureDescriptorHeap()
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.NumDescriptors = kMaxTextureDescriptors;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        const HRESULT hr = context_.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(textureSrvHeap_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateDescriptorHeap for SRV failed: " + HrToString(hr));
            return false;
        }

        textureSrvDescriptorSize_ = context_.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        nextTextureDescriptorIndex_ = 0;
        return true;
    }

    bool Dx12RenderAdapter::BuildUiPipeline()
    {
        return BuildBuiltinProgram("assets/shaders/ui_overlay.shader.json", ProgramKind::Ui, uiProgram_);
    }

    bool Dx12RenderAdapter::ResetCommandList()
    {
        if (FAILED(context_.commandAllocator->Reset()))
        {
            logger_.Error("Command allocator reset failed");
            return false;
        }

        if (FAILED(context_.commandList->Reset(context_.commandAllocator.Get(), nullptr)))
        {
            logger_.Error("Command list reset failed");
            return false;
        }

        boundRootSignature_ = nullptr;
        return true;
    }

    bool Dx12RenderAdapter::CreateUploadBuffer(const void* data, const UINT64 dataSize, Microsoft::WRL::ComPtr<ID3D12Resource>& outBuffer)
    {
        if (data == nullptr || dataSize == 0)
        {
            return false;
        }

        const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(dataSize);

        const HRESULT hr = context_.device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(outBuffer.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommittedResource(upload) failed: " + HrToString(hr));
            return false;
        }

        void* mappedData = nullptr;
        const CD3DX12_RANGE readRange(0, 0);
        if (FAILED(outBuffer->Map(0, &readRange, &mappedData)))
        {
            logger_.Error("Upload buffer map failed");
            outBuffer.Reset();
            return false;
        }

        std::memcpy(mappedData, data, static_cast<std::size_t>(dataSize));
        outBuffer->Unmap(0, nullptr);
        return true;
    }

    bool Dx12RenderAdapter::EnsureFrameUploadBuffer(FrameUploadBuffer& buffer, const UINT64 requiredSize)
    {
        if (requiredSize == 0)
        {
            return false;
        }

        if (buffer.resource != nullptr && buffer.capacity >= requiredSize && buffer.mappedData != nullptr)
        {
            return true;
        }

        const UINT64 previousUsedSize = buffer.used;
        Microsoft::WRL::ComPtr<ID3D12Resource> previousResource = buffer.resource;
        std::uint8_t* previousMappedData = buffer.mappedData;
        const UINT64 previousCapacity = buffer.capacity;

        UINT64 capacity = std::max<UINT64>(64ull * 1024ull, requiredSize);
        capacity = std::max(capacity, previousCapacity * 2ull);
        capacity = AlignUp(capacity, 64ull * 1024ull);

        const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(capacity);

        Microsoft::WRL::ComPtr<ID3D12Resource> newResource;
        const HRESULT hr = context_.device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(newResource.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommittedResource(frame upload) failed: " + HrToString(hr));
            return false;
        }

        void* mappedData = nullptr;
        const CD3DX12_RANGE readRange(0, 0);
        if (FAILED(newResource->Map(0, &readRange, &mappedData)))
        {
            logger_.Error("Map frame upload buffer failed");
            return false;
        }

        auto* newMappedData = static_cast<std::uint8_t*>(mappedData);
        if (previousMappedData != nullptr && previousUsedSize > 0)
        {
            std::memcpy(newMappedData, previousMappedData, static_cast<std::size_t>(previousUsedSize));
        }

        if (previousResource != nullptr)
        {
            if (previousMappedData != nullptr)
            {
                previousResource->Unmap(0, nullptr);
            }

            if (activeFrameTransientResources_ != nullptr)
            {
                activeFrameTransientResources_->push_back(previousResource);
            }
        }

        buffer.resource = std::move(newResource);
        buffer.mappedData = newMappedData;
        buffer.capacity = capacity;
        buffer.used = previousUsedSize;
        return true;
    }

    bool Dx12RenderAdapter::AllocateFrameUploadData(
        FrameUploadBuffer& buffer,
        const void* data,
        const UINT64 dataSize,
        const UINT64 alignment,
        D3D12_GPU_VIRTUAL_ADDRESS& outGpuAddress)
    {
        if (data == nullptr || dataSize == 0)
        {
            return false;
        }

        const UINT64 alignedOffset = AlignUp(buffer.used, alignment);
        const UINT64 requiredSize = alignedOffset + dataSize;
        if (!EnsureFrameUploadBuffer(buffer, requiredSize))
        {
            return false;
        }

        std::memcpy(buffer.mappedData + alignedOffset, data, static_cast<std::size_t>(dataSize));
        outGpuAddress = buffer.resource->GetGPUVirtualAddress() + alignedOffset;
        buffer.used = requiredSize;
        return true;
    }

    void Dx12RenderAdapter::ReleaseFrameUploadBuffer(FrameUploadBuffer& buffer)
    {
        if (buffer.resource != nullptr && buffer.mappedData != nullptr)
        {
            buffer.resource->Unmap(0, nullptr);
        }

        buffer.resource.Reset();
        buffer.mappedData = nullptr;
        buffer.capacity = 0;
        buffer.used = 0;
    }

    bool Dx12RenderAdapter::ExecuteCommandListAndWait(const char* contextLabel)
    {
        if (FAILED(context_.commandList->Close()))
        {
            logger_.Error(std::string("Command list close failed during ") + contextLabel);
            return false;
        }

        ID3D12CommandList* commandLists[] = {context_.commandList.Get()};
        context_.commandQueue->ExecuteCommandLists(1, commandLists);
        WaitForGpu();
        return true;
    }

    bool Dx12RenderAdapter::CreateBuffer(const void* data, const UINT64 dataSize, const D3D12_RESOURCE_STATES finalState, Microsoft::WRL::ComPtr<ID3D12Resource>& outBuffer)
    {
        if (data == nullptr || dataSize == 0)
        {
            return false;
        }

        if (!ResetCommandList())
        {
            return false;
        }

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        const auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(dataSize);
        HRESULT hr = context_.device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(outBuffer.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommittedResource for buffer failed: " + HrToString(hr));
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
        CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
        hr = context_.device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(uploadBuffer.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommittedResource for upload buffer failed: " + HrToString(hr));
            return false;
        }

        void* mappedData = nullptr;
        const CD3DX12_RANGE readRange(0, 0);
        hr = uploadBuffer->Map(0, &readRange, &mappedData);
        if (FAILED(hr))
        {
            logger_.Error("Map upload buffer failed: " + HrToString(hr));
            return false;
        }

        std::memcpy(mappedData, data, static_cast<std::size_t>(dataSize));
        uploadBuffer->Unmap(0, nullptr);

        context_.commandList->ResourceBarrier(1, std::array{CD3DX12_RESOURCE_BARRIER::Transition(outBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)}.data());
        context_.commandList->CopyBufferRegion(outBuffer.Get(), 0, uploadBuffer.Get(), 0, dataSize);
        context_.commandList->ResourceBarrier(1, std::array{CD3DX12_RESOURCE_BARRIER::Transition(outBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState)}.data());

        return ExecuteCommandListAndWait("buffer upload");
    }

    bool Dx12RenderAdapter::CreateTextureResource(const TextureData& textureData, Microsoft::WRL::ComPtr<ID3D12Resource>& outTexture)
    {
        if (!ResetCommandList())
        {
            return false;
        }

        const auto textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, textureData.width, textureData.height);

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = context_.device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(outTexture.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommittedResource for texture failed: " + HrToString(hr));
            return false;
        }

        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(outTexture.Get(), 0, 1);
        const auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
        Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
        CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
        hr = context_.device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(uploadBuffer.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommittedResource for texture upload failed: " + HrToString(hr));
            return false;
        }

        D3D12_SUBRESOURCE_DATA textureSubresource{};
        textureSubresource.pData = textureData.pixelsRgba8.data();
        textureSubresource.RowPitch = static_cast<LONG_PTR>(textureData.width) * 4;
        textureSubresource.SlicePitch = textureSubresource.RowPitch * static_cast<LONG_PTR>(textureData.height);

        UpdateSubresources<1>(
            context_.commandList.Get(),
            outTexture.Get(),
            uploadBuffer.Get(),
            0,
            0,
            1,
            &textureSubresource);

        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(outTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        context_.commandList->ResourceBarrier(1, &barrier);

        return ExecuteCommandListAndWait("texture upload");
    }

    void Dx12RenderAdapter::RebuildSurfaceBuffers(SurfaceData& surface)
    {
        for (auto& backBuffer : surface.backBuffers)
        {
            backBuffer.Reset();
        }
        surface.depthStencil.Reset();
        surface.dsvHeap.Reset();

        surface.currentBackBuffer = surface.swapChain->GetCurrentBackBufferIndex();

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(surface.rtvHeap->GetCPUDescriptorHandleForHeapStart());
        for (UINT i = 0; i < kBackBufferCount; ++i)
        {
            HRESULT hr = surface.swapChain->GetBuffer(i, IID_PPV_ARGS(surface.backBuffers[i].GetAddressOf()));
            if (FAILED(hr))
            {
                logger_.Error("GetBuffer failed during surface rebuild: " + HrToString(hr));
                continue;
            }

            context_.device->CreateRenderTargetView(surface.backBuffers[i].Get(), nullptr, rtvHandle);
            rtvHandle.Offset(1, static_cast<INT>(surface.rtvDescriptorSize));
        }

        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;

        HRESULT hr = context_.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(surface.dsvHeap.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateDescriptorHeap for DSV failed: " + HrToString(hr));
            return;
        }

        D3D12_RESOURCE_DESC depthDesc{};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = surface.width;
        depthDesc.Height = surface.height;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = kDepthFormat;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depthClearValue{};
        depthClearValue.Format = kDepthFormat;
        depthClearValue.DepthStencil.Depth = 1.0f;
        depthClearValue.DepthStencil.Stencil = 0;

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        hr = context_.device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depthClearValue,
            IID_PPV_ARGS(surface.depthStencil.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommittedResource for depth buffer failed: " + HrToString(hr));
            return;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = kDepthFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

        context_.device->CreateDepthStencilView(surface.depthStencil.Get(), &dsvDesc, surface.dsvHeap->GetCPUDescriptorHandleForHeapStart());

        surface.viewport = {0.0f, 0.0f, static_cast<float>(surface.width), static_cast<float>(surface.height), 0.0f, 1.0f};
        surface.scissorRect = {0, 0, static_cast<LONG>(surface.width), static_cast<LONG>(surface.height)};
    }

    void Dx12RenderAdapter::EnterAutoGroup(const AutoGroup group)
    {
        if (activeTarget_ != nullptr || group == autoGroup_)
        {
            return; // inside a render target pass everything belongs to the pass
        }

        CloseAutoGroup();
        const char* name = "Scene";
        switch (group)
        {
            case AutoGroup::Scene: name = "Scene"; break;
            case AutoGroup::DebugLines: name = "Debug Lines"; break;
            case AutoGroup::Ui: name = "UI"; break;
            case AutoGroup::None: return;
        }
        profiler_->BeginPass(context_.commandList.Get(), name);
        autoGroup_ = group;
    }

    void Dx12RenderAdapter::CloseAutoGroup()
    {
        if (autoGroup_ != AutoGroup::None)
        {
            profiler_->EndPass(context_.commandList.Get());
            autoGroup_ = AutoGroup::None;
        }
    }

    void Dx12RenderAdapter::BeginDebugEvent(const std::string& name)
    {
        if (activeSurface_ != nullptr)
        {
            profiler_->BeginPass(context_.commandList.Get(), name);
        }
    }

    void Dx12RenderAdapter::EndDebugEvent()
    {
        if (activeSurface_ != nullptr)
        {
            profiler_->EndPass(context_.commandList.Get());
        }
    }

    bool Dx12RenderAdapter::GetLastFrameStats(GpuFrameStats& out, const bool waitForGpu)
    {
        if (waitForGpu && activeSurface_ == nullptr)
        {
            WaitForGpu();
            // Every slot is finished now (the newest frame wins)
            for (UINT slot = 0; slot < kBackBufferCount; ++slot)
            {
                profiler_->CollectSlot(slot);
            }
        }
        return profiler_->GetLastCompleted(out);
    }

    void Dx12RenderAdapter::LogDeviceRemoved(const char* where)
    {
        if (deviceRemovedLogged_ || context_.device == nullptr)
        {
            return;
        }
        deviceRemovedLogged_ = true;

        logger_.Error(std::string("DX12 device removed (") + where + "), reason " + HrToString(context_.device->GetDeviceRemovedReason()) +
            " at frame " + std::to_string(frameCounter_));
        if (!dredEnabled_)
        {
            logger_.Error("No DRED data: run with --dx12-debug (or --dx12-dred) to get the breadcrumbs of the commands that were executing");
            return;
        }

        static const char* const kOpNames[] = {
            "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect", "Dispatch",
            "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource", "ClearRenderTargetView",
            "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier", "ExecuteBundle", "Present", "ResolveQueryData",
            "BeginSubmission", "EndSubmission", "DecodeFrame", "ProcessFrames", "AtomicCopyBufferUint", "AtomicCopyBufferUint64",
            "ResolveSubresourceRegion", "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2",
            "ProcessFrames1", "BuildRaytracingAccelerationStructure", "EmitRaytracingAccelerationStructurePostBuildInfo",
            "CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand", "ExecuteMetaCommand", "EstimateMotion",
            "ResolveMotionVectorHeap", "SetPipelineState1", "InitializeExtensionCommand", "ExecuteExtensionCommand", "DispatchMesh",
        };
        const auto opName = [&](const int op) -> std::string
        {
            if (op >= 0 && op < static_cast<int>(std::size(kOpNames)))
            {
                return kOpNames[op];
            }
            return "op#" + std::to_string(op);
        };

        Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
        if (FAILED(context_.device.As(&dred)))
        {
            logger_.Error("DRED interface is not available");
            return;
        }

        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
        {
            int listIndex = 0;
            for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode; node != nullptr; node = node->pNext, ++listIndex)
            {
                const UINT completed = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
                std::string line = "DRED breadcrumbs #" + std::to_string(listIndex) +
                    ": commands " + std::to_string(completed) + " of " + std::to_string(node->BreadcrumbCount) + " completed";
                if (node->pCommandListDebugNameA != nullptr)
                {
                    line += ", list \"" + std::string(node->pCommandListDebugNameA) + "\"";
                }
                if (node->pCommandQueueDebugNameA != nullptr)
                {
                    line += ", queue \"" + std::string(node->pCommandQueueDebugNameA) + "\"";
                }
                logger_.Error(line);

                if (node->pCommandHistory != nullptr && node->BreadcrumbCount > 0)
                {
                    const UINT first = completed > 4 ? completed - 4 : 0;
                    const UINT last = std::min(node->BreadcrumbCount, completed + 4);
                    std::string around = "  around the last completed command:";
                    for (UINT index = first; index < last; ++index)
                    {
                        around += (index == completed ? " >>" : " ") + std::to_string(index) + ":" +
                            opName(static_cast<int>(node->pCommandHistory[index])) + (index == completed ? "<<" : "");
                    }
                    logger_.Error(around);
                }
            }
            if (listIndex == 0)
            {
                logger_.Error("DRED: no breadcrumbs were recorded");
            }
        }

        D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)) && pageFault.PageFaultVA != 0)
        {
            std::ostringstream address;
            address << "DRED page fault at GPU address 0x" << std::hex << pageFault.PageFaultVA;
            logger_.Error(address.str());
            const auto logNodes = [&](const D3D12_DRED_ALLOCATION_NODE* node, const char* title)
            {
                for (int count = 0; node != nullptr && count < 8; node = node->pNext, ++count)
                {
                    logger_.Error(std::string("  ") + title + ": " + (node->ObjectNameA != nullptr ? node->ObjectNameA : "(unnamed)") +
                        " type=" + std::to_string(static_cast<int>(node->AllocationType)));
                }
            };
            logNodes(pageFault.pHeadExistingAllocationNode, "existing allocation");
            logNodes(pageFault.pHeadRecentFreedAllocationNode, "recently freed allocation");
        }
    }

    void Dx12RenderAdapter::WaitForGpu()
    {
        ZoneScoped;

        if (context_.commandQueue == nullptr || context_.fence == nullptr || fenceEvent_ == nullptr)
        {
            return;
        }

        ++context_.fenceValue;
        const HRESULT signalResult = context_.commandQueue->Signal(context_.fence.Get(), context_.fenceValue);
        if (FAILED(signalResult))
        {
            logger_.Error("Fence signal failed: " + HrToString(signalResult));
            return;
        }

        if (context_.fence->GetCompletedValue() < context_.fenceValue)
        {
            const HRESULT eventResult = context_.fence->SetEventOnCompletion(context_.fenceValue, fenceEvent_);
            if (FAILED(eventResult))
            {
                logger_.Error("Fence SetEventOnCompletion failed: " + HrToString(eventResult));
                return;
            }
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }

    void Dx12RenderAdapter::WaitForFenceValue(const UINT64 fenceValue)
    {
        ZoneScoped;

        if (context_.fence == nullptr || fenceEvent_ == nullptr || fenceValue == 0)
        {
            return;
        }

        if (context_.fence->GetCompletedValue() >= fenceValue)
        {
            return;
        }

        const HRESULT eventResult = context_.fence->SetEventOnCompletion(fenceValue, fenceEvent_);
        if (FAILED(eventResult))
        {
            logger_.Error("Fence SetEventOnCompletion failed: " + HrToString(eventResult));
            return;
        }

        WaitForSingleObject(fenceEvent_, INFINITE);
    }

    Dx12RenderAdapter::SurfaceData* Dx12RenderAdapter::FindSurface(const RenderSurfaceHandle handle)
    {
        const auto it = surfaces_.find(handle.value);
        if (it == surfaces_.end())
        {
            return nullptr;
        }
        return &it->second;
    }
}
