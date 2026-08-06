#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <DescriptorHeap.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Static mesh loaded from a glTF 2.0 file (.glb or .gltf). All nodes are
// flattened into one vertex/index buffer at load time; each glTF primitive
// becomes a Part carrying its material base color and, when the material has
// them, a base color texture and normal map (SRVs allocated from the
// renderer's descriptor pile). Untextured parts render through the flat lit
// effect; textured parts through the normal-map effect.
class Model
{
public:
    struct Part
    {
        uint32_t indexStart;
        uint32_t indexCount;
        DirectX::XMFLOAT4 color;
        int baseColorTex = -1; // index into Textures(), -1 = untextured
        int normalTex = -1;    // index into Textures(), -1 = flat fallback
    };

    struct Texture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_GPU_DESCRIPTOR_HANDLE srv = {};
    };

    // Uploads static buffers and textures through the given queue (blocks
    // until done). Texture SRVs are allocated from `srvPile`, which must be
    // the shader-visible heap the renderer binds each frame.
    // Throws std::runtime_error on parse or upload failure.
    static std::unique_ptr<Model> LoadFromFile(ID3D12Device* device,
                                               ID3D12CommandQueue* queue,
                                               DirectX::DescriptorPile& srvPile,
                                               const std::string& path);

    const std::vector<Part>& Parts() const { return m_parts; }
    const std::vector<Texture>& Textures() const { return m_textures; }
    const D3D12_VERTEX_BUFFER_VIEW& VertexView() const { return m_vbv; }
    const D3D12_INDEX_BUFFER_VIEW& IndexView() const { return m_ibv; }

    // Every index in the buffer, which is every part end to end: the loader
    // appends them one after another, so a caller that has nothing to say
    // about materials — the shadow pass — can draw the whole model at once
    // instead of walking a parts list to ask each of them the same question.
    uint32_t IndexCount() const { return m_ibv.SizeInBytes / sizeof(uint32_t); }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vbv = {};
    D3D12_INDEX_BUFFER_VIEW m_ibv = {};
    std::vector<Part> m_parts;
    std::vector<Texture> m_textures;
};
