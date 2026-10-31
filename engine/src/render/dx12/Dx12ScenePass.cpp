// Dx12ScenePass.cpp
//
// The scene passes of a surface (docs/render/scene-passes.md). Draw() only collects the draw items; FlushScene turns them into
//   1. the shadow pass: the casters, depth only, into a 2048 (or 1024) map fitted to them,
//   2. the opaque pass: draws of the same mesh and material become one instanced draw,
//   3. the transparent passes: alpha blended back to front, then additive,
// into the HDR (MSAA) scene targets when MSAA or bloom is on, or straight into the back buffer. EndSceneOutput resolves the
// MSAA target, blurs the over-bright parts (bloom) and composites the result into the back buffer, where the UI goes on.
// Render targets (thumbnails, previews) and the wireframe view keep the direct path in Dx12RenderAdapter::Draw.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <functional>
#include <vector>

#include <directx-headers/directx/d3dx12.h>
#include <DirectXMath.h>

#include <myengine/core/Logger.h>
#include <myengine/render/MaterialParams.h>
#include <myengine/render/dx12/Dx12RenderAdapter.h>

#include "Dx12GpuProfiler.h"

#include <tracy/Tracy.hpp>

namespace myengine::render::dx12
{
    namespace
    {
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

        std::string HrText(const HRESULT hr)
        {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(hr));
            return buffer;
        }

        // The size of level `level` of a chain that starts at `base` (never below one texel)
        UINT LevelSize(const UINT base, const UINT level)
        {
            return (std::max)(base >> level, 1u);
        }

