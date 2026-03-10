// Dx12RenderAdapter.cpp

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>

#include "d3dx12.h"
#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <myengine/core/Logger.h>
#include <myengine/render/dx12/Dx12RenderAdapter.h>

namespace myengine::render::dx12
{
    namespace
    {
        std::string HrToString(const HRESULT hr)
        {
            std::ostringstream stream;
            stream << "0x" << std::hex << static_cast<unsigned long>(hr);
            return stream.str();
        }

        std::size_t PrimitiveIndex(const PrimitiveType primitive)
        {
            switch (primitive)
            {
                case PrimitiveType::Line:
                    return 0;
                case PrimitiveType::Triangle:
                    return 1;
                case PrimitiveType::Quad:
                    return 2;
                case PrimitiveType::Cube:
                    return 3;
                default:
                    return 1;
            }
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

        std::filesystem::path GetExecutableDirectory()
        {
            wchar_t modulePath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            return std::filesystem::path(modulePath).parent_path();
        }

        std::filesystem::path ResolveRuntimePath(const std::filesystem::path& relativePath)
        {
            if (relativePath.is_absolute())
            {
                return relativePath;
            }

    #ifdef MYENGINE_SOURCE_DIR
            const std::filesystem::path sourceCandidate = std::filesystem::path(MYENGINE_SOURCE_DIR) / relativePath;
            if (std::filesystem::exists(sourceCandidate))
            {
                return sourceCandidate;
            }
    #endif

            const std::filesystem::path executableCandidate = GetExecutableDirectory() / relativePath;
            if (std::filesystem::exists(executableCandidate))
            {
                return executableCandidate;
            }

            const std::filesystem::path currentDirCandidate = std::filesystem::current_path() / relativePath;
            if (std::filesystem::exists(currentDirCandidate))
            {
                return currentDirCandidate;
            }

    #ifdef MYENGINE_SOURCE_DIR
            return std::filesystem::path(MYENGINE_SOURCE_DIR) / relativePath;
    #else
            return relativePath;
    #endif
        }

    }

    Dx12RenderAdapter::Dx12RenderAdapter(core::Logger& logger)
        : logger_(logger), resourceManager_(logger)
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
        if (!BuildPipeline())
        {
            return false;
        }
        if (!BuildPrimitiveResources())
        {
            return false;
        }
        if (!BuildTextureResources())
        {
            return false;
        }

        logger_.Info("DX12 render adapter initialized");
        return true;
    }

