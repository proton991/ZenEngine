#include "Graphics/RenderCore/V2/BrickScene.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/SceneRayQuery.h"
#include "Graphics/RenderCore/V2/SoftwareSceneRayQuery.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "Graphics/Shared/BrickScene.h"
#include <algorithm>
#include <cmath>

namespace zen::rc
{
namespace
{
// Pool size limit: a quarter of the cells, at most 2^20 bricks (64 MiB).
constexpr uint64_t kMaxBricks = 1ull << 20;

// The trace origin leaves every fine voxel a receiver's own plane can touch (brick_trace.glsl).
constexpr float kOriginBiasVoxels = 1.8f;

RHIBuffer* CreateBrickBuffer(RenderDevice* device, uint64_t bytes, BitField<RHIBufferUsageFlagBits> usage, NameID name)
{
    RHIBufferCreateInfo info;
    info.size         = bytes;
    info.tag          = name;
    info.allocateType = RHIBufferAllocateType::eGPU;
    info.usageFlags   = usage;
    return device->CreateBuffer(info);
}
} // namespace

bool BrickScene::Allocate(uint32_t resolution, uint32_t triangleCount)
{
    const uint64_t cells    = uint64_t(resolution) * resolution * resolution;
    const uint64_t capacity = std::min(cells / 4, kMaxBricks);
    const uint32_t large    = std::max(triangleCount, 1u);
    bool           valid    = true;
    if (m_cells == nullptr || m_resolution != resolution)
    {
        ReleaseBuildResources();
        const RHIGPUInfo& gpu       = m_device->GetGPUInfo();
        uint64_t          cellBytes = 0, bitBytes = 0, maskBytes = 0;
        valid = ValidateGIStorageBuffer(cells, sizeof(uint32_t), gpu, cellBytes) == GIResourceStatus::eSuccess
             && ValidateGIStorageBuffer(capacity * ZEN_BRICK_WORDS, sizeof(uint32_t), gpu, bitBytes)
                    == GIResourceStatus::eSuccess
             && ValidateGIStorageBuffer(capacity, sizeof(uint32_t), gpu, maskBytes) == GIResourceStatus::eSuccess;
        BitField<RHIBufferUsageFlagBits> storage(RHIBufferUsageFlagBits::eStorageBuffer);
        BitField<RHIBufferUsageFlagBits> counters;
        counters.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eIndirectBuffer,
                          RHIBufferUsageFlagBits::eTransferSrcBuffer);
        if (valid)
        {
            m_cells    = CreateBrickBuffer(m_device, cellBytes, storage, "brick_cells");
            m_bits     = CreateBrickBuffer(m_device, bitBytes, storage, "brick_bits");
            m_masks    = CreateBrickBuffer(m_device, maskBytes, storage, "brick_masks");
            m_counters = CreateBrickBuffer(m_device, ZEN_BRICK_COUNTER_WORDS * sizeof(uint32_t), counters, "brick_counters");
            valid      = m_cells != nullptr && m_bits != nullptr && m_masks != nullptr && m_counters != nullptr;
        }
        m_resolution = resolution;
        m_capacity   = static_cast<uint32_t>(capacity);
    }
    if (valid && (m_largeTriangles == nullptr || m_largeCapacity < large))
    {
        m_device->DestroyBuffer(m_largeTriangles);
        uint64_t bytes = 0;
        valid = ValidateGIStorageBuffer(large, sizeof(uint32_t), m_device->GetGPUInfo(), bytes) == GIResourceStatus::eSuccess;
        m_largeTriangles =
            valid ? CreateBrickBuffer(m_device, bytes, BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eStorageBuffer),
                                      "brick_large_triangles")
                  : nullptr;
        m_largeCapacity = m_largeTriangles != nullptr ? large : 0u;
        valid           = m_largeTriangles != nullptr;
    }
    if (valid && m_readback == nullptr)
    {
        // Each rebuild keeps its own copy until completion, including consecutive animated
        // rebuilds and resolution changes. No later GPU copy can race a diagnostic CPU read.
        RHIBufferCreateInfo readback;
        readback.size         = ZEN_BRICK_COUNTER_WORDS * sizeof(uint32_t);
        readback.tag          = "brick_counters_readback";
        readback.allocateType = RHIBufferAllocateType::eCPURead;
        readback.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
        m_readback = m_device->CreateBuffer(readback);
        valid      = m_readback != nullptr;
    }
    if (!valid)
    {
        // Allocation is retried every frame so the scene recovers as soon as memory allows; only the
        // first failure of a streak is logged.
        if (!m_allocationFailureLogged)
        {
            LOGE("Brick scene allocation failed at resolution {}; retrying each frame", resolution);
        }
        m_allocationFailureLogged = true;
        ReleaseBuildResources();
    }
    else
    {
        m_allocationFailureLogged = false;
    }
    return valid;
}

