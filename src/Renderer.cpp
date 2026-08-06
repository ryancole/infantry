#include "Renderer.h"

#include <DirectXHelpers.h>
#include <EffectPipelineStateDescription.h>
#include <RenderTargetState.h>
#include <ResourceUploadBatch.h>

#include <ShadowApplyPS.h>
#include <ShadowDepthVS.h>
#include <FullscreenVS.h>

#include <algorithm>
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
    //
    // Three formats for one resource, because the buffer is written as depth
    // and then read as a texture by ApplyShadows, and D3D12 will only let a
    // resource be both if it is declared as neither: typeless underneath, with
    // the depth view and the shader view each naming the interpretation they
    // want. The shader's drops the stencil byte — R24_UNORM_X8 is the depth
    // half of the same 32 bits — and the stencil the fog wrote is untouched by
    // being looked past.
    constexpr DXGI_FORMAT kDepthResourceFormat = DXGI_FORMAT_R24G8_TYPELESS;
    constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    constexpr DXGI_FORMAT kDepthSrvFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
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

    // The sun. One direction, read by two things that would look wrong the
    // moment they disagreed: the lit effects below shade against it, and the
    // shadow pass casts along it. Which is the whole reason it is a constant
    // here rather than left at the toolkit's default — EnableDefaultLighting
    // sets three lights of its own, and a shadow thrown from a fourth direction
    // nobody was shading against is a shadow the eye reads as a stain.
    //
    // It is not the toolkit's own key light, and that was a decision the
    // shadows forced rather than a taste for a different look. The toolkit
    // points its key at (-0.5265408, -0.5735765, -0.6275069), which is 35° of
    // elevation aimed the same way the camera's default yaw looks — so every
    // shadow in the arena fell directly away from the eye and hid behind the
    // thing that cast it. A soldier's own shadow was a black sliver at their
    // shoulder. Correct, and invisible.
    //
    // This one is 40° up with its heading swung round to put shadows across
    // the view and slightly toward it, which is where an isometric game wants
    // them: a soldier stands on their own shadow instead of behind it. The
    // camera orbits and the sun doesn't, so this is the reading at the yaw the
    // game opens on rather than a promise about all four quarters of the turn.
    //
    // Changing it is this line alone — the effects below are told the same
    // number — but it is a change to how everything in the arena is shaded,
    // not only to where the shadows land.
    constexpr XMFLOAT3 kSunDirection = { -0.242f, -0.643f, 0.727f };

    // 2048 square, covering a fitted patch of ground rather than the arena:
    // hardcorps2t is 197 by 106 and the camera shows 26 units of it, so a map
    // sized to the level would spend 99% of itself on ground nobody is looking
    // at. Fitted to the view instead, a texel is about two centimeters.
    constexpr uint32_t kShadowMapSize = 2048;
    constexpr DXGI_FORMAT kShadowResourceFormat = DXGI_FORMAT_R32_TYPELESS;
    constexpr DXGI_FORMAT kShadowDsvFormat = DXGI_FORMAT_D32_FLOAT;
    constexpr DXGI_FORMAT kShadowSrvFormat = DXGI_FORMAT_R32_FLOAT;

    // How tall the fit assumes the arena is. The map covers the visible floor
    // extruded up by this much, so anything standing taller than it leans out
    // of the map and stops casting — mountains do, which is fine, since a
    // mountain's shadow is a fact about the horizon rather than about the fight.
    constexpr float kCasterHeight = 14.0f;
    // And how far up-sun of that box the near plane is pulled back, so a tree
    // standing just off screen still throws its shadow onto ground that isn't.
    constexpr float kSunBackoff = 40.0f;

    // What a shadowed pixel keeps. Not zero and not close to it: this is one
    // key light among the three the toolkit lights with, and the two fills go
    // on reaching what it doesn't. Picked to read as shade rather than as a
    // hole in the floor.
    constexpr float kShadowStrength = 0.62f;
    // Slack in the depth compare, in the sun's clip units, against a surface
    // shadowing itself out of its own depth quantization. Paired with the
    // slope-scaled bias on the depth pass's rasterizer, which is what handles
    // the ground the sun is raking across at a low angle.
    constexpr float kShadowDepthBias = 0.0016f;

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
    dsvHeapDesc.NumDescriptors = 2; // scene depth, then the sun's
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    HR(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)), "Create DSV heap");

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
    // After the descriptor pile rather than before it, which is the order the
    // depth buffer now needs: it is read by a shader as well as written as
    // depth, so it wants a slot on the pile to be seen through.
    CreateSizedResources();
    CreateFlatNormalTexture();
    CreateEffects();
    CreateSpriteResources();
    CreateShapePrimitives();
    CreatePostProcess();
    CreateOffscreenTargets();
    CreateShadowResources();
    CreateShadowPipelines();
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
    m_shadowMap.Reset();
    m_shadowDepthPso.Reset();
    m_shadowDepthRS.Reset();
    m_shadowApplyPso.Reset();
    m_shadowApplyRS.Reset();
    for (auto& shape : m_shapes)
        shape.reset();
    m_batch.reset();
    m_triEffect.reset();
    m_lineEffect.reset();
    m_alphaEffect.reset();
    m_alphaLineEffect.reset();
    m_seenEffect.reset();
    m_unseenEffect.reset();
    m_fogEdgeEffect.reset();
    m_detailSeenEffect.reset();
    m_detailUnseenEffect.reset();
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
    depthDesc.Format = kDepthResourceFormat;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = kDepthFormat;
    clearValue.DepthStencil.Depth = 1.0f;

    HR(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
                                         D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
                                         IID_PPV_ARGS(&m_depth)),
       "Create depth buffer");

    // Both views spelled out, because the resource itself no longer says what
    // it holds: a typeless buffer has no default view to fall back on.
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = kDepthFormat;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_device->CreateDepthStencilView(m_depth.Get(), &dsvDesc,
                                     m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = kDepthSrvFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    if (m_depthSrvSlot == SIZE_MAX)
        m_depthSrvSlot = m_srvPile->Allocate();
    m_device->CreateShaderResourceView(m_depth.Get(), &srvDesc,
                                       m_srvPile->GetCpuHandle(m_depthSrvSlot));
    m_depthSrv = m_srvPile->GetGpuHandle(m_depthSrvSlot);

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

    // The fade along the edge of that darkness (DrawFogEdge): the same blend
    // as the sheet, drawn where the mark is, and spending it — the pass op
    // wipes the mark so no second band can paint the same pixel. Only on a
    // pixel it actually reaches: a depth fail keeps the mark, or a band
    // crossing behind a wall would consume ground the wall is standing in
    // front of and the band behind it would come out with a hole in it.
    D3D12_DEPTH_STENCIL_DESC fogEdgeStencil = markStencil;
    fogEdgeStencil.FrontFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                 D3D12_STENCIL_OP_ZERO, D3D12_COMPARISON_FUNC_EQUAL };
    fogEdgeStencil.BackFace = fogEdgeStencil.FrontFace;

    const EffectPipelineStateDescription fogEdgeDesc(
        &Vertex::InputLayout, CommonStates::AlphaBlend, fogEdgeStencil,
        CommonStates::CullNone, rtState, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    m_fogEdgeEffect = std::make_unique<BasicEffect>(m_device.Get(), EffectFlags::VertexColor,
                                                    fogEdgeDesc);

    // And the pair that reads the mark for solid geometry (DrawGroundDetail):
    // the same state twice, sorted by the mark into the half that is drawn
    // whole and the half that is drawn dimmed. Depth is tested and not
    // written, because everything here is decoration standing on ground that
    // has already had its say about what's in front of what.
    D3D12_DEPTH_STENCIL_DESC detailSeenStencil = markStencil;
    detailSeenStencil.FrontFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                    D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_EQUAL };
    detailSeenStencil.BackFace = detailSeenStencil.FrontFace;

    D3D12_DEPTH_STENCIL_DESC detailUnseenStencil = detailSeenStencil;
    detailUnseenStencil.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_NOT_EQUAL;
    detailUnseenStencil.BackFace = detailUnseenStencil.FrontFace;

    const EffectPipelineStateDescription detailSeenDesc(
        &Vertex::InputLayout, CommonStates::Opaque, detailSeenStencil,
        CommonStates::CullNone, rtState, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    m_detailSeenEffect = std::make_unique<BasicEffect>(m_device.Get(), EffectFlags::VertexColor,
                                                       detailSeenDesc);

    const EffectPipelineStateDescription detailUnseenDesc(
        &Vertex::InputLayout, CommonStates::Opaque, detailUnseenStencil,
        CommonStates::CullNone, rtState, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    m_detailUnseenEffect = std::make_unique<BasicEffect>(m_device.Get(), EffectFlags::VertexColor,
                                                         detailUnseenDesc);

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

    // The key light restated from our own constant. It is the same direction
    // EnableDefaultLighting just set, so this changes nothing about how the
    // arena looks — what it changes is where the number lives. The shadow pass
    // casts along kSunDirection, and a sun that could be re-aimed for the
    // shadows without the shading following it would be a bug waiting on
    // somebody editing one line and not the other.
    const XMVECTOR sun = XMLoadFloat3(&kSunDirection);
    m_modelEffect->SetLightDirection(0, sun);
    m_texModelEffect->SetLightDirection(0, sun);

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
    if (m_shadowPass)
    {
        BindShadowDraw(world);
        m_shapes[static_cast<size_t>(shape)]->Draw(m_cmdList.Get());
        ++m_drawCalls;
        return;
    }

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

void Renderer::CreateShadowResources()
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kShadowMapSize;
    desc.Height = kShadowMapSize;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kShadowResourceFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = kShadowDsvFormat;
    clearValue.DepthStencil.Depth = 1.0f;

    // Created as the thing it spends most of the frame being. BeginShadowPass
    // borrows it back as a depth buffer and hands it over again on the way out,
    // so the state it is left in between frames is the readable one.
    HR(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
                                         IID_PPV_ARGS(&m_shadowMap)),
       "Create shadow map");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = kShadowDsvFormat;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_device->CreateDepthStencilView(m_shadowMap.Get(), &dsvDesc, dsv);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = kShadowSrvFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    const size_t slot = m_srvPile->Allocate();
    m_device->CreateShaderResourceView(m_shadowMap.Get(), &srvDesc, m_srvPile->GetCpuHandle(slot));
    m_shadowSrv = m_srvPile->GetGpuHandle(slot);
}

