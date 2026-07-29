#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Static mesh loaded from a glTF 2.0 file (.glb or .gltf). All nodes are
// flattened into one vertex/index buffer at load time; each glTF primitive
// becomes a Part carrying its material base color. Rendered flat-shaded
// (position + normal) through the renderer's lit effect.
class Model
{
public:
    struct Part
    {
        uint32_t indexStart;
        uint32_t indexCount;
        DirectX::XMFLOAT4 color;
    };

    // Uploads static buffers through the given queue (blocks until done).
    // Throws std::runtime_error on parse or upload failure.
    static std::unique_ptr<Model> LoadFromFile(ID3D12Device* device,
                                               ID3D12CommandQueue* queue,
                                               const std::string& path);

    const std::vector<Part>& Parts() const { return m_parts; }
    const D3D12_VERTEX_BUFFER_VIEW& VertexView() const { return m_vbv; }
    const D3D12_INDEX_BUFFER_VIEW& IndexView() const { return m_ibv; }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vbv = {};
    D3D12_INDEX_BUFFER_VIEW m_ibv = {};
    std::vector<Part> m_parts;
};