void BrickScene::BindBuildInputs(RDGPassDescBase& pass, const SoftwareSceneRayQuery& triangles, bool clear) const
{
    // The clear pass writes every word of the brick storage (brick_build.glsl), so the trace passes
    // of later frames read defined contents.
    const RDGContentGuarantee initialized = clear ? RDGContentGuarantee::eFullWrite : RDGContentGuarantee::eNone;
    pass.BindValue("uBrickData", m_uniforms);
    pass.BindStorageBuffer("BrickCells", m_cells, initialized);
    pass.BindStorageBuffer("BrickBits", m_bits, initialized);
    pass.BindStorageBuffer("BrickMasks", m_masks, initialized);
    pass.BindStorageBuffer("BrickTriangles", triangles.GetTriangleBuffer());
    pass.BindStorageBuffer("BrickCounters", m_counters, initialized);
    pass.BindStorageBuffer("BrickLargeTriangles", m_largeTriangles, initialized);
}

void BrickScene::AddCellPass(RDGComputePassDesc&& pass) const
{
    const uint32_t groups = m_resolution / 4u;
    m_device->GetCurrentFrameRDG()->AddComputePass(std::move(pass)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(groups, groups, groups);
    });
}

void BrickScene::AddTrianglePass(RDGComputePassDesc&& pass, uint32_t triangleCount) const
{
    const uint32_t groups  = std::max((triangleCount + 63u) / 64u, 1u);
    const uint32_t groupsX = std::min(groups, ZEN_BRICK_LARGE_GROUPS_X);
    const uint32_t groupsY = (groups + groupsX - 1u) / groupsX;
    m_device->GetCurrentFrameRDG()
        ->AddComputePass(std::move(pass))
        .RecordPassCommands([groupsX, groupsY](RDGPassCmdEncoder& encoder) { encoder.Dispatch(groupsX, groupsY, 1); });
}

void BrickScene::AddLargeTrianglePass(RDGComputePassDesc&& pass) const
{
    RHIBuffer* arguments = m_counters;
    pass.UseIndirectBuffer(arguments);
    m_device->GetCurrentFrameRDG()->AddComputePass(std::move(pass)).RecordPassCommands([arguments](RDGPassCmdEncoder& encoder) {
        encoder.DispatchIndirect(arguments, 0);
    });
}

