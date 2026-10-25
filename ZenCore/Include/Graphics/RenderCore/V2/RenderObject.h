#pragma once
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "SceneGraph/AABB.h"

namespace zen::rc
{
// Built-in unit cube used by voxel visualization, independent of source assets.
class RenderObject
{
public:
    explicit RenderObject(RenderDevice* pRenderDevice);

    ~RenderObject();

    bool IsValid() const
    {
        return m_pVertexBuffer != nullptr && m_pIndexBuffer != nullptr;
    }

    RHIBuffer* GetVertexBuffer() const
    {
        return m_pVertexBuffer;
    }

    RHIBuffer* GetIndexBuffer() const
    {
        return m_pIndexBuffer;
    }

    const sg::AABB& GetAABB() const
    {
        return m_bounds;
    }

private:
    RenderDevice* m_pRenderDevice{nullptr};
    sg::AABB      m_bounds{Vec3(-0.5f), Vec3(0.5f)};
    RHIBuffer*    m_pVertexBuffer{nullptr};
    RHIBuffer*    m_pIndexBuffer{nullptr};
};
} // namespace zen::rc
