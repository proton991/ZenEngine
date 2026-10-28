#pragma once

#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/Shared/DistanceFieldScene.h"
#include "Graphics/Shared/DistanceFieldScheduler.h"
#include "Graphics/RenderCore/V2/ResourceRetirement.h"
#include "Math/Math.h"
#include "Math/AABB.h"
#include "Utils/SharedPtr.h"
#include "Utils/UniquePtr.h"
#include <chrono>

namespace zen::rc
{
class RenderDevice;
class RenderScene;

// Matches distance_field_common.glsl. This initial builder has one fixed world level.
struct DistanceFieldSceneUniform
{
    Vec4       gridMinCellSize{0};
    glm::uvec4 dimensions{0};
    glm::uvec4 atlasDimensions{0};
    Vec4       tracing{0};
};
static_assert(sizeof(DistanceFieldSceneUniform) == 64);


// World boxes where the published field no longer matches committed geometry: the old and new bounds of
// instances edited since its snapshot, expanded by the field's hit band. Matches hybrid_provider.glsl.
struct DistanceFieldStaleUniform
{
    glm::uvec4 count{0}; // x boxes in use
    Vec4       minimum[ZEN_FIELD_STALE_BOXES]{};
    Vec4       maximum[ZEN_FIELD_STALE_BOXES]{};
};

struct DistanceFieldSceneSettings
{
    uint32_t cellsPerAxis{64};
    uint32_t atlasTexelsPerAxis{256};
    uint32_t triangleBatchSize{256};
    uint32_t cellBatchSize{64};
    uint32_t jobsPerDispatch{1024};
    uint32_t dispatchesPerFrame{1}; // Dispatches of each heavy phase within one construction cycle.
    // Construction cycles recorded per frame; each records every phase once. With a frame budget,
    // the count adapts so that construction lengthens the frame interval measured at the start of
    // each generation by at most the larger of frameBudgetMicroseconds and frameBudgetShare of that
    // interval, up to maxCyclesPerFrame. Without one, every frame records maxCyclesPerFrame cycles.
    uint32_t maxCyclesPerFrame{1};
    double   frameBudgetMicroseconds{0};
    double   frameBudgetShare{0};
    // Optional initial scene/local preparation budget. Dispatch sizes adapt to GPU timestamps;
    // subsequent edits use the smaller background batches and the ordinary frame budget.
    double   initialBuildBudgetMicroseconds{0};
    uint32_t referenceCapacity{1u << 20};
    uint32_t referencesPerCell{256};
    float    hitTolerance{0.25f / 7.0f}; // Brick units; requires scene/feature qualification.
    // Local detail volumes (SetLocalVolumes): cells per axis and atlas texels per slot side.
    uint32_t localCellsPerAxis{32};
    uint32_t localAtlasTexelsPerAxis{160};
    uint64_t persistentBudgetBytes{256ull * 1024 * 1024};
    uint64_t scratchBudgetBytes{128ull * 1024 * 1024};
};

// One local detail volume as hybrid traces read it (distance_field_trace.glsl, DistanceFieldLocalRecord).
struct DistanceFieldLocalRecord
{
    Vec4       gridMinCellSize{0};
    Vec4       authorityMinimum{0}; // Rays inside the authority box trace the volume instead of the scene field.
    Vec4       authorityMaximum{0};
    glm::uvec4 slot{0}; // x pool column, y pool row, z nonzero once published
};
static_assert(sizeof(DistanceFieldLocalRecord) == 64);

struct DistanceFieldLocalsUniform
{
    struct Node
    {
        Vec4 minimum{0}; // w: leaf slot + 1, or zero for an internal node
        Vec4 maximum{0}; // w: escape index in the preorder tree
    };
    glm::uvec4               header{0}; // x volumes, y cells per axis, z hierarchy levels, w atlas texels per slot side
    DistanceFieldLocalRecord volumes[ZEN_FIELD_LOCAL_VOLUMES]{};
    Node                     nodes[2 * ZEN_FIELD_LOCAL_VOLUMES - 1]{};
};

struct DistanceFieldSceneFailure
{
    uint32_t                  flags{0};
    uint32_t                  site{0};
    uint32_t                  triangle{UINT32_MAX};
    uint32_t                  cell{UINT32_MAX};
    uint32_t                  sample{UINT32_MAX};
    uint64_t                  generation{0};
    DistanceFieldSceneUniform uniform;
};

// Bounded static-region construction with brick recycling and a separate dynamic region. Published
// data remains available outside invalidated bounds while immutable replacement snapshots build;
// a build in flight completes before the next starts, so continuous edits still publish.
class DistanceFieldScene
{
public:
    enum class State
    {
        Pending,
        Ready,
        Failed
    };