bool BrickScene::BuildRenderGraph(RenderScene&                 scene,
                                  const SoftwareSceneRayQuery& triangles,
                                  const Vec3&                  gridMinimum,
                                  float                        gridLength,
                                  uint32_t                     resolution)
{
    PollStatistics();
    const Vec4  grid(gridMinimum, gridLength);
    const float cell  = resolution != 0 ? gridLength / static_cast<float>(resolution) : 0.0f;
    const float voxel = cell / static_cast<float>(ZEN_BRICK_SIZE);
    bool valid        = triangles.IsReady() && triangles.HoldsSceneGeometry() && triangles.GetTriangleBuffer() != nullptr
                     && (resolution == 64 || resolution == 128 || resolution == 256) && std::isfinite(gridLength)
                     && std::isfinite(gridMinimum.x) && std::isfinite(gridMinimum.y) && std::isfinite(gridMinimum.z) && voxel > 0.0f
                     && std::isfinite(1.0f / voxel);
    m_recorded        = false;
    if (valid
        && (!m_built || m_revision != triangles.GetRevision() || m_grid != grid || m_resolution != resolution
            || m_largeCapacity < std::max(triangles.GetTriangleCount(), 1u)))
    {
        valid = Allocate(resolution, triangles.GetTriangleCount());
        if (valid)
        {
            m_uniforms.gridMinCellSize = Vec4(gridMinimum, cell);
            m_uniforms.dimensions      = glm::uvec4(resolution, m_capacity, triangles.GetTriangleCount(), 0);
            m_uniforms.voxel           = Vec4(voxel, 1.0f / voxel, kOriginBiasVoxels * voxel, 0.0f);
            const uint32_t count       = triangles.GetTriangleCount();
            RenderGraph*   graph       = m_device->GetCurrentFrameRDG();

            RDGComputePassDesc clear;
            clear.SetShaderProgramName("BrickClearSP");
            clear.SetPassTag("BrickClear");
            BindBuildInputs(clear, triangles, true);
            AddCellPass(std::move(clear));

            RDGComputePassDesc mark;
            mark.SetShaderProgramName("BrickMarkSP");
            mark.SetPassTag("BrickMark");
            BindBuildInputs(mark, triangles);
            AddTrianglePass(std::move(mark), count);

            RDGComputePassDesc finalize;
            finalize.SetShaderProgramName("BrickFinalizeSP");
            finalize.SetPassTag("BrickFinalize");
            BindBuildInputs(finalize, triangles);
            graph->AddComputePass(std::move(finalize)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(1, 1, 1);
            });

            RDGComputePassDesc markLarge;
            markLarge.SetShaderProgramName("BrickMarkLargeSP");
            markLarge.SetPassTag("BrickMarkLarge");
            BindBuildInputs(markLarge, triangles);
            AddLargeTrianglePass(std::move(markLarge));

            RDGComputePassDesc allocate;
            allocate.SetShaderProgramName("BrickAllocateSP");
            allocate.SetPassTag("BrickAllocate");
            BindBuildInputs(allocate, triangles);
            AddCellPass(std::move(allocate));

            RDGComputePassDesc rasterize;
            rasterize.SetShaderProgramName("BrickRasterizeSP");
            rasterize.SetPassTag("BrickRasterize");
            BindBuildInputs(rasterize, triangles);
            BindSceneRayGeometry(rasterize, *m_device, scene, false);
            AddTrianglePass(std::move(rasterize), count);

            RDGComputePassDesc rasterizeLarge;
            rasterizeLarge.SetShaderProgramName("BrickRasterizeLargeSP");
            rasterizeLarge.SetPassTag("BrickRasterizeLarge");
            BindBuildInputs(rasterizeLarge, triangles);
            BindSceneRayGeometry(rasterizeLarge, *m_device, scene, false);
            AddLargeTrianglePass(std::move(rasterizeLarge));

            RDGComputePassDesc prune;
            prune.SetShaderProgramName("BrickPruneSP");
            prune.SetPassTag("BrickPrune");
            BindBuildInputs(prune, triangles);
            AddCellPass(std::move(prune));

            // Separable Chebyshev distance: x from occupancy, then y, then z into the empty cells.
            RDGResourceManager* resources = graph->GetResourceManager();
            RDGBufferDesc       distanceDesc;
            distanceDesc.size = uint64_t(resolution) * resolution * resolution * sizeof(uint32_t);
            distanceDesc.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
            distanceDesc.name         = "brick_distance_x";
            const RDGBuffer distanceX = resources->CreateBuffer(distanceDesc);
            distanceDesc.name         = "brick_distance_y";
            const RDGBuffer distanceY = resources->CreateBuffer(distanceDesc);

            RDGComputePassDesc distance;
            distance.SetShaderProgramName("BrickDistanceXSP");
            distance.SetPassTag("BrickDistanceX");
            BindBuildInputs(distance, triangles);
            distance.BindStorageBuffer("BrickDistanceOutput", distanceX, RDGContentGuarantee::eFullWrite);
            AddCellPass(std::move(distance));

            distance = RDGComputePassDesc();
            distance.SetShaderProgramName("BrickDistanceYSP");
            distance.SetPassTag("BrickDistanceY");
            BindBuildInputs(distance, triangles);
            distance.BindStorageBuffer("BrickDistanceInput", distanceX);
            distance.BindStorageBuffer("BrickDistanceOutput", distanceY, RDGContentGuarantee::eFullWrite);
            AddCellPass(std::move(distance));

            distance = RDGComputePassDesc();
            distance.SetShaderProgramName("BrickDistanceZSP");
            distance.SetPassTag("BrickDistanceZ");
            BindBuildInputs(distance, triangles);
            distance.BindStorageBuffer("BrickDistanceInput", distanceY);
            AddCellPass(std::move(distance));

            graph->AddTransferPass("ReadBrickCounters")
                .CopyBuffer(m_counters, m_readback, {0, 0, ZEN_BRICK_COUNTER_WORDS * sizeof(uint32_t)})
                .NeverCull();

            m_recorded         = true;
            m_recordedRevision = triangles.GetRevision();
            m_grid             = grid;
        }
    }
    if (!valid)
    {
        m_built = false;
    }
    return valid && IsReady();
}

