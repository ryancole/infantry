#include "Renderer.h"

#include <DirectXHelpers.h>
#include <EffectPipelineStateDescription.h>
#include <RenderTargetState.h>
#include <ResourceUploadBatch.h>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    // Depth with a stencil byte beside it: the fog of war marks the ground its
    // side can see there (Renderer::MarkSeen). 24 bits of depth rather than 32
    // costs nothing here — the camera is orthographic, so depth is linear and
    // an arena's worth of it is precise to well under a millimeter.
    constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    // The value MarkSeen stamps and DrawTrianglesUnseen tests against. Set on
    // the command list once a frame; nothing else in the scene uses stencil.
    constexpr UINT kStencilSeen = 1;

    // Post-process tuning. The extract threshold is high enough that only
    // genuinely bright pixels (projectiles, aim line, vivid class colors)
    // bloom, not the whole scene.
    constexpr float kPerfSmoothWeight = 0.05f; // HUD timing smoothing, ~20 frames

    constexpr float kBloomThreshold = 0.7f;
    constexpr float kBloomBlurSize = 3.0f;
    constexpr float kBloomIntensity = 1.2f;

    void HR(HRESULT hr, const char* what)
    {
        if (FAILED(hr))
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s failed (hr=0x%08X)", what, static_cast<unsigned>(hr));
            throw std::runtime_error(buf);
        }
    }

    std::string ExeDir()
    {
        char buf[MAX_PATH];
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        std::string path(buf);
        return path.substr(0, path.find_last_of("\\/"));
    }
}

void Renderer::Init(HWND hwnd, uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;
    QueryPerformanceFrequency(&m_qpcFreq);

#ifdef _DEBUG
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
    }
#endif

    UINT factoryFlags = 0;
#ifdef _DEBUG
    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    ComPtr<IDXGIFactory6> factory;
    HR(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    HR(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)), "D3D12CreateDevice");

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HR(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue)), "CreateCommandQueue");

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.BufferCount = kFrameCount;
    scDesc.Width = width;
    scDesc.Height = height;
    scDesc.Format = kBackBufferFormat;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain1;
    HR(factory->CreateSwapChainForHwnd(m_queue.Get(), hwnd, &scDesc, nullptr, nullptr, &swapChain1),
       "CreateSwapChainForHwnd");
    HR(swapChain1.As(&m_swapChain), "IDXGISwapChain3 query");
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = kFrameCount + 4; // + scene, post, bloom1, bloom2
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    HR(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)), "Create RTV heap");
    m_rtvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    HR(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)), "Create DSV heap");

    CreateSizedResources();

    HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAlloc)),
       "CreateCommandAllocator");
    HR(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmdAlloc.Get(), nullptr,
                                   IID_PPV_ARGS(&m_cmdList)),
       "CreateCommandList");
    m_cmdList->Close();

    HR(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence");
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
        throw std::runtime_error("CreateEvent failed");

    m_graphicsMemory = std::make_unique<GraphicsMemory>(m_device.Get());
    m_states = std::make_unique<CommonStates>(m_device.Get());
    m_srvPile = std::make_unique<DescriptorPile>(m_device.Get(),
                                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                 D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                                                 kSrvHeapSize);
    m_batch = std::make_unique<PrimitiveBatch<Vertex>>(m_device.Get(), kBatchVertices * 3,
                                                       kBatchVertices);
    CreateFlatNormalTexture();
    CreateEffects();
    CreateSpriteResources();
    CreateShapePrimitives();
    CreatePostProcess();
    CreateOffscreenTargets();
}