    explicit DistanceFieldScene(RenderDevice* device, const DistanceFieldSceneSettings& settings = {});
    ~DistanceFieldScene();
    DistanceFieldScene(const DistanceFieldScene&)            = delete;
    DistanceFieldScene& operator=(const DistanceFieldScene&) = delete;

    // Call after successful RenderScene::Update, inside a begun render graph. Records
    // bounded GPU phase cycles (DistanceFieldSceneSettings::maxCyclesPerFrame). Every
    // recorded graph requires the callback.
    State BuildRenderGraph(RenderScene& scene);
    void  OnRenderGraphExecuted(RenderFrameResult result);
    bool  BindTraceInputs(RDGPassDescBase& pass) const;
    // Whether the published generation matches the scene's committed geometry, with no stale region.
    bool IsCurrent(const RenderScene& scene) const;
    // Stale regions of the published generation, refreshed by BuildRenderGraph.
    const DistanceFieldStaleUniform& GetStaleRegions() const
    {
        return m_stale;
    }
    // Finer volumes around detail the scene field cannot resolve, such as lanterns around lights; at
    // most ZEN_FIELD_LOCAL_VOLUMES. Each is built after the scene field from the same geometry, copied
    // into a pool slot, and traced inside its core expanded by the scene field's hit band. Volumes
    // that are not yet built, failed, or were touched by an edit fall back to the scene field.
    // Dynamic geometry uses an additional mandatory slot: it remains unavailable until published.
    // Returns false for an invalid or oversized set; an unchanged set keeps the published volumes.
    bool     SetLocalVolumes(const HeapVector<AABB>& cores);
    uint32_t GetReadyLocalVolumeCount() const;
    // Whether a local volume is still queued, building or being copied into the pool.
    bool LocalWorkRemains() const;
    // Changes whenever a local volume is published or falls back to the scene field.
    uint32_t GetLocalRevision() const
    {
        return m_localRevision;
    }
    const DistanceFieldLocalsUniform& GetLocalsUniform() const
    {
        return m_locals;
    }
    // Trace inputs of a hybrid GI pass: the scene field, its stale regions and the local volumes.
    bool BindCompositeInputs(RDGPassDescBase& pass) const;
    void Destroy();

    State       GetState() const;
    const char* GetFailureReason() const;
    uint64_t    GetGeneration() const;
    uint64_t    GetSnapshotBytes() const;
    uint64_t    GetSubmittedGraphCount() const;
    // Latest completed counter; includes rejected allocation requests after atlas exhaustion.
    uint32_t GetAllocatedBrickCount() const;
    // Published cells whose material coverage could not be resolved, summed across active levels.
    uint32_t GetUnavailableCellCount() const;
    uint32_t GetBuildRetryCount() const;
    uint32_t GetProcessedTriangleCount() const;
    uint32_t GetTriangleBatchQuota() const;
    // Unique retained source/topology buffer allocations; shared scene inputs, not new scratch.
    uint64_t                         GetRetainedInputBytes() const;
    const DistanceFieldSceneFailure& GetFailureDetails() const;
    // Native buffer/image allocation extents, including retained generations.
    // Allocator pools and residency need separate VMA measurement.
    uint64_t                         GetPersistentBytes() const;
    uint64_t                         GetScratchBytes() const;
    const DistanceFieldSceneUniform& GetUniform() const;

private:
    struct Generation;
    struct GeometrySnapshot;
    struct BuildWorkspace;
    struct LocalSlot
    {
        AABB core;
        Vec4 gridMinCellSize{0};
        // Committed instance bounds and revisions of the published volume, which edits are compared with.
        HeapVector<AABB>     bounds;
        HeapVector<uint64_t> revisions;
        bool                 ready{false};
        bool                 queued{true};
        bool                 failed{false};
        uint32_t             unavailableCells{0};
    };
    bool     Start(RenderScene& scene, bool local = false);
    uint32_t NextCycleCount();
    bool     IsInitialBuild() const;
    void     UpdateDispatchBudgets();
    bool     SetVolumeCores(const HeapVector<AABB>& cores);
    void     UpdateDynamicVolume(const RenderScene& scene);
    AABB     LocalAuthority(const AABB& core) const;
    void     StartNextLocal(RenderScene& scene);
    void     RecordLocalCopy();
    void     UpdateLocalVolumes(const RenderScene& scene);
    bool     LocalVolumeCurrent(const LocalSlot& slot, const RenderScene& scene) const;
    uint32_t BuildLocalTree(HeapVector<uint32_t>& slots, uint32_t first, uint32_t end, uint32_t node);
    bool     CreateLocalPool(uint32_t slots);
    void     ReleaseLocalPool();
    uint64_t GetLocalPoolBytes() const;
    void     Poll();
    void     ReleaseSnapshotSources(Generation& generation);
    void     Fail(const char* reason, bool local = false);
    void     Retire(UniquePtr<Generation>& generation);
    void     Release(Generation& generation, bool completed = false);
    void     ReleaseScratch(Generation& generation, bool completed = false);
    void     ReleaseGeometry(SharedPtr<GeometrySnapshot>& geometry, bool completed);
    void     ReleaseWorkspace(SharedPtr<BuildWorkspace>& workspace, bool completed);
    void     RetireSharedInputs();
    void     CollectBuildInputs(HeapVector<const GeometrySnapshot*>& geometries,
                                HeapVector<const BuildWorkspace*>&   workspaces) const;
    void     BindBuild(RDGPassDescBase& pass, const Generation& generation) const;
    void     BindGeometry(RDGPassDescBase& pass, const Generation& generation) const;
    void     RecordSnapshot(Generation& generation);
    void     RecordBuild(Generation& generation);
    void     UpdateStaleRegions(const RenderScene& scene);

