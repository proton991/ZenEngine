#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/ResourceRetirement.h"
#include "Math/Math.h"

namespace zen::rc
{
class RenderDevice;
class RenderScene;
class SoftwareSceneRayQuery;

// brick_common.glsl uBrickData.
struct BrickSceneUniformData
{
    Vec4       gridMinCellSize{0}; // grid minimum, cell size
    glm::uvec4 dimensions{0};      // cells per axis, brick capacity, triangle count
    Vec4       voxel{0};           // fine voxel size, inverse fine voxel size, trace origin bias
};
static_assert(sizeof(BrickSceneUniformData) == 48);

// Counters of a completed build (BrickSceneStatistics in brick_build.glsl's BrickCounters).
struct BrickSceneStatistics
{
    uint32_t largeTriangles{0};
    uint32_t allocatedBricks{0};
    bool     overflow{false};
};

// Sparse 8x8x8-bit occupancy bricks over a cubic grid of cells (Graphics/Shared/BrickScene.h),
// built on the GPU from the software ray query's world-space triangles. A rebuild is recorded
// whenever the triangles, the grid or the resolution change. Rays traverse it in brick_trace.glsl.
class BrickScene
{
public:
    explicit BrickScene(RenderDevice* device) : m_device(device) {}

    // Records a rebuild when needed. The triangles must be prepared for this frame's graph.
    bool BuildRenderGraph(RenderScene&                 scene,
                          const SoftwareSceneRayQuery& triangles,
                          const Vec3&                  gridMinimum,
                          float                        gridLength,
                          uint32_t                     resolution);

    void BindTraceInputs(RDGPassDescBase& pass) const;

    void OnRenderGraphExecuted(RenderFrameResult result);

    void Destroy();

    // Whether trace inputs describe the current triangles: built, or recorded in this frame.
    bool IsReady() const
    {
        return m_cells != nullptr && (m_built || m_recorded);
    }

    // Nonblocking: returns the latest completed build's counters, or zeros before one completes.
    // Wait for GPU completion first when the caller requires the newest submitted build.
    BrickSceneStatistics ReadStatistics() const;

    uint32_t GetResolution() const
    {
        return m_resolution;
    }

    uint32_t GetCapacity() const
    {
        return m_capacity;
    }

    // Persistent cell, brick and large-triangle storage in bytes.
    uint64_t GetResidentBytes() const;

private:
    struct PendingStatistics
    {
        RHIBuffer*         readback;
        ResourceRetirement retirement;
        uint32_t           resolution;
        uint32_t           capacity;
    };

    bool Allocate(uint32_t resolution, uint32_t triangleCount);

    void PollStatistics() const;

    void ReleaseBuildResources();

    void BindBuildInputs(RDGPassDescBase& pass, const SoftwareSceneRayQuery& triangles, bool clear = false) const;

    void AddCellPass(RDGComputePassDesc&& pass) const;

    void AddTrianglePass(RDGComputePassDesc&& pass, uint32_t triangleCount) const;

    void AddLargeTrianglePass(RDGComputePassDesc&& pass) const;

    RenderDevice*                         m_device;
    RHIBuffer*                            m_cells{nullptr};
    RHIBuffer*                            m_bits{nullptr};
    RHIBuffer*                            m_masks{nullptr}; // Occupied 4x4x4 blocks per brick.
    RHIBuffer*                            m_counters{nullptr};
    RHIBuffer*                            m_readback{nullptr};
    RHIBuffer*                            m_largeTriangles{nullptr};
    BrickSceneUniformData                 m_uniforms;
    uint32_t                              m_resolution{0};
    uint32_t                              m_capacity{0};
    uint32_t                              m_largeCapacity{0};
    uint64_t                              m_revision{0};
    uint64_t                              m_recordedRevision{0};
    Vec4                                  m_grid{0};
    bool                                  m_built{false};
    bool                                  m_recorded{false};
    bool                                  m_allocationFailureLogged{false};
    mutable HeapVector<PendingStatistics> m_pendingStatistics;
    mutable BrickSceneStatistics          m_statistics;
};
} // namespace zen::rc