void BrickScene::BindTraceInputs(RDGPassDescBase& pass) const
{
    pass.BindValue("uBrickData", m_uniforms);
    pass.BindStorageBuffer("BrickCells", m_cells);
    pass.BindStorageBuffer("BrickBits", m_bits);
    pass.BindStorageBuffer("BrickMasks", m_masks);
}

void BrickScene::OnRenderGraphExecuted(RenderFrameResult result)
{
    PollStatistics();
    if (m_recorded && result == RenderFrameResult::eSubmitted)
    {
        m_built    = true;
        m_revision = m_recordedRevision;
        // Capture both accepted serials and queued CPU tickets: frame handoff is not yet
        // native submission in threaded mode, and submission itself is not GPU completion.
        m_pendingStatistics.push_back({m_readback, m_device->CaptureResourceRetirement(), m_resolution, m_capacity});
        m_readback = nullptr;
    }
    else if (m_recorded)
    {
        // A declined or failed build leaves the cells undefined: rebuild next time.
        m_built = false;
        m_device->DestroyBuffer(m_readback);
        m_readback = nullptr;
    }
    m_recorded = false;
}

void BrickScene::PollStatistics() const
{
    // The queue is in build order. Preserve that order so diagnostics always describe the
    // newest completed generation, even when builds used different submission queues.
    while (!m_pendingStatistics.empty() && m_device->IsResourceRetired(m_pendingStatistics.front().retirement))
    {
        const PendingStatistics& pending  = m_pendingStatistics.front();
        const uint32_t*          counters = reinterpret_cast<const uint32_t*>(pending.readback->Map());
        if (counters != nullptr)
        {
            m_statistics.largeTriangles  = counters[3];
            m_statistics.allocatedBricks = std::min(counters[4], pending.capacity);
            m_statistics.overflow        = counters[5] != 0;
            pending.readback->Unmap();
            if (m_statistics.overflow)
            {
                LOGW("Brick scene pool exhausted at resolution {} (capacity {}); unbacked cells block rays conservatively",
                     pending.resolution, pending.capacity);
            }
        }
        else
        {
            LOGE("Brick scene statistics readback could not be mapped");
        }
        m_device->DestroyBuffer(pending.readback);
        m_pendingStatistics.erase(m_pendingStatistics.begin());
    }
}

BrickSceneStatistics BrickScene::ReadStatistics() const
{
    PollStatistics();
    return m_statistics;
}

uint64_t BrickScene::GetResidentBytes() const
{
    const uint64_t cells = uint64_t(m_resolution) * m_resolution * m_resolution * sizeof(uint32_t);
    return m_cells != nullptr ? cells + uint64_t(m_capacity) * (ZEN_BRICK_WORDS + 1u) * sizeof(uint32_t)
                                    + uint64_t(m_largeCapacity) * sizeof(uint32_t)
                              : 0u;
}

void BrickScene::ReleaseBuildResources()
{
    m_device->DestroyBuffer(m_cells);
    m_device->DestroyBuffer(m_bits);
    m_device->DestroyBuffer(m_masks);
    m_device->DestroyBuffer(m_counters);
    m_device->DestroyBuffer(m_readback);
    m_device->DestroyBuffer(m_largeTriangles);
    m_cells          = nullptr;
    m_bits           = nullptr;
    m_masks          = nullptr;
    m_counters       = nullptr;
    m_readback       = nullptr;
    m_largeTriangles = nullptr;
    m_resolution     = 0;
    m_capacity       = 0;
    m_largeCapacity  = 0;
    m_built          = false;
    m_recorded       = false;
}

void BrickScene::Destroy()
{
    PollStatistics();
    for (const PendingStatistics& pending : m_pendingStatistics)
    {
        m_device->DestroyBuffer(pending.readback);
    }
    m_pendingStatistics.clear();
    m_statistics = {};
    ReleaseBuildResources();
}
} // namespace zen::rc