    bool Dx12RenderAdapter::PreloadTexture(const std::string_view texturePath)
    {
        if (texturePath.empty())
        {
            return true;
        }

        const std::filesystem::path resolvedPath = ResolveRuntimePath(std::filesystem::path(texturePath));
        const std::string cacheKey = resolvedPath.lexically_normal().generic_string();

        if (loadedTextures_.find(cacheKey) != loadedTextures_.end())
        {
            return true;
        }

        if (missingTextures_.find(cacheKey) != missingTextures_.end())
        {
            return false;
        }

        resource::TextureData loadedTexture{};
        if (!resourceManager_.LoadTextureRgba8(resolvedPath, loadedTexture))
        {
            logger_.Warning(
                "PreloadTexture: failed to load texture file: " + std::string(texturePath) +
                " (resolved: " + resolvedPath.string() + ")");
            missingTextures_.insert(cacheKey);
            return false;
        }

        TextureRecord texture{};
        texture.width = loadedTexture.width;
        texture.height = loadedTexture.height;
        texture.pixels = std::move(loadedTexture.pixelsRgba8);

        missingTextures_.erase(cacheKey);
        loadedTextures_.insert_or_assign(cacheKey, std::move(texture));
        logger_.Info("PreloadTexture: loaded file " + resolvedPath.string());
        return true;
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
        surface.viewProjection = Matrix4::Identity();

        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = surface.width;
        swapDesc.Height = surface.height;
        swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDesc.Stereo = FALSE;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.SampleDesc.Quality = 0;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = kBackBufferCount;
        swapDesc.Scaling = DXGI_SCALING_STRETCH;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        swapDesc.Flags = 0;

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
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        hr = context_.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(surface.rtvHeap.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateDescriptorHeap failed: " + HrToString(hr));
            return {};
        }

        surface.rtvDescriptorSize = context_.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        RebuildSurfaceBuffers(surface);

        const std::uint32_t surfaceId = nextSurfaceId_++;
        surfaces_.emplace(surfaceId, std::move(surface));

        logger_.Info("Created DX12 surface id=" + std::to_string(surfaceId));
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

        const HRESULT hr = surface->swapChain->ResizeBuffers(kBackBufferCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
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
        if (surface == nullptr)
        {
            return false;
        }
        if (surface->dsvHeap == nullptr || surface->depthStencil == nullptr)
        {
            logger_.Error("BeginFrame failed: depth resources are not initialized");
            return false;
        }

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

        surface->currentBackBuffer = surface->swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = surface->backBuffers[surface->currentBackBuffer].Get();

        auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        context_.commandList->ResourceBarrier(1, &toRenderTarget);

        const CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(surface->rtvHeap->GetCPUDescriptorHandleForHeapStart(), static_cast<INT>(surface->currentBackBuffer), static_cast<INT>(surface->rtvDescriptorSize));
        const auto dsvHandle = surface->dsvHeap->GetCPUDescriptorHandleForHeapStart();

        const float color[4] = {clearColor.r, clearColor.g, clearColor.b, clearColor.a};

        context_.commandList->RSSetViewports(1, &surface->viewport);
        context_.commandList->RSSetScissorRects(1, &surface->scissorRect);
        context_.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
        context_.commandList->ClearRenderTargetView(rtvHandle, color, 0, nullptr);
        context_.commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        context_.commandList->SetGraphicsRootSignature(rootSignature_.Get());

        activeSurface_ = surface;
        return true;
    }

    void Dx12RenderAdapter::SetViewProjection(const RenderSurfaceHandle handle, const Matrix4& view, const Matrix4& projection)
    {
        auto* surface = FindSurface(handle);
        if (surface == nullptr)
        {
            return;
        }

        const DirectX::XMMATRIX viewMatrix = MatrixToXm(view);
        const DirectX::XMMATRIX projectionMatrix = MatrixToXm(projection);
        surface->viewProjection = XmToMatrix(DirectX::XMMatrixMultiply(viewMatrix, projectionMatrix));
    }

    void Dx12RenderAdapter::DrawPrimitive(
        const RenderSurfaceHandle handle,
        const PrimitiveType primitive,
        const Matrix4& model,
        const core::Color& color,
        const std::string_view texturePath)
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

        const PrimitiveGeometry& geometry = primitives_[PrimitiveIndex(primitive)];
        if (geometry.vertexCount == 0)
        {
            return;
        }
        if (textureSrvHeap_ == nullptr)
        {
            return;
        }

        if (geometry.topology == D3D_PRIMITIVE_TOPOLOGY_LINELIST)
        {
            context_.commandList->SetPipelineState(linePipelineState_.Get());
        }
        else
        {
            context_.commandList->SetPipelineState(trianglePipelineState_.Get());
        }

        context_.commandList->IASetPrimitiveTopology(geometry.topology);
        context_.commandList->IASetVertexBuffers(0, 1, &geometry.vertexBufferView);

        ID3D12DescriptorHeap* descriptorHeaps[] = {textureSrvHeap_.Get()};
        context_.commandList->SetDescriptorHeaps(1, descriptorHeaps);

        const UINT textureDescriptorIndex = ResolveTextureDescriptorIndex(texturePath);
        D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = textureSrvHeap_->GetGPUDescriptorHandleForHeapStart();
        textureHandle.ptr += static_cast<UINT64>(textureDescriptorIndex) * static_cast<UINT64>(textureSrvDescriptorSize_);
        context_.commandList->SetGraphicsRootDescriptorTable(1, textureHandle);

        struct DrawConstants
        {
            DirectX::XMFLOAT4X4 model;
            DirectX::XMFLOAT4X4 viewProjection;
            float color[4];
        } constants{};

        const DirectX::XMMATRIX modelMatrix = MatrixToXm(model);
        const DirectX::XMMATRIX viewProjectionMatrix = MatrixToXm(surface->viewProjection);

        DirectX::XMStoreFloat4x4(&constants.model, DirectX::XMMatrixTranspose(modelMatrix));
        DirectX::XMStoreFloat4x4(&constants.viewProjection, DirectX::XMMatrixTranspose(viewProjectionMatrix));

        constants.color[0] = color.r;
        constants.color[1] = color.g;
        constants.color[2] = color.b;
        constants.color[3] = color.a;

        context_.commandList->SetGraphicsRoot32BitConstants(0, 36, &constants, 0);
        context_.commandList->DrawInstanced(geometry.vertexCount, 1, 0, 0);
    }

