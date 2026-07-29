#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <cstdint>

struct Vertex
{
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT4 color;
};

// Minimal immediate-mode style D3D12 renderer: one color-only pipeline,
// per-draw MVP via root constants, dynamic geometry streamed through a
// persistently mapped upload buffer. Single frame in flight (CPU waits for
// the GPU every frame) — simple and plenty for a prototype.
class Renderer
{
public:
    void Init(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    void Resize(uint32_t width, uint32_t height);

    void BeginFrame(const float clearColor[4]);
    void EndFrame();

    void SetViewProj(const DirectX::XMMATRIX& viewProj) { m_viewProj = viewProj; }

    void DrawTriangles(const Vertex* verts, uint32_t count, const DirectX::XMMATRIX& world);
    void DrawLines(const Vertex* verts, uint32_t count, const DirectX::XMMATRIX& world);

    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

private:
    static constexpr uint32_t kFrameCount = 2;
    static constexpr uint64_t kVBBytes = 8 * 1024 * 1024;

    void CreateSizedResources();
    void CreatePipeline();
    void CreateVertexBuffer();
    void WaitForGpu();
    void Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
    void Draw(const Vertex* verts, uint32_t count, const DirectX::XMMATRIX& world,
              D3D_PRIMITIVE_TOPOLOGY topology);

    uint32_t m_width = 0;
    uint32_t m_height = 0;

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_backBuffers[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_cmdAlloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmdList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoTri;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoLine;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;

    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;
    uint32_t m_frameIndex = 0;
    uint32_t m_rtvSize = 0;

    uint8_t* m_vbMapped = nullptr;
    uint64_t m_vbOffset = 0;
    D3D12_GPU_VIRTUAL_ADDRESS m_vbGpuVA = 0;

    D3D12_VIEWPORT m_viewport = {};
    D3D12_RECT m_scissor = {};
    DirectX::XMMATRIX m_viewProj = DirectX::XMMatrixIdentity();
};
