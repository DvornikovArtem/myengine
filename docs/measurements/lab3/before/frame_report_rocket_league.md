# GPU capture report: rocket_league_20261016_172807_frame600.rdc

- API: D3D12
- events: 781
- draws: 79, dispatches: 0, clears: 2, copies: 0, triangles: 9080
- pipelines: 2, shaders: 0

| Group | Draws | Dispatches | Clears | Copies | Triangles | Render targets |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Frame | 79 | 0 | 2 | 0 | 9080 |  |
| &nbsp;&nbsp;Clear Back Buffer | 0 | 0 | 2 | 0 | 0 |  |
| &nbsp;&nbsp;Scene | 51 | 0 | 0 | 0 | 6684 | viewport 856x345 |
| &nbsp;&nbsp;UI | 28 | 0 | 0 | 0 | 2396 | viewport 1280x720 |
| &nbsp;&nbsp;Present Transition | 0 | 0 | 0 | 0 | 0 |  |

## API calls of the frame

| Call | Count |
| --- | ---: |
| ID3D12GraphicsCommandList::SetPipelineState | 79 |
| ID3D12GraphicsCommandList::IASetPrimitiveTopology | 79 |
| ID3D12GraphicsCommandList::IASetVertexBuffers | 79 |
| ID3D12GraphicsCommandList::IASetIndexBuffer | 79 |
| ID3D12GraphicsCommandList::SetDescriptorHeaps | 79 |
| ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable | 79 |
| ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants | 79 |
| ID3D12GraphicsCommandList::DrawIndexedInstanced | 79 |
| ID3D12GraphicsCommandList::RSSetScissorRects | 59 |
| ID3D12GraphicsCommandList::SetGraphicsRootSignature | 57 |
| ID3D12GraphicsCommandList::EndQuery | 10 |
| ID3D12GraphicsCommandList::BeginEvent | 5 |
| ID3D12GraphicsCommandList::EndEvent | 5 |
| ID3D12GraphicsCommandList::RSSetViewports | 3 |
| ID3D12GraphicsCommandList::ResourceBarrier | 2 |
| IDXGISwapChain::GetBuffer | 1 |
| ID3D12GraphicsCommandList::Reset | 1 |
| ID3D12GraphicsCommandList::OMSetRenderTargets | 1 |
| ID3D12GraphicsCommandList::ClearRenderTargetView | 1 |
| ID3D12GraphicsCommandList::ClearDepthStencilView | 1 |
| ID3D12GraphicsCommandList::ResolveQueryData | 1 |
| ID3D12GraphicsCommandList::Close | 1 |
| IDXGISwapChain::Present | 1 |

## Warnings

- none