// The two pipelines the toolkit couldn't supply. Root signatures by hand
// because these carry their own shaders: an Effect owns its signature and its
// bytecode together, which is exactly the coupling that made shadows
// impossible to add to one.
void Renderer::CreateShadowPipelines()
{
    const auto serialize = [&](const D3D12_ROOT_SIGNATURE_DESC& desc, ID3D12RootSignature** out,
                               const char* what) {
        ComPtr<ID3DBlob> blob, error;
        const HRESULT hr =
            D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (FAILED(hr))
        {
            const char* text = error ? static_cast<const char*>(error->GetBufferPointer()) : "";
            throw std::runtime_error(std::string(what) + " serialize failed: " + text);
        }
        HR(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(out)),
           what);
    };

    // The depth pass. One matrix, straight in the root signature: it changes
    // every draw and is 64 bytes, which is under the size where a constant
    // buffer would be buying anything.
    {
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.Num32BitValues = 16;
        param.Constants.ShaderRegister = 0;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = 1;
        rsDesc.pParameters = &param;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;
        serialize(rsDesc, &m_shadowDepthRS, "Shadow depth root signature");

        // Position alone, at offset zero. Both vertex formats in the game put
        // it there and differ only in what follows, and what follows is not
        // described here — so this one layout accepts the batched
        // position/color geometry and the models' position/normal/texcoord
        // alike. See src/shaders/ShadowDepthVS.hlsl.
        const D3D12_INPUT_ELEMENT_DESC posOnly = { "POSITION",
                                                   0,
                                                   DXGI_FORMAT_R32G32B32_FLOAT,
                                                   0,
                                                   0,
                                                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                                                   0 };

        // Sloped surfaces are what a shadow map gets wrong first: a floor the
        // sun rakes across covers many world units per texel, so one depth
        // stands for a range of heights and half of it shadows itself. The
        // slope-scaled term biases by how fast depth is changing across the
        // triangle, which is the quantity that actually goes wrong.
        D3D12_RASTERIZER_DESC raster = CommonStates::CullNone;
        raster.SlopeScaledDepthBias = 2.5f;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = m_shadowDepthRS.Get();
        pso.VS = { g_ShadowDepthVS, sizeof(g_ShadowDepthVS) };
        pso.BlendState = CommonStates::Opaque;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState = raster;
        pso.DepthStencilState = CommonStates::DepthDefault;
        pso.InputLayout = { &posOnly, 1 };
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 0; // depth is the entire output
        pso.DSVFormat = kShadowDsvFormat;
        pso.SampleDesc.Count = 1;
        HR(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_shadowDepthPso)),
           "Create shadow depth pipeline");
    }

    // And the pass that reads it back over the scene.
    {
        const D3D12_DESCRIPTOR_RANGE ranges[2] = {
            { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
            { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
        };

        D3D12_ROOT_PARAMETER params[3] = {};
        // Two matrices and four floats — 36 of the 64 root DWORDs, with the
        // two tables below costing one each. Nothing here outlives the draw.
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 36;
        params[0].Constants.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        for (int i = 0; i < 2; ++i)
        {
            params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
            params[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
            params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }

        // A comparison sampler: the depth test happens inside the filter, so
        // one tap returns a blend of four already-answered comparisons rather
        // than one depth to compare afterwards. Clamped, because a point off
        // the edge of the map is a point the pass has already rejected.
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = static_cast<UINT>(std::size(params));
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &sampler;
        // No input layout flag: the fullscreen triangle is built from the
        // vertex id and there is no vertex buffer to describe.
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;
        serialize(rsDesc, &m_shadowApplyRS, "Shadow apply root signature");

        // dest * src, which is the whole reason this pass never reads the color
        // target it writes. It returns the fraction of itself each pixel keeps
        // and the blender does the multiply, so there is no ping-pong through a
        // second full-res target and no barrier in the middle of the scene.
        // Alpha is left alone: the scene target's is nobody's business here.
        D3D12_BLEND_DESC blend = {};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ZERO;
        blend.RenderTarget[0].DestBlend = D3D12_BLEND_SRC_COLOR;
        blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = m_shadowApplyRS.Get();
        pso.VS = { g_FullscreenVS, sizeof(g_FullscreenVS) };
        pso.PS = { g_ShadowApplyPS, sizeof(g_ShadowApplyPS) };
        pso.BlendState = blend;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState = CommonStates::CullNone;
        pso.DepthStencilState = CommonStates::DepthNone;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = kBackBufferFormat;
        pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc.Count = 1;
        HR(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_shadowApplyPso)),
           "Create shadow apply pipeline");
    }
}

