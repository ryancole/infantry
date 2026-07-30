#include "Model.h"

#include <BufferHelpers.h>
#include <DirectXHelpers.h>
#include <ResourceUploadBatch.h>
#include <VertexTypes.h>
#include <WICTextureLoader.h>

#include <objbase.h>

#include <cstring>
#include <map>
#include <stdexcept>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    using ModelVertex = VertexPositionNormalTexture;

    void Fail(const std::string& path, const char* what)
    {
        throw std::runtime_error("Model load failed (" + path + "): " + what);
    }

    // glTF stores matrices column-major; DirectXMath multiplies row vectors,
    // so the raw memcpy already lands as the transpose we want.
    XMMATRIX NodeWorldTransform(const cgltf_node* node)
    {
        float m[16];
        cgltf_node_transform_world(node, m);
        XMFLOAT4X4 f;
        std::memcpy(&f, m, sizeof(f));
        return XMLoadFloat4x4(&f);
    }

    const cgltf_accessor* FindAttribute(const cgltf_primitive& prim, cgltf_attribute_type type)
    {
        for (cgltf_size i = 0; i < prim.attributes_count; ++i)
            if (prim.attributes[i].type == type && prim.attributes[i].index == 0)
                return prim.attributes[i].data;
        return nullptr;
    }

    std::wstring Widen(const std::string& s)
    {
        std::wstring out(s.size(), L'\0');
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                          out.data(), static_cast<int>(out.size()));
        out.resize(static_cast<size_t>(n < 0 ? 0 : n));
        return out;
    }

    // Loads each distinct glTF image once, creating its GPU resource and an
    // SRV on the renderer's descriptor pile. Images either sit in a GLB
    // buffer view or reference a file next to the .gltf.
    class TextureLoader
    {
    public:
        TextureLoader(ID3D12Device* device, ResourceUploadBatch& upload, DescriptorPile& pile,
                      const std::string& path, std::vector<Model::Texture>& out)
            : m_device(device), m_upload(upload), m_pile(pile), m_path(path), m_out(out)
        {
            const size_t slash = path.find_last_of("\\/");
            m_dir = (slash == std::string::npos) ? "" : path.substr(0, slash + 1);
        }

        // Returns an index into the model's texture array, or -1 if the
        // glTF texture slot is empty.
        int Load(const cgltf_texture* texture)
        {
            if (!texture || !texture->image)
                return -1;
            const cgltf_image* image = texture->image;
            if (const auto it = m_cache.find(image); it != m_cache.end())
                return it->second;

            // The render pipeline is gamma-naive (UNORM backbuffer, color
            // factors passed through raw), so keep texels raw too instead of
            // tagging base color as sRGB.
            constexpr WIC_LOADER_FLAGS kFlags = static_cast<WIC_LOADER_FLAGS>(
                WIC_LOADER_MIP_AUTOGEN | WIC_LOADER_IGNORE_SRGB | WIC_LOADER_FORCE_RGBA32);

            Model::Texture tex;
            HRESULT hr = E_FAIL;
            if (image->buffer_view)
            {
                const cgltf_buffer_view* view = image->buffer_view;
                const auto* bytes = static_cast<const uint8_t*>(view->buffer->data) + view->offset;
                hr = CreateWICTextureFromMemoryEx(m_device, m_upload, bytes, view->size, 0,
                                                  D3D12_RESOURCE_FLAG_NONE, kFlags,
                                                  tex.resource.GetAddressOf());
            }
            else if (image->uri && std::strncmp(image->uri, "data:", 5) != 0)
            {
                std::string uri = image->uri;
                cgltf_decode_uri(uri.data());
                uri.resize(std::strlen(uri.c_str()));
                hr = CreateWICTextureFromFileEx(m_device, m_upload, Widen(m_dir + uri).c_str(), 0,
                                                D3D12_RESOURCE_FLAG_NONE, kFlags,
                                                tex.resource.GetAddressOf());
            }
            if (FAILED(hr))
                Fail(m_path, "texture load failed");

            const size_t slot = m_pile.Allocate();
            CreateShaderResourceView(m_device, tex.resource.Get(), m_pile.GetCpuHandle(slot));
            tex.srv = m_pile.GetGpuHandle(slot);

            const int index = static_cast<int>(m_out.size());
            m_out.push_back(std::move(tex));
            m_cache.emplace(image, index);
            return index;
        }

    private:
        ID3D12Device* m_device;
        ResourceUploadBatch& m_upload;
        DescriptorPile& m_pile;
        const std::string& m_path;
        std::string m_dir;
        std::vector<Model::Texture>& m_out;
        std::map<const cgltf_image*, int> m_cache;
    };
}

