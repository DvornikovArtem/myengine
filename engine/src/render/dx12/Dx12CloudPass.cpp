// Dx12CloudPass.cpp
//
// Point clouds (docs/flybrain/brain-view.md): one soft sprite per point, additive, into an HDR target of the size of the render target that is being rendered,
// then the bloom chain of the scene passes (post.hlsl) and a tone-mapped composite into the render target. Everything of a point that does not change (its
// place and colour) is in a vertex buffer made once; the flash is one float per point (the time of its last spike) that the frame writes into its upload
// ring, the vertex shader turns it into brightness and size (brain_cloud.hlsl).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <directx-headers/directx/d3dx12.h>
#include <DirectXMath.h>

#include <myengine/core/AppPaths.h>
#include <myengine/core/Logger.h>
#include <myengine/render/dx12/Dx12RenderAdapter.h>

#include "Dx12GpuProfiler.h"
#include "ShaderCompiler.h"

#include <tracy/Tracy.hpp>

namespace myengine::render::dx12
{
    namespace
    {
        constexpr char kCloudShaderPath[] = "assets/shaders/brain_cloud.hlsl";

        std::string HrText(const HRESULT hr)
        {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(hr));
            return buffer;
        }

        DirectX::XMMATRIX MatrixToXm(const Matrix4& matrix)
        {
            DirectX::XMFLOAT4X4 value{};
            std::memcpy(&value, matrix.data.data(), sizeof(float) * 16);
            return DirectX::XMLoadFloat4x4(&value);
        }

        UINT LevelSize(const UINT base, const UINT level)
        {
            return (std::max)(base >> level, 1u);
        }

        // The file of a shader of the engine: the working directory, the folder of the exe (the assets are copied there), the sources
        std::filesystem::path ResolveShaderFile(const char* relativePath)
        {
            std::error_code ec;
            const std::filesystem::path relative = relativePath;
            if (std::filesystem::exists(relative, ec))
            {
                return relative;
            }
            wchar_t modulePath[MAX_PATH]{};
            const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            if (length > 0 && length < MAX_PATH)
            {
                const auto next = std::filesystem::path(modulePath).parent_path() / relative;
                if (std::filesystem::exists(next, ec))
                {
                    return next;
                }
            }
            const std::filesystem::path sourceRoot = core::apppaths::SourceRoot();
            return sourceRoot.empty() ? relative : sourceRoot / relative;
        }

