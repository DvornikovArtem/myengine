# GPU capture report: benchmark_20261016_172745_frame600.rdc

- API: D3D12
- events: 8143
- draws: 999, dispatches: 0, clears: 2, copies: 0, triangles: 832054
- pipelines: 3, shaders: 0

| Group | Draws | Dispatches | Clears | Copies | Triangles | Render targets |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Frame | 999 | 0 | 2 | 0 | 832054 |  |
| &nbsp;&nbsp;Clear Back Buffer | 0 | 0 | 2 | 0 | 0 |  |
| &nbsp;&nbsp;Scene | 970 | 0 | 0 | 0 | 829972 | viewport 856x345 |
| &nbsp;&nbsp;Debug Lines | 1 | 0 | 0 | 0 | 0 | viewport 856x345 |
| &nbsp;&nbsp;UI | 28 | 0 | 0 | 0 | 2082 | viewport 1280x720 |
| &nbsp;&nbsp;Present Transition | 0 | 0 | 0 | 0 | 0 |  |

## API calls of the frame

| Call | Count |
| --- | ---: |
| ID3D12GraphicsCommandList::SetPipelineState | 999 |
| ID3D12GraphicsCommandList::IASetPrimitiveTopology | 999 |
| ID3D12GraphicsCommandList::IASetVertexBuffers | 999 |
| ID3D12GraphicsCommandList::IASetIndexBuffer | 998 |
| ID3D12GraphicsCommandList::SetDescriptorHeaps | 998 |
| ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable | 998 |
| ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants | 998 |
| ID3D12GraphicsCommandList::DrawIndexedInstanced | 998 |
| ID3D12GraphicsCommandList::RSSetScissorRects | 59 |
| ID3D12GraphicsCommandList::SetGraphicsRootSignature | 59 |
| ID3D12GraphicsCommandList::EndQuery | 12 |
| ID3D12GraphicsCommandList::BeginEvent | 6 |
| ID3D12GraphicsCommandList::EndEvent | 6 |
| ID3D12GraphicsCommandList::RSSetViewports | 3 |
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
