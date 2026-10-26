#pragma once
#include "Graphics/RenderCore/V2/RenderCoreDefs.h"
#include "Graphics/RenderCore/V2/FrameBufferRing.h"
#include "Graphics/RenderCore/V2/RenderGraph/RDGPassCompiler.h"
#include "Math/Math.h"
#include "Utils/UniquePtr.h"
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
    enum class PreparationResult
    {
        Ready,
        Pending,
        Failed
    };

    explicit SoftwareSceneRayQuery(RenderDevice* device);
    ~SoftwareSceneRayQuery();
    bool Prepare(RenderScene& scene);
    // Copies immutable CPU inputs on the caller, builds on one worker, and polls without
    // waiting. A finished build is published on the caller if the scene has not changed
    // since its snapshot; otherwise that call rebuilds the current scene synchronously, so a
    // scene that changes every frame still becomes Ready. Only a scene replacement abandons a build.
    PreparationResult PrepareAsync(RenderScene& scene);
    // Includes cancelled work until the worker's completion has been polled.
    bool IsPreparing() const;
    // Retains the scene BVH and pending construction across all-HW frames. Creates
    // one-element bindings only when no software buffers have been prepared yet.
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
    struct AsyncPreparation;
    void CancelPreparation();
    // Builds and publishes the scene's current geometry on the caller.
    bool Rebuild(RenderScene& scene);
    bool Publish(const HeapVector<SoftwareRayNode>& nodes, const HeapVector<SoftwareRayTriangle>& triangles, uint64_t revision);

    UniquePtr<AsyncPreparation> m_preparation;
    RenderDevice*               m_device;
    RenderScene*                m_scene{nullptr};
    FrameBufferRing             m_nodeBuffers;
    FrameBufferRing             m_triangleBuffers;
    uint64_t                    m_revision{0};
    uint32_t                    m_nodeCount{0};
    uint32_t                    m_triangleCount{0};
    bool                        m_placeholder{false};
};
} // namespace zen::rc