        // The constant buffer of brain_cloud.hlsl
        struct CloudConstants
        {
            float viewProjection[16];
            float viewport[4];
            float timing[4];
            float sizes[4];
            float palette[16][4];
        };
        static_assert(sizeof(CloudConstants) % 16 == 0, "a constant buffer is a multiple of 16 bytes");
    }

    bool Dx12RenderAdapter::BuildCloudPipeline()
    {
        const std::filesystem::path path = ResolveShaderFile(kCloudShaderPath);
        std::string text;
        std::vector<ShaderDiagnostic> diagnostics;
        if (!ReadShaderSource(path, text, diagnostics))
        {
            LogShaderDiagnostics(diagnostics);
            return false;
        }
        Microsoft::WRL::ComPtr<ID3DBlob> vertexShader;
        Microsoft::WRL::ComPtr<ID3DBlob> pixelShader;
        for (const bool vertex : {true, false})
        {
            ShaderStageSource request;
            request.sourceName = path.string();
            request.sourcePath = path;
            request.source = &text;
            request.entry = vertex ? "VSMain" : "PSMain";
            request.profile = vertex ? "vs_5_0" : "ps_5_0";
            request.stage = vertex ? ShaderStage::Vertex : ShaderStage::Pixel;
            if (!CompileShaderStage(request, vertex ? vertexShader : pixelShader, diagnostics))
            {
                LogShaderDiagnostics(diagnostics);
                return false;
            }
        }

        // One root CBV, b0: the constants of the draw
        CD3DX12_ROOT_PARAMETER parameter;
        parameter.InitAsConstantBufferView(0);
        CD3DX12_ROOT_SIGNATURE_DESC rootDesc(1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(hr))
        {
            logger_.Error("The point cloud root signature failed: " + HrText(hr));
            return false;
        }
        hr = context_.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(cloudRootSignature_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateRootSignature (point cloud) failed: " + HrText(hr));
            return false;
        }

        const D3D12_INPUT_ELEMENT_DESC elements[] = {
            {"CLOUDPOS", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"CLOUDSPIKE", 0, DXGI_FORMAT_R32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = cloudRootSignature_.Get();
        desc.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
        desc.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
        D3D12_BLEND_DESC blend = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.BlendState = blend;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.InputLayout = {elements, static_cast<UINT>(std::size(elements))};
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = kSceneColorFormat;
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        hr = context_.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(cloudPipeline_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateGraphicsPipelineState (point cloud) failed: " + HrText(hr));
            cloudRootSignature_.Reset();
            return false;
        }
        return true;
    }

    PointCloudHandle Dx12RenderAdapter::CreatePointCloud(const PointCloudData& data)
    {
        ZoneScoped;
        const std::uint32_t count = data.Count();
        if (count == 0 || cloudPipeline_ == nullptr)
        {
            return {};
        }
        if (activeSurface_ != nullptr)
        {
            logger_.Warning("CreatePointCloud ignored: the points go to the GPU between frames");
            return {};
        }
        std::vector<float> records(static_cast<std::size_t>(count) * 4);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            records[4ull * i + 0] = data.positions[3ull * i + 0];
            records[4ull * i + 1] = data.positions[3ull * i + 1];
            records[4ull * i + 2] = data.positions[3ull * i + 2];
            records[4ull * i + 3] = i < data.colorIndex.size() ? static_cast<float>(data.colorIndex[i] & 15u) : 0.0f;
        }
        CloudRecord record;
        const UINT64 bytes = static_cast<UINT64>(records.size()) * sizeof(float);
        if (!CreateBuffer(records.data(), bytes, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, record.points))
        {
            return {};
        }
        record.count = count;
        record.view.BufferLocation = record.points->GetGPUVirtualAddress();
        record.view.SizeInBytes = static_cast<UINT>(bytes);
        record.view.StrideInBytes = 16;
        const std::uint32_t id = nextCloudId_++;
        clouds_.insert_or_assign(id, std::move(record));
        return PointCloudHandle{id};
    }

    void Dx12RenderAdapter::DestroyPointCloud(const PointCloudHandle cloud)
    {
        const auto it = clouds_.find(cloud.value);
        if (it == clouds_.end())
        {
            return;
        }
        RetiredResource retired;
        retired.resource = std::move(it->second.points);
        clouds_.erase(it);
        RetireResources(std::move(retired));
    }

    void Dx12RenderAdapter::RetireCloudTargets(CloudTargets&& targets)
    {
        RetiredResource retired;
        retired.resource = std::move(targets.color);
        retired.hasDescriptor = true;
        retired.descriptorIndex = targets.srvColor;
        for (auto& level : targets.bloomLevels)
        {
            if (level != nullptr)
            {
                retired.objects.push_back(std::move(level));
            }
        }
        if (targets.rtvHeap != nullptr)
        {
            retired.objects.push_back(std::move(targets.rtvHeap));
        }
        RetireResources(std::move(retired));
        for (const UINT index : targets.srvBloom)
        {
            RetiredResource descriptor;
            descriptor.hasDescriptor = true;
            descriptor.descriptorIndex = index;
            RetireResources(std::move(descriptor));
        }
    }

    Dx12RenderAdapter::CloudTargets* Dx12RenderAdapter::EnsureCloudTargets(const std::uint32_t renderTargetId, const RenderTargetRecord& target)
    {
        auto found = cloudTargets_.find(renderTargetId);
        if (found != cloudTargets_.end() && found->second.width == target.width && found->second.height == target.height)
        {
            return &found->second;
        }

        ZoneScopedN("CloudTargets::Create");
        CollectRetiredResources();
        if (found != cloudTargets_.end())
        {
            RetireCloudTargets(std::move(found->second));
            cloudTargets_.erase(found);
        }

        CloudTargets targets;
        targets.width = target.width;
        targets.height = target.height;

        UINT indices[1 + kBloomLevels] = {};
        for (UINT& index : indices)
        {
            if (!AllocateTextureDescriptor(index))
            {
                logger_.Warning("Point cloud targets: the descriptor heap is full");
                return nullptr;
            }
        }
        targets.srvColor = indices[0];
        for (UINT level = 0; level < kBloomLevels; ++level)
        {
            targets.srvBloom[level] = indices[1 + level];
        }

        const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        const auto colorDesc = CD3DX12_RESOURCE_DESC::Tex2D(kSceneColorFormat, target.width, target.height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        HRESULT hr = context_.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &colorDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(targets.color.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("Point cloud targets: the colour target failed: " + HrText(hr));
            RetireCloudTargets(std::move(targets));
            return nullptr;
        }
        targets.bloom = true;
        for (UINT level = 0; level < kBloomLevels; ++level)
        {
            const auto levelDesc = CD3DX12_RESOURCE_DESC::Tex2D(kSceneColorFormat, LevelSize(target.width, level + 1), LevelSize(target.height, level + 1), 1, 1, 1, 0,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            hr = context_.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &levelDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(targets.bloomLevels[level].GetAddressOf()));
            if (FAILED(hr))
            {
                logger_.Warning("Point cloud targets: a bloom level failed (" + HrText(hr) + "): no bloom");
                targets.bloom = false;
                for (auto& created : targets.bloomLevels)
                {
                    created.Reset();
                }
                break;
            }
        }

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.NumDescriptors = 1 + kBloomLevels;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(context_.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(targets.rtvHeap.GetAddressOf()))))
        {
            logger_.Error("Point cloud targets: the descriptor heap failed");
            RetireCloudTargets(std::move(targets));
            return nullptr;
        }
        const UINT rtvSize = context_.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = targets.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        context_.device->CreateRenderTargetView(targets.color.Get(), nullptr, rtv);
        const auto createSrv = [&](ID3D12Resource* resource, const UINT index)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = kSceneColorFormat;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE handle = textureSrvHeap_->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<UINT64>(index) * static_cast<UINT64>(textureSrvDescriptorSize_);
            context_.device->CreateShaderResourceView(resource, &srv, handle);
        };
        createSrv(targets.color.Get(), targets.srvColor);
        for (UINT level = 0; level < kBloomLevels; ++level)
        {
            if (targets.bloomLevels[level] != nullptr)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv;
                handle.ptr += static_cast<UINT64>(1 + level) * rtvSize;
                context_.device->CreateRenderTargetView(targets.bloomLevels[level].Get(), nullptr, handle);
                createSrv(targets.bloomLevels[level].Get(), targets.srvBloom[level]);
            }
        }
        return &cloudTargets_.insert_or_assign(renderTargetId, std::move(targets)).first->second;
    }

    bool Dx12RenderAdapter::DrawPointCloud(const RenderSurfaceHandle surfaceHandle, const PointCloudHandle cloud, const PointCloudDrawParams& params)
    {
        ZoneScoped;
        if (cloudPipeline_ == nullptr || postProgram_.pipelineState == nullptr || activeSurface_ == nullptr || activeTarget_ == nullptr || params.flashTimes == nullptr)
        {
            return false;
        }
        SurfaceData* surface = FindSurface(surfaceHandle);
        const auto cloudIt = clouds_.find(cloud.value);
        if (surface == nullptr || surface != activeSurface_ || cloudIt == clouds_.end())
        {
            return false;
        }
        std::uint32_t targetId = 0;
        for (const auto& [id, record] : renderTargets_)
        {
            if (&record == activeTarget_)
            {
                targetId = id;
                break;
            }
        }
        if (targetId == 0)
        {
            return false;
        }
        const CloudRecord& points = cloudIt->second;
        CloudTargets* targets = EnsureCloudTargets(targetId, *activeTarget_);
        if (targets == nullptr)
        {
            return false;
        }

        // The data of the frame: the flash times and the constants go into the upload rings of the surface (rewound at BeginFrame)
        const UINT backBuffer = surface->currentBackBuffer;
        D3D12_GPU_VIRTUAL_ADDRESS flashAddress = 0;
        D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = 0;
        const UINT64 flashBytes = static_cast<UINT64>(points.count) * sizeof(float);
        if (!AllocateFrameUploadData(surface->instanceUploadBuffers[backBuffer], params.flashTimes, flashBytes, 16, flashAddress))
        {
            return false;
        }
        CloudConstants constants{};
        {
            const DirectX::XMMATRIX viewProjection = DirectX::XMMatrixMultiply(MatrixToXm(params.view), MatrixToXm(params.projection));
            DirectX::XMFLOAT4X4 matrix{};
            DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMatrixTranspose(viewProjection));
            std::memcpy(constants.viewProjection, &matrix, sizeof(matrix));
        }
        constants.viewport[0] = static_cast<float>(targets->width);
        constants.viewport[1] = static_cast<float>(targets->height);
        constants.timing[0] = params.time;
        constants.timing[1] = 1.0f / (std::max)(params.decaySeconds, 1e-3f);
        constants.timing[2] = params.baseBrightness;
        constants.timing[3] = params.flashBrightness;
        constants.sizes[0] = params.baseSizePixels;
        constants.sizes[1] = params.flashSizePixels;
        constants.sizes[2] = params.referenceDepth;
        constants.sizes[3] = params.depthRange;
        std::memcpy(constants.palette, params.palette, sizeof(constants.palette));
        if (!AllocateFrameUploadData(surface->constantUploadBuffers[backBuffer], &constants, sizeof(constants), 256, constantsAddress))
        {
            return false;
        }

        auto* commandList = context_.commandList.Get();
        const auto transition = [&](ID3D12Resource* resource, const D3D12_RESOURCE_STATES from, const D3D12_RESOURCE_STATES to)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, from, to);
            commandList->ResourceBarrier(1, &barrier);
            profiler_->NoteBarriers(1);
        };
        const UINT rtvSize = context_.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const D3D12_VIEWPORT fullViewport{0.0f, 0.0f, static_cast<float>(targets->width), static_cast<float>(targets->height), 0.0f, 1.0f};
        const D3D12_RECT fullScissor{0, 0, static_cast<LONG>(targets->width), static_cast<LONG>(targets->height)};

        BeginDebugEvent("Point cloud");

        // 1. the points, additive, into the HDR target
        transition(targets->color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = targets->rtvHeap->GetCPUDescriptorHandleForHeapStart();
            commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            commandList->RSSetViewports(1, &fullViewport);
            commandList->RSSetScissorRects(1, &fullScissor);
            const float clear[4] = {params.background.r, params.background.g, params.background.b, params.background.a};
            commandList->ClearRenderTargetView(rtv, clear, 0, nullptr);
            SetRootSignature(cloudRootSignature_.Get());
            commandList->SetPipelineState(cloudPipeline_.Get());
            profiler_->NotePipeline(cloudPipeline_.Get());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const D3D12_VERTEX_BUFFER_VIEW views[2] = {points.view, {flashAddress, static_cast<UINT>(flashBytes), sizeof(float)}};
            commandList->IASetVertexBuffers(0, 2, views);
            commandList->SetGraphicsRootConstantBufferView(0, constantsAddress);
            commandList->DrawInstanced(6, points.count, 0, 0);
            profiler_->NoteDraw(static_cast<UINT>((std::min<std::uint64_t>)(2ull * points.count, 0xFFFFFFFFull)));
        }
        transition(targets->color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // 2. the bloom: the bright part at half size, blurred down two levels, added back up (the passes of the scene, post.hlsl)
        postThresholdOverride_ = params.bloomThreshold;
        postIntensityOverride_ = params.bloomIntensity;
        postExposure_ = params.exposure;
        const bool bloom = params.bloom && targets->bloom && targets->bloomLevels[0] != nullptr;
        if (bloom)
        {
            const auto setLevelTarget = [&](const UINT level)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE handle = targets->rtvHeap->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += static_cast<UINT64>(1 + level) * rtvSize;
                commandList->OMSetRenderTargets(1, &handle, FALSE, nullptr);
                const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(LevelSize(targets->width, level + 1)), static_cast<float>(LevelSize(targets->height, level + 1)),
                    0.0f, 1.0f};
                const D3D12_RECT scissor{0, 0, static_cast<LONG>(viewport.Width), static_cast<LONG>(viewport.Height)};
                commandList->RSSetViewports(1, &viewport);
                commandList->RSSetScissorRects(1, &scissor);
            };
            PipelineKey overwrite;
            overwrite.colorFormat = kSceneColorFormat;
            PipelineKey add = overwrite;
            add.blend = BlendMode::Additive;
            for (UINT level = 0; level < kBloomLevels; ++level)
            {
                transition(targets->bloomLevels[level].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                setLevelTarget(level);
                if (level == 0)
                {
                    DrawFullscreen(0.0f, targets->srvColor, targets->srvColor, 1.0f / targets->width, 1.0f / targets->height, overwrite);
                }
                else
                {
                    DrawFullscreen(1.0f, targets->srvBloom[level - 1], targets->srvBloom[level - 1], 1.0f / LevelSize(targets->width, level),
                        1.0f / LevelSize(targets->height, level), overwrite);
                }
                transition(targets->bloomLevels[level].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            for (UINT level = kBloomLevels - 1; level > 0; --level)
            {
                transition(targets->bloomLevels[level - 1].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                setLevelTarget(level - 1);
                DrawFullscreen(2.0f, targets->srvBloom[level], targets->srvBloom[level], 1.0f / LevelSize(targets->width, level + 1),
                    1.0f / LevelSize(targets->height, level + 1), add);
                transition(targets->bloomLevels[level - 1].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }

        // 3. back into the render target: the HDR picture and the bloom, tone mapped
        {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = targetRtvHeap_->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += static_cast<UINT64>(activeTarget_->slot) * static_cast<UINT64>(targetRtvDescriptorSize_);
            D3D12_CPU_DESCRIPTOR_HANDLE dsv = targetDsvHeap_->GetCPUDescriptorHandleForHeapStart();
            dsv.ptr += static_cast<UINT64>(activeTarget_->slot) * static_cast<UINT64>(targetDsvDescriptorSize_);
            commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            commandList->RSSetViewports(1, &fullViewport);
            commandList->RSSetScissorRects(1, &fullScissor);
            PipelineKey composite;
            composite.colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            DrawFullscreen(bloom ? 5.0f : 6.0f, targets->srvColor, bloom ? targets->srvBloom[0] : targets->srvColor, 1.0f / targets->width, 1.0f / targets->height, composite);
        }
        postThresholdOverride_ = -1.0f;
        postIntensityOverride_ = -1.0f;
        postExposure_ = 1.0f;

        EndDebugEvent();
        return true;
    }
}