    void Dx12RenderAdapter::EndFrame(const RenderSurfaceHandle handle)
    {
        auto* surface = FindSurface(handle);
        if (surface == nullptr || activeSurface_ == nullptr || surface != activeSurface_)
        {
            return;
        }

        ID3D12Resource* backBuffer = surface->backBuffers[surface->currentBackBuffer].Get();
        auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        context_.commandList->ResourceBarrier(1, &toPresent);

        if (FAILED(context_.commandList->Close()))
        {
            logger_.Error("Command list close failed");
            return;
        }

        ID3D12CommandList* commandLists[] = {context_.commandList.Get()};
        context_.commandQueue->ExecuteCommandLists(1, commandLists);

        const HRESULT presentResult = surface->swapChain->Present(1, 0);
        if (FAILED(presentResult))
        {
            logger_.Error("Present failed: " + HrToString(presentResult));
        }

        WaitForGpu();
        activeSurface_ = nullptr;
    }

    void Dx12RenderAdapter::Shutdown()
    {
        if (context_.device == nullptr)
        {
            return;
        }

        WaitForGpu();

        surfaces_.clear();

        for (auto& primitive : primitives_)
        {
            primitive.vertexUploadBuffer.Reset();
            primitive.vertexBuffer.Reset();
            primitive.vertexBufferView = {};
            primitive.vertexCount = 0;
            primitive.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        }

        loadedTextures_.clear();
        missingTextures_.clear();
        textureSrvHeap_.Reset();
        textureSrvDescriptorSize_ = 0;
        nextTextureDescriptorIndex_ = 0;
        linePipelineState_.Reset();
        trianglePipelineState_.Reset();
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

        logger_.Info("DX12 render adapter shutdown complete");
    }

