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
#include "Vertex.h"

// Unit-sized lit primitives drawn via Renderer::DrawShape; size and place
// them with the world matrix.
//
// Curved shapes come in tessellation tiers. A sphere at the default tier is
// 1056 triangles, which is wasted on the soldier model's parts: at gameplay
// zoom a helmet covers ~13 pixels and a hand ~5, so the coarse tiers are
// visually identical there and cut the triangle count several-fold. Pick the
// tier from a part's on-screen size, not its role.
enum class Shape
{
    Box,         // 1x1x1, centered on the origin
    Sphere,      // diameter 1
    SphereMed,   // diameter 1, for features ~10px across
    SphereLow,   // diameter 1, for features a few px across
    Cylinder,    // height 1 along y, diameter 1
    CylinderLow, // height 1 along y, diameter 1, for thin limb-sized parts
    Cone,        // height 1 along y, base diameter 1
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

    // The fog of war's two passes, which exist because a side's sight is the
    // union of several soldiers' and darkness doesn't union: drawn per soldier
    // it would stack in the overlaps, and it would cover ground one squadmate
    // sees clearly because the next one doesn't.
    //
    // So the light goes down first and the darkness once. MarkSeen writes
    // nothing but a mark in the stencil buffer — no color, no depth — over
    // every pixel its triangles cover, and may be called as many times as
    // there are eyes; marking twice is marking. DrawTrianglesUnseen then draws
    // alpha triangles everywhere the mark isn't. Both test depth without
    // writing it, like the other overlays, so a wall still punches through.
    // The mark is cleared with the depth buffer at BeginFrame.
    void MarkSeen(const Vertex* verts, uint32_t count, const DirectX::XMMATRIX& world);
    void DrawTrianglesUnseen(const Vertex* verts, uint32_t count, const DirectX::XMMATRIX& world);

    // The soft edge on that darkness. Alpha triangles drawn where the mark
    // *is* — over ground the side can see — carrying their own alpha per
    // vertex, so the far edge of sight can be handed a band that fades in
    // rather than a line the ground changes brightness across. A gradient is
    // the one thing a stencil can't be: it holds a yes or a no, and this is
    // the pass that puts the in-between back.
    //
    // It clears the mark as it paints, which is what makes a squad's worth of
    // these safe to lay down one after another. Darkness still doesn't union,
    // and these bands overlap wherever two soldiers' ranges do; consuming each
    // pixel means the first band to reach it is the only one that paints it.
    // What makes that sound rather than arbitrary is the caller's business:
    // every band works its alpha out from the whole squad's reach rather than
    // from its own, so they all agree about any pixel they share and which one
    // arrives first stops mattering.
    //
    // Nothing may read the mark after this — it is the frame's last stencil
    // pass, and it leaves the mark behind it wiped.
    void DrawFogEdge(const Vertex* verts, uint32_t count, const DirectX::XMMATRIX& world);

    // Opaque triangles that stand a little off the floor and therefore have to
    // be fogged by hand — the grass, today, and anything else that is ground
    // detail rather than a landmark.
    //
    // The fog is a flat sheet at a fixed height, which works for everything
    // lying on the floor and for everything tall enough to be meant to punch
    // through it. Between those two is a gap: a blade a third of a unit tall is
    // nearer the eye than the darkness, so it would stay lit through it and
    // speckle the unseen half of the map. Skipping it there instead is worse
    // again — the ground under the fog is meant to read as ground, and ground
    // with the grass cut out of it doesn't.
    //
    // So it is drawn twice against the same mark, after the fog: once whole
    // where the mark is, and once through `unseenTint` where it isn't. Pass the
    // fraction of itself the fog leaves the floor — one minus the sheet's alpha
    // — and detail dims exactly as much as the ground it grows in.
    //
    // Depth is tested and not written, so a wall or a soldier in front still
    // hides it and nothing drawn afterwards — the aim line, the rings — gets
    // chewed up by it.
    void DrawGroundDetail(const Vertex* verts, uint32_t count, const DirectX::XMMATRIX& world,
                          float unseenTint);

