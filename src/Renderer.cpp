#include "Renderer.h"

#include <CommonStates.h>
#include <EffectPipelineStateDescription.h>
#include <RenderTargetState.h>

#include <cstdio>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

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
    rtvHeapDesc.NumDescriptors = kFrameCount;
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
    m_batch = std::make_unique<PrimitiveBatch<Vertex>>(m_device.Get(), kBatchVertices * 3,
                                                       kBatchVertices);
    CreateEffects();
}

void Renderer::Shutdown()
{
    if (!m_device)
        return;
    WaitForGpu();
    m_batch.reset();
    m_triEffect.reset();
    m_lineEffect.reset();
    m_modelEffect.reset();
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

    const EffectPipelineStateDescription modelDesc(
        &VertexPositionNormal::InputLayout, CommonStates::Opaque, CommonStates::DepthDefault,
        CommonStates::CullNone, rtState, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    m_modelEffect = std::make_unique<BasicEffect>(m_device.Get(), EffectFlags::Lighting, modelDesc);
    m_modelEffect->EnableDefaultLighting();
    m_modelEffect->DisableSpecular();
}

void Renderer::BeginFrame(const float clearColor[4])
{
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    HR(m_cmdAlloc->Reset(), "Command allocator Reset");
    HR(m_cmdList->Reset(m_cmdAlloc.Get(), nullptr), "Command list Reset");

    Transition(m_backBuffers[m_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT,
               D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    m_cmdList->RSSetViewports(1, &m_viewport);
    m_cmdList->RSSetScissorRects(1, &m_scissor);
}

void Renderer::EndFrame()
{
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
    return Model::LoadFromFile(m_device.Get(), m_queue.Get(), resolved);
}

void Renderer::DrawModel(const Model& model, const XMMATRIX& world)
{
    m_modelEffect->SetMatrices(world, XMMatrixIdentity(), m_viewProj);
    m_cmdList->IASetVertexBuffers(0, 1, &model.VertexView());
    m_cmdList->IASetIndexBuffer(&model.IndexView());
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (const Model::Part& part : model.Parts())
    {
        m_modelEffect->SetDiffuseColor(XMLoadFloat4(&part.color));
        m_modelEffect->Apply(m_cmdList.Get());
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