    bool Dx12RenderAdapter::CreateDevice()
    {
    #if defined(_DEBUG)
        Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debugController.GetAddressOf()))))
        {
            debugController->EnableDebugLayer();
        }
    #endif

        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(context_.factory.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateDXGIFactory1 failed: " + HrToString(hr));
            return false;
        }

        hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(context_.device.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Warning("Hardware device creation failed. Falling back to WARP.");

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
        }

        return true;
    }

    bool Dx12RenderAdapter::CreateCommandObjects()
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

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

        fenceEvent_ = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        if (fenceEvent_ == nullptr)
        {
            logger_.Error("CreateEventEx failed for DX12 fence synchronization");
            return false;
        }

        context_.fenceValue = 0;
        return true;
    }

    bool Dx12RenderAdapter::BuildPipeline()
    {
        const std::wstring shaderPath = ResolveShaderPath();

        UINT compileFlags = 0;
    #if defined(_DEBUG)
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    #endif

        Microsoft::WRL::ComPtr<ID3DBlob> vertexShader;
        Microsoft::WRL::ComPtr<ID3DBlob> pixelShader;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;

        HRESULT hr = D3DCompileFromFile(shaderPath.c_str(),
                                        nullptr,
                                        D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                        "VSMain",
                                        "vs_5_0",
                                        compileFlags,
                                        0,
                                        vertexShader.GetAddressOf(),
                                        errors.GetAddressOf());
        if (errors != nullptr)
        {
            logger_.Error(static_cast<const char*>(errors->GetBufferPointer()));
        }
        if (FAILED(hr))
        {
            logger_.Error("Vertex shader compilation failed: " + HrToString(hr));
            return false;
        }

        errors.Reset();

        hr = D3DCompileFromFile(shaderPath.c_str(),
                                nullptr,
                                D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                "PSMain",
                                "ps_5_0",
                                compileFlags,
                                0,
                                pixelShader.GetAddressOf(),
                                errors.GetAddressOf());
        if (errors != nullptr)
        {
            logger_.Error(static_cast<const char*>(errors->GetBufferPointer()));
        }
        if (FAILED(hr))
        {
            logger_.Error("Pixel shader compilation failed: " + HrToString(hr));
            return false;
        }

        CD3DX12_DESCRIPTOR_RANGE textureRange;
        textureRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        std::array<CD3DX12_ROOT_PARAMETER, 2> rootParameters;
        rootParameters[0].InitAsConstants(36, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
        rootParameters[1].InitAsDescriptorTable(1, &textureRange, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC samplerDesc(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
        samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.ShaderRegister = 0;
        samplerDesc.RegisterSpace = 0;
        samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init(
            static_cast<UINT>(rootParameters.size()),
            rootParameters.data(),
            1,
            &samplerDesc,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSignature;
        Microsoft::WRL::ComPtr<ID3DBlob> rootError;

        hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, serializedRootSignature.GetAddressOf(), rootError.GetAddressOf());
        if (rootError != nullptr)
        {
            logger_.Error(static_cast<const char*>(rootError->GetBufferPointer()));
        }
        if (FAILED(hr))
        {
            logger_.Error("D3D12SerializeRootSignature failed: " + HrToString(hr));
            return false;
        }

        hr = context_.device->CreateRootSignature(0, serializedRootSignature->GetBufferPointer(), serializedRootSignature->GetBufferSize(), IID_PPV_ARGS(rootSignature_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateRootSignature failed: " + HrToString(hr));
            return false;
        }

        std::array<D3D12_INPUT_ELEMENT_DESC, 2> inputLayout =
        {
            D3D12_INPUT_ELEMENT_DESC{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            D3D12_INPUT_ELEMENT_DESC{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC baseDesc{};
        baseDesc.pRootSignature = rootSignature_.Get();
        baseDesc.VS = {reinterpret_cast<BYTE*>(vertexShader->GetBufferPointer()), vertexShader->GetBufferSize()};
        baseDesc.PS = {reinterpret_cast<BYTE*>(pixelShader->GetBufferPointer()), pixelShader->GetBufferSize()};
        baseDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        baseDesc.SampleMask = UINT_MAX;
        baseDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        baseDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        baseDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        baseDesc.DepthStencilState.DepthEnable = TRUE;
        baseDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        baseDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        baseDesc.DepthStencilState.StencilEnable = FALSE;
        baseDesc.InputLayout = {inputLayout.data(), static_cast<UINT>(inputLayout.size())};
        baseDesc.NumRenderTargets = 1;
        baseDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        baseDesc.DSVFormat = kDepthFormat;
        baseDesc.SampleDesc.Count = 1;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC triangleDesc = baseDesc;
        triangleDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        hr = context_.device->CreateGraphicsPipelineState(&triangleDesc, IID_PPV_ARGS(trianglePipelineState_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateGraphicsPipelineState (triangle) failed: " + HrToString(hr));
            return false;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC lineDesc = baseDesc;
        lineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        hr = context_.device->CreateGraphicsPipelineState(&lineDesc, IID_PPV_ARGS(linePipelineState_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateGraphicsPipelineState (line) failed: " + HrToString(hr));
            return false;
        }

        return true;
    }

    bool Dx12RenderAdapter::BuildPrimitiveResources()
    {
        const std::array<Vertex, 2> lineVertices =
        {
            Vertex{{-0.5f, 0.0f, 0.0f}, {0.0f, 0.5f}},
            Vertex{{0.5f, 0.0f, 0.0f}, {1.0f, 0.5f}},
        };

        const std::array<Vertex, 3> triangleVertices =
        {
            Vertex{{0.0f, 0.35f, 0.0f}, {0.5f, 0.0f}},
            Vertex{{0.35f, -0.35f, 0.0f}, {1.0f, 1.0f}},
            Vertex{{-0.35f, -0.35f, 0.0f}, {0.0f, 1.0f}},
        };

        const std::array<Vertex, 6> quadVertices =
        {
            Vertex{{-0.35f, 0.35f, 0.0f}, {0.0f, 0.0f}},
            Vertex{{0.35f, 0.35f, 0.0f}, {1.0f, 0.0f}},
            Vertex{{0.35f, -0.35f, 0.0f}, {1.0f, 1.0f}},
            Vertex{{-0.35f, 0.35f, 0.0f}, {0.0f, 0.0f}},
            Vertex{{0.35f, -0.35f, 0.0f}, {1.0f, 1.0f}},
            Vertex{{-0.35f, -0.35f, 0.0f}, {0.0f, 1.0f}},
        };

        const std::array<Vertex, 36> cubeVertices =
        {
            Vertex{{-0.2f, -0.2f, 0.2f}, {0.0f, 1.0f}}, Vertex{{-0.2f, 0.2f, 0.2f}, {0.0f, 0.0f}}, Vertex{{0.2f, 0.2f, 0.2f}, {1.0f, 0.0f}},
            Vertex{{-0.2f, -0.2f, 0.2f}, {0.0f, 1.0f}}, Vertex{{0.2f, 0.2f, 0.2f}, {1.0f, 0.0f}}, Vertex{{0.2f, -0.2f, 0.2f}, {1.0f, 1.0f}},

            Vertex{{-0.2f, -0.2f, -0.2f}, {1.0f, 1.0f}}, Vertex{{0.2f, 0.2f, -0.2f}, {0.0f, 0.0f}}, Vertex{{-0.2f, 0.2f, -0.2f}, {1.0f, 0.0f}},
            Vertex{{-0.2f, -0.2f, -0.2f}, {1.0f, 1.0f}}, Vertex{{0.2f, -0.2f, -0.2f}, {0.0f, 1.0f}}, Vertex{{0.2f, 0.2f, -0.2f}, {0.0f, 0.0f}},

            Vertex{{-0.2f, 0.2f, -0.2f}, {0.0f, 1.0f}}, Vertex{{0.2f, 0.2f, -0.2f}, {1.0f, 1.0f}}, Vertex{{0.2f, 0.2f, 0.2f}, {1.0f, 0.0f}},
            Vertex{{-0.2f, 0.2f, -0.2f}, {0.0f, 1.0f}}, Vertex{{0.2f, 0.2f, 0.2f}, {1.0f, 0.0f}}, Vertex{{-0.2f, 0.2f, 0.2f}, {0.0f, 0.0f}},

            Vertex{{-0.2f, -0.2f, -0.2f}, {0.0f, 0.0f}}, Vertex{{0.2f, -0.2f, 0.2f}, {1.0f, 1.0f}}, Vertex{{0.2f, -0.2f, -0.2f}, {1.0f, 0.0f}},
            Vertex{{-0.2f, -0.2f, -0.2f}, {0.0f, 0.0f}}, Vertex{{-0.2f, -0.2f, 0.2f}, {0.0f, 1.0f}}, Vertex{{0.2f, -0.2f, 0.2f}, {1.0f, 1.0f}},

            Vertex{{-0.2f, -0.2f, -0.2f}, {1.0f, 1.0f}}, Vertex{{-0.2f, 0.2f, 0.2f}, {0.0f, 0.0f}}, Vertex{{-0.2f, 0.2f, -0.2f}, {1.0f, 0.0f}},
            Vertex{{-0.2f, -0.2f, -0.2f}, {1.0f, 1.0f}}, Vertex{{-0.2f, -0.2f, 0.2f}, {0.0f, 1.0f}}, Vertex{{-0.2f, 0.2f, 0.2f}, {0.0f, 0.0f}},

            Vertex{{0.2f, -0.2f, -0.2f}, {0.0f, 1.0f}}, Vertex{{0.2f, 0.2f, -0.2f}, {0.0f, 0.0f}}, Vertex{{0.2f, 0.2f, 0.2f}, {1.0f, 0.0f}},
            Vertex{{0.2f, -0.2f, -0.2f}, {0.0f, 1.0f}}, Vertex{{0.2f, 0.2f, 0.2f}, {1.0f, 0.0f}}, Vertex{{0.2f, -0.2f, 0.2f}, {1.0f, 1.0f}},
        };

        if (!UploadPrimitiveGeometry(primitives_[PrimitiveIndex(PrimitiveType::Line)], lineVertices.data(), static_cast<UINT>(lineVertices.size()), D3D_PRIMITIVE_TOPOLOGY_LINELIST))
        {
            return false;
        }
        if (!UploadPrimitiveGeometry(primitives_[PrimitiveIndex(PrimitiveType::Triangle)], triangleVertices.data(), static_cast<UINT>(triangleVertices.size()), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST))
        {
            return false;
        }
        if (!UploadPrimitiveGeometry(primitives_[PrimitiveIndex(PrimitiveType::Quad)], quadVertices.data(), static_cast<UINT>(quadVertices.size()), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST))
        {
            return false;
        }
        if (!UploadPrimitiveGeometry(primitives_[PrimitiveIndex(PrimitiveType::Cube)], cubeVertices.data(), static_cast<UINT>(cubeVertices.size()), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST))
        {
            return false;
        }

        return true;
    }

    bool Dx12RenderAdapter::BuildTextureResources()
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.NumDescriptors = kMaxTextureDescriptors;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        const HRESULT hr = context_.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(textureSrvHeap_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateDescriptorHeap (SRV) failed: " + HrToString(hr));
            return false;
        }

        textureSrvDescriptorSize_ = context_.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        nextTextureDescriptorIndex_ = 0;
        missingTextures_.clear();
        loadedTextures_.clear();

        TextureRecord defaultTexture{};
        defaultTexture.width = 1;
        defaultTexture.height = 1;
        defaultTexture.pixels = {255, 255, 255, 255};
        loadedTextures_.emplace(defaultTextureKey_, std::move(defaultTexture));
        return true;
    }

    bool Dx12RenderAdapter::EnsureTextureUploaded(TextureRecord& texture)
    {
        if (texture.uploaded)
        {
            return true;
        }
        if (textureSrvHeap_ == nullptr || context_.device == nullptr || context_.commandList == nullptr)
        {
            return false;
        }
        if (activeSurface_ == nullptr)
        {
            return false;
        }
        if (texture.width == 0 || texture.height == 0 || texture.pixels.empty())
        {
            return false;
        }
        if (nextTextureDescriptorIndex_ >= kMaxTextureDescriptors)
        {
            logger_.Warning("EnsureTextureUploaded: SRV heap is full");
            return false;
        }

        const auto textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R8G8B8A8_UNORM,
            texture.width,
            texture.height);

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = context_.device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(texture.textureResource.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommittedResource for texture failed: " + HrToString(hr));
            return false;
        }

        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(texture.textureResource.Get(), 0, 1);
        const auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
        CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
        hr = context_.device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(texture.uploadResource.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommittedResource for texture upload failed: " + HrToString(hr));
            return false;
        }

        D3D12_SUBRESOURCE_DATA textureData{};
        textureData.pData = texture.pixels.data();
        textureData.RowPitch = static_cast<LONG_PTR>(texture.width) * 4;
        textureData.SlicePitch = textureData.RowPitch * static_cast<LONG_PTR>(texture.height);

        UpdateSubresources<1>(
            context_.commandList.Get(),
            texture.textureResource.Get(),
            texture.uploadResource.Get(),
            0,
            0,
            1,
            &textureData);

        const auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
            texture.textureResource.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        context_.commandList->ResourceBarrier(1, &toShaderResource);

        const UINT descriptorIndex = nextTextureDescriptorIndex_++;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = textureSrvHeap_->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += static_cast<UINT64>(descriptorIndex) * static_cast<UINT64>(textureSrvDescriptorSize_);
        context_.device->CreateShaderResourceView(texture.textureResource.Get(), &srvDesc, cpuHandle);

        texture.descriptorIndex = descriptorIndex;
        texture.uploaded = true;
        texture.pixels.clear();
        texture.pixels.shrink_to_fit();
        return true;
    }

    UINT Dx12RenderAdapter::ResolveTextureDescriptorIndex(const std::string_view texturePath)
    {
        auto getDefaultTexture = [this]() -> TextureRecord*
        {
            const auto it = loadedTextures_.find(defaultTextureKey_);
            if (it == loadedTextures_.end())
            {
                return nullptr;
            }
            return &it->second;
        };

        if (TextureRecord* defaultTexture = getDefaultTexture(); defaultTexture != nullptr)
        {
            EnsureTextureUploaded(*defaultTexture);
        }

        if (!texturePath.empty())
        {
            const std::filesystem::path resolvedPath = ResolveRuntimePath(std::filesystem::path(texturePath));
            const std::string key = resolvedPath.lexically_normal().generic_string();

            auto it = loadedTextures_.find(key);
            if (it == loadedTextures_.end() && missingTextures_.find(key) == missingTextures_.end())
            {
                PreloadTexture(texturePath);
                it = loadedTextures_.find(key);
            }

            if (it != loadedTextures_.end() && EnsureTextureUploaded(it->second))
            {
                return it->second.descriptorIndex;
            }
        }

        if (TextureRecord* defaultTexture = getDefaultTexture(); defaultTexture != nullptr && defaultTexture->uploaded)
        {
            return defaultTexture->descriptorIndex;
        }

        return 0;
    }

    bool Dx12RenderAdapter::UploadPrimitiveGeometry(
        PrimitiveGeometry& geometry,
        const Vertex* vertices,
        const UINT vertexCount,
        const D3D_PRIMITIVE_TOPOLOGY topology)
    {
        const UINT vertexBufferSize = static_cast<UINT>(sizeof(Vertex) * vertexCount);

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);

        HRESULT hr = context_.device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(geometry.vertexBuffer.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommittedResource for vertex buffer failed: " + HrToString(hr));
            return false;
        }

        CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
        hr = context_.device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(geometry.vertexUploadBuffer.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommittedResource for upload buffer failed: " + HrToString(hr));
            return false;
        }

        if (FAILED(context_.commandAllocator->Reset()))
        {
            logger_.Error("Command allocator reset failed while uploading primitive geometry");
            return false;
        }

        if (FAILED(context_.commandList->Reset(context_.commandAllocator.Get(), nullptr)))
        {
            logger_.Error("Command list reset failed while uploading primitive geometry");
            return false;
        }

        D3D12_SUBRESOURCE_DATA vertexData{};
        vertexData.pData = vertices;
        vertexData.RowPitch = vertexBufferSize;
        vertexData.SlicePitch = vertexBufferSize;

        auto toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(geometry.vertexBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        context_.commandList->ResourceBarrier(1, &toCopyDest);

        UpdateSubresources<1>(context_.commandList.Get(), geometry.vertexBuffer.Get(), geometry.vertexUploadBuffer.Get(), 0, 0, 1, &vertexData);

        auto toVertexBuffer = CD3DX12_RESOURCE_BARRIER::Transition(geometry.vertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        context_.commandList->ResourceBarrier(1, &toVertexBuffer);

        if (FAILED(context_.commandList->Close()))
        {
            logger_.Error("Command list close failed during primitive upload");
            return false;
        }

        ID3D12CommandList* commandLists[] = {context_.commandList.Get()};
        context_.commandQueue->ExecuteCommandLists(1, commandLists);
        WaitForGpu();

        geometry.vertexBufferView.BufferLocation = geometry.vertexBuffer->GetGPUVirtualAddress();
        geometry.vertexBufferView.StrideInBytes = sizeof(Vertex);
        geometry.vertexBufferView.SizeInBytes = vertexBufferSize;
        geometry.topology = topology;
        geometry.vertexCount = vertexCount;

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
        dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        HRESULT dsvHeapResult = context_.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(surface.dsvHeap.GetAddressOf()));
        if (FAILED(dsvHeapResult))
        {
            logger_.Error("CreateDescriptorHeap for DSV failed: " + HrToString(dsvHeapResult));
            return;
        }

        D3D12_RESOURCE_DESC depthDesc{};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Alignment = 0;
        depthDesc.Width = surface.width;
        depthDesc.Height = surface.height;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = kDepthFormat;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.SampleDesc.Quality = 0;
        depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depthClearValue{};
        depthClearValue.Format = kDepthFormat;
        depthClearValue.DepthStencil.Depth = 1.0f;
        depthClearValue.DepthStencil.Stencil = 0;

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT depthResult = context_.device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depthClearValue,
            IID_PPV_ARGS(surface.depthStencil.GetAddressOf()));
        if (FAILED(depthResult))
        {
            logger_.Error("CreateCommittedResource for depth buffer failed: " + HrToString(depthResult));
            return;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = kDepthFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

        context_.device->CreateDepthStencilView(surface.depthStencil.Get(), &dsvDesc, surface.dsvHeap->GetCPUDescriptorHandleForHeapStart());

        surface.viewport.TopLeftX = 0.0f;
        surface.viewport.TopLeftY = 0.0f;
        surface.viewport.Width = static_cast<float>(surface.width);
        surface.viewport.Height = static_cast<float>(surface.height);
        surface.viewport.MinDepth = 0.0f;
        surface.viewport.MaxDepth = 1.0f;

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

    Dx12RenderAdapter::SurfaceData* Dx12RenderAdapter::FindSurface(const RenderSurfaceHandle handle)
    {
        const auto it = surfaces_.find(handle.value);
        if (it == surfaces_.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    const Dx12RenderAdapter::SurfaceData* Dx12RenderAdapter::FindSurface(const RenderSurfaceHandle handle) const
    {
        const auto it = surfaces_.find(handle.value);
        if (it == surfaces_.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    std::wstring Dx12RenderAdapter::ResolveShaderPath() const
    {
        const std::filesystem::path resolved = ResolveRuntimePath("assets/shaders/primitive.hlsl");
        return resolved.wstring();
    }
}