std::unique_ptr<Model> Model::LoadFromFile(ID3D12Device* device, ID3D12CommandQueue* queue,
                                           DescriptorPile& srvPile, const std::string& path)
{
    // WIC decoding lives on COM; safe to call again if the thread already did it.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    cgltf_options options = {};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success)
        Fail(path, "could not parse file");
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success)
    {
        cgltf_free(data);
        Fail(path, "could not load buffers");
    }

    auto model = std::make_unique<Model>();
    std::vector<ModelVertex> verts;
    std::vector<uint32_t> indices;
    std::vector<Part> parts;

    ResourceUploadBatch upload(device);
    upload.Begin();
    TextureLoader textures(device, upload, srvPile, path, model->m_textures);

    for (cgltf_size n = 0; n < data->nodes_count; ++n)
    {
        const cgltf_node& node = data->nodes[n];
        if (!node.mesh)
            continue;

        const XMMATRIX world = NodeWorldTransform(&node);
        // Normals ignore translation; assumes no non-uniform scale (fine for
        // our flat-shaded assets — normalize below covers uniform scale).
        XMMATRIX normalXf = world;
        normalXf.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);

        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p)
        {
            const cgltf_primitive& prim = node.mesh->primitives[p];
            if (prim.type != cgltf_primitive_type_triangles)
                continue;

            const cgltf_accessor* posAcc = FindAttribute(prim, cgltf_attribute_type_position);
            if (!posAcc)
                continue;
            const cgltf_accessor* normAcc = FindAttribute(prim, cgltf_attribute_type_normal);
            const cgltf_accessor* uvAcc = FindAttribute(prim, cgltf_attribute_type_texcoord);

            const uint32_t baseVertex = static_cast<uint32_t>(verts.size());
            for (cgltf_size v = 0; v < posAcc->count; ++v)
            {
                float pos[3] = {};
                cgltf_accessor_read_float(posAcc, v, pos, 3);
                XMVECTOR pv = XMVector3TransformCoord(XMVectorSet(pos[0], pos[1], pos[2], 1.0f),
                                                      world);

                XMVECTOR nv = XMVectorZero();
                if (normAcc)
                {
                    float norm[3] = {};
                    cgltf_accessor_read_float(normAcc, v, norm, 3);
                    nv = XMVector3Normalize(XMVector3TransformNormal(
                        XMVectorSet(norm[0], norm[1], norm[2], 0.0f), normalXf));
                }

                ModelVertex mv;
                XMStoreFloat3(&mv.position, pv);
                XMStoreFloat3(&mv.normal, nv);
                mv.textureCoordinate = { 0.0f, 0.0f };
                if (uvAcc)
                    cgltf_accessor_read_float(uvAcc, v, &mv.textureCoordinate.x, 2);
                // glTF is right-handed; mirror z into our left-handed world.
                mv.position.z = -mv.position.z;
                mv.normal.z = -mv.normal.z;
                verts.push_back(mv);
            }

            Part part = {};
            part.indexStart = static_cast<uint32_t>(indices.size());
            if (prim.indices)
            {
                for (cgltf_size i = 0; i < prim.indices->count; ++i)
                    indices.push_back(baseVertex +
                                      static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
            }
            else
            {
                for (cgltf_size i = 0; i < posAcc->count; ++i)
                    indices.push_back(baseVertex + static_cast<uint32_t>(i));
            }
            part.indexCount = static_cast<uint32_t>(indices.size()) - part.indexStart;

            part.color = { 0.8f, 0.8f, 0.8f, 1.0f };
            if (prim.material)
            {
                if (prim.material->has_pbr_metallic_roughness)
                {
                    const cgltf_pbr_metallic_roughness& pbr =
                        prim.material->pbr_metallic_roughness;
                    const float* c = pbr.base_color_factor;
                    part.color = { c[0], c[1], c[2], c[3] };
                    // Textures only matter if the mesh has UVs to sample with.
                    if (uvAcc)
                        part.baseColorTex = textures.Load(pbr.base_color_texture.texture);
                }
                if (uvAcc && part.baseColorTex >= 0)
                    part.normalTex = textures.Load(prim.material->normal_texture.texture);
            }

            // Flat normals for primitives that ship without them.
            if (!normAcc)
            {
                for (uint32_t i = part.indexStart; i + 2 < part.indexStart + part.indexCount; i += 3)
                {
                    ModelVertex& a = verts[indices[i]];
                    ModelVertex& b = verts[indices[i + 1]];
                    ModelVertex& c = verts[indices[i + 2]];
                    const XMVECTOR n = XMVector3Normalize(XMVector3Cross(
                        XMLoadFloat3(&b.position) - XMLoadFloat3(&a.position),
                        XMLoadFloat3(&c.position) - XMLoadFloat3(&a.position)));
                    XMStoreFloat3(&a.normal, n);
                    XMStoreFloat3(&b.normal, n);
                    XMStoreFloat3(&c.normal, n);
                }
            }

            parts.push_back(part);
        }
    }

    cgltf_free(data);

    if (verts.empty() || indices.empty())
        Fail(path, "no triangle geometry found");

    model->m_parts = std::move(parts);

    if (FAILED(CreateStaticBuffer(device, upload, verts.data(), verts.size(), sizeof(ModelVertex),
                                  D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                                  model->m_vertexBuffer.GetAddressOf())))
        Fail(path, "vertex buffer upload failed");
    if (FAILED(CreateStaticBuffer(device, upload, indices.data(), indices.size(), sizeof(uint32_t),
                                  D3D12_RESOURCE_STATE_INDEX_BUFFER,
                                  model->m_indexBuffer.GetAddressOf())))
        Fail(path, "index buffer upload failed");
    upload.End(queue).wait();

    model->m_vbv.BufferLocation = model->m_vertexBuffer->GetGPUVirtualAddress();
    model->m_vbv.SizeInBytes = static_cast<UINT>(verts.size() * sizeof(ModelVertex));
    model->m_vbv.StrideInBytes = sizeof(ModelVertex);
    model->m_ibv.BufferLocation = model->m_indexBuffer->GetGPUVirtualAddress();
    model->m_ibv.SizeInBytes = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    model->m_ibv.Format = DXGI_FORMAT_R32_UINT;
    return model;
}