void Renderer::Shutdown()
{
    if (!m_device)
        return;
    WaitForGpu();
    m_font.reset();
    m_spriteBatch.reset();
    m_bloomExtract.reset();
    m_bloomBlur.reset();
    m_monochromePass.reset();
    m_bloomCombine.reset();
    m_sceneColor.resource.Reset();
    m_postColor.resource.Reset();
    m_bloom1.resource.Reset();
    m_bloom2.resource.Reset();
    for (auto& shape : m_shapes)
        shape.reset();
    m_batch.reset();
    m_triEffect.reset();
    m_lineEffect.reset();
    m_alphaEffect.reset();
    m_alphaLineEffect.reset();
    m_seenEffect.reset();
    m_unseenEffect.reset();
    m_seenTriEffect.reset();
    m_modelEffect.reset();
    m_texModelEffect.reset();
    m_screenTriEffect.reset();
    m_flatNormalTex.Reset();
    m_srvPile.reset();
    m_states.reset();
    m_graphicsMemory.reset();
    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

void Renderer::Resize(uint32_t width, uint32_t height)
{
    if (!m_device || width == 0 || height == 0 || (width == m_width && height == m_height))
        return;

    WaitForGpu();
    for (auto& buffer : m_backBuffers)
        buffer.Reset();
    m_depth.Reset();

    m_width = width;
    m_height = height;
    HR(m_swapChain->ResizeBuffers(kFrameCount, width, height, kBackBufferFormat, 0),
       "ResizeBuffers");
    CreateSizedResources();
    CreateOffscreenTargets();
    if (m_spriteBatch)
        m_spriteBatch->SetViewport(m_viewport);
}

void Renderer::CreateSizedResources()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        HR(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])), "SwapChain GetBuffer");
        m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtv);
        rtv.ptr += m_rtvSize;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = m_width;
    depthDesc.Height = m_height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = kDepthFormat;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = kDepthFormat;
    clearValue.DepthStencil.Depth = 1.0f;

    HR(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
                                         D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
                                         IID_PPV_ARGS(&m_depth)),
       "Create depth buffer");
    m_device->CreateDepthStencilView(m_depth.Get(), nullptr,
                                     m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    m_viewport = { 0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
    m_scissor = { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
}

void Renderer::CreateEffects()
{
    const RenderTargetState rtState(kBackBufferFormat, kDepthFormat);

    const EffectPipelineStateDescription triDesc(
        &Vertex::InputLayout, CommonStates::Opaque, CommonStates::DepthDefault,
        CommonStates::CullNone, rtState, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    m_triEffect = std::make_unique<BasicEffect>(m_device.Get(), EffectFlags::VertexColor, triDesc);

    const EffectPipelineStateDescription lineDesc(
        &Vertex::InputLayout, CommonStates::Opaque, CommonStates::DepthDefault,
        CommonStates::CullNone, rtState, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
    m_lineEffect = std::make_unique<BasicEffect>(m_device.Get(), EffectFlags::VertexColor, lineDesc);

    const EffectPipelineStateDescription alphaDesc(
        &Vertex::InputLayout, CommonStates::AlphaBlend, CommonStates::DepthRead,
        CommonStates::CullNone, rtState, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    m_alphaEffect = std::make_unique<BasicEffect>(m_device.Get(), EffectFlags::VertexColor,
                                                  alphaDesc);

    const EffectPipelineStateDescription alphaLineDesc(
        &Vertex::InputLayout, CommonStates::AlphaBlend, CommonStates::DepthRead,
        CommonStates::CullNone, rtState, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
    m_alphaLineEffect = std::make_unique<BasicEffect>(m_device.Get(), EffectFlags::VertexColor,
                                                      alphaLineDesc);

    // The fog's mask pass: alpha states for the depth handling, a write mask
    // of nothing so it paints no pixels, and a stencil op that stamps the
    // reference value wherever it draws.
    D3D12_BLEND_DESC noColor = CommonStates::Opaque;
    noColor.RenderTarget[0].RenderTargetWriteMask = 0;

    D3D12_DEPTH_STENCIL_DESC markStencil = CommonStates::DepthRead;
    markStencil.StencilEnable = TRUE;
    markStencil.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    markStencil.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    markStencil.FrontFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                              D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS };
    markStencil.BackFace = markStencil.FrontFace;

    const EffectPipelineStateDescription seenDesc(
        &Vertex::InputLayout, noColor, markStencil,
        CommonStates::CullNone, rtState, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    m_seenEffect = std::make_unique<BasicEffect>(m_device.Get(), EffectFlags::VertexColor,
                                                 seenDesc);

    // And the pass that reads it: the same blend the other overlays use, drawn
    // only where the mark isn't.
    D3D12_DEPTH_STENCIL_DESC unseenStencil = markStencil;
    unseenStencil.FrontFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_NOT_EQUAL };
    unseenStencil.BackFace = unseenStencil.FrontFace;

    const EffectPipelineStateDescription unseenDesc(
        &Vertex::InputLayout, CommonStates::AlphaBlend, unseenStencil,
        CommonStates::CullNone, rtState, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    m_unseenEffect = std::make_unique<BasicEffect>(m_device.Get(), EffectFlags::VertexColor,
                                                   unseenDesc);

    // And the pass that reads it the other way round (see DrawTrianglesSeen):
    // solid geometry, drawn where the mark is and nowhere else. Depth is
    // tested and not written, because everything here is decoration standing
    // on ground that has already had its say about what's in front of what.
    D3D12_DEPTH_STENCIL_DESC seenTriStencil = markStencil;
    seenTriStencil.FrontFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                 D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_EQUAL };
    seenTriStencil.BackFace = seenTriStencil.FrontFace;

    const EffectPipelineStateDescription seenTriDesc(
        &Vertex::InputLayout, CommonStates::Opaque, seenTriStencil,
        CommonStates::CullNone, rtState, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    m_seenTriEffect = std::make_unique<BasicEffect>(m_device.Get(), EffectFlags::VertexColor,
                                                    seenTriDesc);

    const EffectPipelineStateDescription modelDesc(
        &VertexPositionNormalTexture::InputLayout, CommonStates::Opaque, CommonStates::DepthDefault,
        CommonStates::CullNone, rtState, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    m_modelEffect = std::make_unique<BasicEffect>(m_device.Get(), EffectFlags::Lighting, modelDesc);
    m_modelEffect->EnableDefaultLighting();
    m_modelEffect->DisableSpecular();

    m_texModelEffect = std::make_unique<NormalMapEffect>(m_device.Get(), EffectFlags::None,
                                                         modelDesc);
    m_texModelEffect->EnableDefaultLighting();
    m_texModelEffect->DisableSpecular();

    // Overlay geometry draws on the resolved backbuffer, where no depth buffer
    // is bound — hence a render target state without one, and DepthNone.
    // NonPremultiplied rather than the scene's premultiplied blend: the HUD
    // writes plain colors with an alpha it expects to be honored.
    const RenderTargetState screenRtState(kBackBufferFormat, DXGI_FORMAT_UNKNOWN);

    const EffectPipelineStateDescription screenTriDesc(
        &Vertex::InputLayout, CommonStates::NonPremultiplied, CommonStates::DepthNone,
        CommonStates::CullNone, screenRtState, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    m_screenTriEffect = std::make_unique<BasicEffect>(m_device.Get(), EffectFlags::VertexColor,
                                                      screenTriDesc);

}

// NormalMapEffect always samples a normal map, so parts without one get this
// single flat texel (x=y=0.5 → straight-up tangent-space normal).
void Renderer::CreateFlatNormalTexture()
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    HR(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&m_flatNormalTex)),
       "Create flat normal texture");

    static const uint8_t texel[4] = { 128, 128, 255, 255 };
    D3D12_SUBRESOURCE_DATA sub = {};
    sub.pData = texel;
    sub.RowPitch = sizeof(texel);
    sub.SlicePitch = sizeof(texel);

    ResourceUploadBatch upload(m_device.Get());
    upload.Begin();
    upload.Upload(m_flatNormalTex.Get(), 0, &sub, 1);
    upload.Transition(m_flatNormalTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    upload.End(m_queue.Get()).wait();

    const size_t slot = m_srvPile->Allocate();
    CreateShaderResourceView(m_device.Get(), m_flatNormalTex.Get(), m_srvPile->GetCpuHandle(slot));
    m_flatNormalSrv = m_srvPile->GetGpuHandle(slot);
}

void Renderer::CreateSpriteResources()
{
    ResourceUploadBatch upload(m_device.Get());
    upload.Begin();

    const RenderTargetState rtState(kBackBufferFormat, kDepthFormat);
    const SpriteBatchPipelineStateDescription spriteDesc(rtState);
    m_spriteBatch = std::make_unique<SpriteBatch>(m_device.Get(), upload, spriteDesc);

    // Same exe-dir-then-repo-root resolution as LoadModel.
    std::string path = ExeDir() + "\\assets\\fonts\\hud.spritefont";
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        path = "assets/fonts/hud.spritefont";
    const std::wstring wpath(path.begin(), path.end());

    const size_t slot = m_srvPile->Allocate();
    m_font = std::make_unique<SpriteFont>(m_device.Get(), upload, wpath.c_str(),
                                          m_srvPile->GetCpuHandle(slot),
                                          m_srvPile->GetGpuHandle(slot));
    upload.End(m_queue.Get()).wait();

    m_spriteBatch->SetViewport(m_viewport);

    const SpriteFont::Glyph* cap = m_font->FindGlyph(L'X');
    m_fontCapHeight = static_cast<float>(cap->Subrect.bottom - cap->Subrect.top);
    m_fontCapOffsetY = cap->YOffset;
}

// Unit primitives with GPU-resident buffers; DrawShape scales them into place.
// Sphere tessellation halves per tier, for 1056 / 272 / 72 triangles; the
// cylinder tiers are 94 and 30. See the Shape enum for how to pick one.
void Renderer::CreateShapePrimitives()
{
    // rhcoords = false: our world is left-handed.
    m_shapes[static_cast<size_t>(Shape::Box)] =
        GeometricPrimitive::CreateCube(1.0f, false, m_device.Get());
    m_shapes[static_cast<size_t>(Shape::Sphere)] =
        GeometricPrimitive::CreateSphere(1.0f, 16, false, false, m_device.Get());
    m_shapes[static_cast<size_t>(Shape::SphereMed)] =
        GeometricPrimitive::CreateSphere(1.0f, 8, false, false, m_device.Get());
    m_shapes[static_cast<size_t>(Shape::SphereLow)] =
        GeometricPrimitive::CreateSphere(1.0f, 4, false, false, m_device.Get());
    m_shapes[static_cast<size_t>(Shape::Cylinder)] =
        GeometricPrimitive::CreateCylinder(1.0f, 1.0f, 24, false, m_device.Get());
    m_shapes[static_cast<size_t>(Shape::CylinderLow)] =
        GeometricPrimitive::CreateCylinder(1.0f, 1.0f, 8, false, m_device.Get());
    m_shapes[static_cast<size_t>(Shape::Cone)] =
        GeometricPrimitive::CreateCone(1.0f, 1.0f, 24, false, m_device.Get());

    ResourceUploadBatch upload(m_device.Get());
    upload.Begin();
    for (auto& shape : m_shapes)
        shape->LoadStaticBuffers(m_device.Get(), upload);
    upload.End(m_queue.Get()).wait();
}

void Renderer::DrawShape(Shape shape, const XMMATRIX& world, const XMFLOAT4& color)
{
    m_modelEffect->SetMatrices(world, XMMatrixIdentity(), m_viewProj);
    m_modelEffect->SetDiffuseColor(XMLoadFloat4(&color));
    m_modelEffect->Apply(m_cmdList.Get());
    m_shapes[static_cast<size_t>(shape)]->Draw(m_cmdList.Get());
    ++m_drawCalls;
}

// The camera is orthographic, so a sphere's projected size doesn't vary with
// depth: transform the center to NDC and widen the [-1, 1] bounds by the
// radius. Columns 0 and 1 of the combined matrix map a world offset into clip
// x and y, so their lengths give the largest extent the radius can cover.
// Depth isn't tested — the iso camera's near/far bracket the whole arena.
bool Renderer::IsSphereVisible(const XMFLOAT3& center, float radius) const
{
    XMFLOAT4 ndc;
    XMStoreFloat4(&ndc, XMVector3Transform(XMLoadFloat3(&center), m_viewProj));

    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, m_viewProj);
    const float rx = radius * std::sqrt(m._11 * m._11 + m._21 * m._21 + m._31 * m._31);
    const float ry = radius * std::sqrt(m._12 * m._12 + m._22 * m._22 + m._32 * m._32);

    return std::abs(ndc.x) <= 1.0f + rx && std::abs(ndc.y) <= 1.0f + ry;
}

// Clip space, then the viewport transform: x from [-1, 1] across the width and
// y from [1, -1] down the height, because NDC counts up and pixels count down.
// The divide by w is a divide by one under this camera, and is done anyway so
// the answer doesn't quietly become wrong the day the projection stops being
// orthographic.
bool Renderer::WorldToScreen(const XMFLOAT3& world, XMFLOAT2& out) const
{
    XMFLOAT4 clip;
    XMStoreFloat4(&clip, XMVector3Transform(XMLoadFloat3(&world), m_viewProj));
    if (clip.w <= 0.0f)
        return false;

    const float ndcX = clip.x / clip.w;
    const float ndcY = clip.y / clip.w;
    if (std::abs(ndcX) > 1.0f || std::abs(ndcY) > 1.0f)
        return false;

    out.x = (ndcX * 0.5f + 0.5f) * static_cast<float>(m_width);
    out.y = (0.5f - ndcY * 0.5f) * static_cast<float>(m_height);
    return true;
}

void Renderer::CreatePostProcess()
{
    // Post passes draw a fullscreen triangle with no depth buffer bound.
    const RenderTargetState postRtState(kBackBufferFormat, DXGI_FORMAT_UNKNOWN);
    m_bloomExtract = std::make_unique<BasicPostProcess>(m_device.Get(), postRtState,
                                                        BasicPostProcess::BloomExtract);
    m_bloomBlur = std::make_unique<BasicPostProcess>(m_device.Get(), postRtState,
                                                     BasicPostProcess::BloomBlur);
    m_monochromePass = std::make_unique<BasicPostProcess>(m_device.Get(), postRtState,
                                                          BasicPostProcess::Monochrome);
    m_bloomCombine = std::make_unique<DualPostProcess>(m_device.Get(), postRtState,
                                                       DualPostProcess::BloomCombine);
}

// (Re)creates the window-sized post targets. SRV pile slots are allocated on
// first use and then reused, so resizing doesn't leak descriptors.
void Renderer::CreateOffscreenTargets()
{
    PostTarget* targets[] = { &m_sceneColor, &m_postColor, &m_bloom1, &m_bloom2 };

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    // The optimized clear color only matters for the scene target; a
    // mismatch with BeginFrame's clear is legal, just slower.
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = kBackBufferFormat;
    clearValue.Color[0] = 0.030f;
    clearValue.Color[1] = 0.038f;
    clearValue.Color[2] = 0.055f;
    clearValue.Color[3] = 1.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kFrameCount) * m_rtvSize;

    for (size_t i = 0; i < std::size(targets); ++i)
    {
        PostTarget& target = *targets[i];

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = std::max<uint32_t>((i >= 2) ? m_width / 2 : m_width, 1u);
        desc.Height = std::max<uint32_t>((i >= 2) ? m_height / 2 : m_height, 1u);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kBackBufferFormat;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        target.resource.Reset();
        HR(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                             &clearValue, IID_PPV_ARGS(&target.resource)),
           "Create post target");

        m_device->CreateRenderTargetView(target.resource.Get(), nullptr, rtv);
        rtv.ptr += m_rtvSize;

        if (target.srvSlot == SIZE_MAX)
            target.srvSlot = m_srvPile->Allocate();
        CreateShaderResourceView(m_device.Get(), target.resource.Get(),
                                 m_srvPile->GetCpuHandle(target.srvSlot));
        target.srv = m_srvPile->GetGpuHandle(target.srvSlot);
    }
}

// Resolves the offscreen scene to the backbuffer: optional monochrome, then
// bloom extract -> separable blur (half res) -> combine.
void Renderer::RunPostChain()
{
    const auto rtvAt = [&](uint32_t index) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(index) * m_rtvSize;
        return rtv;
    };
    const auto setTarget = [&](uint32_t rtvIndex, uint32_t width, uint32_t height) {
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvAt(rtvIndex);
        m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(width),
                                    static_cast<float>(height), 0.0f, 1.0f };
        const D3D12_RECT sc = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        m_cmdList->RSSetViewports(1, &vp);
        m_cmdList->RSSetScissorRects(1, &sc);
    };

    Transition(m_sceneColor.resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    const PostTarget* base = &m_sceneColor;
    if (m_monochrome)
    {
        Transition(m_postColor.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        setTarget(kFrameCount + 1, m_width, m_height);
        m_monochromePass->SetSourceTexture(m_sceneColor.srv, m_sceneColor.resource.Get());
        m_monochromePass->Process(m_cmdList.Get());
        Transition(m_postColor.resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        base = &m_postColor;
    }

    const uint32_t halfW = std::max(m_width / 2, 1u);
    const uint32_t halfH = std::max(m_height / 2, 1u);

    Transition(m_bloom1.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    setTarget(kFrameCount + 2, halfW, halfH);
    m_bloomExtract->SetBloomExtractParameter(kBloomThreshold);
    m_bloomExtract->SetSourceTexture(base->srv, base->resource.Get());
    m_bloomExtract->Process(m_cmdList.Get());
    Transition(m_bloom1.resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    Transition(m_bloom2.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    setTarget(kFrameCount + 3, halfW, halfH);
    m_bloomBlur->SetBloomBlurParameters(true, kBloomBlurSize, 1.0f);
    m_bloomBlur->SetSourceTexture(m_bloom1.srv, m_bloom1.resource.Get());
    m_bloomBlur->Process(m_cmdList.Get());
    Transition(m_bloom2.resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    Transition(m_bloom1.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    setTarget(kFrameCount + 2, halfW, halfH);
    m_bloomBlur->SetBloomBlurParameters(false, kBloomBlurSize, 1.0f);
    m_bloomBlur->SetSourceTexture(m_bloom2.srv, m_bloom2.resource.Get());
    m_bloomBlur->Process(m_cmdList.Get());
    Transition(m_bloom1.resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    setTarget(m_frameIndex, m_width, m_height);
    m_bloomCombine->SetBloomCombineParameters(kBloomIntensity, 1.0f, 1.0f, 1.0f);
    m_bloomCombine->SetSourceTexture(m_bloom1.srv);
    m_bloomCombine->SetSourceTexture2(base->srv);
    m_bloomCombine->Process(m_cmdList.Get());
}

void Renderer::DrawScreenText(std::string_view text, float x, float y, float size,
                              const XMFLOAT4& color)
{
    const float scale = size / m_fontCapHeight;
    // Shift so the capital's top lands on y (DrawString positions the top of
    // the full line cell, which sits above the cap by the glyph Y offset).
    m_textDraws.push_back({ std::string(text), x, y - m_fontCapOffsetY * scale, scale, color });
}

void Renderer::DrawScreenTextOutlined(std::string_view text, float x, float y, float size,
                                      const XMFLOAT4& color)
{
    // Ring first, fill last: the sprite batch is in its default deferred mode,
    // so what is queued later is drawn over what came before.
    constexpr float kEdgeShare = 0.09f; // of the cap height
    constexpr XMFLOAT4 kEdgeColor = { 0.0f, 0.0f, 0.0f, 0.85f };
    static constexpr float kRing[8][2] = {
        { -1.0f, 0.0f }, { 1.0f, 0.0f },  { 0.0f, -1.0f }, { 0.0f, 1.0f },
        { -1.0f, -1.0f }, { 1.0f, -1.0f }, { -1.0f, 1.0f }, { 1.0f, 1.0f },
    };

    const float edge = std::max(1.0f, size * kEdgeShare);
    for (const float(&offset)[2] : kRing)
        DrawScreenText(text, x + offset[0] * edge, y + offset[1] * edge, size, kEdgeColor);
    DrawScreenText(text, x, y, size, color);
}

float Renderer::MeasureScreenText(std::string_view text, float size) const
{
    const std::string str(text);
    return XMVectorGetX(m_font->MeasureString(str.c_str())) * (size / m_fontCapHeight);
}

void Renderer::DrawScreenTriangles(const Vertex* verts, uint32_t count)
{
    m_screenTris.insert(m_screenTris.end(), verts, verts + count);
}

// Flushes everything queued by DrawScreenTriangles onto the backbuffer. Called
// from EndFrame once the post chain has resolved there.
void Renderer::DrawScreenGeometry()
{
    if (m_screenTris.empty())
        return;

    // Pixel space with the origin at the top-left corner and y running down —
    // the coordinates the overlay is authored in, and the same convention the
    // text uses.
    const XMMATRIX proj =
        XMMatrixOrthographicOffCenterLH(0.0f, static_cast<float>(m_width),
                                        static_cast<float>(m_height), 0.0f, 0.0f, 1.0f);

    if (m_screenTris.size() <= kBatchVertices)
    {
        m_screenTriEffect->SetMatrices(XMMatrixIdentity(), XMMatrixIdentity(), proj);
        m_screenTriEffect->Apply(m_cmdList.Get());
        m_batch->Begin(m_cmdList.Get());
        m_batch->Draw(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, m_screenTris.data(),
                      static_cast<uint32_t>(m_screenTris.size()));
        m_batch->End();
        ++m_drawCalls;
    }
    m_screenTris.clear();
}

void Renderer::BeginFrame(const float clearColor[4])
{
    QueryPerformanceCounter(&m_frameStart);
    m_lastDrawCalls = m_drawCalls; // hold the completed count for the HUD
    m_drawCalls = 0;

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    HR(m_cmdAlloc->Reset(), "Command allocator Reset");
    HR(m_cmdList->Reset(m_cmdAlloc.Get(), nullptr), "Command list Reset");

    Transition(m_backBuffers[m_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(m_sceneColor.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);

    // The scene draws into the offscreen color target; EndFrame's post chain
    // resolves it to the backbuffer.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kFrameCount) * m_rtvSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                     1.0f, 0, 0, nullptr);
    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissor);
    // The fog's two passes stamp and test this; every other pass ignores the
    // stencil entirely, so it's set once here and never touched again.
    m_cmdList->OMSetStencilRef(kStencilSeen);

    ID3D12DescriptorHeap* heaps[] = { m_srvPile->Heap(), m_states->Heap() };
    m_cmdList->SetDescriptorHeaps(static_cast<UINT>(std::size(heaps)), heaps);
}

void Renderer::EndFrame()
{
    RunPostChain();

    DrawScreenGeometry();

    if (!m_textDraws.empty())
    {
        m_spriteBatch->Begin(m_cmdList.Get());
        for (const TextDraw& t : m_textDraws)
            m_font->DrawString(m_spriteBatch.get(), t.text.c_str(), XMFLOAT2(t.x, t.y),
                               XMLoadFloat4(&t.color), 0.0f, XMFLOAT2(0.0f, 0.0f), t.scale);
        m_spriteBatch->End();
        m_textDraws.clear();
    }

    Transition(m_backBuffers[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PRESENT);
    HR(m_cmdList->Close(), "Command list Close");

    // Sampled here, before the queue submit: everything after this point is
    // GPU execution and the wait for vblank, neither of which is CPU cost.
    // Smoothed with a fixed weight — a raw per-frame number is unreadable.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float raw = 1000.0f * static_cast<float>(now.QuadPart - m_frameStart.QuadPart) /
                      static_cast<float>(m_qpcFreq.QuadPart);
    m_cpuFrameMs += (raw - m_cpuFrameMs) * kPerfSmoothWeight;

    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    HR(m_swapChain->Present(1, 0), "Present");
    m_graphicsMemory->Commit(m_queue.Get());
    WaitForGpu();
}

void Renderer::DrawTriangles(const Vertex* verts, uint32_t count, const XMMATRIX& world)
{
    DrawBatch(verts, count, world, m_triEffect.get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::DrawLines(const Vertex* verts, uint32_t count, const XMMATRIX& world)
{
    DrawBatch(verts, count, world, m_lineEffect.get(), D3D_PRIMITIVE_TOPOLOGY_LINELIST);
}

void Renderer::DrawTrianglesAlpha(const Vertex* verts, uint32_t count, const XMMATRIX& world)
{
    DrawBatch(verts, count, world, m_alphaEffect.get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::DrawLinesAlpha(const Vertex* verts, uint32_t count, const XMMATRIX& world)
{
    DrawBatch(verts, count, world, m_alphaLineEffect.get(), D3D_PRIMITIVE_TOPOLOGY_LINELIST);
}

void Renderer::MarkSeen(const Vertex* verts, uint32_t count, const XMMATRIX& world)
{
    DrawBatch(verts, count, world, m_seenEffect.get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::DrawTrianglesUnseen(const Vertex* verts, uint32_t count, const XMMATRIX& world)
{
    DrawBatch(verts, count, world, m_unseenEffect.get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::DrawTrianglesSeen(const Vertex* verts, uint32_t count, const XMMATRIX& world)
{
    DrawBatch(verts, count, world, m_seenTriEffect.get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::DrawBatch(const Vertex* verts, uint32_t count, const XMMATRIX& world,
                         BasicEffect* effect, D3D_PRIMITIVE_TOPOLOGY topology)
{
    if (count == 0 || count > kBatchVertices)
        return;

    effect->SetMatrices(world, XMMatrixIdentity(), m_viewProj);
    effect->Apply(m_cmdList.Get());
    m_batch->Begin(m_cmdList.Get());
    m_batch->Draw(topology, verts, count);
    m_batch->End();
    ++m_drawCalls;
}

std::unique_ptr<Model> Renderer::LoadModel(const std::string& path)
{
    // Look next to the exe first (post-build copy), then fall back to the
    // path as given for runs launched from the repo root.
    std::string resolved = ExeDir() + "\\" + path;
    if (GetFileAttributesA(resolved.c_str()) == INVALID_FILE_ATTRIBUTES)
        resolved = path;
    return Model::LoadFromFile(m_device.Get(), m_queue.Get(), *m_srvPile, resolved);
}

void Renderer::DrawModel(const Model& model, const XMMATRIX& world)
{
    m_cmdList->IASetVertexBuffers(0, 1, &model.VertexView());
    m_cmdList->IASetIndexBuffer(&model.IndexView());
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (const Model::Part& part : model.Parts())
    {
        if (part.baseColorTex >= 0)
        {
            m_texModelEffect->SetMatrices(world, XMMatrixIdentity(), m_viewProj);
            m_texModelEffect->SetDiffuseColor(XMLoadFloat4(&part.color));
            m_texModelEffect->SetTexture(model.Textures()[part.baseColorTex].srv,
                                         m_states->AnisotropicWrap());
            m_texModelEffect->SetNormalTexture(part.normalTex >= 0
                                                   ? model.Textures()[part.normalTex].srv
                                                   : m_flatNormalSrv);
            m_texModelEffect->Apply(m_cmdList.Get());
        }
        else
        {
            m_modelEffect->SetMatrices(world, XMMatrixIdentity(), m_viewProj);
            m_modelEffect->SetDiffuseColor(XMLoadFloat4(&part.color));
            m_modelEffect->Apply(m_cmdList.Get());
        }
        m_cmdList->DrawIndexedInstanced(part.indexCount, 1, part.indexStart, 0, 0);
        ++m_drawCalls;
    }
}

void Renderer::WaitForGpu()
{
    const uint64_t value = ++m_fenceValue;
    HR(m_queue->Signal(m_fence.Get(), value), "Queue Signal");
    if (m_fence->GetCompletedValue() < value)
    {
        HR(m_fence->SetEventOnCompletion(value, m_fenceEvent), "SetEventOnCompletion");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void Renderer::Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = res;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmdList->ResourceBarrier(1, &barrier);
}