    // The sun's shadows, in three calls around the scene.
    //
    // BeginShadowPass aims an orthographic camera down the sun's direction at
    // the patch of floor currently on screen and points the command list at a
    // depth map instead of the scene. Every DrawModel, DrawShape and
    // DrawTriangles between it and EndShadowPass then records how far the sun
    // gets rather than drawing anything — same calls, same matrices, so what
    // casts a shadow is submitted by the same code that draws it and the two
    // can't drift apart. Nothing else about the renderer changes state across
    // the pair: EndShadowPass puts the scene's targets, viewport and stencil
    // reference back exactly as BeginFrame left them.
    //
    // ApplyShadows is the pass that spends it. It reads the frame's own depth
    // buffer, works out where on the floor each pixel is standing, asks the map
    // whether the sun reached there, and multiplies the ones it didn't down —
    // which is why the toolkit's effects never had to learn about any of this.
    // Call it once every opaque thing has written depth and before the fog of
    // war goes over the top, so shadow lands on the ground and darkness lands
    // on the shadow rather than the other way about.
    void BeginShadowPass();
    void EndShadowPass();
    void ApplyShadows();

    // Loads a glTF model (path relative to the exe dir or the repo root).
    std::unique_ptr<Model> LoadModel(const std::string& path);
    void DrawModel(const Model& model, const DirectX::XMMATRIX& world);

    // Draws a lit unit primitive transformed by `world` and tinted `color`.
    void DrawShape(Shape shape, const DirectX::XMMATRIX& world,
                   const DirectX::XMFLOAT4& color);

    // Conservative test of whether a world-space sphere can touch the viewport,
    // against the matrix given to SetViewProj. Cheap enough to call per object
    // and worth it for anything that costs more than a few draws to submit.
    bool IsSphereVisible(const DirectX::XMFLOAT3& center, float radius) const;

    // Where a world point lands on screen, in the same pixel coordinates
    // DrawScreenText and DrawScreenTriangles take — the other direction from
    // IsoCamera::ScreenToGround, and here rather than there because it is the
    // matrix given to SetViewProj that decides it, which is this class's to
    // know. False when the point is off the viewport or behind the eye, in
    // which case `out` is not written: a label anchored to something nobody can
    // see is a label with nowhere to go.
    bool WorldToScreen(const DirectX::XMFLOAT3& world, DirectX::XMFLOAT2& out) const;

    // Renders the frame in grayscale while enabled (death/spectator flash).
    void SetMonochrome(bool enabled) { m_monochrome = enabled; }

    // Command-recording cost (smoothed) from BeginFrame to the last command
    // written, and the draw calls recorded last frame. Present is synced to
    // vblank, so wall-clock frame time sits at the refresh interval and hides
    // this; these are the numbers that move when draw submission gets cheaper.
    // Both lag by a frame — the current one is still recording while game code
    // is asking.
    float CpuFrameMs() const { return m_cpuFrameMs; }
    uint32_t LastDrawCalls() const { return m_lastDrawCalls; }

    // Queues screen-space text, drawn on top of everything else at EndFrame
    // via SpriteBatch/SpriteFont. (x, y) is the top-left of a capital letter
    // and `size` its pixel height, matching the old DebugText metrics.
    void DrawScreenText(std::string_view text, float x, float y, float size,
                        const DirectX::XMFLOAT4& color);

    // The same text with a dark edge around it, for the words that are drawn
    // over the arena rather than over a panel. Everything else the HUD says sits
    // on a background this file chose; a label pinned to a soldier is read
    // against whatever that soldier is standing on, and green letters over a
    // palm or red ones over a blood stain are letters that aren't there.
    //
    // Eight offset copies underneath and the text over them, which is what an
    // outline costs when the font is an alpha atlas and the batch has no shader
    // of its own. The edge is a share of the cap height rather than a fixed
    // number of pixels, so it stays an edge rather than becoming a smudge on a
    // large window or vanishing on a small one, and it never goes under one
    // physical pixel, which is the width below which it stops existing.
    void DrawScreenTextOutlined(std::string_view text, float x, float y, float size,
                                const DirectX::XMFLOAT4& color);

    float MeasureScreenText(std::string_view text, float size) const;

    // Queues alpha-blended screen-space triangles in pixel coordinates (x
    // right, y down, z ignored), drawn at EndFrame between the post chain and
    // the text. Overlay art built from these — the HUD — therefore escapes
    // bloom and the death desaturation, the same way the text already does, and
    // sits under it. Depth isn't tested: submission order is draw order.
    void DrawScreenTriangles(const Vertex* verts, uint32_t count);

    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

