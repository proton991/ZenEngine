#pragma once
#include "Graphics/RenderCore/V2/RenderCoreDefs.h"
#include "Graphics/RenderCore/V2/FrameBufferRing.h"
#include "Graphics/RenderCore/V2/RenderGraph/RDGPassCompiler.h"
#include "Math/Math.h"
#include <cstddef>

namespace zen::rc
{
class RenderScene;
class RenderDevice;

// Depth-first BVH. Internal nodes descend to the next node; escape skips the
// entire subtree. Leaves reference full triangles, independently of voxel bounds.
struct SoftwareRayNode
{
    Vec3     minimum{0};
    uint32_t firstAndCount{0}; // triangle offset in high bits, leaf count in low three bits
    Vec3     maximum{0};
    uint32_t escape{0};
};
static_assert(sizeof(SoftwareRayNode) == 32);
static_assert(offsetof(SoftwareRayNode, firstAndCount) == 12);
static_assert(offsetof(SoftwareRayNode, maximum) == 16);
static_assert(offsetof(SoftwareRayNode, escape) == 28);

struct SoftwareRayTriangle
{
    Vec4       a{0};
    Vec4       b{0};
    Vec4       c{0};
    glm::uvec4 record{0}; // index offset, node, material, enabled class
};
static_assert(sizeof(SoftwareRayTriangle) == 64);

// CPU construction, ordinary storage buffers, shader traversal: no hardware
// ray-query or acceleration-structure features are required.
class SoftwareSceneRayQuery
{
public:
    explicit SoftwareSceneRayQuery(RenderDevice* device) : m_device(device) {}
    bool Prepare(RenderScene& scene);
    // Releases the scene BVH while keeping one-element bindings for shaders whose
    // software path is compiled in but not selected, such as all-HW RT tracing.
    bool PrepareUnused();
    void BindTraversalInputs(RDGPassDescBase& pass) const;
    void BindInputs(RDGPassDescBase& pass, RenderScene& scene, bool forwardMaterialBound = false) const;
    void OnRenderGraphExecuted(RenderFrameResult result);
    void Destroy();
    bool IsReady() const;
    // False while only the unused-path placeholders are bound.
    bool HoldsSceneGeometry() const
    {
        return m_nodeBuffers.GetBuffer() != nullptr && !m_placeholder;
    }
    // World-space triangles in BVH order, for passes that read them without traversal (BrickScene).
    RHIBuffer* GetTriangleBuffer() const
    {
        return m_triangleBuffers.GetBuffer();
    }
    // Scene triangles in the buffer; zero for placeholders, whose buffer holds one unused element.
    uint32_t GetTriangleCount() const
    {
        return m_placeholder ? 0u : m_triangleCount;
    }
    // Geometry revision of the prepared triangles.
    uint64_t GetRevision() const
    {
        return m_revision;
    }

private:
    RenderDevice*   m_device;
    RenderScene*    m_scene{nullptr};
    FrameBufferRing m_nodeBuffers;
    FrameBufferRing m_triangleBuffers;
    uint64_t        m_revision{0};
    uint32_t        m_nodeCount{0};
    uint32_t        m_triangleCount{0};
    bool            m_placeholder{false};
};
} // namespace zen::rc