// Where to put the sun so its map covers what the camera can see, and no more.
//
// The naive fit — a box around the whole view volume — is wrong here by two
// orders of magnitude. The iso camera's near and far planes bracket the entire
// arena so that nothing ever clips, which makes the view volume 26 units wide
// and 140 deep; fitted to that, a shadow texel would be a foot across. What is
// actually being looked at is the floor, so that is what gets fitted: the
// viewport's four corners cast down onto y = 0, extruded up by the height a
// caster is allowed to be.
//
// The fit is then a sphere rather than a box, which is what keeps the shadows
// still. A box around a rotating quad changes size as it turns, and a shadow
// map whose extent changes every frame crawls along every edge in the scene.
// The sphere around that quad doesn't rotate at all, so orbiting the camera
// leaves the map's size alone and only moves its center — and the center is
// snapped to whole texels, so it moves in steps the map can't tell apart from
// standing still. Zoom does change the radius, and does shimmer, which is a
// trade: it happens while the player is turning a wheel and looking at the
// scale of things rather than at the edge of a shadow.
XMMATRIX Renderer::ComputeLightViewProj() const
{
    const XMMATRIX invViewProj = XMMatrixInverse(nullptr, m_viewProj);

    // Both ends of each corner's depth range, so the difference is the view
    // direction — under an orthographic projection the same one for all four.
    XMVECTOR ground[4];
    static constexpr float kCornerX[4] = { -1.0f, 1.0f, -1.0f, 1.0f };
    static constexpr float kCornerY[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
    for (int i = 0; i < 4; ++i)
    {
        const XMVECTOR nearPt = XMVector3TransformCoord(
            XMVectorSet(kCornerX[i], kCornerY[i], 0.0f, 1.0f), invViewProj);
        const XMVECTOR farPt = XMVector3TransformCoord(
            XMVectorSet(kCornerX[i], kCornerY[i], 1.0f, 1.0f), invViewProj);

        const XMVECTOR dir = XMVectorSubtract(farPt, nearPt);
        const float dirY = XMVectorGetY(dir);
        // A camera looking level at the horizon would never reach the floor.
        // This one is pitched 35° down and can't, but the guard costs nothing
        // and the alternative is a division that quietly produces infinities.
        const float t = (std::abs(dirY) > 1e-6f) ? -XMVectorGetY(nearPt) / dirY : 0.0f;
        ground[i] = XMVectorAdd(nearPt, XMVectorScale(dir, t));
    }

    XMVECTOR center = XMVectorZero();
    for (const XMVECTOR& corner : ground)
        center = XMVectorAdd(center, corner);
    center = XMVectorScale(center, 0.25f);
    center = XMVectorSetY(center, kCasterHeight * 0.5f);

    float radius = 0.0f;
    for (const XMVECTOR& corner : ground)
    {
        // Measured from the floor, then opened up to clear the top of the
        // caster box: the corners are all at y = 0 and the center is halfway up.
        const XMVECTOR flat = XMVectorSetY(corner, kCasterHeight * 0.5f);
        radius = std::max(radius, XMVectorGetX(XMVector3Length(XMVectorSubtract(flat, center))));
    }
    radius = std::sqrt(radius * radius + kCasterHeight * kCasterHeight * 0.25f);

    // Anchored at the world origin rather than at the sphere, because the
    // snapping below has to happen in a light space that doesn't move when the
    // thing being snapped does.
    const XMVECTOR sun = XMVector3Normalize(XMLoadFloat3(&kSunDirection));
    const XMMATRIX lightView =
        XMMatrixLookToLH(XMVectorZero(), sun, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));

    const XMVECTOR centerLs = XMVector3TransformCoord(center, lightView);
    const float unitsPerTexel = 2.0f * radius / static_cast<float>(kShadowMapSize);
    const float snappedX = std::floor(XMVectorGetX(centerLs) / unitsPerTexel) * unitsPerTexel;
    const float snappedY = std::floor(XMVectorGetY(centerLs) / unitsPerTexel) * unitsPerTexel;
    const float centerZ = XMVectorGetZ(centerLs);

    // The near plane is pulled kSunBackoff further up-sun than the sphere
    // needs, so geometry standing off screen on the sunny side is still in the
    // map and still throws its shadow onto ground that is on screen.
    const XMMATRIX lightProj =
        XMMatrixOrthographicOffCenterLH(snappedX - radius, snappedX + radius, snappedY - radius,
                                        snappedY + radius, centerZ - radius - kSunBackoff,
                                        centerZ + radius);

    return lightView * lightProj;
}

