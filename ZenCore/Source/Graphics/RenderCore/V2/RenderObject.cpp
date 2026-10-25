#include "Graphics/RenderCore/V2/RenderObject.h"
#include "AssetLib/Types.h"

namespace zen::rc
{
RenderObject::RenderObject(RenderDevice* pRenderDevice) : m_pRenderDevice(pRenderDevice)
{
    // Retain the Box mesh's face-split ordering, unit extents and outward winding.
    const Vec3    positions[]{{-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f},   {0.5f, 0.5f, 0.5f},
                              {0.5f, -0.5f, 0.5f},   {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f},  {-0.5f, -0.5f, -0.5f},
                              {0.5f, 0.5f, 0.5f},    {0.5f, -0.5f, 0.5f},  {0.5f, 0.5f, -0.5f},   {0.5f, -0.5f, -0.5f},
                              {-0.5f, 0.5f, 0.5f},   {0.5f, 0.5f, 0.5f},   {-0.5f, 0.5f, -0.5f},  {0.5f, 0.5f, -0.5f},
                              {-0.5f, -0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f},  {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
                              {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, -0.5f, -0.5f},  {0.5f, 0.5f, -0.5f}};
    const Vec3    normals[]{{0, 0, 1}, {0, -1, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, 0, -1}};
    asset::Vertex vertices[24]{};
    uint32_t      indices[36]{};
    for (uint32_t face = 0; face < 6; ++face)
    {
        for (uint32_t corner = 0; corner < 4; ++corner)
        {
            const uint32_t index   = face * 4 + corner;
            vertices[index].pos    = Vec4(positions[index], 1.0f);
            vertices[index].normal = Vec4(normals[face], 0.0f);
            vertices[index].color  = Vec4(1.0f);
        }
        const uint32_t corners[]{0, 1, 2, 3, 2, 1};
        for (uint32_t corner = 0; corner < 6; ++corner)
        {
            indices[face * 6 + corner] = face * 4 + corners[corner];
        }
    }
    m_pVertexBuffer = m_pRenderDevice->CreateVertexBuffer(sizeof(vertices), reinterpret_cast<const uint8_t*>(vertices));
    if (m_pVertexBuffer != nullptr)
    {
        m_pIndexBuffer = m_pRenderDevice->CreateIndexBuffer(sizeof(indices), reinterpret_cast<const uint8_t*>(indices));
    }
    if (!IsValid())
    {
        m_pRenderDevice->DestroyBuffer(m_pVertexBuffer);
        m_pVertexBuffer = nullptr;
        LOGE("Cannot allocate the built-in voxel visualization cube");
    }
}

RenderObject::~RenderObject()
{
    m_pRenderDevice->DestroyBuffer(m_pVertexBuffer);

    m_pRenderDevice->DestroyBuffer(m_pIndexBuffer);
}
} // namespace zen::rc