    RenderDevice*                     m_device;
    DistanceFieldSceneSettings        m_settings;
    UniquePtr<Generation>             m_active;
    UniquePtr<Generation>             m_pending;
    UniquePtr<Generation>             m_local;
    SharedPtr<GeometrySnapshot>       m_geometry;
    SharedPtr<BuildWorkspace>         m_workspace;
    HeapVector<UniquePtr<Generation>> m_retired;
    State                             m_state{State::Pending};
    const char*                       m_failureReason{""};
    uint64_t                          m_nextGeneration{1};
    uint64_t                          m_lastSnapshotBytes{0};
    uint64_t                          m_lastSubmittedGraphs{0};
    uint32_t                          m_lastAllocatedBricks{0};
    uint32_t                          m_lastBuildRetries{0};
    uint32_t                          m_lastProcessedTriangles{0};
    uint32_t                          m_lastTriangleQuota{0};
    DistanceFieldSceneFailure         m_failureDetails;
    DistanceFieldStaleUniform         m_stale;
    // Local volumes: one slot each in the pool's cell, hierarchy and atlas resources.
    HeapVector<LocalSlot>      m_slots;
    HeapVector<AABB>           m_detailCores;
    AABB                       m_dynamicCore;
    bool                       m_hasDynamic{false};
    uint32_t                   m_nextLocalSlot{0};
    RHIBuffer*                 m_poolCells{nullptr};
    RHIBuffer*                 m_poolHierarchy{nullptr};
    RHITexture*                m_poolAtlas{nullptr};
    uint32_t                   m_poolSlots{0}; // Volumes the pool holds; it exists while m_poolAtlas does.
    uint32_t                   m_poolColumns{1};
    int32_t                    m_buildingSlot{-1}; // Slot of the local generation being built.
    int32_t                    m_copySlot{-1};     // Slot the published m_local is copied into.
    bool                       m_copyRecorded{false};
    uint32_t                   m_localRevision{0};
    DistanceFieldLocalsUniform m_locals;
    uint64_t                   m_failedScene{0};
    uint64_t                   m_failedRevision{0};
    bool                       m_recorded{false};
    AABB                       m_localCore;       // Core of the local volume being built.
    uint64_t                   m_localRequest{0}; // Bumped by each new volume set.
    uint32_t                   m_cyclesPerFrame{1};
    bool                       m_initialBuild{true};
    uint32_t                   m_dispatchLimits[ZEN_FIELD_PHASE_COMPLETE]{};
    struct DispatchTiming
    {
        RHIGPUTimingPtr result;
        uint64_t        generation;
        uint32_t        phase;
        uint32_t        limit;
    };
    HeapVector<DispatchTiming> m_dispatchTimings;
    uint32_t                   m_recordedTimings{0}; // Tail samples belonging to the unsubmitted graph.
    uint32_t                   m_recordedCycles{0};  // Cycles in this frame's graph, until it executes.
    uint32_t                   m_baselineFrames{0};  // Frames of this generation measured at one cycle.
    double                     m_baselineUs{0};      // Frame interval with one cycle per frame.
    double                     m_frameIntervalUs{0}; // Smoothed interval between BuildRenderGraph calls.

    std::chrono::steady_clock::time_point m_lastFrame{};
};
} // namespace zen::rc
