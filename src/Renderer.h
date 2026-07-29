#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <string>

#include <Effects.h>
#include <GraphicsMemory.h>
#include <PrimitiveBatch.h>
#include <VertexTypes.h>

#include "Model.h"

// Dynamic debug/gameplay geometry vertex (position + color).
using Vertex = DirectX::VertexPositionColor;

// D3D12 renderer built on DirectXTK12: the toolkit supplies shaders/PSOs
// (BasicEffect), per-frame dynamic memory (GraphicsMemory), and dynamic
// geometry submission (PrimitiveBatch); we keep the device/swap chain/fence
// plumbing. Single frame in flight (CPU waits for the GPU every frame) —
// simple and plenty for a prototype.
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

    // Alpha-blended triangles that test depth but don't write it — overlays
    // like the fog of war that must darken the ground yet sit behind walls.
    // Draw these after all opaque geometry.
    void DrawTrianglesAlpha(const Vertex* verts, uint32_t count, const DirectX::XMMATRIX& world);

    // Loads a glTF model (path relative to the exe dir or the repo root).
    std::unique_ptr<Model> LoadModel(const std::string& path);
    void DrawModel(const Model& model, const DirectX::XMMATRIX& world);

    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

private:
    static constexpr uint32_t kFrameCount = 2;
    static constexpr size_t kBatchVertices = 16384;

    void CreateSizedResources();
    void CreateEffects();
    void WaitForGpu();
    void Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
    void DrawBatch(const Vertex* verts, uint32_t count, const DirectX::XMMATRIX& world,
                   DirectX::BasicEffect* effect, D3D_PRIMITIVE_TOPOLOGY topology);

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

    std::unique_ptr<DirectX::GraphicsMemory> m_graphicsMemory;
    std::unique_ptr<DirectX::BasicEffect> m_triEffect;
    std::unique_ptr<DirectX::BasicEffect> m_lineEffect;
    std::unique_ptr<DirectX::BasicEffect> m_alphaEffect;
    std::unique_ptr<DirectX::BasicEffect> m_modelEffect;
    std::unique_ptr<DirectX::PrimitiveBatch<Vertex>> m_batch;

    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;
    uint32_t m_frameIndex = 0;
    uint32_t m_rtvSize = 0;

    D3D12_VIEWPORT m_viewport = {};
    D3D12_RECT m_scissor = {};
    DirectX::XMMATRIX m_viewProj = DirectX::XMMatrixIdentity();
};
