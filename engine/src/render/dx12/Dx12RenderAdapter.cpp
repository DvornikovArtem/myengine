// Dx12RenderAdapter.cpp

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <vector>

#include <directx-headers/directx/d3dx12.h>
#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <myengine/core/Logger.h>
#include <myengine/render/dx12/Dx12RenderAdapter.h>

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
            return std::filesystem::path(MYENGINE_SOURCE_DIR) / relativePath;
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
        if (!BuildRootSignature())
        {
            return false;
        }
        if (!BuildDebugLinePipeline())
        {
            return false;
        }
        if (!BuildTextureDescriptorHeap())
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
        if (meshData.vertices.empty() || meshData.indices.empty())
        {
            logger_.Warning("UploadMesh failed: mesh is empty");
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
        if (textureSrvHeap_ == nullptr)
        {
            logger_.Warning("CreateTexture failed: SRV heap is not initialized");
            return {};
        }
        if (textureData.width == 0 || textureData.height == 0 || textureData.pixelsRgba8.empty())
        {
            logger_.Warning("CreateTexture failed: invalid texture data");
            return {};
        }
        if (nextTextureDescriptorIndex_ >= kMaxTextureDescriptors)
        {
            logger_.Warning("CreateTexture failed: descriptor heap is full");
            return {};
        }

        TextureRecord textureRecord;
        if (!CreateTextureResource(textureData, textureRecord.textureResource))
        {
            return {};
        }

        textureRecord.descriptorIndex = nextTextureDescriptorIndex_++;

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

        textures_.erase(texture.value);
    }

    ShaderHandle Dx12RenderAdapter::CreateShaderProgram(const ShaderProgramData& shaderProgram)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> vertexShader;
        Microsoft::WRL::ComPtr<ID3DBlob> pixelShader;
        if (!CompileShaderBlob(shaderProgram.sourcePath, shaderProgram.vertexEntry, shaderProgram.vertexProfile, vertexShader))
        {
            return {};
        }
        if (!CompileShaderBlob(shaderProgram.sourcePath, shaderProgram.pixelEntry, shaderProgram.pixelProfile, pixelShader))
        {
            return {};
        }

        static constexpr D3D12_INPUT_ELEMENT_DESC inputLayout[] =
        {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,                       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, sizeof(float) * 3,       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, sizeof(float) * (3 + 3), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
        pipelineDesc.pRootSignature = rootSignature_.Get();
        pipelineDesc.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
        pipelineDesc.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
        pipelineDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pipelineDesc.SampleMask = UINT_MAX;
        pipelineDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pipelineDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pipelineDesc.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
        pipelineDesc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipelineDesc.NumRenderTargets = 1;
        pipelineDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipelineDesc.DSVFormat = kDepthFormat;
        pipelineDesc.SampleDesc.Count = 1;

        ShaderRecord shaderRecord;
        const HRESULT hr = context_.device->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(shaderRecord.pipelineState.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateGraphicsPipelineState failed: " + HrToString(hr));
            return {};
        }

        const std::uint32_t shaderId = nextShaderId_++;
        shaders_.insert_or_assign(shaderId, std::move(shaderRecord));
        return ShaderHandle{shaderId};
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
        auto* surface = FindSurface(handle);
        if (surface == nullptr || surface->dsvHeap == nullptr || surface->depthStencil == nullptr)
        {
            return false;
        }

        surface->currentBackBuffer = surface->swapChain->GetCurrentBackBufferIndex();
        const UINT backBufferIndex = surface->currentBackBuffer;

        if (surface->frameFenceValues[backBufferIndex] != 0 &&
            context_.fence->GetCompletedValue() < surface->frameFenceValues[backBufferIndex])
        {
            WaitForFenceValue(surface->frameFenceValues[backBufferIndex]);
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

        ID3D12Resource* backBuffer = surface->backBuffers[backBufferIndex].Get();

        const auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        context_.commandList->ResourceBarrier(1, &toRenderTarget);

        const CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(surface->rtvHeap->GetCPUDescriptorHandleForHeapStart(), static_cast<INT>(surface->currentBackBuffer), static_cast<INT>(surface->rtvDescriptorSize));
        const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = surface->dsvHeap->GetCPUDescriptorHandleForHeapStart();

        const float color[4] = {clearColor.r, clearColor.g, clearColor.b, clearColor.a};

        context_.commandList->RSSetViewports(1, &surface->viewport);
        context_.commandList->RSSetScissorRects(1, &surface->scissorRect);
        context_.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
        context_.commandList->ClearRenderTargetView(rtvHandle, color, 0, nullptr);
        context_.commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        context_.commandList->SetGraphicsRootSignature(rootSignature_.Get());

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

        if (activeSurface_ == nullptr || surface != activeSurface_)
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

        surface->viewProjection = XmToMatrix(DirectX::XMMatrixMultiply(MatrixToXm(view), MatrixToXm(projection)));
    }

    void Dx12RenderAdapter::Draw(const RenderSurfaceHandle handle, const DrawItem& drawItem)
    {
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

        context_.commandList->SetPipelineState(shader.pipelineState.Get());
        context_.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_.commandList->IASetVertexBuffers(0, 1, &mesh.vertexBufferView);
        context_.commandList->IASetIndexBuffer(&mesh.indexBufferView);

        ID3D12DescriptorHeap* descriptorHeaps[] = {textureSrvHeap_.Get()};
        context_.commandList->SetDescriptorHeaps(1, descriptorHeaps);

        D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = textureSrvHeap_->GetGPUDescriptorHandleForHeapStart();
        textureHandle.ptr += static_cast<UINT64>(texture.descriptorIndex) * static_cast<UINT64>(textureSrvDescriptorSize_);
        context_.commandList->SetGraphicsRootDescriptorTable(1, textureHandle);

        struct DrawConstants
        {
            DirectX::XMFLOAT4X4 model;
            DirectX::XMFLOAT4X4 viewProjection;
            float color[4];
            float lightDirection[4];
        } constants{};

        const DirectX::XMMATRIX modelMatrix = MatrixToXm(drawItem.model);
        const DirectX::XMMATRIX viewProjectionMatrix = MatrixToXm(surface->viewProjection);

        DirectX::XMStoreFloat4x4(&constants.model, DirectX::XMMatrixTranspose(modelMatrix));
        DirectX::XMStoreFloat4x4(&constants.viewProjection, DirectX::XMMatrixTranspose(viewProjectionMatrix));

        constants.color[0] = drawItem.color.r;
        constants.color[1] = drawItem.color.g;
        constants.color[2] = drawItem.color.b;
        constants.color[3] = drawItem.color.a;

        constants.lightDirection[0] = 0.35f;
        constants.lightDirection[1] = -1.0f;
        constants.lightDirection[2] = 0.25f;
        constants.lightDirection[3] = 0.25f;

        context_.commandList->SetGraphicsRoot32BitConstants(0, 40, &constants, 0);
        context_.commandList->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
    }

    void Dx12RenderAdapter::DrawDebugLines(const RenderSurfaceHandle handle, const std::vector<DebugLine>& lines)
    {
        if (activeSurface_ == nullptr || debugLinePipelineState_ == nullptr || debugRootSignature_ == nullptr || lines.empty())
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

        const DirectX::XMMATRIX viewProjectionMatrix = MatrixToXm(surface->viewProjection);

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

        context_.commandList->SetGraphicsRootSignature(debugRootSignature_.Get());
        context_.commandList->SetPipelineState(debugLinePipelineState_.Get());
        context_.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        context_.commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
        context_.commandList->DrawInstanced(static_cast<UINT>(vertices.size()), 1, 0, 0);

        context_.commandList->SetGraphicsRootSignature(rootSignature_.Get());
    }

    void Dx12RenderAdapter::DrawUiGeometry(const RenderSurfaceHandle handle, const UiDrawData& drawData)
    {
        if (activeSurface_ == nullptr || uiPipelineState_ == nullptr || uiRootSignature_ == nullptr)
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

        ID3D12DescriptorHeap* descriptorHeaps[] = {textureSrvHeap_.Get()};
        context_.commandList->SetDescriptorHeaps(1, descriptorHeaps);
        context_.commandList->SetGraphicsRootSignature(uiRootSignature_.Get());
        context_.commandList->SetPipelineState(uiPipelineState_.Get());
        context_.commandList->RSSetScissorRects(1, &scissorRect);
        context_.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_.commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
        context_.commandList->IASetIndexBuffer(&indexBufferView);

        const float uiConstants[4] =
        {
            std::max(1.0f, static_cast<float>(surface->width)),
            std::max(1.0f, static_cast<float>(surface->height)),
            drawData.translationX,
            drawData.translationY,
        };
        context_.commandList->SetGraphicsRoot32BitConstants(0, 4, uiConstants, 0);

        D3D12_GPU_DESCRIPTOR_HANDLE gpuTextureHandle = textureSrvHeap_->GetGPUDescriptorHandleForHeapStart();
        gpuTextureHandle.ptr += static_cast<UINT64>(textureIt->second.descriptorIndex) * static_cast<UINT64>(textureSrvDescriptorSize_);
        context_.commandList->SetGraphicsRootDescriptorTable(1, gpuTextureHandle);
        context_.commandList->DrawIndexedInstanced(static_cast<UINT>(drawData.indexCount), 1, 0, 0, 0);

        context_.commandList->RSSetScissorRects(1, &fullScissorRect);
        context_.commandList->SetGraphicsRootSignature(rootSignature_.Get());
    }

    void Dx12RenderAdapter::EndFrame(const RenderSurfaceHandle handle)
    {
        auto* surface = FindSurface(handle);
        if (surface == nullptr || surface != activeSurface_)
        {
            return;
        }

        ID3D12Resource* backBuffer = surface->backBuffers[surface->currentBackBuffer].Get();
        const auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        context_.commandList->ResourceBarrier(1, &toPresent);

        if (FAILED(context_.commandList->Close()))
        {
            logger_.Error("Command list close failed");
            return;
        }

        ID3D12CommandList* commandLists[] = {context_.commandList.Get()};
        context_.commandQueue->ExecuteCommandLists(1, commandLists);

        const UINT syncInterval = vsyncEnabled_ ? 1u : 0u;
        const UINT presentFlags = (!vsyncEnabled_ && allowTearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        const HRESULT presentResult = surface->swapChain->Present(syncInterval, presentFlags);
        if (FAILED(presentResult))
        {
            logger_.Error("Present failed: " + HrToString(presentResult));
            if (presentResult == DXGI_ERROR_DEVICE_REMOVED || presentResult == DXGI_ERROR_DEVICE_RESET)
            {
                logger_.Error("DX12 device removed reason: " + HrToString(context_.device->GetDeviceRemovedReason()));
            }

            activeFrameTransientResources_ = nullptr;
            activeSurface_ = nullptr;
            return;
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

    void Dx12RenderAdapter::Shutdown()
    {
        if (context_.device == nullptr)
        {
            return;
        }

        WaitForGpu();

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
        }

        surfaces_.clear();
        meshes_.clear();
        textures_.clear();
        shaders_.clear();

        uiPipelineState_.Reset();
        uiRootSignature_.Reset();
        debugLinePipelineState_.Reset();
        debugRootSignature_.Reset();
        textureSrvHeap_.Reset();
        rootSignature_.Reset();

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
#if defined(_DEBUG)
        const bool enableDebugLayer = IsEnvironmentEnabled("MYENGINE_DX12_DEBUG");
        if (enableDebugLayer)
        {
            Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debugController.GetAddressOf()))))
            {
                debugController->EnableDebugLayer();
                factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                logger_.Info("DX12 debug layer enabled via MYENGINE_DX12_DEBUG");
            }
        }
        else
        {
            logger_.Info("DX12 debug layer disabled. Set MYENGINE_DX12_DEBUG=1 to enable it.");
        }