    // The most vertices one of the batched draws above will take. A call over
    // it is dropped rather than split, so anything building geometry by the
    // thousand — the blood on the floor, the grass on it — has to bound itself
    // against this or feed it in pieces.
    static constexpr size_t kBatchVertices = 16384;

private:
    static constexpr uint32_t kFrameCount = 2;
    static constexpr size_t kSrvHeapSize = 256;

    void CreateSizedResources();
    void CreateEffects();
    void DrawScreenGeometry();
    void CreateFlatNormalTexture();
    void CreateSpriteResources();
    void CreateShapePrimitives();
    void CreatePostProcess();
    void CreateOffscreenTargets();
    void CreateShadowResources();
    void CreateShadowPipelines();
    // Where to stand the sun so its map covers the ground the camera can see.
    // Derived from the matrix given to SetViewProj rather than handed in: what
    // is on screen is a fact about that matrix, and asking the caller for it
    // again would be asking them to agree with it.
    DirectX::XMMATRIX ComputeLightViewProj() const;
    // Sets the depth-only pipeline and this draw's matrix, for the calls that
    // are routed into the shadow map rather than into the scene.
    void BindShadowDraw(const DirectX::XMMATRIX& world);
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
    // The fog's mask and the sheet that reads it; see MarkSeen.
    std::unique_ptr<DirectX::BasicEffect> m_seenEffect;
    std::unique_ptr<DirectX::BasicEffect> m_unseenEffect;
    std::unique_ptr<DirectX::BasicEffect> m_fogEdgeEffect;
    // DrawGroundDetail's pair: the same opaque state, one testing the fog's
    // mark for equal and one for not.
    std::unique_ptr<DirectX::BasicEffect> m_detailSeenEffect;
    std::unique_ptr<DirectX::BasicEffect> m_detailUnseenEffect;
    std::unique_ptr<DirectX::BasicEffect> m_modelEffect;
    std::unique_ptr<DirectX::NormalMapEffect> m_texModelEffect;
    // Overlay geometry: no depth (nothing is bound by then) and straight-alpha
    // blending, so a translucent HUD panel darkens what's behind it by exactly
    // the alpha it asks for.
    std::unique_ptr<DirectX::BasicEffect> m_screenTriEffect;
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
    // The sun's depth map and the two pipelines that write and read it. These
    // are ours rather than the toolkit's — see src/shaders — which is why they
    // carry root signatures of their own instead of going through an Effect.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowMap;
    D3D12_GPU_DESCRIPTOR_HANDLE m_shadowSrv = {};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_shadowDepthRS;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_shadowDepthPso;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_shadowApplyRS;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_shadowApplyPso;
    // The scene's own depth, as something a shader can read. Allocated once and
    // rebuilt in place on resize, like the post targets below.
    D3D12_GPU_DESCRIPTOR_HANDLE m_depthSrv = {};
    size_t m_depthSrvSlot = SIZE_MAX;
    // Set between BeginShadowPass and EndShadowPass; what the draw calls check
    // to decide whether they are drawing the arena or measuring it.
    bool m_shadowPass = false;
    DirectX::XMMATRIX m_lightViewProj = DirectX::XMMatrixIdentity();

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
    std::vector<Vertex> m_screenTris;
    // Metrics of the baked 'X' glyph, so `size` maps to capital height.
    float m_fontCapHeight = 1.0f;
    float m_fontCapOffsetY = 0.0f;

    // Frame instrumentation (see CpuFrameMs / LastDrawCalls).
    LARGE_INTEGER m_qpcFreq = {};
    LARGE_INTEGER m_frameStart = {};
    float m_cpuFrameMs = 0.0f;
    uint32_t m_drawCalls = 0;
    uint32_t m_lastDrawCalls = 0;

    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;
    uint32_t m_frameIndex = 0;
    uint32_t m_rtvSize = 0;

    D3D12_VIEWPORT m_viewport = {};
    D3D12_RECT m_scissor = {};
    DirectX::XMMATRIX m_viewProj = DirectX::XMMatrixIdentity();
};
