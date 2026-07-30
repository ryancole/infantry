#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <string>

#include <CommonStates.h>
#include <DescriptorHeap.h>
#include <Effects.h>
#include <GeometricPrimitive.h>
#include <GraphicsMemory.h>
#include <PostProcess.h>
#include <PrimitiveBatch.h>
#include <SpriteBatch.h>
#include <SpriteFont.h>
#include <VertexTypes.h>

#include <string_view>
#include <vector>

#include "Model.h"

// Dynamic debug/gameplay geometry vertex (position + color).
using Vertex = DirectX::VertexPositionColor;

// Unit-sized lit primitives drawn via Renderer::DrawShape; size and place
// them with the world matrix.
enum class Shape
{
    Box,      // 1x1x1, centered on the origin
    Sphere,   // diameter 1
    Cylinder, // height 1 along y, diameter 1
    Cone,     // height 1 along y, base diameter 1
    Count
};

// D3D12 renderer built on DirectXTK12: the toolkit supplies shaders/PSOs
// (BasicEffect), per-frame dynamic memory (GraphicsMemory), and dynamic
// geometry submission (PrimitiveBatch); we keep the device/swap chain/fence
// plumbing. Single frame in flight (CPU waits for the GPU every frame) —
// simple and plenty for a prototype.
//
// The scene renders to an offscreen color target, then EndFrame resolves it
// to the backbuffer through a post chain: optional monochrome (death flash),
// then bloom (extract -> separable blur at half res -> combine). Screen text
// draws directly on the backbuffer afterwards, so the HUD stays crisp.
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

    // Same blend/depth states with line topology (translucent indicators).
    void DrawLinesAlpha(const Vertex* verts, uint32_t count, const DirectX::XMMATRIX& world);

    // Loads a glTF model (path relative to the exe dir or the repo root).
    std::unique_ptr<Model> LoadModel(const std::string& path);
    void DrawModel(const Model& model, const DirectX::XMMATRIX& world);

    // Draws a lit unit primitive transformed by `world` and tinted `color`.
    void DrawShape(Shape shape, const DirectX::XMMATRIX& world,
                   const DirectX::XMFLOAT4& color);

    // Renders the frame in grayscale while enabled (death/spectator flash).
    void SetMonochrome(bool enabled) { m_monochrome = enabled; }

    // Queues screen-space text, drawn on top of everything else at EndFrame
    // via SpriteBatch/SpriteFont. (x, y) is the top-left of a capital letter
    // and `size` its pixel height, matching the old DebugText metrics.
    void DrawScreenText(std::string_view text, float x, float y, float size,
                        const DirectX::XMFLOAT4& color);
    float MeasureScreenText(std::string_view text, float size) const;

    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

private:
    static constexpr uint32_t kFrameCount = 2;
    static constexpr size_t kBatchVertices = 16384;
    static constexpr size_t kSrvHeapSize = 256;

    void CreateSizedResources();
    void CreateEffects();
    void CreateFlatNormalTexture();
    void CreateSpriteResources();
    void CreateShapePrimitives();
    void CreatePostProcess();
    void CreateOffscreenTargets();
    void RunPostChain();
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
    std::unique_ptr<DirectX::CommonStates> m_states;
    std::unique_ptr<DirectX::DescriptorPile> m_srvPile;
    std::unique_ptr<DirectX::BasicEffect> m_triEffect;
    std::unique_ptr<DirectX::BasicEffect> m_lineEffect;
    std::unique_ptr<DirectX::BasicEffect> m_alphaEffect;
    std::unique_ptr<DirectX::BasicEffect> m_alphaLineEffect;
    std::unique_ptr<DirectX::BasicEffect> m_modelEffect;
    std::unique_ptr<DirectX::NormalMapEffect> m_texModelEffect;
    std::unique_ptr<DirectX::PrimitiveBatch<Vertex>> m_batch;
    std::unique_ptr<DirectX::GeometricPrimitive> m_shapes[static_cast<size_t>(Shape::Count)];

    // 1x1 (0.5, 0.5, 1) normal map bound for textured parts whose material
    // ships no normal texture (NormalMapEffect requires one).
    Microsoft::WRL::ComPtr<ID3D12Resource> m_flatNormalTex;
    D3D12_GPU_DESCRIPTOR_HANDLE m_flatNormalSrv = {};

    // Post-process chain targets. Indexed into the RTV heap after the
    // swap-chain buffers and into fixed SRV slots on the descriptor pile
    // (allocated once; resize recreates the views in place).
    struct PostTarget
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_GPU_DESCRIPTOR_HANDLE srv = {};
        size_t srvSlot = SIZE_MAX;
    };
    PostTarget m_sceneColor; // full res, scene renders here
    PostTarget m_postColor;  // full res, monochrome intermediate
    PostTarget m_bloom1;     // half res, extract + blur ping-pong
    PostTarget m_bloom2;     // half res
    std::unique_ptr<DirectX::BasicPostProcess> m_bloomExtract;
    std::unique_ptr<DirectX::BasicPostProcess> m_bloomBlur;
    std::unique_ptr<DirectX::BasicPostProcess> m_monochromePass;
    std::unique_ptr<DirectX::DualPostProcess> m_bloomCombine;
    bool m_monochrome = false;

    struct TextDraw
    {
        std::string text;
        float x;
        float y;
        float scale;
        DirectX::XMFLOAT4 color;
    };
    std::unique_ptr<DirectX::SpriteBatch> m_spriteBatch;
    std::unique_ptr<DirectX::SpriteFont> m_font;
    std::vector<TextDraw> m_textDraws;
    // Metrics of the baked 'X' glyph, so `size` maps to capital height.
    float m_fontCapHeight = 1.0f;
    float m_fontCapOffsetY = 0.0f;

    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;
    uint32_t m_frameIndex = 0;
    uint32_t m_rtvSize = 0;

    D3D12_VIEWPORT m_viewport = {};
    D3D12_RECT m_scissor = {};
    DirectX::XMMATRIX m_viewProj = DirectX::XMMatrixIdentity();
};