#endif

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
        vsyncEnabled_ = true;
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

    bool Dx12RenderAdapter::BuildRootSignature()
    {
        CD3DX12_DESCRIPTOR_RANGE descriptorRange;
        descriptorRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_ROOT_PARAMETER rootParameters[2];
        rootParameters[0].InitAsConstants(40, 0);
        rootParameters[1].InitAsDescriptorTable(1, &descriptorRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC samplerDesc(
            0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP);

        CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init(
            static_cast<UINT>(std::size(rootParameters)),
            rootParameters,
            1,
            &samplerDesc,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSignature;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, serializedRootSignature.GetAddressOf(), errorBlob.GetAddressOf());
        if (FAILED(hr))
        {
            const char* errorText = errorBlob != nullptr ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "unknown root signature error";
            logger_.Error("D3D12SerializeRootSignature failed: " + std::string(errorText));
            return false;
        }

        hr = context_.device->CreateRootSignature(0, serializedRootSignature->GetBufferPointer(), serializedRootSignature->GetBufferSize(), IID_PPV_ARGS(rootSignature_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateRootSignature failed: " + HrToString(hr));
            return false;
        }

        return true;
    }

    bool Dx12RenderAdapter::BuildDebugLinePipeline()
    {
        CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init(0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSignature;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3D12SerializeRootSignature(
            &rootSignatureDesc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSignature.GetAddressOf(),
            errorBlob.GetAddressOf());
        if (FAILED(hr))
        {
            const char* errorText = errorBlob != nullptr ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "unknown debug root signature error";
            logger_.Error("D3D12SerializeRootSignature(debug) failed: " + std::string(errorText));
            return false;
        }

        hr = context_.device->CreateRootSignature(
            0,
            serializedRootSignature->GetBufferPointer(),
            serializedRootSignature->GetBufferSize(),
            IID_PPV_ARGS(debugRootSignature_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateRootSignature(debug) failed: " + HrToString(hr));
            return false;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> vertexShader;
        Microsoft::WRL::ComPtr<ID3DBlob> pixelShader;
        if (!CompileShaderBlob(ResolveSourcePath("assets/shaders/debug_line.hlsl"), "VSMain", "vs_5_0", vertexShader) ||
            !CompileShaderBlob(ResolveSourcePath("assets/shaders/debug_line.hlsl"), "PSMain", "ps_5_0", pixelShader))
        {
            return false;
        }

        static constexpr D3D12_INPUT_ELEMENT_DESC inputLayout[] =
        {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, sizeof(float) * 4, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
        pipelineDesc.pRootSignature = debugRootSignature_.Get();
        pipelineDesc.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
        pipelineDesc.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
        pipelineDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pipelineDesc.SampleMask = UINT_MAX;
        pipelineDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pipelineDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pipelineDesc.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
        pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        pipelineDesc.NumRenderTargets = 1;
        pipelineDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipelineDesc.DSVFormat = kDepthFormat;
        pipelineDesc.SampleDesc.Count = 1;

        hr = context_.device->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(debugLinePipelineState_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateGraphicsPipelineState(debug) failed: " + HrToString(hr));
            return false;
        }

        return true;
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
        CD3DX12_DESCRIPTOR_RANGE descriptorRange;
        descriptorRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_ROOT_PARAMETER rootParameters[2];
        rootParameters[0].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
        rootParameters[1].InitAsDescriptorTable(1, &descriptorRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC samplerDesc(
            0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init(
            static_cast<UINT>(std::size(rootParameters)),
            rootParameters,
            1,
            &samplerDesc,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSignature;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3D12SerializeRootSignature(
            &rootSignatureDesc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSignature.GetAddressOf(),
            errorBlob.GetAddressOf());
        if (FAILED(hr))
        {
            const char* errorText = errorBlob != nullptr ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "unknown ui root signature error";
            logger_.Error("D3D12SerializeRootSignature(ui) failed: " + std::string(errorText));
            return false;
        }

        hr = context_.device->CreateRootSignature(
            0,
            serializedRootSignature->GetBufferPointer(),
            serializedRootSignature->GetBufferSize(),
            IID_PPV_ARGS(uiRootSignature_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateRootSignature(ui) failed: " + HrToString(hr));
            return false;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> vertexShader;
        Microsoft::WRL::ComPtr<ID3DBlob> pixelShader;
        if (!CompileShaderBlob(ResolveSourcePath("assets/shaders/ui_overlay.hlsl"), "VSMain", "vs_5_0", vertexShader) ||
            !CompileShaderBlob(ResolveSourcePath("assets/shaders/ui_overlay.hlsl"), "PSMain", "ps_5_0", pixelShader))
        {
            return false;
        }

        static constexpr D3D12_INPUT_ELEMENT_DESC inputLayout[] =
        {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, sizeof(float) * 2, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, sizeof(float) * 2 + sizeof(std::uint8_t) * 4, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
        pipelineDesc.pRootSignature = uiRootSignature_.Get();
        pipelineDesc.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
        pipelineDesc.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};

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

        D3D12_RASTERIZER_DESC rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
        pipelineDesc.RasterizerState = rasterizerDesc;

        D3D12_DEPTH_STENCIL_DESC depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        depthStencilDesc.DepthEnable = FALSE;
        depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pipelineDesc.DepthStencilState = depthStencilDesc;

        pipelineDesc.SampleMask = UINT_MAX;
        pipelineDesc.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
        pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipelineDesc.NumRenderTargets = 1;
        pipelineDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipelineDesc.DSVFormat = kDepthFormat;
        pipelineDesc.SampleDesc.Count = 1;

        hr = context_.device->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(uiPipelineState_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateGraphicsPipelineState(ui) failed: " + HrToString(hr));
            return false;
        }

        return true;
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

    bool Dx12RenderAdapter::CompileShaderBlob(const std::filesystem::path& sourcePath, const std::string& entryPoint, const std::string& profile, Microsoft::WRL::ComPtr<ID3DBlob>& outBlob) const
    {
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        const HRESULT hr = D3DCompileFromFile(
            sourcePath.c_str(),
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entryPoint.c_str(),
            profile.c_str(),
            compileFlags,
            0,
            outBlob.GetAddressOf(),
            errorBlob.GetAddressOf());
        if (FAILED(hr))
        {
            const char* errorText = errorBlob != nullptr
                ? static_cast<const char*>(errorBlob->GetBufferPointer())
                : "unknown shader compilation error";
            logger_.Error(
                "D3DCompileFromFile failed for " +
                sourcePath.string() +
                " entry=" + entryPoint +
                " profile=" + profile +
                " error=" + errorText);
            return false;
        }

        return true;
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

    void Dx12RenderAdapter::WaitForGpu()
    {
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