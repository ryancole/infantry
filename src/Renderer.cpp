#include "Renderer.h"

#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    void HR(HRESULT hr, const char* what)
    {
        if (FAILED(hr))
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s failed (hr=0x%08X)", what, static_cast<unsigned>(hr));
            throw std::runtime_error(buf);
        }
    }

    std::wstring ExeDir()
    {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::wstring path(buf);
        return path.substr(0, path.find_last_of(L"\\/"));
    }

    ComPtr<ID3DBlob> CompileShader(const std::wstring& path, const char* entry, const char* target)
    {
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> errors;
        HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                        entry, target, flags, 0, &code, &errors);
        if (FAILED(hr))
        {
            std::string msg = "Shader compile failed: ";
            if (errors)
                msg.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
            throw std::runtime_error(msg);
        }
        return code;
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
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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

    CreatePipeline();
    CreateVertexBuffer();
}

void Renderer::Shutdown()
{
    if (!m_device)
        return;
    WaitForGpu();
    if (m_vbMapped)
    {
        m_vertexBuffer->Unmap(0, nullptr);
        m_vbMapped = nullptr;
    }
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
    HR(m_swapChain->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0),
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
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
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

void Renderer::CreatePipeline()
{
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.ShaderRegister = 0;
    param.Constants.Num32BitValues = 16;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &param;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig;
    ComPtr<ID3DBlob> err;
    HR(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err),
       "SerializeRootSignature");
    HR(m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                     IID_PPV_ARGS(&m_rootSig)),
       "CreateRootSignature");

    // Look next to the exe first (post-build copy), then fall back to the
    // source tree for runs launched from the repo root.
    std::wstring shaderPath = ExeDir() + L"\\shaders\\basic.hlsl";
    if (GetFileAttributesW(shaderPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        shaderPath = L"src\\shaders\\basic.hlsl";

    ComPtr<ID3DBlob> vs = CompileShader(shaderPath, "VSMain", "vs_5_0");
    ComPtr<ID3DBlob> ps = CompileShader(shaderPath, "PSMain", "ps_5_0");

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RENDER_TARGET_BLEND_DESC blend = {};
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_rootSig.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { layout, _countof(layout) };
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0] = blend;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    HR(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoTri)), "Create triangle PSO");

    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    HR(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoLine)), "Create line PSO");
}

void Renderer::CreateVertexBuffer()
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = kVBBytes;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HR(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                         IID_PPV_ARGS(&m_vertexBuffer)),
       "Create vertex buffer");

    D3D12_RANGE noRead = { 0, 0 };
    HR(m_vertexBuffer->Map(0, &noRead, reinterpret_cast<void**>(&m_vbMapped)), "Map vertex buffer");
    m_vbGpuVA = m_vertexBuffer->GetGPUVirtualAddress();
}

void Renderer::BeginFrame(const float clearColor[4])
{
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    HR(m_cmdAlloc->Reset(), "Command allocator Reset");
    HR(m_cmdList->Reset(m_cmdAlloc.Get(), m_psoTri.Get()), "Command list Reset");

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
    m_cmdList->SetGraphicsRootSignature(m_rootSig.Get());

    m_vbOffset = 0;
}

void Renderer::EndFrame()
{
    Transition(m_backBuffers[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PRESENT);
    HR(m_cmdList->Close(), "Command list Close");

    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    HR(m_swapChain->Present(1, 0), "Present");
    WaitForGpu();
}

void Renderer::DrawTriangles(const Vertex* verts, uint32_t count, const XMMATRIX& world)
{
    Draw(verts, count, world, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::DrawLines(const Vertex* verts, uint32_t count, const XMMATRIX& world)
{
    Draw(verts, count, world, D3D_PRIMITIVE_TOPOLOGY_LINELIST);
}

void Renderer::Draw(const Vertex* verts, uint32_t count, const XMMATRIX& world,
                    D3D_PRIMITIVE_TOPOLOGY topology)
{
    if (count == 0)
        return;
    const uint64_t bytes = static_cast<uint64_t>(count) * sizeof(Vertex);
    if (m_vbOffset + bytes > kVBBytes)
        return; // out of scratch space this frame; drop the draw

    std::memcpy(m_vbMapped + m_vbOffset, verts, bytes);

    XMFLOAT4X4 mvp;
    XMStoreFloat4x4(&mvp, world * m_viewProj);
    m_cmdList->SetGraphicsRoot32BitConstants(0, 16, &mvp, 0);

    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    vbv.BufferLocation = m_vbGpuVA + m_vbOffset;
    vbv.SizeInBytes = static_cast<UINT>(bytes);
    vbv.StrideInBytes = sizeof(Vertex);
    m_cmdList->IASetVertexBuffers(0, 1, &vbv);

    m_cmdList->SetPipelineState(topology == D3D_PRIMITIVE_TOPOLOGY_LINELIST ? m_psoLine.Get()
                                                                            : m_psoTri.Get());
    m_cmdList->IASetPrimitiveTopology(topology);
    m_cmdList->DrawInstanced(count, 1, 0, 0);

    m_vbOffset = (m_vbOffset + bytes + 255) & ~255ull;
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
