#include "Renderer.h"

#include <DirectXHelpers.h>
#include <EffectPipelineStateDescription.h>
#include <RenderTargetState.h>
#include <ResourceUploadBatch.h>

#include <cstdio>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

    // Post-process tuning. The extract threshold is high enough that only
    // genuinely bright pixels (projectiles, aim line, vivid class colors)
    // bloom, not the whole scene.
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
    m_modelEffect.reset();
    m_texModelEffect.reset();
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
void Renderer::CreateShapePrimitives()
{
    // rhcoords = false: our world is left-handed.
    m_shapes[static_cast<size_t>(Shape::Box)] =
        GeometricPrimitive::CreateCube(1.0f, false, m_device.Get());
    m_shapes[static_cast<size_t>(Shape::Sphere)] =
        GeometricPrimitive::CreateSphere(1.0f, 16, false, false, m_device.Get());
    m_shapes[static_cast<size_t>(Shape::Cylinder)] =
        GeometricPrimitive::CreateCylinder(1.0f, 1.0f, 24, false, m_device.Get());
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

float Renderer::MeasureScreenText(std::string_view text, float size) const
{
    const std::string str(text);
    return XMVectorGetX(m_font->MeasureString(str.c_str())) * (size / m_fontCapHeight);
}

void Renderer::BeginFrame(const float clearColor[4])
{
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
    m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissor);

    ID3D12DescriptorHeap* heaps[] = { m_srvPile->Heap(), m_states->Heap() };
    m_cmdList->SetDescriptorHeaps(static_cast<UINT>(std::size(heaps)), heaps);
}

void Renderer::EndFrame()
{
    RunPostChain();

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