// The pipeline is already bound — BeginShadowPass set it once and nothing
// between there and EndShadowPass changes it, which is the whole point of the
// pass being a stretch of the frame rather than a flag on each draw. A soldier
// alone is fifteen of these calls.
void Renderer::BindShadowDraw(const XMMATRIX& world)
{
    XMFLOAT4X4 mvp;
    XMStoreFloat4x4(&mvp, XMMatrixTranspose(world * m_lightViewProj));
    m_cmdList->SetGraphicsRoot32BitConstants(0, 16, &mvp, 0);
}

void Renderer::BeginShadowPass()
{
    m_lightViewProj = ComputeLightViewProj();
    m_shadowPass = true;

    Transition(m_shadowMap.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_DEPTH_WRITE);

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(kShadowMapSize),
                                static_cast<float>(kShadowMapSize), 0.0f, 1.0f };
    const D3D12_RECT sc = { 0, 0, static_cast<LONG>(kShadowMapSize),
                            static_cast<LONG>(kShadowMapSize) };
    m_cmdList->RSSetViewports(1, &vp);
    m_cmdList->RSSetScissorRects(1, &sc);

    // Bound once for the whole pass. Every caster in the arena goes through
    // this one pipeline, and the only thing that changes between them is the
    // matrix BindShadowDraw pushes.
    m_cmdList->SetGraphicsRootSignature(m_shadowDepthRS.Get());
    m_cmdList->SetPipelineState(m_shadowDepthPso.Get());
}