        constexpr float kShadowDepthBias = 0.0006f;
    }

    void Dx12RenderAdapter::SetRenderSettings(const RenderSettings& settings)
    {
        RenderSettings next = settings;
        next.shadowQuality = (std::min<std::uint8_t>)(next.shadowQuality, 2);
        next.msaaSamples = next.msaaSamples >= 4 ? 4 : 1;
        next.bloomThreshold = (std::max)(next.bloomThreshold, 0.0f);
        next.bloomIntensity = (std::max)(next.bloomIntensity, 0.0f);
        settings_ = next;
    }

    bool Dx12RenderAdapter::RequestScreenshot(const std::string& path)
    {
        screenshotPath_ = path;
        return !path.empty();
    }

    // --------------------------------------------------------------------------- pipeline state variants

    ID3D12PipelineState* Dx12RenderAdapter::GetPipelineVariant(const ShaderRecord& record, const PipelineKey& key)
    {
        if (record.rootSignature == nullptr || record.vertexBlob == nullptr)
        {
            return nullptr;
        }
        const std::uint64_t hash = key.Hash();
        if (const auto found = record.variants.find(hash); found != record.variants.end())
        {
            return found->second.Get();
        }

        ZoneScopedN("Shader::BuildVariant");
        const std::vector<D3D12_INPUT_ELEMENT_DESC> elements = ToD3D12Elements(record.inputLayout);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = record.rootSignature.Get();
        desc.VS = {record.vertexBlob->GetBufferPointer(), record.vertexBlob->GetBufferSize()};
        if (!key.depthOnly && record.pixelBlob != nullptr)
        {
            desc.PS = {record.pixelBlob->GetBufferPointer(), record.pixelBlob->GetBufferSize()};
        }
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.InputLayout = {elements.data(), static_cast<UINT>(elements.size())};
        desc.DSVFormat = kDepthFormat;
        desc.SampleDesc.Count = key.depthOnly ? 1u : key.samples;

        if (key.depthOnly)
        {
            desc.NumRenderTargets = 0;
            desc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
            desc.RasterizerState.DepthBias = 1500;
            desc.RasterizerState.SlopeScaledDepthBias = 1.5f;
            desc.RasterizerState.DepthBiasClamp = 0.0f;
        }
        else
        {
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = key.colorFormat;
        }
        if (key.doubleSided)
        {
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        }

        switch (record.kind)
        {
            case ProgramKind::Scene:
                break;
            case ProgramKind::DebugLines:
                desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
                break;
            case ProgramKind::Post:
                desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                desc.DepthStencilState.DepthEnable = FALSE;
                desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
                desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
                break;
            case ProgramKind::Ui:
                return nullptr; // the UI always draws into the back buffer with its own pipeline state
        }

        if (key.blend != BlendMode::Opaque && !key.depthOnly)
        {
            D3D12_BLEND_DESC blend = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            auto& target = blend.RenderTarget[0];
            target.BlendEnable = TRUE;
            target.BlendOp = D3D12_BLEND_OP_ADD;
            target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            if (record.kind == ProgramKind::Post)
            {
                // the upsampling of the bloom: add the blurred level to the level below it
                target.SrcBlend = D3D12_BLEND_ONE;
                target.DestBlend = D3D12_BLEND_ONE;
                target.SrcBlendAlpha = D3D12_BLEND_ZERO;
                target.DestBlendAlpha = D3D12_BLEND_ONE;
            }
            else if (key.blend == BlendMode::Alpha)
            {
                target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                target.SrcBlendAlpha = D3D12_BLEND_ONE;
                target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            }
            else
            {
                target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                target.DestBlend = D3D12_BLEND_ONE;
                target.SrcBlendAlpha = D3D12_BLEND_ZERO;
                target.DestBlendAlpha = D3D12_BLEND_ONE;
                desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            }
            desc.BlendState = blend;
        }

        Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
        const HRESULT hr = context_.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(state.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateGraphicsPipelineState (variant) failed for " + record.sourceName + ": " + HrText(hr));
            record.variants.emplace(hash, nullptr);
            return nullptr;
        }
        ID3D12PipelineState* result = state.Get();
        record.variants.emplace(hash, std::move(state));
        return result;
    }

    // --------------------------------------------------------------------------- targets

    void Dx12RenderAdapter::RetireSceneTargets()
    {
        RetiredResource retired;
        retired.resource = std::move(sceneTargets_.color);
        retired.secondResource = std::move(sceneTargets_.depth);
        if (sceneTargets_.resolved != nullptr)
        {
            retired.objects.push_back(std::move(sceneTargets_.resolved));
        }
        for (auto& level : sceneTargets_.bloomLevels)
        {
            if (level != nullptr)
            {
                retired.objects.push_back(std::move(level));
            }
        }
        if (sceneTargets_.rtvHeap != nullptr)
        {
            retired.objects.push_back(std::move(sceneTargets_.rtvHeap));
        }
        if (sceneTargets_.dsvHeap != nullptr)
        {
            retired.objects.push_back(std::move(sceneTargets_.dsvHeap));
        }
        if (retired.resource != nullptr || retired.secondResource != nullptr || !retired.objects.empty())
        {
            RetireResources(std::move(retired));
        }
        sceneTargets_.width = 0;
        sceneTargets_.height = 0;
    }

    void Dx12RenderAdapter::RetireShadowMap()
    {
        RetiredResource retired;
        retired.resource = std::move(shadowMap_);
        if (shadowDsvHeap_ != nullptr)
        {
            retired.objects.push_back(std::move(shadowDsvHeap_));
        }
        if (retired.resource != nullptr || !retired.objects.empty())
        {
            RetireResources(std::move(retired));
        }
        shadowMapSize_ = 0;
        shadowMapReady_ = false;
    }

    bool Dx12RenderAdapter::EnsureSceneTargets(const SurfaceData& surface)
    {
        const std::uint8_t samples = settings_.msaaSamples >= 4 ? 4 : 1;
        bool bloom = settings_.bloom;
        if (sceneTargets_.color != nullptr && sceneTargets_.width == surface.width && sceneTargets_.height == surface.height &&
            sceneTargets_.samples == samples && sceneTargets_.bloom == bloom)
        {
            return true;
        }

        ZoneScopedN("SceneTargets::Create");
        CollectRetiredResources();
        RetireSceneTargets();

        if (!sceneTargets_.descriptorsAllocated)
        {
            UINT index = 0;
            if (!AllocateTextureDescriptor(index))
            {
                logger_.Warning("Scene targets: the descriptor heap is full");
                return false;
            }
            sceneTargets_.srvResolved = index;
            for (UINT level = 0; level < kBloomLevels; ++level)
            {
                if (!AllocateTextureDescriptor(index))
                {
                    logger_.Warning("Scene targets: the descriptor heap is full");
                    return false;
                }
                sceneTargets_.srvBloom[level] = index;
            }
            sceneTargets_.descriptorsAllocated = true;
        }

        const UINT width = surface.width;
        const UINT height = surface.height;
        const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);

        const auto colorDesc = CD3DX12_RESOURCE_DESC::Tex2D(kSceneColorFormat, width, height, 1, 1, samples, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        HRESULT hr = context_.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &colorDesc, D3D12_RESOURCE_STATE_RENDER_TARGET,
            nullptr, IID_PPV_ARGS(sceneTargets_.color.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("Scene targets: the colour target failed: " + HrText(hr));
            return false;
        }
        sceneTargets_.colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;

        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = kDepthFormat;
        depthClear.DepthStencil.Depth = 1.0f;
        const auto depthDesc = CD3DX12_RESOURCE_DESC::Tex2D(kDepthFormat, width, height, 1, 1, samples, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        hr = context_.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depthClear, IID_PPV_ARGS(sceneTargets_.depth.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("Scene targets: the depth target failed: " + HrText(hr));
            sceneTargets_.color.Reset();
            return false;
        }

        if (samples > 1)
        {
            const auto resolvedDesc = CD3DX12_RESOURCE_DESC::Tex2D(kSceneColorFormat, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_NONE);
            hr = context_.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &resolvedDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                nullptr, IID_PPV_ARGS(sceneTargets_.resolved.GetAddressOf()));
            if (FAILED(hr))
            {
                logger_.Error("Scene targets: the resolve target failed: " + HrText(hr));
                sceneTargets_.color.Reset();
                sceneTargets_.depth.Reset();
                return false;
            }
        }
        if (bloom)
        {
            for (UINT level = 0; level < kBloomLevels; ++level)
            {
                const auto levelDesc = CD3DX12_RESOURCE_DESC::Tex2D(kSceneColorFormat, LevelSize(width, level + 1), LevelSize(height, level + 1), 1, 1, 1, 0,
                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
                hr = context_.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &levelDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    nullptr, IID_PPV_ARGS(sceneTargets_.bloomLevels[level].GetAddressOf()));
                if (FAILED(hr))
                {
                    logger_.Error("Scene targets: a bloom level failed: " + HrText(hr));
                    bloom = false;
                    for (auto& created : sceneTargets_.bloomLevels)
                    {
                        created.Reset();
                    }
                    break;
                }
            }
        }

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.NumDescriptors = 2 + kBloomLevels;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(context_.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(sceneTargets_.rtvHeap.GetAddressOf()))) ||
            FAILED(context_.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(sceneTargets_.dsvHeap.GetAddressOf()))))
        {
            logger_.Error("Scene targets: the descriptor heaps failed");
            sceneTargets_.color.Reset();
            sceneTargets_.depth.Reset();
            sceneTargets_.resolved.Reset();
            return false;
        }
        const UINT rtvSize = context_.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = sceneTargets_.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        context_.device->CreateRenderTargetView(sceneTargets_.color.Get(), nullptr, rtv);
        for (UINT level = 0; level < kBloomLevels; ++level)
        {
            if (sceneTargets_.bloomLevels[level] != nullptr)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv;
                handle.ptr += static_cast<UINT64>(2 + level) * rtvSize;
                context_.device->CreateRenderTargetView(sceneTargets_.bloomLevels[level].Get(), nullptr, handle);
            }
        }
        context_.device->CreateDepthStencilView(sceneTargets_.depth.Get(), nullptr, sceneTargets_.dsvHeap->GetCPUDescriptorHandleForHeapStart());

        // The shader views: what the composite reads (the resolved colour, or the colour itself without MSAA) and the bloom levels
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
        createSrv(samples > 1 ? sceneTargets_.resolved.Get() : sceneTargets_.color.Get(), sceneTargets_.srvResolved);
        for (UINT level = 0; level < kBloomLevels; ++level)
        {
            if (sceneTargets_.bloomLevels[level] != nullptr)
            {
                createSrv(sceneTargets_.bloomLevels[level].Get(), sceneTargets_.srvBloom[level]);
            }
        }

        sceneTargets_.width = width;
        sceneTargets_.height = height;
        sceneTargets_.samples = samples;
        sceneTargets_.bloom = bloom;
        return true;
    }

    bool Dx12RenderAdapter::EnsureShadowMap()
    {
        const std::uint32_t size = settings_.shadowQuality >= 2 ? 2048u : 1024u;
        if (shadowMap_ != nullptr && shadowMapSize_ == size)
        {
            return true;
        }
        CollectRetiredResources();
        RetireShadowMap();

        if (shadowSrv_ == 0)
        {
            UINT index = 0;
            if (!AllocateTextureDescriptor(index))
            {
                logger_.Warning("Shadow map: the descriptor heap is full");
                return false;
            }
            shadowSrv_ = index;
        }

        const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_CLEAR_VALUE clear{};
        clear.Format = kDepthFormat;
        clear.DepthStencil.Depth = 1.0f;
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, size, size, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        HRESULT hr = context_.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clear, IID_PPV_ARGS(shadowMap_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("Shadow map: CreateCommittedResource failed: " + HrText(hr));
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.NumDescriptors = 1;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(context_.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(shadowDsvHeap_.GetAddressOf()))))
        {
            shadowMap_.Reset();
            return false;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = kDepthFormat;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        context_.device->CreateDepthStencilView(shadowMap_.Get(), &dsv, shadowDsvHeap_->GetCPUDescriptorHandleForHeapStart());

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = textureSrvHeap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(shadowSrv_) * static_cast<UINT64>(textureSrvDescriptorSize_);
        context_.device->CreateShaderResourceView(shadowMap_.Get(), &srv, handle);

        shadowMapSize_ = size;
        return true;
    }

    // --------------------------------------------------------------------------- output of the surface

    void Dx12RenderAdapter::SetSurfaceOutput(SurfaceData& surface)
    {
        if (sceneOutputActive_ && sceneTargets_.color != nullptr)
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = sceneTargets_.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv = sceneTargets_.dsvHeap->GetCPUDescriptorHandleForHeapStart();
            context_.commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        }
        else
        {
            const CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(surface.rtvHeap->GetCPUDescriptorHandleForHeapStart(), static_cast<INT>(surface.currentBackBuffer),
                static_cast<INT>(surface.rtvDescriptorSize));
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv = surface.dsvHeap->GetCPUDescriptorHandleForHeapStart();
            context_.commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        }
        context_.commandList->RSSetViewports(1, &surface.viewport);
        context_.commandList->RSSetScissorRects(1, &surface.scissorRect);
    }

    void Dx12RenderAdapter::BeginSceneOutput(SurfaceData& surface)
    {
        if (sceneOutputActive_ || !useSceneTargets_)
        {
            return;
        }
        if (!EnsureSceneTargets(surface))
        {
            useSceneTargets_ = false; // no HDR target: the frame goes straight to the back buffer
            return;
        }

        if (sceneTargets_.colorState != D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            const auto toTarget = CD3DX12_RESOURCE_BARRIER::Transition(sceneTargets_.color.Get(), sceneTargets_.colorState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            context_.commandList->ResourceBarrier(1, &toTarget);
            profiler_->NoteBarriers(1);
            sceneTargets_.colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        sceneOutputActive_ = true;
        SetSurfaceOutput(surface);

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = sceneTargets_.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = sceneTargets_.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        const float clear[4] = {sceneClearColor_.r, sceneClearColor_.g, sceneClearColor_.b, sceneClearColor_.a};
        context_.commandList->ClearRenderTargetView(rtv, clear, 0, nullptr);
        context_.commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }

    void Dx12RenderAdapter::EndSceneOutput(SurfaceData& surface)
    {
        if (!sceneOutputActive_)
        {
            return;
        }
        RunBloomAndComposite(surface);
        sceneOutputActive_ = false;
        SetSurfaceOutput(surface);
    }

    // --------------------------------------------------------------------------- post passes

    void Dx12RenderAdapter::DrawFullscreen(const float mode, const UINT sourceA, const UINT sourceB, const float texelX, const float texelY, const PipelineKey& key)
    {
        ID3D12PipelineState* pipeline = GetPipelineVariant(postProgram_, key);
        if (pipeline == nullptr)
        {
            return;
        }
        profiler_->NotePipeline(pipeline);
        SetRootSignature(postProgram_.rootSignature.Get());
        context_.commandList->SetPipelineState(pipeline);
        context_.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12DescriptorHeap* heaps[] = {textureSrvHeap_.Get()};
        context_.commandList->SetDescriptorHeaps(1, heaps);

        // The textures of the post shader by name: gSource, gBloom
        std::vector<const ShaderResource*> textures;
        for (const ShaderResource& resource : postProgram_.layout.resources)
        {
            if (resource.type == ShaderResourceType::Texture)
            {
                textures.push_back(&resource);
            }
        }
        for (const TextureSlot& slot : postProgram_.slots.textures)
        {
            if (slot.materialIndex >= textures.size())
            {
                continue;
            }
            const UINT index = textures[slot.materialIndex]->name == "gBloom" ? sourceB : sourceA;
            D3D12_GPU_DESCRIPTOR_HANDLE handle = textureSrvHeap_->GetGPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<UINT64>(index) * static_cast<UINT64>(textureSrvDescriptorSize_);
            context_.commandList->SetGraphicsRootDescriptorTable(slot.rootParameter, handle);
        }
        for (const ConstantBufferSlot& slot : postProgram_.slots.constantBuffers)
        {
            constantScratch_.assign(slot.sizeBytes, 0);
            const float params[4] = {mode, postThresholdOverride_ >= 0.0f ? postThresholdOverride_ : settings_.bloomThreshold,
                postIntensityOverride_ >= 0.0f ? postIntensityOverride_ : settings_.bloomIntensity, postExposure_};
            const float texel[4] = {texelX, texelY, 0.0f, 0.0f};
            if (slot.postParams != kNoOffset)
            {
                std::memcpy(constantScratch_.data() + slot.postParams, params, sizeof(params));
            }
            if (slot.texelSize != kNoOffset)
            {
                std::memcpy(constantScratch_.data() + slot.texelSize, texel, sizeof(texel));
            }
            SetConstants(slot, *activeSurface_);
        }
        context_.commandList->DrawInstanced(3, 1, 0, 0);
        profiler_->NoteDraw(1);
    }

    void Dx12RenderAdapter::RunBloomAndComposite(SurfaceData& surface)
    {
        ZoneScopedN("Composite");
        SceneTargets& targets = sceneTargets_;
        auto* commandList = context_.commandList.Get();
        const auto transition = [&](ID3D12Resource* resource, const D3D12_RESOURCE_STATES from, const D3D12_RESOURCE_STATES to)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, from, to);
            commandList->ResourceBarrier(1, &barrier);
            profiler_->NoteBarriers(1);
        };

        // MSAA: resolve into the single sample copy; without it the colour target itself is read
        CloseAutoGroup();
        EnterAutoGroup(AutoGroup::Bloom);
        if (targets.samples > 1)
        {
            transition(targets.color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            transition(targets.resolved.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            commandList->ResolveSubresource(targets.resolved.Get(), 0, targets.color.Get(), 0, kSceneColorFormat);
            transition(targets.resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            targets.colorState = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        }
        else
        {
            transition(targets.color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            targets.colorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        const bool bloom = targets.bloom && targets.bloomLevels[0] != nullptr && postProgram_.pipelineState != nullptr;
        const UINT rtvSize = context_.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        if (bloom)
        {
            const auto setLevelTarget = [&](const UINT level)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE handle = targets.rtvHeap->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += static_cast<UINT64>(2 + level) * rtvSize;
                commandList->OMSetRenderTargets(1, &handle, FALSE, nullptr);
                const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(LevelSize(targets.width, level + 1)),
                    static_cast<float>(LevelSize(targets.height, level + 1)), 0.0f, 1.0f};
                const D3D12_RECT scissor{0, 0, static_cast<LONG>(viewport.Width), static_cast<LONG>(viewport.Height)};
                commandList->RSSetViewports(1, &viewport);
                commandList->RSSetScissorRects(1, &scissor);
            };
            PipelineKey overwrite;
            overwrite.colorFormat = kSceneColorFormat;
            PipelineKey add = overwrite;
            add.blend = BlendMode::Additive;

            // level 0 = the bright parts of the scene at half size; 1, 2 = blurred down; then added back up
            for (UINT level = 0; level < kBloomLevels; ++level)
            {
                transition(targets.bloomLevels[level].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                setLevelTarget(level);
                if (level == 0)
                {
                    DrawFullscreen(0.0f, targets.srvResolved, targets.srvResolved, 1.0f / targets.width, 1.0f / targets.height, overwrite);
                }
                else
                {
                    DrawFullscreen(1.0f, targets.srvBloom[level - 1], targets.srvBloom[level - 1], 1.0f / LevelSize(targets.width, level),
                        1.0f / LevelSize(targets.height, level), overwrite);
                }
                transition(targets.bloomLevels[level].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            for (UINT level = kBloomLevels - 1; level > 0; --level)
            {
                // add level `level` into level `level - 1`
                transition(targets.bloomLevels[level - 1].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                setLevelTarget(level - 1);
                DrawFullscreen(2.0f, targets.srvBloom[level], targets.srvBloom[level], 1.0f / LevelSize(targets.width, level + 1),
                    1.0f / LevelSize(targets.height, level + 1), add);
                transition(targets.bloomLevels[level - 1].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }

        // The composite writes the region of the back buffer that the scene was drawn into
        CloseAutoGroup();
        EnterAutoGroup(AutoGroup::Composite);
        const CD3DX12_CPU_DESCRIPTOR_HANDLE backBufferRtv(surface.rtvHeap->GetCPUDescriptorHandleForHeapStart(), static_cast<INT>(surface.currentBackBuffer),
            static_cast<INT>(surface.rtvDescriptorSize));
        commandList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
        commandList->RSSetViewports(1, &surface.viewport);
        commandList->RSSetScissorRects(1, &surface.scissorRect);
        PipelineKey composite;
        composite.colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        DrawFullscreen(bloom ? 3.0f : 4.0f, targets.srvResolved, bloom ? targets.srvBloom[0] : targets.srvResolved, 1.0f / targets.width, 1.0f / targets.height, composite);
        CloseAutoGroup();
    }

    // --------------------------------------------------------------------------- engine constants and the recording of draws

    void Dx12RenderAdapter::WriteEngineConstants(const ConstantBufferSlot& slot, const PendingDraw& draw, const PassParams& pass)
    {
        const auto put = [this](const std::uint32_t offset, const void* value, const std::size_t size)
        {
            if (offset != kNoOffset)
            {
                std::memcpy(constantScratch_.data() + offset, value, size);
            }
        };
        DirectX::XMFLOAT4X4 matrix{};
        DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMatrixTranspose(MatrixToXm(draw.model)));
        put(slot.model, &matrix, sizeof(matrix));
        DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMatrixTranspose(MatrixToXm(pass.viewProjection)));
        put(slot.viewProjection, &matrix, sizeof(matrix));
        DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMatrixTranspose(MatrixToXm(pass.lightViewProjection)));
        put(slot.lightViewProjection, &matrix, sizeof(matrix));

        const float light[4] = {lighting_.direction.x, lighting_.direction.y, lighting_.direction.z, 0.25f};
        put(slot.lightDirection, light, sizeof(light));
        const float lightColor[4] = {lighting_.color.x, lighting_.color.y, lighting_.color.z, 1.0f};
        put(slot.lightColor, lightColor, sizeof(lightColor));
        const float sky[4] = {lighting_.skyAmbient.x, lighting_.skyAmbient.y, lighting_.skyAmbient.z, 1.0f};
        put(slot.ambientSky, sky, sizeof(sky));
        const float ground[4] = {lighting_.groundAmbient.x, lighting_.groundAmbient.y, lighting_.groundAmbient.z, 1.0f};
        put(slot.ambientGround, ground, sizeof(ground));
        const float camera[4] = {pass.cameraPosition.x, pass.cameraPosition.y, pass.cameraPosition.z, 1.0f};
        put(slot.cameraPosition, camera, sizeof(camera));
        const float right[4] = {pass.cameraRight.x, pass.cameraRight.y, pass.cameraRight.z, 0.0f};
        put(slot.cameraRight, right, sizeof(right));
        const float up[4] = {pass.cameraUp.x, pass.cameraUp.y, pass.cameraUp.z, 0.0f};
        put(slot.cameraUp, up, sizeof(up));
        put(slot.shadowParams, pass.shadowParams, sizeof(pass.shadowParams));
    }

    void Dx12RenderAdapter::RecordDraws(SurfaceData& surface, const std::vector<std::size_t>& order, const PassParams& pass)
    {
        ZoneScopedN("RecordDraws");
        const auto whiteIt = textures_.find(defaultWhiteTexture_.value);
        ID3D12DescriptorHeap* heaps[] = {textureSrvHeap_.Get()};
        context_.commandList->SetDescriptorHeaps(1, heaps);

        std::vector<float> instanceMatrices;
        std::size_t position = 0;
        while (position < order.size())
        {
            const PendingDraw& first = pendingDraws_[order[position]];
            const auto meshIt = meshes_.find(first.mesh.value);
            const auto shaderIt = shaders_.find(first.shader.value);
            if (meshIt == meshes_.end() || shaderIt == shaders_.end())
            {
                ++position;
                continue;
            }
            const MeshRecord& mesh = meshIt->second;
            const ShaderRecord& shader = shaderIt->second;

            // The group: the next draws of the same mesh, program, material and state become instances of this one
            std::size_t end = position + 1;
            if (shader.instanced && settings_.instancing)
            {
                while (end < order.size())
                {
                    const PendingDraw& other = pendingDraws_[order[end]];
                    if (other.mesh.value != first.mesh.value || other.shader.value != first.shader.value || other.material != first.material ||
                        other.texture.value != first.texture.value || other.blend != first.blend || other.doubleSided != first.doubleSided ||
                        (first.material == nullptr && (other.color.r != first.color.r || other.color.g != first.color.g || other.color.b != first.color.b || other.color.a != first.color.a)))
                    {
                        break;
                    }
                    ++end;
                }
            }
            const std::size_t groupDraws = end - position;
            if (first.instanceCount > 0 && !shader.instanced)
            {
                position = end; // a batch of instances needs a program that reads the instance stream
                continue;
            }
            std::size_t totalInstances = 0;
            for (std::size_t i = position; i < end; ++i)
            {
                const PendingDraw& member = pendingDraws_[order[i]];
                totalInstances += member.instanceCount > 0 ? member.instanceCount : 1;
            }

            const MaterialParams* material = first.material;
            bool useMaterial = material != nullptr;
            if (useMaterial && material->LayoutHash() != shader.materialLayoutHash)
            {
                if (!shader.materialMismatchLogged)
                {
                    shader.materialMismatchLogged = true;
                    logger_.Warning("Draw: the material parameters were built for another layout of " + shader.sourceName +
                        " (the shader changed?): zeros and white textures are used until the material is checked again");
                }
                useMaterial = false;
            }
            if (material == nullptr && textures_.find(first.texture.value) == textures_.end())
            {
                position = end;
                continue;
            }

            PipelineKey key = pass.key;
            key.blend = pass.depthOnly ? BlendMode::Opaque : first.blend;
            key.doubleSided = first.doubleSided;
            ID3D12PipelineState* pipeline = GetPipelineVariant(shader, key);
            if (pipeline == nullptr)
            {
                position = end;
                continue;
            }

            profiler_->NotePipeline(pipeline);
            SetRootSignature(shader.rootSignature.Get());
            context_.commandList->SetPipelineState(pipeline);
            context_.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context_.commandList->IASetVertexBuffers(0, 1, &mesh.vertexBufferView);
            context_.commandList->IASetIndexBuffer(&mesh.indexBufferView);

            if (shader.instanced)
            {
                instanceMatrices.clear();
                instanceMatrices.reserve(totalInstances * 16);
                for (std::size_t i = position; i < end; ++i)
                {
                    const PendingDraw& member = pendingDraws_[order[i]];
                    if (member.instanceCount > 0)
                    {
                        const float* batch = pendingInstanceData_.data() + member.instanceOffset;
                        instanceMatrices.insert(instanceMatrices.end(), batch, batch + static_cast<std::size_t>(member.instanceCount) * 16);
                    }
                    else
                    {
                        instanceMatrices.insert(instanceMatrices.end(), member.model.data.begin(), member.model.data.end());
                    }
                }
                D3D12_GPU_VIRTUAL_ADDRESS address = 0;
                if (!AllocateFrameUploadData(surface.instanceUploadBuffers[surface.currentBackBuffer], instanceMatrices.data(),
                        instanceMatrices.size() * sizeof(float), 16, address))
                {
                    position = end;
                    continue;
                }
                D3D12_VERTEX_BUFFER_VIEW view{};
                view.BufferLocation = address;
                view.StrideInBytes = kInstanceStride;
                view.SizeInBytes = static_cast<UINT>(instanceMatrices.size() * sizeof(float));
                context_.commandList->IASetVertexBuffers(1, 1, &view);
            }

            for (const TextureSlot& slot : shader.slots.textures)
            {
                D3D12_GPU_DESCRIPTOR_HANDLE handle = textureSrvHeap_->GetGPUDescriptorHandleForHeapStart();
                if (slot.rootParameter == shader.slots.shadowMapParameter && shadowMapReady_ && !pass.depthOnly)
                {
                    handle.ptr += static_cast<UINT64>(shadowSrv_) * static_cast<UINT64>(textureSrvDescriptorSize_);
                    context_.commandList->SetGraphicsRootDescriptorTable(slot.rootParameter, handle);
                    continue;
                }
                TextureHandle slotTexture;
                if (useMaterial)
                {
                    const std::vector<MaterialParams::TextureSlot>& materialTextures = material->Textures();
                    if (slot.materialIndex < materialTextures.size())
                    {
                        slotTexture = materialTextures[slot.materialIndex].texture;
                    }
                }
                else if (material == nullptr && slot.rootParameter == shader.slots.textureParameter)
                {
                    slotTexture = first.texture;
                }
                if (slot.rootParameter == shader.slots.shadowMapParameter)
                {
                    slotTexture = {}; // no map in this pass: the white texture, the shader is told by gShadowParams.w
                }
                auto textureIt = textures_.find(slotTexture.value);
                if (textureIt == textures_.end())
                {
                    textureIt = whiteIt;
                }
                if (textureIt == textures_.end())
                {
                    continue;
                }
                profiler_->NoteTexture(textureIt->second.descriptorIndex);
                handle.ptr += static_cast<UINT64>(textureIt->second.descriptorIndex) * static_cast<UINT64>(textureSrvDescriptorSize_);
                context_.commandList->SetGraphicsRootDescriptorTable(slot.rootParameter, handle);
            }

            // One draw for the whole group when the program takes its model matrix from the instance stream, else one per draw with gModel
            const std::size_t constantPasses = shader.instanced ? 1 : groupDraws;
            for (std::size_t pass_i = 0; pass_i < constantPasses; ++pass_i)
            {
                const PendingDraw& draw = pendingDraws_[order[position + pass_i]];
                for (const ConstantBufferSlot& slot : shader.slots.constantBuffers)
                {
                    constantScratch_.assign(slot.sizeBytes, 0);
                    if (useMaterial && slot.layoutIndex < material->ConstantBufferCount())
                    {
                        const std::vector<std::uint8_t>& shadow = material->ConstantBufferBytes(slot.layoutIndex);
                        std::memcpy(constantScratch_.data(), shadow.data(), (std::min)(shadow.size(), constantScratch_.size()));
                    }
                    WriteEngineConstants(slot, draw, pass);
                    if (slot.color != kNoOffset)
                    {
                        if (material == nullptr)
                        {
                            const float color[4] = {draw.color.r, draw.color.g, draw.color.b, draw.color.a};
                            std::memcpy(constantScratch_.data() + slot.color, color, sizeof(color));
                        }
                        else if (!useMaterial)
                        {
                            const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                            std::memcpy(constantScratch_.data() + slot.color, white, sizeof(white));
                        }
                    }
                    SetConstants(slot, surface);
                }
                const UINT instances = shader.instanced ? static_cast<UINT>(totalInstances) : 1u;
                context_.commandList->DrawIndexedInstanced(mesh.indexCount, instances, 0, 0, 0);
                profiler_->NoteDraw(static_cast<std::uint64_t>(mesh.indexCount / 3) * instances);
            }
            position = end;
        }
    }

    // --------------------------------------------------------------------------- the shadow pass

    void Dx12RenderAdapter::RenderShadowPass(SurfaceData& surface, const std::vector<std::size_t>& casters, PassParams& mainPass)
    {
        ZoneScopedN("ShadowPass");
        if (!EnsureShadowMap())
        {
            return;
        }

        // The light's view: the casters, as spheres, decide the area the map covers (only what is above a caster can be in its shadow)
        DirectX::XMVECTOR direction = DirectX::XMVector3Normalize(DirectX::XMVectorSet(lighting_.direction.x, lighting_.direction.y, lighting_.direction.z, 0.0f));
        if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(direction)) < 0.5f)
        {
            direction = DirectX::XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);
        }
        const bool nearlyVertical = std::fabs(DirectX::XMVectorGetY(direction)) > 0.99f;
        const DirectX::XMVECTOR up = nearlyVertical ? DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f) : DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        const DirectX::XMMATRIX view = DirectX::XMMatrixLookToLH(DirectX::XMVectorZero(), direction, up);

        float minX = 1e30f;
        float minY = 1e30f;
        float minZ = 1e30f;
        float maxX = -1e30f;
        float maxY = -1e30f;
        float maxZ = -1e30f;
        for (const std::size_t index : casters)
        {
            const PendingDraw& draw = pendingDraws_[index];
            DirectX::XMVECTOR center = DirectX::XMVectorSet(draw.boundsCenter.x, draw.boundsCenter.y, draw.boundsCenter.z, 1.0f);
            float radius = draw.boundsRadius;
            if (radius <= 0.0f)
            {
                center = DirectX::XMVectorSet(draw.model.data[12], draw.model.data[13], draw.model.data[14], 1.0f);
                radius = 1.0f;
            }
            const DirectX::XMVECTOR lightSpace = DirectX::XMVector3TransformCoord(center, view);
            const float x = DirectX::XMVectorGetX(lightSpace);
            const float y = DirectX::XMVectorGetY(lightSpace);
            const float z = DirectX::XMVectorGetZ(lightSpace);
            minX = (std::min)(minX, x - radius);
            maxX = (std::max)(maxX, x + radius);
            minY = (std::min)(minY, y - radius);
            maxY = (std::max)(maxY, y + radius);
            minZ = (std::min)(minZ, z - radius);
            maxZ = (std::max)(maxZ, z + radius);
        }
        // A frame of casters spread over a huge area (a long line of them) is covered around its middle only
        constexpr float kMaxExtent = 240.0f;
        const auto limit = [&](float& low, float& high)
        {
            if (high - low > kMaxExtent)
            {
                const float middle = (low + high) * 0.5f;
                low = middle - kMaxExtent * 0.5f;
                high = middle + kMaxExtent * 0.5f;
            }
        };
        limit(minX, maxX);
        limit(minY, maxY);
        const float margin = 1.0f;
        minX -= margin;
        maxX += margin;
        minY -= margin;
        maxY += margin;
        // The map moves in whole texels, so the edges of a shadow do not crawl when the casters move
        const float texel = (std::max)(maxX - minX, maxY - minY) / static_cast<float>(shadowMapSize_);
        minX = std::floor(minX / texel) * texel;
        minY = std::floor(minY / texel) * texel;
        maxX = minX + texel * static_cast<float>(shadowMapSize_);
        maxY = minY + texel * static_cast<float>(shadowMapSize_);
        const float nearZ = minZ - 10.0f;
        const float farZ = maxZ + 150.0f;
        const DirectX::XMMATRIX projection = DirectX::XMMatrixOrthographicOffCenterLH(minX, maxX, minY, maxY, nearZ, farZ);
        const DirectX::XMMATRIX lightViewProjection = DirectX::XMMatrixMultiply(view, projection);

        mainPass.lightViewProjection = XmToMatrix(lightViewProjection);
        mainPass.shadowParams[0] = 1.0f / static_cast<float>(shadowMapSize_);
        mainPass.shadowParams[1] = kShadowDepthBias;
        mainPass.shadowParams[2] = texel * 1.5f; // the normal offset: a texel and a half, in metres
        mainPass.shadowParams[3] = 1.0f;

        CloseAutoGroup();
        EnterAutoGroup(AutoGroup::Shadow);
        auto* commandList = context_.commandList.Get();
        const auto toDepth = CD3DX12_RESOURCE_BARRIER::Transition(shadowMap_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        commandList->ResourceBarrier(1, &toDepth);
        profiler_->NoteBarriers(1);
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = shadowDsvHeap_->GetCPUDescriptorHandleForHeapStart();
        commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(shadowMapSize_), static_cast<float>(shadowMapSize_), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(shadowMapSize_), static_cast<LONG>(shadowMapSize_)};
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
        commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        PassParams shadowPass;
        shadowPass.viewProjection = mainPass.lightViewProjection;
        shadowPass.lightViewProjection = mainPass.lightViewProjection;
        shadowPass.cameraPosition = mainPass.cameraPosition;
        shadowPass.cameraRight = mainPass.cameraRight;
        shadowPass.cameraUp = mainPass.cameraUp;
        shadowPass.depthOnly = true;
        shadowPass.key.depthOnly = true;
        shadowPass.key.colorFormat = DXGI_FORMAT_UNKNOWN;
        shadowPass.key.samples = 1;
        RecordDraws(surface, casters, shadowPass);

        const auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(shadowMap_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &toRead);
        profiler_->NoteBarriers(1);
        shadowMapReady_ = true;
        SetSurfaceOutput(surface);
    }

    // --------------------------------------------------------------------------- the scene

    void Dx12RenderAdapter::FlushScene()
    {
        if (pendingDraws_.empty() || activeSurface_ == nullptr || activeTarget_ != nullptr)
        {
            return;
        }
        ZoneScopedN("FlushScene");
        SurfaceData& surface = *activeSurface_;
        BeginSceneOutput(surface);

        PassParams main;
        main.viewProjection = surface.viewProjection;
        main.lightViewProjection = Matrix4::Identity();
        main.cameraPosition = surface.cameraPosition;
        main.cameraRight = surface.cameraRight;
        main.cameraUp = surface.cameraUp;
        main.key.colorFormat = sceneOutputActive_ ? kSceneColorFormat : DXGI_FORMAT_R8G8B8A8_UNORM;
        main.key.samples = sceneOutputActive_ ? sceneTargets_.samples : 1;

        // The sort of the opaque draws puts what can be drawn together next to each other: program, material, mesh
        std::vector<std::size_t> opaque;
        std::vector<std::size_t> alpha;
        std::vector<std::size_t> additive;
        std::vector<std::size_t> casters;
        opaque.reserve(pendingDraws_.size());
        for (std::size_t i = 0; i < pendingDraws_.size(); ++i)
        {
            const PendingDraw& draw = pendingDraws_[i];
            switch (draw.blend)
            {
                case BlendMode::Opaque:
                    opaque.push_back(i);
                    if (draw.castShadow)
                    {
                        casters.push_back(i);
                    }
                    break;
                case BlendMode::Alpha:
                    alpha.push_back(i);
                    break;
                case BlendMode::Additive:
                    additive.push_back(i);
                    break;
            }
        }
        const auto groupLess = [this](const std::size_t a, const std::size_t b)
        {
            const PendingDraw& x = pendingDraws_[a];
            const PendingDraw& y = pendingDraws_[b];
            if (x.shader.value != y.shader.value)
            {
                return x.shader.value < y.shader.value;
            }
            if (x.material != y.material)
            {
                return std::less<const MaterialParams*>()(x.material, y.material);
            }
            if (x.texture.value != y.texture.value)
            {
                return x.texture.value < y.texture.value;
            }
            if (x.doubleSided != y.doubleSided)
            {
                return x.doubleSided < y.doubleSided;
            }
            return x.mesh.value < y.mesh.value;
        };
        std::stable_sort(opaque.begin(), opaque.end(), groupLess);
        std::stable_sort(casters.begin(), casters.end(), groupLess);
        std::stable_sort(additive.begin(), additive.end(), groupLess);
        // Alpha blended draws go from the far ones to the near ones, by the distance of the middle of their bounds
        const auto distanceSquared = [&](const PendingDraw& draw)
        {
            const float cx = draw.boundsRadius > 0.0f ? draw.boundsCenter.x : draw.model.data[12];
            const float cy = draw.boundsRadius > 0.0f ? draw.boundsCenter.y : draw.model.data[13];
            const float cz = draw.boundsRadius > 0.0f ? draw.boundsCenter.z : draw.model.data[14];
            const float dx = cx - surface.cameraPosition.x;
            const float dy = cy - surface.cameraPosition.y;
            const float dz = cz - surface.cameraPosition.z;
            return dx * dx + dy * dy + dz * dz;
        };
        std::stable_sort(alpha.begin(), alpha.end(), [&](const std::size_t a, const std::size_t b)
            { return distanceSquared(pendingDraws_[a]) > distanceSquared(pendingDraws_[b]); });

        // Shadows only when something takes them: a program that reads gShadowMap (the old textured_lit does not)
        bool anyReceiver = false;
        for (const PendingDraw& draw : pendingDraws_)
        {
            const auto shaderIt = shaders_.find(draw.shader.value);
            if (shaderIt != shaders_.end() && shaderIt->second.slots.shadowMapParameter != kNoRootParameter)
            {
                anyReceiver = true;
                break;
            }
        }
        if (settings_.shadowQuality > 0 && !casters.empty() && anyReceiver)
        {
            RenderShadowPass(surface, casters, main);
        }
        else
        {
            main.shadowParams[3] = 0.0f;
        }

        if (!opaque.empty())
        {
            EnterAutoGroup(AutoGroup::Scene);
            RecordDraws(surface, opaque, main);
        }
        if (!alpha.empty() || !additive.empty())
        {
            EnterAutoGroup(AutoGroup::SceneTransparent);
            RecordDraws(surface, alpha, main);
            RecordDraws(surface, additive, main);
        }
        pendingDraws_.clear();
        pendingInstanceData_.clear();
    }

    void Dx12RenderAdapter::DrawInstances(const RenderSurfaceHandle handle, const DrawItem& drawItem, const float* instanceData, const std::uint32_t instanceCount)
    {
        if (activeSurface_ == nullptr || !handle.IsValid() || instanceData == nullptr || instanceCount == 0 || activeTarget_ != nullptr || wireframe_)
        {
            return;
        }
        auto* surface = FindSurface(handle);
        if (surface == nullptr || surface != activeSurface_)
        {
            return;
        }
        PendingDraw draw;
        draw.mesh = drawItem.mesh;
        draw.shader = drawItem.shader;
        draw.material = drawItem.material;
        draw.texture = drawItem.texture;
        draw.model = Matrix4::Identity();
        draw.color = drawItem.color;
        draw.blend = drawItem.blend;
        draw.doubleSided = drawItem.doubleSided;
        draw.castShadow = false; // a batch of instances is not a model: it does not go into the shadow map
        draw.boundsCenter = drawItem.boundsCenter;
        draw.boundsRadius = drawItem.boundsRadius;
        draw.instanceCount = instanceCount;
        draw.instanceOffset = pendingInstanceData_.size();
        pendingInstanceData_.insert(pendingInstanceData_.end(), instanceData, instanceData + static_cast<std::size_t>(instanceCount) * 16);
        pendingDraws_.push_back(draw);
    }
}
