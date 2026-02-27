// Dx12RenderAdapter.cpp

#include <algorithm>
#include <array>
#include <filesystem>
#include <sstream>
#include <string>

#include "d3dx12.h"
#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <myengine/render/dx12/Dx12RenderAdapter.h>
#include <myengine/core/Logger.h>

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
    }

    Dx12RenderAdapter::Dx12RenderAdapter(core::Logger& logger) : logger_(logger) {}

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
        if (!BuildTriangleResources())
        {
            return false;
        }

        logger_.Info("DX12 render adapter initialized");
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

        if (FAILED(context_.commandAllocator->Reset()))
        {
            logger_.Error("Command allocator reset failed");
            return false;
        }

        if (FAILED(context_.commandList->Reset(context_.commandAllocator.Get(), pipelineState_.Get())))
        {
            logger_.Error("Command list reset failed");
            return false;
        }

        surface->currentBackBuffer = surface->swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = surface->backBuffers[surface->currentBackBuffer].Get();

        auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        context_.commandList->ResourceBarrier(1, &toRenderTarget);

        const CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(surface->rtvHeap->GetCPUDescriptorHandleForHeapStart(), static_cast<INT>(surface->currentBackBuffer), static_cast<INT>(surface->rtvDescriptorSize));

        const float color[4] = {clearColor.r, clearColor.g, clearColor.b, clearColor.a};

        context_.commandList->RSSetViewports(1, &surface->viewport);
        context_.commandList->RSSetScissorRects(1, &surface->scissorRect);
        context_.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        context_.commandList->ClearRenderTargetView(rtvHandle, color, 0, nullptr);

        context_.commandList->SetGraphicsRootSignature(rootSignature_.Get());
        context_.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_.commandList->IASetVertexBuffers(0, 1, &vertexBufferView_);

        activeSurface_ = surface;
        return true;
    }

    void Dx12RenderAdapter::DrawPrimitive(const RenderSurfaceHandle handle, const Transform2D& transform)
    {
        if (activeSurface_ == nullptr || !handle.IsValid())
        {
            return;
        }

        const DirectX::XMMATRIX model = DirectX::XMMatrixScaling(transform.scale, transform.scale, 1.0f) *
            DirectX::XMMatrixRotationZ(DirectX::XMConvertToRadians(transform.rotationDeg)) *
            DirectX::XMMatrixTranslation(transform.positionX, transform.positionY, 0.0f);

        DirectX::XMFLOAT4X4 transformMatrix{};
        XMStoreFloat4x4(&transformMatrix, XMMatrixTranspose(model));

        context_.commandList->SetGraphicsRoot32BitConstants(0, 16, &transformMatrix, 0);
        context_.commandList->DrawInstanced(3, 1, 0, 0);
    }

    void Dx12RenderAdapter::EndFrame(const RenderSurfaceHandle handle)
    {
        auto* surface = FindSurface(handle);
        if (surface == nullptr || activeSurface_ == nullptr)
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

        vertexUploadBuffer_.Reset();
        vertexBuffer_.Reset();
        pipelineState_.Reset();
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

        CD3DX12_ROOT_PARAMETER rootParameter;
        rootParameter.InitAsConstants(16, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);

        CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init(1, &rootParameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

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
            D3D12_INPUT_ELEMENT_DESC{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = rootSignature_.Get();
        psoDesc.VS = {reinterpret_cast<BYTE*>(vertexShader->GetBufferPointer()), vertexShader->GetBufferSize()};
        psoDesc.PS = {reinterpret_cast<BYTE*>(pixelShader->GetBufferPointer()), pixelShader->GetBufferSize()};
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.InputLayout = {inputLayout.data(), static_cast<UINT>(inputLayout.size())};
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;

        hr = context_.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(pipelineState_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateGraphicsPipelineState failed: " + HrToString(hr));
            return false;
        }

        return true;
    }

    bool Dx12RenderAdapter::BuildTriangleResources()
    {
        const std::array<Vertex, 3> vertices =
        {
            Vertex{{0.0f, 0.35f, 0.0f}, {1.0f, 0.3f, 0.3f, 1.0f}},
            Vertex{{0.35f, -0.35f, 0.0f}, {0.3f, 1.0f, 0.3f, 1.0f}},
            Vertex{{-0.35f, -0.35f, 0.0f}, {0.3f, 0.3f, 1.0f, 1.0f}},
        };

        const UINT vertexBufferSize = static_cast<UINT>(sizeof(Vertex) * vertices.size());

        HRESULT hr = S_OK;

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);

        hr = context_.device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(vertexBuffer_.GetAddressOf()));
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
            IID_PPV_ARGS(vertexUploadBuffer_.GetAddressOf()));
        if (FAILED(hr))
        {
            logger_.Error("CreateCommittedResource for upload buffer failed: " + HrToString(hr));
            return false;
        }

        if (FAILED(context_.commandAllocator->Reset()))
        {
            logger_.Error("Command allocator reset failed while uploading vertex buffer");
            return false;
        }

        if (FAILED(context_.commandList->Reset(context_.commandAllocator.Get(), nullptr)))
        {
            logger_.Error("Command list reset failed while uploading vertex buffer");
            return false;
        }

        D3D12_SUBRESOURCE_DATA vertexData{};
        vertexData.pData = vertices.data();
        vertexData.RowPitch = vertexBufferSize;
        vertexData.SlicePitch = vertexData.RowPitch;

        auto toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(vertexBuffer_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        context_.commandList->ResourceBarrier(1, &toCopyDest);

        UpdateSubresources<1>(context_.commandList.Get(), vertexBuffer_.Get(), vertexUploadBuffer_.Get(), 0, 0, 1, &vertexData);

        auto toVertexBuffer = CD3DX12_RESOURCE_BARRIER::Transition(vertexBuffer_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        context_.commandList->ResourceBarrier(1, &toVertexBuffer);

        if (FAILED(context_.commandList->Close()))
        {
            logger_.Error("Command list close failed during vertex upload");
            return false;
        }

        ID3D12CommandList* commandLists[] = {context_.commandList.Get()};
        context_.commandQueue->ExecuteCommandLists(1, commandLists);
        WaitForGpu();

        vertexBufferView_.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
        vertexBufferView_.StrideInBytes = sizeof(Vertex);
        vertexBufferView_.SizeInBytes = vertexBufferSize;

        return true;
    }

    void Dx12RenderAdapter::RebuildSurfaceBuffers(SurfaceData& surface)
    {
        for (auto& backBuffer : surface.backBuffers)
        {
            backBuffer.Reset();
        }

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
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);

        const std::filesystem::path executablePath(modulePath);
        const std::filesystem::path executableDir = executablePath.parent_path();

        const std::filesystem::path firstCandidate = executableDir / L"assets/shaders/primitive.hlsl";
        if (std::filesystem::exists(firstCandidate))
        {
            return firstCandidate.wstring();
        }

        const std::filesystem::path secondCandidate = L"assets/shaders/primitive.hlsl";
        if (std::filesystem::exists(secondCandidate))
        {
            return secondCandidate.wstring();
        }

        return firstCandidate.wstring();
    }
}