void Renderer::EndShadowPass()
{
    m_shadowPass = false;

    Transition(m_shadowMap.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Everything BeginFrame set up, set up again — the pass borrowed the
    // command list's targets and viewport and this is where they are returned.
    // The stencil reference with them: nothing in the shadow pass touches it,
    // but a caller reading this pair should be able to take "the scene is back
    // as it was" at face value rather than checking.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kFrameCount) * m_rtvSize;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissor);
    m_cmdList->OMSetStencilRef(kStencilSeen);
}

void Renderer::ApplyShadows()
{
    // The depth buffer changes hands for the length of one draw. It has to be
    // unbound as depth to be read as a texture — a resource can't be both at
    // once — which is why the render target is re-set without it below and set
    // back with it afterwards.
    Transition(m_depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kFrameCount) * m_rtvSize;
    m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    struct Constants
    {
        XMFLOAT4X4 invViewProj;
        XMFLOAT4X4 lightViewProj;
        XMFLOAT4 params;
    } cb;
    XMStoreFloat4x4(&cb.invViewProj, XMMatrixTranspose(XMMatrixInverse(nullptr, m_viewProj)));
    XMStoreFloat4x4(&cb.lightViewProj, XMMatrixTranspose(m_lightViewProj));
    cb.params = { kShadowStrength, kShadowDepthBias, 1.0f / static_cast<float>(kShadowMapSize),
                  0.0f };

    m_cmdList->SetGraphicsRootSignature(m_shadowApplyRS.Get());
    m_cmdList->SetPipelineState(m_shadowApplyPso.Get());
    m_cmdList->SetGraphicsRoot32BitConstants(0, 36, &cb, 0);
    m_cmdList->SetGraphicsRootDescriptorTable(1, m_depthSrv);
    m_cmdList->SetGraphicsRootDescriptorTable(2, m_shadowSrv);
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmdList->DrawInstanced(3, 1, 0, 0);
    ++m_drawCalls;

    Transition(m_depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
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

void Renderer::DrawFogEdge(const Vertex* verts, uint32_t count, const XMMATRIX& world)
{
    DrawBatch(verts, count, world, m_fogEdgeEffect.get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// Two passes over one buffer. BasicEffect's vertex-color variant multiplies
// the vertex through the effect's diffuse, so the dimmed half costs a constant
// rather than a second set of vertices in a second color.
void Renderer::DrawGroundDetail(const Vertex* verts, uint32_t count, const XMMATRIX& world,
                                float unseenTint)
{
    m_detailSeenEffect->SetDiffuseColor(g_XMOne);
    DrawBatch(verts, count, world, m_detailSeenEffect.get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_detailUnseenEffect->SetDiffuseColor(XMVectorReplicate(unseenTint));
    DrawBatch(verts, count, world, m_detailUnseenEffect.get(), D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::DrawBatch(const Vertex* verts, uint32_t count, const XMMATRIX& world,
                         BasicEffect* effect, D3D_PRIMITIVE_TOPOLOGY topology)
{
    if (count == 0 || count > kBatchVertices)
        return;

    // Inside the shadow pass the effect is beside the point — what a caster is
    // colored, whether it blends, and what it does to the fog's stencil are all
    // questions about the scene. The sun asks one question, and the depth
    // pipeline is the same one for everything that answers it.
    if (m_shadowPass)
    {
        BindShadowDraw(world);
        m_batch->Begin(m_cmdList.Get());
        m_batch->Draw(topology, verts, count);
        m_batch->End();
        ++m_drawCalls;
        return;
    }

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

    // One draw for the whole mesh rather than one per part: the parts exist to
    // carry materials, and the sun has no opinion about materials.
    if (m_shadowPass)
    {
        BindShadowDraw(world);
        m_cmdList->DrawIndexedInstanced(model.IndexCount(), 1, 0, 0, 0);
        ++m_drawCalls;
        return;
    }

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
