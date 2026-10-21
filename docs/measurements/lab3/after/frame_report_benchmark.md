# GPU capture report: benchmark_20261021_001824_frame600.rdc

- API: D3D12
- events: 8071
- draws: 997, dispatches: 0, clears: 2, copies: 0, triangles: 829658
- pipelines: 3, shaders: 0

| Group | Draws | Dispatches | Clears | Copies | Triangles | Render targets |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Frame | 997 | 0 | 2 | 0 | 829658 |  |
| &nbsp;&nbsp;Clear Back Buffer | 0 | 0 | 2 | 0 | 0 |  |
| &nbsp;&nbsp;Scene | 968 | 0 | 0 | 0 | 827752 | viewport 878x508 |
| &nbsp;&nbsp;Debug Lines | 1 | 0 | 0 | 0 | 0 | viewport 878x508 |
| &nbsp;&nbsp;UI | 28 | 0 | 0 | 0 | 1906 | viewport 1280x720 |
| &nbsp;&nbsp;Present Transition | 0 | 0 | 0 | 0 | 0 |  |

## API calls of the frame

| Call | Count |
| --- | ---: |
| ID3D12GraphicsCommandList::SetPipelineState | 997 |
| ID3D12GraphicsCommandList::IASetPrimitiveTopology | 997 |
| ID3D12GraphicsCommandList::IASetVertexBuffers | 997 |
| ID3D12GraphicsCommandList::IASetIndexBuffer | 996 |
| ID3D12GraphicsCommandList::SetDescriptorHeaps | 996 |
| ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable | 996 |
| ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants | 996 |
| ID3D12GraphicsCommandList::DrawIndexedInstanced | 996 |
| ID3D12GraphicsCommandList::RSSetScissorRects | 59 |
| ID3D12GraphicsCommandList::EndQuery | 12 |
| ID3D12GraphicsCommandList::BeginEvent | 6 |
| ID3D12GraphicsCommandList::EndEvent | 6 |
| ID3D12GraphicsCommandList::RSSetViewports | 3 |
| ID3D12GraphicsCommandList::SetGraphicsRootSignature | 3 |
| ID3D12GraphicsCommandList::ResourceBarrier | 2 |
| IDXGISwapChain::GetBuffer | 1 |
| ID3D12GraphicsCommandList::Reset | 1 |
| ID3D12GraphicsCommandList::OMSetRenderTargets | 1 |
| ID3D12GraphicsCommandList::ClearRenderTargetView | 1 |
| ID3D12GraphicsCommandList::ClearDepthStencilView | 1 |
| ID3D12GraphicsCommandList::DrawInstanced | 1 |
| ID3D12GraphicsCommandList::ResolveQueryData | 1 |
| ID3D12GraphicsCommandList::Close | 1 |
| IDXGISwapChain::Present | 1 |

## Warnings

- none
