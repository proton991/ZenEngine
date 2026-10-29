#include "Graphics/RenderCore/V2/DistanceFieldScene.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "Graphics/Shared/DistanceFieldScene.h"
#include "Graphics/Shared/DistanceFieldScheduler.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstring>

namespace zen::rc
{
namespace
{
constexpr uint32_t kSnapshotBuffers = 4;
constexpr uint32_t kStatusSlots     = RenderFrameState::kMaxFramesInFlight + 1;
// Frames of each generation that run one cycle and measure the frame interval it adapts from.
constexpr uint32_t              kBaselineFrames = 8;
const DistanceFieldSceneUniform kEmptyUniform{};

enum class BuildStage
{
    Snapshot,
    Building,
    AwaitCompletion
};

struct FieldBuildUniform
{
    glm::uvec4 triangleRange{0};
    glm::uvec4 limits{0};
    glm::uvec4 workRange{0};
    glm::uvec4 hierarchy{0};
};
static_assert(sizeof(FieldBuildUniform) == 64);

// Closed-interval overlap; an empty box (minimum above maximum) overlaps nothing.
bool BoxesOverlap(const AABB& a, const AABB& b)
{
    return glm::all(glm::lessThanEqual(a.GetMin(), b.GetMax())) && glm::all(glm::lessThanEqual(b.GetMin(), a.GetMax()));
}

uint32_t HierarchyWords(uint32_t side)
{
    uint32_t words = 0;
    while (side != 0)
    {
        words += side * side * side;
        side  /= 2;
    }
    return words;
}

uint64_t BufferBytes(const RHIBuffer* buffer)
{
    return buffer != nullptr ? std::max(buffer->GetRequiredSize(), buffer->GetAllocationSize()) : 0;
}

// The emit packs a dense cell's reference chunk into 7 bits of its active entry.
static_assert(ZEN_FIELD_MAX_CELL_REFS / ZEN_FIELD_EMIT_CHUNK_REFERENCES <= 128);

// Emit entries a workspace holds: one per cell, plus one per further reference chunk of a dense
// cell. A batch's references, at most the reference capacity, bound the chunks and dense cells.
uint64_t ActiveEntries(uint64_t cells, uint32_t references)
{
    return cells + references / ZEN_FIELD_EMIT_CHUNK_REFERENCES;
}

uint64_t SplitSlots(uint32_t references)
{
    return std::max<uint64_t>(references / ZEN_FIELD_EMIT_CHUNK_REFERENCES, 1);
}

// Elements of the given stride that one storage-buffer binding can address (ValidateGIStorageBuffer).
uint64_t StorageElements(const RHIGPUInfo& gpu, uint32_t stride)
{
    const uint64_t alignment = std::max(uint64_t(1), uint64_t(gpu.storageBufferAlignment));
    const uint64_t range     = std::min(uint64_t(gpu.maxStorageBufferRange), uint64_t(UINT32_MAX) - (alignment - 1));
    return range / stride;
}

// Device-local bytes still free for new allocations: the driver's budget headroom where it reports
// one, otherwise the capacity this allocator has not committed. UINT64_MAX when neither is known.
uint64_t FreeDeviceMemory(const RenderDevice& device)
{
    const RHIGPUMemoryStats memory    = device.GetGPUMemoryStats();
    const uint64_t          capacity  = device.GetGPUInfo().deviceLocalMemoryBytes;
    uint64_t                available = capacity == 0                      ? UINT64_MAX
                                      : capacity > memory.deviceLocalBytes ? capacity - memory.deviceLocalBytes
                                                                           : 0;
    if (memory.budgetAvailable)
    {
        uint64_t headroom = 0;
        for (uint32_t index = 0; index < memory.heapCount && index < memory.heaps.size(); ++index)
        {
            const RHIGPUMemoryStats::Heap& heap = memory.heaps[index];
            if (heap.deviceLocal && heap.budgetBytes > heap.usageBytes)
            {
                headroom = std::max(headroom, heap.budgetBytes - heap.usageBytes);
            }
        }
        available = std::min(available, headroom);
    }
    return available;
}

uint32_t SamplerCoverageFlags(const RHISamplerCreateInfo& sampler)
{
    const bool supported = sampler.minLod == 0.0f && sampler.lodBias == 0.0f && sampler.maxLod >= 0.0f && !sampler.useAnisotropy
                        && !sampler.enableCompare && !sampler.unnormalizedUVW;
    return (sampler.magFilter == RHISamplerFilter::eLinear ? 1u : 0u) | (supported ? 2u : 0u);
}

RHIBuffer* CreateFieldBuffer(RenderDevice&                    device,
                             uint64_t                         bytes,
                             NameID                           name,
                             bool                             readback        = false,
                             BitField<RHIBufferUsageFlagBits> additionalUsage = {})
{
    RHIBufferCreateInfo info;
    info.usageFlags   = additionalUsage;
    info.size         = bytes;
    info.tag          = name;
    info.allocateType = readback ? RHIBufferAllocateType::eCPURead : RHIBufferAllocateType::eGPU;
    info.usageFlags.SetFlag(readback ? RHIBufferUsageFlagBits::eTransferDstBuffer : RHIBufferUsageFlagBits::eStorageBuffer);
    if (!readback)
    {
        info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferSrcBuffer);
    }
    return device.CreateBuffer(info);
}

void DispatchField(RenderGraph& graph, RDGComputePassDesc&& pass, uint32_t groups)
{
    pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    const uint32_t groupsX = std::min(groups, 65535u);
    const uint32_t groupsY = (groups + groupsX - 1u) / groupsX;
    graph.AddComputePass(std::move(pass)).RecordPassCommands([groupsX, groupsY](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(groupsX, groupsY, 1);
    });
}
} // namespace

struct DistanceFieldScene::GeometrySnapshot
{
    uint64_t                                   scene{0};
    uint64_t                                   revision{0};
    uint64_t                                   bytes{0};
    uint64_t                                   snapshotBytes{0};
    uint32_t                                   triangleCount{0};
    bool                                       hasCoverageMaterials{false};
    RHIBuffer*                                 snapshots[kSnapshotBuffers]{};
    RHIResourcePtr<RHIBuffer>                  sources[kSnapshotBuffers];
    RHIResourcePtr<RHIBuffer>                  indices;
    RHIResourcePtr<RHIBuffer>                  triangles;
    HeapVector<RHIResourcePtr<RHITexture>>     textures;
    HeapVector<RHIResourcePtr<RHITextureView>> views;
    HeapVector<RHIResourcePtr<RHISampler>>     samplers;
    RHIBuffer*                                 samplerInfo{nullptr};
    ResourceRetirement                         retirement;
    ResourceRetirement                         snapshotRetirement;
    bool                                       snapshotSubmitted{false};
    bool                                       snapshotSourcesReleased{false};

    void AppendRetainedInputs(HeapVector<RHIBuffer*>& buffers) const
    {
        for (const RHIResourcePtr<RHIBuffer>& source : sources)
        {
            if (source)
            {
                buffers.push_back(source.Get());
            }
        }
        for (RHIBuffer* buffer : {indices.Get(), triangles.Get()})
        {
            if (buffer != nullptr)
            {
                buffers.push_back(buffer);
            }
        }
    }
};

struct DistanceFieldScene::BuildWorkspace
{
    struct StatusSlot
    {
        RHIBuffer*         buffer{nullptr};
        ResourceRetirement retirement;
        uint64_t           graph{0};
        bool               pending{false};
    };
    uint64_t           bytes{0};
    uint64_t           cells{0}; // Cell capacity of its per-cell arrays; smaller local volumes reuse it.
    uint64_t           bricks{0};
    uint32_t           jobCapacity{0};
    uint32_t           referenceCapacity{0};
    uint32_t           coverageSlots{0};
    bool               memoryLimited{false}; // Capacities were scaled down to the memory budget.
    RHIBuffer*         brickUsed{nullptr};
    RHIBuffer*         freeBricks{nullptr};
    RHIBuffer*         counters{nullptr};
    RHIBuffer*         jobs{nullptr};
    RHIBuffer*         counts{nullptr};
    RHIBuffer*         offsets{nullptr};
    RHIBuffer*         cursors{nullptr};
    RHIBuffer*         references{nullptr};
    RHIBuffer*         active{nullptr};
    RHIBuffer*         scan{nullptr};
    RHIBuffer*         control{nullptr};
    RHIBuffer*         indirect{nullptr};
    RHIBuffer*         coverage{nullptr};
    RHIBuffer*         coverageQueue{nullptr};
    RHIBuffer*         emitSplit{nullptr};
    StatusSlot         status[kStatusSlots];
    ResourceRetirement retirement;
};

struct DistanceFieldScene::Generation
{
    uint64_t                              scene{0};
    uint64_t                              revision{0};
    uint64_t                              generation{0};
    uint64_t                              request{0};
    uint64_t                              snapshotBytes{0};
    uint64_t                              persistentBytes{0};
    uint64_t                              submittedGraphs{0};
    uint64_t                              submittedCycles{0};
    bool                                  local{false};
    bool                                  partial{false};
    DistanceFieldStaleUniform             dirty;
    std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
    DistanceFieldSceneUniform             uniform;
    FieldBuildUniform                     build;
    RHIBuffer*                            cells{nullptr};
    RHIBuffer*                            hierarchy{nullptr};
    RHITexture*                           atlas{nullptr};
    RHIBuffer*                            ranges{nullptr};
    SharedPtr<GeometrySnapshot>           geometry;
    SharedPtr<BuildWorkspace>             workspace;
    int32_t                               recordedStatus{-1};
    uint64_t                              lastStatusGraph{0};
    uint64_t                              lastProgressGraph{0};
    ResourceRetirement                    retirement;
    BuildStage                            stage{BuildStage::Snapshot};
    uint32_t                              triangleCount{0};
    uint32_t                              unavailableCells{0};
    uint32_t                              allocatedBricks{0};
    uint32_t                              buildRetries{0};
    uint32_t                              processedTriangles{0};
    uint32_t                              triangleQuota{0};
    // Committed instance bounds and revisions at the snapshot, which later edits are compared with.
    HeapVector<RenderScene::ShadowCaster> casters;
};

DistanceFieldScene::DistanceFieldScene(RenderDevice* device, const DistanceFieldSceneSettings& settings) :
    m_device(device), m_settings(settings)
{}

DistanceFieldScene::~DistanceFieldScene()
{
    Destroy();
}

DistanceFieldScene::State DistanceFieldScene::GetState() const
{
    return m_state;
}

bool DistanceFieldScene::SetLocalVolumes(const HeapVector<AABB>& cores)
{
    HeapVector<AABB> combined = cores;
    if (m_hasDynamic)
    {
        combined.push_back(m_dynamicCore);
    }
    const bool valid = SetVolumeCores(combined);
    if (valid)
    {
        m_detailCores = cores;
    }
    return valid;
}

void DistanceFieldScene::UpdateDynamicVolume(const RenderScene& scene)
{
    Vec3                                                  minimum(std::numeric_limits<float>::max());
    Vec3                                                  maximum(std::numeric_limits<float>::lowest());
    const HeapVector<RenderScene::InstanceTriangleRange>& ranges  = scene.GetInstanceTriangleRanges();
    const HeapVector<RenderScene::ShadowCaster>&          casters = scene.GetShadowCasters();
    bool                                                  dynamic = false;
    for (size_t index = 0; index < ranges.size(); ++index)
    {
        if ((ranges[index].committedMask & GI_DYNAMIC) != 0 && ranges[index].triangleCount != 0)
        {
            minimum = glm::min(minimum, casters[index].bounds.GetMin());
            maximum = glm::max(maximum, casters[index].bounds.GetMax());
            dynamic = true;
        }
    }
    if (dynamic
        && (!m_hasDynamic || !glm::all(glm::greaterThanEqual(minimum, m_dynamicCore.GetMin()))
            || !glm::all(glm::lessThanEqual(maximum, m_dynamicCore.GetMax()))))
    {
        // A loose region stays fixed during ordinary motion, so an in-flight build is not restarted.
        // It contains static neighbours too; its field replaces the static base only within this box.
        const Vec3  size   = maximum - minimum;
        const float extent = std::max({0.01f, size.x, size.y, size.z});
        const Vec3  padding(extent * 0.5f);
        m_dynamicCore = AABB(minimum - padding, maximum + padding);
    }
    m_hasDynamic           = dynamic;
    HeapVector<AABB> cores = m_detailCores;
    if (dynamic)
    {
        cores.push_back(m_dynamicCore);
    }
    if (!SetVolumeCores(cores))
    {
        Fail("dynamic visibility region exceeds local volume capacity");
    }
}

bool DistanceFieldScene::SetVolumeCores(const HeapVector<AABB>& cores)
{
    bool valid = cores.size() <= ZEN_FIELD_LOCAL_VOLUMES;
    for (const AABB& core : cores)
    {
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            valid = valid && std::isfinite(core.GetMin()[axis]) && std::isfinite(core.GetMax()[axis])
                 && core.GetMax()[axis] > core.GetMin()[axis];
        }
    }
    if (valid && cores.size() == m_slots.size())
    {
        // A moved volume rebuilds alone, dropping its build in flight or its unrecorded copy.
        for (size_t index = 0; index < cores.size(); ++index)
        {
            if (cores[index] != m_slots[index].core)
            {
                if (m_buildingSlot == static_cast<int32_t>(index))
                {
                    if (m_pending && m_pending->local)
                    {
                        Retire(m_pending);
                    }
                    m_buildingSlot = -1;
                }
                if (m_copySlot == static_cast<int32_t>(index) && !m_copyRecorded)
                {
                    Retire(m_local);
                }
                m_copySlot     = m_copySlot == static_cast<int32_t>(index) ? -1 : m_copySlot;
                m_slots[index] = LocalSlot{cores[index]};
                ++m_localRevision;
            }
        }
    }
    else if (valid)
    {
        // A new set rebuilds every volume; the in-flight local build and any pending copy are dropped.
        if (m_pending && m_pending->local)
        {
            Retire(m_pending);
        }
        Retire(m_local);
        m_buildingSlot  = -1;
        m_copySlot      = -1;
        m_copyRecorded  = false;
        m_nextLocalSlot = cores.empty() ? 0u : static_cast<uint32_t>(cores.size() - 1);
        m_slots.clear();
        for (const AABB& core : cores)
        {
            LocalSlot slot;
            slot.core = core;
            m_slots.push_back(slot);
        }
        ReleaseLocalPool();
        ++m_localRequest;
        ++m_localRevision;
    }
    return valid;
}

uint32_t DistanceFieldScene::GetReadyLocalVolumeCount() const
{
    uint32_t count = 0;
    for (const LocalSlot& slot : m_slots)
    {
        count += slot.ready ? 1u : 0u;
    }
    return count;
}

AABB DistanceFieldScene::LocalAuthority(const AABB& core) const
{
    float guard = 0.0f;
    if (m_active)
    {
        // Cover the scene field's hit band, a full trilinear stencil diagonal and one R8 quantization
        // step: outside the authority box, scene-field surfaces inside the core cannot cause a hit.
        guard =
            m_active->uniform.gridMinCellSize.w * (2.0f * m_active->uniform.tracing.x + std::sqrt(3.0f) / 7.0f + 1.0f / 255.0f);
    }
    return AABB(core.GetMin() - Vec3(guard), core.GetMax() + Vec3(guard));
}

bool DistanceFieldScene::CreateLocalPool(uint32_t slots)
{
    ReleaseLocalPool();
    bool           valid  = false;
    const uint64_t side   = m_settings.localCellsPerAxis;
    const uint64_t texels = m_settings.localAtlasTexelsPerAxis;
    const uint64_t slotBytes =
        texels * texels * texels + (side * side * side + HierarchyWords(static_cast<uint32_t>(side))) * sizeof(uint32_t);
    const uint64_t columns = std::min(std::max(slots, 1u), 8u);
    const uint64_t tiles   = ((slots + columns - 1) / columns) * columns;
    // Keep space for the temporary volume copied into the pool. A permanently oversized pool
    // uses the valid scene field; an overlap with retiring resources only defers allocation.
    const uint64_t estimate = slots != 0 ? tiles * slotBytes : 65536;
    const uint64_t resident = m_active ? m_active->persistentBytes : 0;
    const bool     fits     = resident + estimate + (slots != 0 ? slotBytes : 0) <= m_settings.persistentBudgetBytes;
    const bool     waiting =
        fits && GetPersistentBytes() + estimate + (slots != 0 ? slotBytes : 0) > m_settings.persistentBudgetBytes;
    if (!fits)
    {
        LOGW("Distance field local pool exceeds the persistent budget for {} volumes; using the scene field", slots);
        for (LocalSlot& slot : m_slots)
        {
            slot.failed = true;
        }
        if (m_hasDynamic)
        {
            Fail("dynamic visibility region exceeds the persistent budget");
        }
    }
    // Without volumes, or when their pool does not fit, a minimal pool keeps the composite bindings
    // valid; traces never read it. A failed pool leaves every volume on the scene field.
    for (uint32_t attempt = fits ? 0u : 1u; attempt < 2 && !valid && !waiting; ++attempt)
    {
        const uint32_t volumes        = attempt == 0 ? slots : 0;
        const uint32_t side           = volumes != 0 ? m_settings.localCellsPerAxis : 1;
        const uint32_t texels         = volumes != 0 ? m_settings.localAtlasTexelsPerAxis : 1;
        const uint32_t count          = std::max(volumes, 1u);
        m_poolColumns                 = std::min(count, 8u);
        const uint32_t rows           = (count + m_poolColumns - 1) / m_poolColumns;
        const uint64_t cellBytes      = uint64_t(count) * side * side * side * sizeof(uint32_t);
        const uint64_t hierarchyBytes = uint64_t(count) * HierarchyWords(side) * sizeof(uint32_t);
        m_poolCells     = CreateFieldBuffer(*m_device, cellBytes, "field_local_cells", false,
                                            BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer));
        m_poolHierarchy = CreateFieldBuffer(*m_device, hierarchyBytes, "field_local_hierarchy", false,
                                            BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer));
        RHITextureCreateInfo texture;
        texture.type   = RHITextureType::e3D;
        texture.width  = texels * m_poolColumns;
        texture.height = texels * rows;
        texture.depth  = texels;
        texture.format = DataFormat::eR8UNORM;
        texture.tag    = "field_local_atlas";
        texture.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eTransferDst);
        m_poolAtlas = m_device->CreateTexture(texture);
        valid       = m_poolCells != nullptr && m_poolHierarchy != nullptr && m_poolAtlas != nullptr;
        valid       = valid && GetPersistentBytes() <= m_settings.persistentBudgetBytes;
        if (valid)
        {
            // Copies define one slot at a time; defining every byte once lets passes bind the whole
            // pool with known contents, although they never read an unpublished slot.
            const HeapVector<uint8_t> zeros(std::max(cellBytes, hierarchyBytes), 0);
            valid = m_device->UpdateBuffer(m_poolCells, static_cast<uint32_t>(cellBytes), zeros.data())
                 && m_device->UpdateBuffer(m_poolHierarchy, static_cast<uint32_t>(hierarchyBytes), zeros.data());
            m_device->GetCurrentFrameRDG()
                ->AddTransferPass("FieldClearLocalPool")
                .ClearTexture(m_poolAtlas, Color(0.0f))
                .NeverCull();
        }
        if (!valid)
        {
            LOGE("Distance field local volume pool allocation failed for {} volumes", volumes);
            ReleaseLocalPool();
            for (LocalSlot& slot : m_slots)
            {
                slot.failed = true;
            }
        }
    }
    m_poolSlots = valid ? slots : 0;
    return valid;
}

void DistanceFieldScene::ReleaseLocalPool()
{
    if (m_device != nullptr && (m_poolCells != nullptr || m_poolHierarchy != nullptr || m_poolAtlas != nullptr))
    {
        // Reuse generation retirement so replaced pools stay charged through actual GPU completion.
        UniquePtr<Generation> holder = MakeUnique<Generation>();
        holder->persistentBytes      = GetLocalPoolBytes();
        holder->cells                = m_poolCells;
        holder->hierarchy            = m_poolHierarchy;
        holder->atlas                = m_poolAtlas;
        Retire(holder);
    }
    m_poolCells     = nullptr;
    m_poolHierarchy = nullptr;
    m_poolAtlas     = nullptr;
    m_poolSlots     = 0;
    for (LocalSlot& slot : m_slots)
    {
        slot.ready  = false;
        slot.queued = !slot.failed;
    }
}

uint64_t DistanceFieldScene::GetLocalPoolBytes() const
{
    return BufferBytes(m_poolCells) + BufferBytes(m_poolHierarchy)
         + (m_poolAtlas != nullptr ? m_poolAtlas->GetAllocationSize() : 0);
}

bool DistanceFieldScene::LocalWorkRemains() const
{
    bool remains = m_buildingSlot >= 0 || m_copySlot >= 0;
    for (const LocalSlot& slot : m_slots)
    {
        remains = remains || (slot.queued && !slot.failed);
    }
    return remains;
}

void DistanceFieldScene::StartNextLocal(RenderScene& scene)
{
    for (size_t visited = 0; visited < m_slots.size() && m_buildingSlot < 0; ++visited)
    {
        const size_t index = (m_nextLocalSlot + visited) % m_slots.size();
        LocalSlot&   slot  = m_slots[index];
        if (slot.queued && !slot.failed)
        {
            slot.queued    = false;
            m_buildingSlot = static_cast<int32_t>(index);
            m_localCore    = slot.core;
            if (!Start(scene, true) && !slot.failed)
            {
                // Budget or workspace retirement deferred it; retry on a later frame.
                slot.queued    = true;
                m_buildingSlot = -1;
            }
            else
            {
                m_nextLocalSlot = static_cast<uint32_t>((index + 1) % m_slots.size());
            }
        }
    }
}

void DistanceFieldScene::RecordLocalCopy()
{
    const uint32_t       side   = m_settings.localCellsPerAxis;
    const uint32_t       texels = m_settings.localAtlasTexelsPerAxis;
    const uint64_t       cells  = uint64_t(side) * side * side * sizeof(uint32_t);
    const uint64_t       words  = HierarchyWords(side) * sizeof(uint32_t);
    const uint32_t       slot   = static_cast<uint32_t>(m_copySlot);
    RHITextureCopyRegion region;
    region.srcOffset = Vec3i(0);
    region.dstOffset =
        Vec3i(static_cast<int32_t>(slot % m_poolColumns * texels), static_cast<int32_t>(slot / m_poolColumns * texels), 0);
    region.size = Vec3i(static_cast<int32_t>(texels));
    region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    m_device->GetCurrentFrameRDG()
        ->AddTransferPass("FieldPublishLocalVolume")
        .CopyBuffer(m_local->cells, m_poolCells, {0, slot * cells, cells})
        .CopyBuffer(m_local->hierarchy, m_poolHierarchy, {0, slot * words, words})
        .CopyTexture(m_local->atlas, m_poolAtlas, MakeVecView(&region, 1))
        .NeverCull();
    m_copyRecorded = true;
}

bool DistanceFieldScene::LocalVolumeCurrent(const LocalSlot& slot, const RenderScene& scene) const
{
    const HeapVector<RenderScene::ShadowCaster>& current   = scene.GetShadowCasters();
    bool                                         valid     = slot.ready && slot.revisions.size() == current.size();
    const AABB                                   authority = LocalAuthority(slot.core);
    for (size_t instance = 0; valid && instance < current.size(); ++instance)
    {
        if (current[instance].revision != slot.revisions[instance])
        {
            const AABB after(current[instance].bounds.GetMin(), current[instance].bounds.GetMax());
            valid = !BoxesOverlap(slot.bounds[instance], authority) && !BoxesOverlap(after, authority);
        }
    }
    return valid;
}

void DistanceFieldScene::UpdateLocalVolumes(const RenderScene& scene)
{
    m_locals        = {};
    uint32_t levels = 1;
    for (uint32_t levelSide = m_settings.localCellsPerAxis; levelSide > 1; levelSide /= 2)
    {
        ++levels;
    }
    m_locals.header = glm::uvec4(static_cast<uint32_t>(m_slots.size()), m_settings.localCellsPerAxis, levels,
                                 m_settings.localAtlasTexelsPerAxis);
    HeapVector<uint32_t> readySlots;
    for (size_t index = 0; index < m_slots.size(); ++index)
    {
        LocalSlot& slot      = m_slots[index];
        const AABB authority = LocalAuthority(slot.core);
        // An edited instance overlapping the authority box, before or after the edit, rebuilds it.
        const bool edited = slot.ready && !LocalVolumeCurrent(slot, scene);
        if (edited)
        {
            slot.ready  = false;
            slot.queued = true;
            ++m_localRevision;
        }
        DistanceFieldLocalRecord& record = m_locals.volumes[index];
        record.gridMinCellSize           = slot.gridMinCellSize;
        record.authorityMinimum          = Vec4(authority.GetMin(), 0);
        record.authorityMaximum          = Vec4(authority.GetMax(), 0);
        const uint32_t unavailable       = m_hasDynamic && index == m_detailCores.size() ? 2u : 0u;
        record.slot = glm::uvec4(static_cast<uint32_t>(index) % m_poolColumns, static_cast<uint32_t>(index) / m_poolColumns,
                                 slot.ready && m_state == State::Ready ? 1u : unavailable, 0u);
        if (record.slot.z != 0)
        {
            readySlots.push_back(static_cast<uint32_t>(index));
        }
    }
    if (!readySlots.empty())
    {
        BuildLocalTree(readySlots, 0, static_cast<uint32_t>(readySlots.size()), 0);
    }
}

uint32_t DistanceFieldScene::BuildLocalTree(HeapVector<uint32_t>& slots, uint32_t first, uint32_t end, uint32_t node)
{
    Vec3 minimum(std::numeric_limits<float>::max());
    Vec3 maximum(std::numeric_limits<float>::lowest());
    for (uint32_t index = first; index < end; ++index)
    {
        const DistanceFieldLocalRecord& volume = m_locals.volumes[slots[index]];
        minimum                                = glm::min(minimum, Vec3(volume.authorityMinimum));
        maximum                                = glm::max(maximum, Vec3(volume.authorityMaximum));
    }
    uint32_t   next              = node + 1;
    const bool leaf              = end - first == 1;
    m_locals.nodes[node].minimum = Vec4(minimum, leaf ? static_cast<float>(slots[first] + 1) : 0.0f);
    if (!leaf)
    {
        const Vec3 extent = maximum - minimum;
        uint32_t   axis   = extent.x > extent.y ? 0u : 1u;
        axis              = extent[axis] > extent.z ? axis : 2u;
        std::sort(slots.begin() + first, slots.begin() + end, [this, axis](uint32_t a, uint32_t b) {
            return m_slots[a].core.GetCenter()[axis] < m_slots[b].core.GetCenter()[axis];
        });
        const uint32_t middle = first + (end - first) / 2;
        next                  = BuildLocalTree(slots, first, middle, next);
        next                  = BuildLocalTree(slots, middle, end, next);
    }
    m_locals.nodes[node].maximum = Vec4(maximum, static_cast<float>(next));
    return next;
}

bool DistanceFieldScene::BindCompositeInputs(RDGPassDescBase& pass) const
{
    const bool ready = BindTraceInputs(pass) && m_poolAtlas != nullptr;
    if (ready)
    {
        RHISamplerCreateInfo sampler;
        sampler.minFilter = RHISamplerFilter::eLinear;
        sampler.magFilter = RHISamplerFilter::eLinear;
        sampler.maxLod    = 0;
        pass.BindValue("uDistanceFieldStale", m_stale);
        pass.BindValue("uDistanceFieldLocals", m_locals);
        pass.BindStorageBuffer("DistanceFieldLocalCells", m_poolCells);
        pass.BindStorageBuffer("DistanceFieldLocalHierarchy", m_poolHierarchy);
        pass.BindSampledTexture("localFieldAtlas", m_device->CreateSampler(sampler), m_poolAtlas->GetDefaultView());
    }
    return ready;
}

const char* DistanceFieldScene::GetFailureReason() const
{
    return m_failureReason;
}

uint64_t DistanceFieldScene::GetGeneration() const
{
    return m_state == State::Ready && m_active ? m_active->generation : 0;
}

uint64_t DistanceFieldScene::GetSnapshotBytes() const
{
    return m_pending ? m_pending->snapshotBytes : m_active ? m_active->snapshotBytes : m_lastSnapshotBytes;
}

uint64_t DistanceFieldScene::GetSubmittedGraphCount() const
{
    return m_pending ? m_pending->submittedGraphs : m_active ? m_active->submittedGraphs : m_lastSubmittedGraphs;
}

uint32_t DistanceFieldScene::GetAllocatedBrickCount() const
{
    return m_pending ? m_pending->allocatedBricks : m_active ? m_active->allocatedBricks : m_lastAllocatedBricks;
}

uint32_t DistanceFieldScene::GetUnavailableCellCount() const
{
    uint32_t count = m_active ? m_active->unavailableCells : 0u;
    for (const LocalSlot& slot : m_slots)
    {
        if (slot.ready)
        {
            count += slot.unavailableCells;
        }
    }
    return count;
}

uint32_t DistanceFieldScene::GetBuildRetryCount() const
{
    return m_pending ? m_pending->buildRetries : m_active ? m_active->buildRetries : m_lastBuildRetries;
}

uint32_t DistanceFieldScene::GetProcessedTriangleCount() const
{
    return m_pending ? m_pending->processedTriangles : m_active ? m_active->processedTriangles : m_lastProcessedTriangles;
}

uint32_t DistanceFieldScene::GetTriangleBatchQuota() const
{
    return m_pending ? m_pending->triangleQuota : m_active ? m_active->triangleQuota : m_lastTriangleQuota;
}

const DistanceFieldSceneFailure& DistanceFieldScene::GetFailureDetails() const
{
    return m_failureDetails;
}

void DistanceFieldScene::CollectBuildInputs(HeapVector<const GeometrySnapshot*>& geometries,
                                            HeapVector<const BuildWorkspace*>&   workspaces) const
{
    if (m_geometry)
    {
        geometries.push_back(m_geometry.Get());
    }
    if (m_workspace)
    {
        workspaces.push_back(m_workspace.Get());
    }
    HeapVector<const Generation*> generations{m_active.Get(), m_pending.Get(), m_local.Get()};
    for (const UniquePtr<Generation>& retired : m_retired)
    {
        generations.push_back(retired.Get());
    }
    for (const Generation* generation : generations)
    {
        if (generation != nullptr)
        {
            if (generation->geometry)
            {
                geometries.push_back(generation->geometry.Get());
            }
            if (generation->workspace)
            {
                workspaces.push_back(generation->workspace.Get());
            }
        }
    }
    std::sort(geometries.begin(), geometries.end(), std::less<const GeometrySnapshot*>());
    geometries.erase(std::unique(geometries.begin(), geometries.end()), geometries.end());
    std::sort(workspaces.begin(), workspaces.end(), std::less<const BuildWorkspace*>());
    workspaces.erase(std::unique(workspaces.begin(), workspaces.end()), workspaces.end());
}

uint64_t DistanceFieldScene::GetRetainedInputBytes() const
{
    HeapVector<const GeometrySnapshot*> geometries;
    HeapVector<const BuildWorkspace*>   workspaces;
    CollectBuildInputs(geometries, workspaces);
    HeapVector<RHIBuffer*> buffers;
    for (const GeometrySnapshot* geometry : geometries)
    {
        geometry->AppendRetainedInputs(buffers);
    }
    std::sort(buffers.begin(), buffers.end(), std::less<RHIBuffer*>());
    uint64_t bytes = 0;
    for (size_t index = 0; index < buffers.size(); ++index)
    {
        if (index == 0 || buffers[index] != buffers[index - 1])
        {
            bytes += BufferBytes(buffers[index]);
        }
    }
    return bytes;
}

uint64_t DistanceFieldScene::GetPersistentBytes() const
{
    uint64_t bytes = (m_active ? m_active->persistentBytes : 0) + (m_pending ? m_pending->persistentBytes : 0)
                   + (m_local ? m_local->persistentBytes : 0) + GetLocalPoolBytes();
    for (const UniquePtr<Generation>& generation : m_retired)
    {
        bytes += generation->persistentBytes;
    }
    return bytes;
}

uint64_t DistanceFieldScene::GetScratchBytes() const
{
    HeapVector<const GeometrySnapshot*> geometries;
    HeapVector<const BuildWorkspace*>   workspaces;
    CollectBuildInputs(geometries, workspaces);
    uint64_t bytes = 0;
    for (const GeometrySnapshot* geometry : geometries)
    {
        bytes += geometry->bytes;
    }
    for (const BuildWorkspace* workspace : workspaces)
    {
        bytes += workspace->bytes;
    }
    for (const Generation* generation : {m_active.Get(), m_pending.Get(), m_local.Get()})
    {
        if (generation != nullptr)
        {
            bytes += BufferBytes(generation->ranges);
        }
    }
    for (const UniquePtr<Generation>& generation : m_retired)
    {
        bytes += BufferBytes(generation->ranges);
    }
    return bytes;
}

const DistanceFieldSceneUniform& DistanceFieldScene::GetUniform() const
{
    return m_state == State::Ready && m_active ? m_active->uniform : kEmptyUniform;
}

void DistanceFieldScene::ReleaseGeometry(SharedPtr<GeometrySnapshot>& geometry, bool completed)
{
    if (geometry && geometry.Unique())
    {
        for (RHIBuffer* buffer : geometry->snapshots)
        {
            if (completed && buffer != nullptr)
            {
                buffer->ReleaseReference();
            }
            else
            {
                m_device->DestroyBuffer(buffer);
            }
        }
        if (completed && geometry->samplerInfo != nullptr)
        {
            geometry->samplerInfo->ReleaseReference();
        }
        else
        {
            m_device->DestroyBuffer(geometry->samplerInfo);
        }
    }
    geometry.Reset();
}

void DistanceFieldScene::ReleaseWorkspace(SharedPtr<BuildWorkspace>& workspace, bool completed)
{
    if (workspace && workspace.Unique())
    {
        for (RHIBuffer* buffer :
             {workspace->counters, workspace->jobs, workspace->counts, workspace->offsets, workspace->cursors,
              workspace->references, workspace->active, workspace->scan, workspace->control, workspace->indirect,
              workspace->coverage, workspace->coverageQueue, workspace->emitSplit, workspace->brickUsed, workspace->freeBricks})
        {
            if (completed && buffer != nullptr)
            {
                buffer->ReleaseReference();
            }
            else
            {
                m_device->DestroyBuffer(buffer);
            }
        }
        for (BuildWorkspace::StatusSlot& slot : workspace->status)
        {
            if (completed && slot.buffer != nullptr)
            {
                slot.buffer->ReleaseReference();
            }
            else
            {
                m_device->DestroyBuffer(slot.buffer);
            }
        }
    }
    workspace.Reset();
}

void DistanceFieldScene::ReleaseScratch(Generation& generation, bool completed)
{
    ReleaseGeometry(generation.geometry, completed);
    ReleaseWorkspace(generation.workspace, completed);
    if (completed && generation.ranges != nullptr)
    {
        generation.ranges->ReleaseReference();
    }
    else
    {
        m_device->DestroyBuffer(generation.ranges);
    }
    generation.ranges = nullptr;
}

void DistanceFieldScene::RetireSharedInputs()
{
    if (m_geometry || m_workspace)
    {
        // The retirement holder has no published volume. Shared input allocations
        // remain charged until their last submitted construction user completes.
        UniquePtr<Generation> holder = MakeUnique<Generation>();
        holder->geometry             = std::move(m_geometry);
        holder->workspace            = std::move(m_workspace);
        Retire(holder);
    }
}

void DistanceFieldScene::Release(Generation& generation, bool completed)
{
    ReleaseScratch(generation, completed);
    if (completed)
    {
        for (RHIResource* resource :
             {static_cast<RHIResource*>(generation.cells), static_cast<RHIResource*>(generation.hierarchy),
              static_cast<RHIResource*>(generation.atlas)})
        {
            if (resource != nullptr)
            {
                resource->ReleaseReference();
            }
        }
    }
    else
    {
        m_device->DestroyBuffer(generation.cells);
        m_device->DestroyBuffer(generation.hierarchy);
        m_device->DestroyTexture(generation.atlas);
    }
    generation.cells           = nullptr;
    generation.hierarchy       = nullptr;
    generation.atlas           = nullptr;
    generation.persistentBytes = 0;
}

void DistanceFieldScene::Retire(UniquePtr<Generation>& generation)
{
    if (generation)
    {
        generation->retirement.Extend(m_device->CaptureResourceRetirement());
        m_retired.push_back(std::move(generation));
    }
}

void DistanceFieldScene::Fail(const char* reason, bool local)
{
    const bool localBuild   = local || (m_pending && m_pending->local);
    const bool required     = localBuild && m_hasDynamic && m_buildingSlot == static_cast<int32_t>(m_detailCores.size());
    const bool localFailure = localBuild && !required;
    if (m_pending)
    {
        m_failedScene               = m_pending->scene;
        m_failedRevision            = m_pending->geometry ? m_pending->geometry->revision : m_pending->revision;
        m_lastSubmittedGraphs       = m_pending->submittedGraphs;
        m_failureDetails.generation = m_pending->generation;
        m_failureDetails.uniform    = m_pending->uniform;
    }
    if (localBuild)
    {
        if (m_buildingSlot >= 0)
        {
            m_slots[m_buildingSlot].failed = true;
            m_buildingSlot                 = -1;
        }
    }
    if (!localFailure)
    {
        m_state         = State::Failed;
        m_failureReason = reason;
    }
    LOGE("Distance field construction failed: {}", reason);
    Retire(m_pending);
    if (!localFailure)
    {
        RetireSharedInputs();
    }
}

void DistanceFieldScene::ReleaseSnapshotSources(Generation& generation)
{
    if (generation.geometry && generation.geometry->snapshotSubmitted && !generation.geometry->snapshotSourcesReleased
        && m_device->IsResourceRetired(generation.geometry->snapshotRetirement))
    {
        for (RHIResourcePtr<RHIBuffer>& source : generation.geometry->sources)
        {
            source.Reset();
        }
        generation.geometry->snapshotSourcesReleased = true;
    }
}

bool DistanceFieldScene::UsesThroughputBuild() const
{
    return m_settings.throughputFrameBudgetMicroseconds > 0.0;
}

// Largest triangle batch cap: fixed capacities take triangleBatchSize; derived ones any batch that
// the jobs phase can launch, one workgroup per 64 triangles.
uint32_t DistanceFieldScene::BatchCeiling() const
{
    const uint64_t groups = m_device->GetGPUInfo().maxComputeWorkGroupCount[0];
    return m_settings.deriveCapacities ? static_cast<uint32_t>(std::min<uint64_t>(ZEN_FIELD_MAX_TRIANGLE_BATCH, groups * 64))
                                       : m_settings.triangleBatchSize;
}

uint32_t DistanceFieldScene::NextCycleCount()
{
    uint32_t cycles = m_settings.maxCyclesPerFrame;
    if (UsesThroughputBuild())
    {
        // Whole-batch dispatches make a cycle's cost scene- and device-dependent: start each
        // generation at one cycle and the device's last batch cap, so a slow device cannot record
        // seconds of work in one submission. Within the budget, grow the batch cap first, which
        // finishes the generation in fewer dispatches than more cycles would; over it, shrink the
        // cycles first.
        const uint32_t ceiling = BatchCeiling();
        cycles                 = m_pending && m_pending->submittedCycles != 0 ? m_cyclesPerFrame : 1u;
        if (m_pending && m_pending->submittedCycles != 0 && m_frameIntervalUs <= m_settings.throughputFrameBudgetMicroseconds)
        {
            if (m_batchCap < std::min(ceiling, m_pending->triangleCount))
            {
                m_batchCap = std::min(ceiling, m_batchCap * 2u);
            }
            else
            {
                cycles = std::min(m_settings.maxCyclesPerFrame, m_cyclesPerFrame * 2u);
            }
        }
        else if (m_pending && m_pending->submittedCycles != 0)
        {
            if (m_cyclesPerFrame > 1)
            {
                cycles = m_cyclesPerFrame / 2u;
            }
            else
            {
                m_batchCap = std::max(1u, m_batchCap / 2u);
            }
        }
    }
    else if (m_settings.frameBudgetMicroseconds > 0.0 || m_settings.frameBudgetShare > 0.0)
    {
        // A cycle costs CPU recording and serialized GPU latency (about 28 dependent dispatches), and
        // either may set the frame rate. Rather than predict either, measure the frame interval with
        // one cycle, then add a cycle per frame while the interval stays within the allowance and
        // halve the count when it does not. VSync waits and CPU-bound frames (Debug) leave room for
        // more cycles. The baseline is measured again for each generation.
        if (m_baselineFrames < kBaselineFrames)
        {
            cycles       = 1;
            m_baselineUs = m_frameIntervalUs;
            ++m_baselineFrames;
        }
        else
        {
            const double allowance = std::max(m_settings.frameBudgetMicroseconds, m_settings.frameBudgetShare * m_baselineUs);
            cycles                 = m_frameIntervalUs <= m_baselineUs + allowance
                                       ? std::min(m_settings.maxCyclesPerFrame, m_cyclesPerFrame + 1)
                                       : std::max(1u, m_cyclesPerFrame / 2);
        }
    }
    m_cyclesPerFrame = cycles;
    return cycles;
}

void DistanceFieldScene::Poll()
{
    for (size_t index = 0; index < m_retired.size();)
    {
        ReleaseSnapshotSources(*m_retired[index]);
        if (m_device->IsResourceRetired(m_retired[index]->retirement))
        {
            Release(*m_retired[index], true);
            m_retired.erase(m_retired.begin() + index);
        }
        else
        {
            ++index;
        }
    }
    if (m_pending)
    {
        ReleaseSnapshotSources(*m_pending);
        m_pending->retirement.Resolve();
    }
    for (uint32_t index = 0; m_pending && index < kStatusSlots; ++index)
    {
        BuildWorkspace::StatusSlot& slot = m_pending->workspace->status[index];
        if (slot.pending && m_device->IsResourceRetired(slot.retirement))
        {
            slot.pending          = false;
            const uint8_t* mapped = slot.buffer->Map();
            if (mapped == nullptr)
            {
                Fail("status readback mapping failed");
            }
            else
            {
                uint32_t status[ZEN_FIELD_STATUS_WORDS]{};
                std::memcpy(status, mapped, sizeof(status));
                slot.buffer->Unmap();
                if (slot.graph > m_pending->lastStatusGraph)
                {
                    m_pending->lastStatusGraph = slot.graph;
                    const uint32_t* counters   = status + ZEN_FIELD_CONTROL_WORDS;
                    m_lastAllocatedBricks      = counters[ZEN_FIELD_COUNTER_ALLOCATED] + counters[ZEN_FIELD_COUNTER_RETAINED];
                    m_lastBuildRetries         = status[ZEN_FIELD_CONTROL_RETRIES];
                    m_lastProcessedTriangles   = status[ZEN_FIELD_CONTROL_PROCESSED_TRIANGLES];
                    m_lastTriangleQuota        = status[ZEN_FIELD_CONTROL_TRIANGLE_QUOTA];
                    m_pending->allocatedBricks = m_lastAllocatedBricks;
                    m_pending->buildRetries    = m_lastBuildRetries;
                    m_pending->processedTriangles = m_lastProcessedTriangles;
                    m_pending->triangleQuota      = m_lastTriangleQuota;
                    m_pending->unavailableCells   = counters[ZEN_FIELD_COUNTER_UNAVAILABLE];
                    if (m_pending->submittedGraphs - m_pending->lastProgressGraph >= 4096)
                    {
                        m_pending->lastProgressGraph = m_pending->submittedGraphs;
                        LOGI(
                            "Distance field generation {} building: graphs={}, phase={}, triangles={}/{}, coverage={}, pending={}, jobs={}, unavailable={}",
                            m_pending->generation, m_pending->submittedGraphs, status[ZEN_FIELD_CONTROL_PHASE],
                            m_lastProcessedTriangles, m_pending->triangleCount, counters[ZEN_FIELD_COUNTER_COVERAGE_COUNT],
                            counters[ZEN_FIELD_COUNTER_COVERAGE_PENDING], counters[ZEN_FIELD_COUNTER_JOBS],
                            m_pending->unavailableCells);
                    }
                    if (counters[ZEN_FIELD_COUNTER_FAILURE] != 0u)
                    {
                        m_failureDetails.flags = counters[ZEN_FIELD_COUNTER_FAILURE];
                        m_failureDetails.site  = counters[ZEN_FIELD_COUNTER_FAILURE_SITE];
                        m_failureDetails.triangle =
                            m_failureDetails.site != 0 ? counters[ZEN_FIELD_COUNTER_FAILURE_TRIANGLE] : UINT32_MAX;
                        m_failureDetails.cell =
                            m_failureDetails.site != 0 ? counters[ZEN_FIELD_COUNTER_FAILURE_CELL] : UINT32_MAX;
                        m_failureDetails.sample =
                            m_failureDetails.site != 0 ? counters[ZEN_FIELD_COUNTER_FAILURE_SAMPLE] : UINT32_MAX;
                        LOGE("Distance field GPU failure flags {}, site {}, triangle {}, cell {}, sample {}",
                             m_failureDetails.flags, m_failureDetails.site, m_failureDetails.triangle, m_failureDetails.cell,
                             m_failureDetails.sample);
                        LOGE(
                            "Distance field failed after {} submitted graphs, {} allocated bricks, {} retries, {} processed triangles",
                            m_pending->submittedGraphs, m_lastAllocatedBricks, m_lastBuildRetries, m_lastProcessedTriangles);
                        const char* reason = (m_failureDetails.flags & ZEN_FIELD_FAILURE_COVERAGE) != 0 ? "unsupported_coverage"
                                           : (m_failureDetails.flags & ZEN_FIELD_FAILURE_COVERAGE_BUDGET) != 0
                                               ? "coverage_refinement_budget_exhausted"
                                               : "GPU construction overflow or invalid geometry";
                        Fail(reason);
                    }
                    else if (status[ZEN_FIELD_CONTROL_DONE] == 1u)
                    {
                        m_pending->stage = BuildStage::AwaitCompletion;
                    }
                    else if (status[ZEN_FIELD_CONTROL_DONE] != 0u)
                    {
                        Fail("GPU scheduler failed without a failure reason");
                    }
                }
            }
        }
    }
    if (m_pending && m_pending->stage == BuildStage::AwaitCompletion && m_device->IsResourceRetired(m_pending->retirement))
    {
        // Later submitted cycles may have observed completion and performed no work,
        // but they still reference scratch descriptors. Retire every actual user.
        const bool local = m_pending->local;
        ReleaseScratch(*m_pending, true);
        if (local)
        {
            Retire(m_local);
            m_local        = std::move(m_pending);
            m_copySlot     = m_buildingSlot;
            m_buildingSlot = -1;
        }
        else
        {
            Retire(m_active);
            m_active        = std::move(m_pending);
            m_state         = State::Ready;
            m_failureReason = "";
        }
        if (!LocalWorkRemains())
        {
            ReleaseGeometry(m_geometry, true);
            ReleaseWorkspace(m_workspace, true);
        }
        const Generation& published = local ? *m_local : *m_active;
        const double      elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - published.started).count();
        LOGI(
            "Distance field generation {} ready after {} submitted graphs, {} ms elapsed, {} allocated bricks, {} retries, {} processed triangles, {} unavailable cells, {} construction cycles",
            published.generation, published.submittedGraphs, elapsed, m_lastAllocatedBricks, m_lastBuildRetries,
            m_lastProcessedTriangles, published.unavailableCells, published.submittedCycles);
    }
}

namespace
{
// Accumulates validity and remembers the first failed requirement for the rejection log.
void Require(bool condition, const char* requirement, bool& valid, const char*& failed)
{
    if (valid && !condition)
    {
        failed = requirement;
    }
    valid = valid && condition;
}
} // namespace

bool DistanceFieldScene::Start(RenderScene& scene, bool local)
{
    m_lastAllocatedBricks    = 0;
    m_lastBuildRetries       = 0;
    m_lastProcessedTriangles = 0;
    m_lastTriangleQuota      = 0;
    m_lastSubmittedGraphs    = 0;
    m_failureDetails         = {};
    // Costs and the frame around them differ between generations: measure again from a single cycle.
    m_cyclesPerFrame = 1;
    m_baselineFrames = 0;
    m_baselineUs     = 0;
    const uint32_t side       = local ? m_settings.localCellsPerAxis : m_settings.cellsPerAxis;
    const uint32_t atlasSide  = local ? m_settings.localAtlasTexelsPerAxis : m_settings.atlasTexelsPerAxis;
    const uint64_t cells      = uint64_t(side) * side * side;
    const uint64_t atlasBytes = uint64_t(atlasSide) * atlasSide * atlasSide;
    const uint64_t tiles      = atlasSide / 8;
    const uint64_t capacity   = tiles * tiles * tiles;
    bool           valid      = true;
    const char*    failed     = "";
    Require(side >= 4 && side <= ZEN_FIELD_MAX_SIDE && (side & (side - 1)) == 0 && atlasSide >= 8 && atlasSide <= 2048
                && atlasSide % 8 == 0 && capacity <= UINT32_MAX && m_settings.triangleBatchSize > 0
                && m_settings.triangleBatchSize <= ZEN_FIELD_MAX_TRIANGLE_BATCH && m_settings.cellBatchSize > 0
                && m_settings.cellBatchSize <= 65535 && m_settings.jobsPerDispatch > 0 && m_settings.jobsPerDispatch <= 65535
                && m_settings.dispatchesPerFrame > 0 && m_settings.dispatchesPerFrame <= 16 && m_settings.maxCyclesPerFrame > 0
                && m_settings.maxCyclesPerFrame <= 64 && m_settings.frameBudgetMicroseconds >= 0.0
                && std::isfinite(m_settings.frameBudgetMicroseconds) && m_settings.frameBudgetShare >= 0.0
                && std::isfinite(m_settings.frameBudgetShare) && m_settings.throughputFrameBudgetMicroseconds >= 0.0
                && std::isfinite(m_settings.throughputFrameBudgetMicroseconds) && m_settings.jobCapacity > 0
                && m_settings.referencesPerCell > 0 && m_settings.referencesPerCell <= ZEN_FIELD_MAX_CELL_REFS
                && m_settings.referenceCapacity > 0 && m_settings.hitTolerance > 0.0f && m_settings.hitTolerance < 1.0f
                && std::isfinite(m_settings.hitTolerance) && m_settings.deviceMemoryShare > 0.0
                && m_settings.deviceMemoryShare <= 1.0,
            "settings", valid, failed);
    const bool reuseGeometry =
        m_geometry && m_geometry->scene == scene.GetIdentity() && m_geometry->revision == scene.GetGeometryRevision();
    const RHIGPUInfo& gpu = m_device->GetGPUInfo();
    Require(gpu.maxComputeWorkGroupInvocations >= 512 && gpu.maxComputeWorkGroupSize[0] >= 256
                && gpu.maxComputeWorkGroupSize[1] >= 8 && gpu.maxComputeWorkGroupSize[2] >= 8,
            "device compute workgroup limits", valid, failed);
    Require(RenderDevice::GetTextureCopyCapabilities(DataFormat::eR8UNORM).linearFilter, "R8 linear filtering", valid, failed);
    const RHIBuffer* inputs[]      = {scene.GetVertexBuffer(), scene.GetNodesDataSSBO(), scene.GetMaterialsDataSSBO(),
                                      scene.GetUVBuffer()};
    uint64_t         snapshotBytes = 0;
    uint64_t         snapshotSizes[kSnapshotBuffers]{};
    for (uint32_t index = 0; index < kSnapshotBuffers; ++index)
    {
        const RHIBuffer* input = inputs[index];
        Require(input != nullptr && input->GetRequiredSize() > 0 && input->GetRequiredSize() % sizeof(uint32_t) == 0,
                "scene geometry input buffers", valid, failed);
        if (input != nullptr)
        {
            if (index == 0)
            {
                Require(input->GetRequiredSize() % sizeof(asset::Vertex) == 0, "whole vertex records", valid, failed);
                snapshotSizes[index] = input->GetRequiredSize() / sizeof(asset::Vertex) * sizeof(Vec4);
            }
            else
            {
                snapshotSizes[index] = input->GetRequiredSize();
            }
        }
        snapshotBytes += snapshotSizes[index];
    }
    m_lastSnapshotBytes = snapshotBytes;
    Require(scene.GetVoxelTriangleCount() == 0
                || (scene.GetVoxelTriangleBuffer() != nullptr && scene.GetIndexBuffer() != nullptr),
            "triangle record and index buffers", valid, failed);
    const uint64_t samplerBytes = (scene.GetSceneSamplers().size() + 3) * sizeof(uint32_t);
    const uint64_t persistent   = atlasBytes + (cells + HierarchyWords(side)) * sizeof(uint32_t);
    const AABB     sceneBounds = local ? LocalAuthority(m_localCore) : scene.GetAABB();
    // A scene without triangles has no occluder to cover: any unit volume certifies empty space.
    const AABB bounds  = scene.GetVoxelTriangleCount() == 0 && !local ? AABB(Vec3(-0.5f), Vec3(0.5f)) : sceneBounds;
    const Vec3 center  = bounds.GetCenter();
    float      length  = bounds.GetMaxExtent() * (static_cast<float>(side) / (side - 2));
    Vec3       minimum = center - Vec3(length * 0.5f);
    bool       partial = !local && m_active && m_active->scene == scene.GetIdentity()
                      && m_active->casters.size() == scene.GetShadowCasters().size() && m_stale.count.x != 0;
    if (partial)
    {
        const Vec3  previousMinimum(m_active->uniform.gridMinCellSize);
        const float previousLength = m_active->uniform.gridMinCellSize.w * side;
        partial                    = glm::all(glm::greaterThanEqual(bounds.GetMin(), previousMinimum))
                                  && glm::all(glm::lessThanEqual(bounds.GetMax(), previousMinimum + Vec3(previousLength)));
        if (partial)
        {
            minimum = previousMinimum;
            length  = previousLength;
        }
    }
    Require(length > 0 && std::isfinite(length) && std::isfinite(minimum.x) && std::isfinite(minimum.y)
                && std::isfinite(minimum.z) && std::isfinite(1.0f / (length / side)),
            "full-scene bounds", valid, failed);
    const float               brickSize = length / side;
    DistanceFieldStaleUniform dirty;
    if (partial)
    {
        dirty.count = m_stale.count;
        for (uint32_t box = 0; box < m_stale.count.x; ++box)
        {
            // Distances affect a full brick beyond the surface, in addition to the hit band.
            dirty.minimum[box] =
                Vec4(glm::max(glm::floor((Vec3(m_stale.minimum[box]) - minimum) / brickSize - Vec3(1.5f)), Vec3(0)), 0);
            dirty.maximum[box] =
                Vec4(glm::min(glm::ceil((Vec3(m_stale.maximum[box]) - minimum) / brickSize + Vec3(1.5f)), Vec3(side)), 0);
        }
    }
    // Static construction excludes dynamic instances. Their bounded composite region is rebuilt
    // independently, including only the scene instances intersecting that region. A partial
    // refresh takes only the instances overlapping its dirty boxes.
    const HeapVector<RenderScene::InstanceTriangleRange>& committedRanges = scene.GetInstanceTriangleRanges();
    const HeapVector<RenderScene::ShadowCaster>&          casters         = scene.GetShadowCasters();
    Require(committedRanges.size() == casters.size(), "instance triangle ranges", valid, failed);
    HeapVector<glm::uvec4> ranges;
    uint32_t               selectedTriangles = 0;
    // Expected work if every selected triangle went into one batch, averaged over the triangles'
    // placements in the grid, in cells for a triangle with bounding-box extents e, diagonal d, area a
    // and unit normal n. References: cells whose 3-cell test box (FieldVisitJob) meets the triangle,
    // the volume it sweeps, 3 a |n|1 + 9 (ex + ey + ez) + 27. Jobs: 4^3-cell regions meeting its box
    // widened by one cell (FieldPatchRegionLayout), (ex / 4 + 1.5) (ey / 4 + 1.5) (ez / 4 + 1.5).
    // Bounding |n|1 by sqrt(3), ex + ey + ez by sqrt(3) d, the extents' pairwise products by d^2 and
    // their product by (d / sqrt(3))^3 leaves sums the scene records per instance (triangleSums) and
    // makes both estimates upper ones. No triangle can reach more cells or regions than the instance's
    // bounds do within the grid.
    double referenceDemand = 0.0;
    double jobDemand       = 0.0;
    {
        const Vec3 selectedMinimum = minimum - Vec3(brickSize);
        const Vec3 selectedMaximum = minimum + Vec3(length + brickSize);
        for (size_t index = 0; valid && index < committedRanges.size(); ++index)
        {
            const RenderScene::InstanceTriangleRange& range    = committedRanges[index];
            const AABB&                               instance = casters[index].bounds;
            bool                                      selected = local || !partial;
            for (uint32_t box = 0; !selected && box < dirty.count.x; ++box)
            {
                const AABB dirtyBounds(minimum + Vec3(dirty.minimum[box]) * brickSize - Vec3(brickSize),
                                       minimum + Vec3(dirty.maximum[box]) * brickSize + Vec3(brickSize));
                selected = BoxesOverlap(instance, dirtyBounds);
            }
            if (selected && range.triangleCount != 0 && (range.committedMask & (local ? GI_ALL : GI_STATIC)) != 0
                && glm::all(glm::greaterThanEqual(instance.GetMax(), selectedMinimum))
                && glm::all(glm::lessThanEqual(instance.GetMin(), selectedMaximum)))
            {
                ranges.push_back(glm::uvec4(range.firstTriangle, range.triangleCount, selectedTriangles, 0));
                selectedTriangles += range.triangleCount;
                const glm::mat3 linear(scene.GetInstanceTransform(static_cast<uint32_t>(index)));
                const double    root3 = std::sqrt(3.0);
                const double c = std::max({glm::length(linear[0]), glm::length(linear[1]), glm::length(linear[2])}) / brickSize;
                const glm::dvec4 sums(range.triangleSums);
                const double     count      = range.triangleCount;
                const double     references = 3.0 * root3 * c * c * sums.w + 9.0 * root3 * c * sums.x + 27.0 * count;
                const double     jobs       = 3.375 * count + 2.25 * root3 * c / 4.0 * sums.x + 1.5 * c * c / 16.0 * sums.y
                                            + c * c * c / (64.0 * 3.0 * root3) * sums.z;
                const glm::dvec3 lower = glm::max(glm::dvec3(instance.GetMin() - minimum) / double(brickSize), glm::dvec3(0));
                const glm::dvec3 upper =
                    glm::min(glm::dvec3(instance.GetMax() - minimum) / double(brickSize), glm::dvec3(side));
                const glm::dvec3 extent         = glm::max(upper - lower, glm::dvec3(0));
                const glm::dvec3 cellsPerAxis   = glm::min(extent + 3.0, glm::dvec3(side));
                const glm::dvec3 regionsPerAxis = glm::min(extent / 4.0 + 1.5, glm::dvec3(side / ZEN_FIELD_REGION_SIDE));
                referenceDemand += std::min(references, count * cellsPerAxis.x * cellsPerAxis.y * cellsPerAxis.z);
                jobDemand       += std::min(jobs, count * regionsPerAxis.x * regionsPerAxis.y * regionsPerAxis.z);
            }
        }
    }
    // Alpha-tested and blended triangles each take one coverage slot while their batch refines them.
    // Without per-material counts, every selected triangle may need one.
    const HeapVector<sg::MaterialData>& materials         = scene.GetMaterialsData();
    const HeapVector<uint32_t>&         materialTriangles = scene.GetMaterialTriangleCounts();
    uint64_t coverageTriangles = materialTriangles.size() == materials.size() ? 0 : selectedTriangles;
    for (size_t material = 0; material < materials.size() && material < materialTriangles.size(); ++material)
    {
        coverageTriangles += materials[material].surfaceProperties.y != 0.0f ? materialTriangles[material] : 0;
    }
    coverageTriangles = std::max<uint64_t>(std::min<uint64_t>(coverageTriangles, selectedTriangles), 1);
    // Workspace bytes that do not depend on the capacities, and per reference: its entry plus, per
    // chunk of a dense cell, an emit entry and a split slot.
    const uint64_t cellWorkspaceBytes =
        cells * (3 * sizeof(uint32_t) + sizeof(glm::uvec2)) + ((cells + 255) / 256) * sizeof(uint32_t)
        + (ZEN_FIELD_STATUS_WORDS * (kStatusSlots + 1) + ZEN_FIELD_INDIRECT_WORDS) * sizeof(uint32_t)
        + 2 * capacity * sizeof(uint32_t);
    const double referenceBytes =
        sizeof(uint32_t)
        + double(sizeof(glm::uvec2) + ZEN_FIELD_EMIT_SPLIT_WORDS * sizeof(uint32_t)) / ZEN_FIELD_EMIT_CHUNK_REFERENCES;
    const uint64_t coverageSlotBytes = (ZEN_FIELD_COVERAGE_STATE_WORDS + 1) * sizeof(uint32_t);
    const uint64_t geometryEstimate  = snapshotBytes + samplerBytes;
    const bool     retiredWorkspace  = m_workspace && m_device->IsResourceRetired(m_workspace->retirement);
    uint32_t       jobCapacity       = m_settings.jobCapacity;
    uint32_t       referenceCapacity = m_settings.referenceCapacity;
    // A batch holds at most the generation's triangles.
    uint32_t coverageSlots = std::max(std::min(m_settings.triangleBatchSize, selectedTriangles), 1u);
    bool     memoryLimited = false;
    // A retired workspace that is too small is released before its replacement is created, so its
    // bytes count as free while sizing the replacement.
    const uint64_t reclaimable = retiredWorkspace ? m_workspace->bytes : 0;
    const uint64_t available   = m_settings.deriveCapacities ? FreeDeviceMemory(*m_device) : UINT64_MAX;
    if (m_settings.deriveCapacities)
    {
        const double share  = available == UINT64_MAX ? double(m_settings.scratchBudgetBytes)
                                                      : m_settings.deviceMemoryShare * double(available + reclaimable);
        const double budget = share - double(reuseGeometry ? 0 : geometryEstimate) - double(cellWorkspaceBytes);
        const double wanted =
            referenceDemand * referenceBytes + jobDemand * sizeof(glm::uvec4) + double(coverageTriangles) * coverageSlotBytes;
        // Short of memory, every capacity shrinks by the same factor, so batches stay balanced.
        const double scale = wanted > budget ? std::max(budget, 0.0) / wanted : 1.0;
        memoryLimited      = scale < 1.0;
        referenceCapacity =
            static_cast<uint32_t>(std::min(std::max(referenceDemand * scale, double(m_settings.referenceCapacity)),
                                           double(std::min<uint64_t>(StorageElements(gpu, sizeof(uint32_t)), UINT32_MAX))));
        jobCapacity =
            static_cast<uint32_t>(std::min(std::max(jobDemand * scale, double(m_settings.jobCapacity)),
                                           double(std::min<uint64_t>(StorageElements(gpu, sizeof(glm::uvec4)), UINT32_MAX))));
        coverageSlots = static_cast<uint32_t>(
            std::min(std::max(double(coverageTriangles) * scale, 1.0),
                     double(std::min<uint64_t>(StorageElements(gpu, ZEN_FIELD_COVERAGE_STATE_WORDS * sizeof(uint32_t)),
                                               ZEN_FIELD_MAX_TRIANGLE_BATCH))));
    }
    // A workspace already sized to the memory budget is kept for another generation that is also
    // short of memory: replacing it would only follow small changes in free memory.
    const bool fits = m_workspace && m_workspace->cells >= cells && m_workspace->bricks >= capacity
                   && ((m_workspace->jobCapacity >= jobCapacity && m_workspace->referenceCapacity >= referenceCapacity
                        && m_workspace->coverageSlots >= coverageSlots)
                       || (m_workspace->memoryLimited && memoryLimited));
    if (m_workspace && !fits && retiredWorkspace)
    {
        // A finer local region or a larger scene may need a larger workspace.
        ReleaseWorkspace(m_workspace, true);
    }
    const bool reuseWorkspace = fits && retiredWorkspace;
    if (reuseWorkspace)
    {
        jobCapacity       = m_workspace->jobCapacity;
        referenceCapacity = m_workspace->referenceCapacity;
        coverageSlots     = m_workspace->coverageSlots;
    }
    // Scratch already allocated is part of the device's usage; a released workspace's is free again.
    const uint64_t released = m_workspace ? 0 : reclaimable;
    m_scratchBudget =
        !m_settings.deriveCapacities || available == UINT64_MAX
            ? m_settings.scratchBudgetBytes
            : GetScratchBytes() + static_cast<uint64_t>(m_settings.deviceMemoryShare * double(available + released));
    const uint64_t coverageBytes = uint64_t(coverageSlots) * ZEN_FIELD_COVERAGE_STATE_WORDS * sizeof(uint32_t);
    const uint64_t activeBytes   = ActiveEntries(cells, referenceCapacity) * sizeof(glm::uvec2);
    const uint64_t splitBytes    = SplitSlots(referenceCapacity) * ZEN_FIELD_EMIT_SPLIT_WORDS * sizeof(uint32_t);
    const uint64_t workspaceEstimate =
        cellWorkspaceBytes - cells * sizeof(glm::uvec2) + activeBytes + splitBytes + uint64_t(jobCapacity) * sizeof(glm::uvec4)
        + uint64_t(referenceCapacity) * sizeof(uint32_t) + coverageBytes + uint64_t(coverageSlots) * sizeof(uint32_t);
    const uint64_t scratch      = geometryEstimate + workspaceEstimate;
    uint64_t       checkedBytes = 0;
    for (uint64_t bytes : {snapshotBytes, uint64_t(jobCapacity) * sizeof(glm::uvec4),
                           uint64_t(referenceCapacity) * sizeof(uint32_t), activeBytes, splitBytes, coverageBytes})
    {
        Require(ValidateGIStorageBuffer(bytes, 1, gpu, checkedBytes) == GIResourceStatus::eSuccess,
                "device storage-buffer range", valid, failed);
    }
    const uint64_t newScratch = (reuseGeometry ? 0 : geometryEstimate) + (reuseWorkspace ? 0 : workspaceEstimate);
    if (!valid || persistent > m_settings.persistentBudgetBytes || scratch > m_scratchBudget)
    {
        LOGE(
            "Distance field allocation rejected ({}): snapshot {} bytes (vertices {}, nodes {}, materials {}, UV {}), persistent {}, scratch {}",
            valid ? "memory budget" : failed, snapshotBytes, inputs[0] != nullptr ? inputs[0]->GetRequiredSize() : 0,
            inputs[1] != nullptr ? inputs[1]->GetRequiredSize() : 0, inputs[2] != nullptr ? inputs[2]->GetRequiredSize() : 0,
            inputs[3] != nullptr ? inputs[3]->GetRequiredSize() : 0, persistent, scratch);
        m_failedScene    = scene.GetIdentity();
        m_failedRevision = scene.GetGeometryRevision();
        Fail(valid ? "generation exceeds configured memory budget" : "invalid settings, scene buffers, or full-scene bounds",
             local);
        valid = false;
    }
    else if ((m_workspace && !reuseWorkspace) || persistent + GetPersistentBytes() > m_settings.persistentBudgetBytes
             || newScratch + GetScratchBytes() > m_scratchBudget)
    {
        // Retired generations remain charged until the GPU is finished; try again on a later frame.
        // A refresh that cannot fit beside the published generation at all replaces it instead, as a
        // scene change does: traces wait for the rebuild rather than keep a stale field forever.
        const uint64_t resident =
            (m_active ? m_active->persistentBytes : 0) + (m_local ? m_local->persistentBytes : 0) + GetLocalPoolBytes();
        if (!local && m_active && persistent + resident > m_settings.persistentBudgetBytes)
        {
            LOGW("Distance field refresh exceeds the persistent budget beside generation {}; rebuilding in place",
                 m_active->generation);
            Retire(m_active);
            m_state = State::Pending;
        }
        valid = false;
    }
    else
    {
        m_pending              = MakeUnique<Generation>();
        Generation& generation = *m_pending;
        generation.local       = local;
        generation.partial     = partial;
        generation.dirty       = dirty;
        generation.request = m_localRequest;
        if (!reuseGeometry)
        {
            m_geometry = MakeShared<GeometrySnapshot>();
        }
        if (!reuseWorkspace)
        {
            m_workspace = MakeShared<BuildWorkspace>();
        }
        generation.geometry      = m_geometry;
        generation.workspace     = m_workspace;
        generation.scene         = scene.GetIdentity();
        generation.revision      = scene.GetGeometryRevision(local ? GI_ALL : GI_STATIC);
        generation.casters       = scene.GetShadowCasters();
        generation.generation    = m_nextGeneration++;
        generation.snapshotBytes = snapshotBytes;
        generation.triangleCount = selectedTriangles;
        if (!reuseGeometry)
        {
            generation.geometry->scene         = generation.scene;
            generation.geometry->revision      = scene.GetGeometryRevision();
            generation.geometry->triangleCount = scene.GetVoxelTriangleCount();
            generation.geometry->snapshotBytes = snapshotBytes;
            for (const sg::MaterialData& material : scene.GetMaterialsData())
            {
                generation.geometry->hasCoverageMaterials |= material.surfaceProperties.y != 0.0f;
            }
        }
        const uint32_t rangeCount = static_cast<uint32_t>(ranges.size());
        if (ranges.empty())
        {
            ranges.push_back(glm::uvec4(0));
        }
        const uint64_t rangeBytes = ranges.size() * sizeof(glm::uvec4);
        generation.ranges = CreateFieldBuffer(*m_device, rangeBytes, "field_instance_ranges", false,
                                              BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer));
        valid             = valid && generation.ranges != nullptr
                         && m_device->UpdateBuffer(generation.ranges, static_cast<uint32_t>(rangeBytes),
                                                   reinterpret_cast<const uint8_t*>(ranges.data()));
        generation.uniform.gridMinCellSize = Vec4(minimum, length / side);
        uint32_t levels                    = 1;
        for (uint32_t levelSide = side; levelSide > 1; levelSide /= 2)
        {
            ++levels;
        }
        generation.uniform.dimensions      = glm::uvec4(side, static_cast<uint32_t>(capacity), levels, 2048);
        generation.uniform.atlasDimensions = glm::uvec4(atlasSide, atlasSide, atlasSide, 0);
        // Test-owner starting tolerance; this is not a qualified thin-feature preset.
        generation.uniform.tracing = Vec4(m_settings.hitTolerance, 64, 0, 0);
        generation.build.limits =
            glm::uvec4(referenceCapacity, m_settings.referencesPerCell,
                       (local ? ZEN_FIELD_BUILD_CROP_VOLUME : 0u) | (partial ? ZEN_FIELD_BUILD_PARTIAL : 0u), rangeCount);
        // Throughput builds keep the adapted batch cap across generations (NextCycleCount).
        m_batchCap =
            std::min(UsesThroughputBuild() && m_batchCap != 0 ? m_batchCap : m_settings.triangleBatchSize, BatchCeiling());
        generation.build.triangleRange = glm::uvec4(0, m_batchCap, generation.triangleCount, jobCapacity);
        if (!reuseGeometry)
        {
            for (uint32_t index = 0; index < kSnapshotBuffers; ++index)
            {
                generation.geometry->sources[index] = const_cast<RHIBuffer*>(inputs[index]);
                generation.geometry->snapshots[index] =
                    CreateFieldBuffer(*m_device, snapshotSizes[index], "field_geometry_snapshot");
                valid = valid && generation.geometry->snapshots[index] != nullptr;
            }
            generation.geometry->indices   = scene.GetIndexBuffer();
            generation.geometry->triangles = scene.GetVoxelTriangleBuffer();
            for (RHITextureView* view : scene.GetSceneTextureViews())
            {
                valid = valid && view != nullptr && view->GetTexture() != nullptr;
                if (view != nullptr && view->GetTexture() != nullptr)
                {
                    generation.geometry->views.emplace_back(view);
                    generation.geometry->textures.emplace_back(view->GetTexture());
                }
            }
            for (RHISampler* sampler : scene.GetSceneSamplers())
            {
                valid = valid && sampler != nullptr;
                generation.geometry->samplers.emplace_back(sampler);
            }
            HeapVector<uint32_t> samplerInfo;
            samplerInfo.push_back(static_cast<uint32_t>(generation.geometry->samplers.size()) + 1u);
            samplerInfo.push_back(gpu.subTexelPrecisionBits);
            samplerInfo.push_back(SamplerCoverageFlags(RHISamplerCreateInfo::CreateLinearRepeat()));
            for (const RHIResourcePtr<RHISampler>& sampler : generation.geometry->samplers)
            {
                samplerInfo.push_back(sampler ? SamplerCoverageFlags(sampler->GetCreateInfo()) : 0u);
            }
            // Completed scratch is released directly. Initialized-buffer helpers register
            // a teardown alias that instead requires removal through DestroyBuffer.
            generation.geometry->samplerInfo =
                CreateFieldBuffer(*m_device, samplerBytes, "field_sampler_info", false,
                                  BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer));
            valid                      = valid && generation.geometry->samplerInfo != nullptr
                                      && m_device->UpdateBuffer(generation.geometry->samplerInfo, static_cast<uint32_t>(samplerBytes),
                                                                reinterpret_cast<const uint8_t*>(samplerInfo.data()));
            generation.geometry->bytes = BufferBytes(generation.geometry->samplerInfo);
            for (RHIBuffer* buffer : generation.geometry->snapshots)
            {
                generation.geometry->bytes += BufferBytes(buffer);
            }
        }
        generation.cells     = CreateFieldBuffer(*m_device, cells * sizeof(uint32_t), "field_cells");
        generation.hierarchy = CreateFieldBuffer(*m_device, HierarchyWords(side) * sizeof(uint32_t), "field_hierarchy");
        if (!reuseWorkspace)
        {
            generation.workspace->counters =
                CreateFieldBuffer(*m_device, ZEN_FIELD_COUNTER_WORDS * sizeof(uint32_t), "field_counters");
            generation.workspace->control =
                CreateFieldBuffer(*m_device, ZEN_FIELD_CONTROL_WORDS * sizeof(uint32_t), "field_control");
            generation.workspace->coverage = CreateFieldBuffer(*m_device, coverageBytes, "field_coverage");
            generation.workspace->coverageQueue =
                CreateFieldBuffer(*m_device, uint64_t(coverageSlots) * sizeof(uint32_t), "field_coverage_queue");
            generation.workspace->indirect =
                CreateFieldBuffer(*m_device, ZEN_FIELD_INDIRECT_WORDS * sizeof(uint32_t), "field_indirect", false,
                                  BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eIndirectBuffer));
            for (BuildWorkspace::StatusSlot& slot : generation.workspace->status)
            {
                slot.buffer = CreateFieldBuffer(*m_device, ZEN_FIELD_STATUS_WORDS * sizeof(uint32_t), "field_status", true);
            }
            generation.workspace->jobs = CreateFieldBuffer(*m_device, uint64_t(jobCapacity) * sizeof(glm::uvec4), "field_jobs");
            generation.workspace->cells             = cells;
            generation.workspace->bricks            = capacity;
            generation.workspace->jobCapacity       = jobCapacity;
            generation.workspace->referenceCapacity = referenceCapacity;
            generation.workspace->coverageSlots     = coverageSlots;
            generation.workspace->memoryLimited     = memoryLimited;
            generation.workspace->brickUsed  = CreateFieldBuffer(*m_device, capacity * sizeof(uint32_t), "field_brick_used");
            generation.workspace->freeBricks = CreateFieldBuffer(*m_device, capacity * sizeof(uint32_t), "field_free_bricks");
            generation.workspace->counts     = CreateFieldBuffer(*m_device, cells * sizeof(uint32_t), "field_counts");
            generation.workspace->offsets    = CreateFieldBuffer(*m_device, cells * sizeof(uint32_t), "field_offsets");
            generation.workspace->cursors    = CreateFieldBuffer(*m_device, cells * sizeof(uint32_t), "field_cursors");
            generation.workspace->references =
                CreateFieldBuffer(*m_device, uint64_t(referenceCapacity) * sizeof(uint32_t), "field_references");
            generation.workspace->active    = CreateFieldBuffer(*m_device, activeBytes, "field_active");
            generation.workspace->emitSplit = CreateFieldBuffer(*m_device, splitBytes, "field_emit_split");
            generation.workspace->scan   = CreateFieldBuffer(*m_device, ((cells + 255) / 256) * sizeof(uint32_t), "field_scan");
        }
        for (BuildWorkspace::StatusSlot& slot : generation.workspace->status)
        {
            slot.pending    = false;
            slot.graph      = 0;
            slot.retirement = {};
        }
        RHITextureCreateInfo texture;
        texture.type  = RHITextureType::e3D;
        texture.width = texture.height = texture.depth = atlasSide;
        texture.format                                 = DataFormat::eR8UNORM;
        texture.tag                                    = "field_distance_atlas";
        // Local volumes are copied from their own atlas into the pool's (RecordLocalCopy).
        texture.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eSampled,
                                    RHITextureUsageFlagBits::eTransferDst, RHITextureUsageFlagBits::eTransferSrc);
        generation.atlas = m_device->CreateTexture(texture);
        valid = valid && generation.cells != nullptr && generation.hierarchy != nullptr && generation.atlas != nullptr;
        const uint64_t atlasAllocation = generation.atlas != nullptr ? generation.atlas->GetAllocationSize() : 0;
        valid                          = valid && atlasAllocation != 0;
        generation.persistentBytes     = BufferBytes(generation.cells) + BufferBytes(generation.hierarchy) + atlasAllocation;
        for (RHIBuffer* buffer :
             {generation.workspace->counters, generation.workspace->jobs, generation.workspace->counts,
              generation.workspace->offsets, generation.workspace->cursors, generation.workspace->references,
              generation.workspace->active, generation.workspace->scan, generation.workspace->control,
              generation.workspace->indirect, generation.workspace->coverage, generation.workspace->coverageQueue,
              generation.workspace->emitSplit, generation.workspace->brickUsed, generation.workspace->freeBricks})
        {
            valid = valid && buffer != nullptr;
            if (!reuseWorkspace)
            {
                generation.workspace->bytes += BufferBytes(buffer);
            }
        }
        for (const BuildWorkspace::StatusSlot& slot : generation.workspace->status)
        {
            valid = valid && slot.buffer != nullptr;
            if (!reuseWorkspace)
            {
                generation.workspace->bytes += BufferBytes(slot.buffer);
            }
        }
        valid = valid && GetPersistentBytes() <= m_settings.persistentBudgetBytes && GetScratchBytes() <= m_scratchBudget;
        // Throughput dispatches take a phase's whole pending work, bounded only by its buffers and
        // the device's group count.
        const uint32_t groupLimit                 = gpu.maxComputeWorkGroupCount[0];
        m_dispatchLimits[ZEN_FIELD_PHASE_COUNT]   = std::min(jobCapacity, groupLimit);
        m_dispatchLimits[ZEN_FIELD_PHASE_COMPACT] = m_dispatchLimits[ZEN_FIELD_PHASE_COUNT];
        m_dispatchLimits[ZEN_FIELD_PHASE_EMIT] =
            static_cast<uint32_t>(std::min<uint64_t>(ActiveEntries(cells, referenceCapacity), groupLimit));
        m_dispatchLimits[ZEN_FIELD_PHASE_COVERAGE] = std::min(coverageSlots, groupLimit);
        if (!valid)
        {
            LOGE("Distance field native allocations: persistent {} / {} bytes, scratch {} / {} bytes, atlas {} bytes",
                 GetPersistentBytes(), m_settings.persistentBudgetBytes, GetScratchBytes(), m_scratchBudget, atlasAllocation);
            Fail(generation.atlas != nullptr && atlasAllocation == 0
                     ? "atlas_allocation_size_unavailable"
                     : "resource allocation failed or aligned allocations exceed memory budget");
        }
        else
        {
            LOGI(
                "Distance field generation {}: snapshot {} bytes, persistent {} bytes, scratch {} / {} bytes, {} triangles, {} jobs, {} references, {} coverage slots{}",
                generation.generation, generation.snapshotBytes, generation.persistentBytes, GetScratchBytes(), m_scratchBudget,
                generation.triangleCount, jobCapacity, referenceCapacity, coverageSlots,
                memoryLimited ? " (memory-limited)" : "");
        }
    }
    return valid;
}

void DistanceFieldScene::BindGeometry(RDGPassDescBase& pass, const Generation& generation) const
{
    pass.BindStorageBuffer("FieldVertexBuffer", generation.geometry->snapshots[0]);
    pass.BindStorageBuffer("NodeBuffer", generation.geometry->snapshots[1]);
    pass.BindStorageBuffer("MaterialBuffer", generation.geometry->snapshots[2]);
    pass.BindStorageBuffer("UVBuffer", generation.geometry->snapshots[3]);
    pass.BindStorageBuffer("IndexBuffer", generation.geometry->indices.Get());
    pass.BindStorageBuffer("TriangleRecords", generation.geometry->triangles.Get());
    if (pass.shaderProgramName == NameID("DistanceFieldJobsSP") || pass.shaderProgramName == NameID("DistanceFieldCoverageSP"))
    {
        pass.BindStorageBuffer("FieldSamplerInfo", generation.geometry->samplerInfo);
        pass.BindStorageBuffer("FieldBuildRanges", generation.ranges);
    }
    HeapVector<RHITextureView*> views;
    HeapVector<RHISampler*>     samplers;
    for (const RHIResourcePtr<RHITextureView>& view : generation.geometry->views)
    {
        views.push_back(view.Get());
    }
    for (const RHIResourcePtr<RHISampler>& sampler : generation.geometry->samplers)
    {
        samplers.push_back(sampler.Get());
    }
    BindSceneTextureArray(pass, m_device->CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat()), views, samplers);
}

void DistanceFieldScene::BindBuild(RDGPassDescBase& pass, const Generation& generation) const
{
    const bool                clear       = pass.shaderProgramName == NameID("DistanceFieldClearSP");
    const bool                retain      = pass.shaderProgramName == NameID("DistanceFieldRetainSP");
    const bool                recycle     = pass.shaderProgramName == NameID("DistanceFieldRecycleSP");
    const bool                batchClear  = pass.shaderProgramName == NameID("DistanceFieldBatchClearSP");
    const bool                jobs        = pass.shaderProgramName == NameID("DistanceFieldJobsSP");
    const bool                coverage    = pass.shaderProgramName == NameID("DistanceFieldCoverageSP");
    const bool                count       = pass.shaderProgramName == NameID("DistanceFieldCountSP");
    const bool                scan        = pass.shaderProgramName == NameID("DistanceFieldScanSP");
    const bool                scanSums    = pass.shaderProgramName == NameID("DistanceFieldScanSumsSP");
    const bool                allocate    = pass.shaderProgramName == NameID("DistanceFieldAllocateSP");
    const bool                compact     = pass.shaderProgramName == NameID("DistanceFieldCompactSP");
    const bool                emit        = pass.shaderProgramName == NameID("DistanceFieldEmitSP");
    const bool                prepare     = pass.shaderProgramName == NameID("DistanceFieldPrepareSP");
    const RDGContentGuarantee initialized = clear ? RDGContentGuarantee::eFullWrite : RDGContentGuarantee::eNone;
    pass.BindValue("uDistanceFieldData", generation.uniform);
    pass.BindValue("uDistanceFieldBuild", generation.build);
    if (retain || count || compact)
    {
        pass.BindValue("uFieldDirty", generation.dirty);
    }
    if (clear || retain || recycle)
    {
        pass.BindStorageBuffer("FieldBrickUsed", generation.workspace->brickUsed, initialized);
    }
    if (clear || recycle || allocate)
    {
        pass.BindStorageBuffer("FieldFreeBricks", generation.workspace->freeBricks, initialized);
    }
    if (retain)
    {
        pass.BindStorageBuffer("FieldPreviousCells", m_active->cells);
    }
    pass.BindStorageBuffer("DistanceFieldCells", generation.cells, initialized);
    pass.BindStorageBuffer("DistanceFieldHierarchy", generation.hierarchy, initialized);
    pass.BindStorageBuffer("FieldBuildCounters", generation.workspace->counters, initialized);
    pass.BindStorageBuffer("FieldBuildControl", generation.workspace->control, initialized);
    if (clear || prepare)
    {
        pass.BindStorageBuffer("FieldBuildIndirect", generation.workspace->indirect, RDGContentGuarantee::eFullWrite);
    }
    // Clear initializes complete arrays, including prefixes read by inactive indirect
    // phases. Subsequent partial writes preserve that defined-content contract.
    if (clear || jobs || coverage || count || compact || emit)
    {
        pass.BindStorageBuffer("FieldBuildJobs", generation.workspace->jobs, initialized);
    }
    if (clear || batchClear || count || scan || allocate || compact || emit)
    {
        pass.BindStorageBuffer("FieldBuildCounts", generation.workspace->counts, initialized);
    }
    if (clear || scan || allocate || compact || emit)
    {
        pass.BindStorageBuffer("FieldBuildOffsets", generation.workspace->offsets, initialized);
    }
    if (clear || batchClear || compact)
    {
        pass.BindStorageBuffer("FieldBuildCursors", generation.workspace->cursors, initialized);
    }
    if (clear || compact || emit)
    {
        pass.BindStorageBuffer("FieldBuildReferences", generation.workspace->references, initialized);
    }
    if (clear || allocate || emit)
    {
        pass.BindStorageBuffer("FieldBuildActive", generation.workspace->active, initialized);
        pass.BindStorageBuffer("FieldBuildEmitSplit", generation.workspace->emitSplit, initialized);
    }
    if (clear || scan || scanSums || allocate)
    {
        pass.BindStorageBuffer("FieldBuildScan", generation.workspace->scan, initialized);
    }
    if (clear || jobs || coverage || prepare)
    {
        pass.BindStorageBuffer("FieldBuildCoverage", generation.workspace->coverage, initialized);
    }
    if (clear || jobs || coverage || prepare)
    {
        pass.BindStorageBuffer("FieldBuildCoverageQueue", generation.workspace->coverageQueue, initialized);
    }
    pass.BindStorageImage("distanceFieldBuildAtlas", generation.atlas->GetDefaultView());
}

void DistanceFieldScene::RecordSnapshot(Generation& generation)
{
    RenderGraph& graph = *m_device->GetCurrentFrameRDG();
    for (uint32_t index = 0; !generation.geometry->snapshotSubmitted && index < kSnapshotBuffers; ++index)
    {
        RDGComputePassDesc pass;
        pass.SetShaderProgramName(index == 0 ? "DistanceFieldSnapshotVerticesSP" : "DistanceFieldSnapshotSP");
        pass.SetPassTag("FieldSnapshotGeometry");
        const uint32_t count  = static_cast<uint32_t>(generation.geometry->snapshots[index]->GetRequiredSize()
                                                      / (index == 0 ? sizeof(Vec4) : sizeof(uint32_t)));
        const uint32_t stride = sizeof(asset::Vertex) / sizeof(uint32_t);
        const uint32_t alpha  = (offsetof(asset::Vertex, color) + 3 * sizeof(float)) / sizeof(uint32_t);
        pass.BindValue("uFieldCopy", glm::uvec4(0, count, index == 0 ? stride : 0, index == 0 ? alpha : 0));
        pass.BindStorageBuffer("FieldCopySource", generation.geometry->sources[index].Get());
        pass.BindStorageBuffer("FieldCopyDestination", generation.geometry->snapshots[index], RDGContentGuarantee::eFullWrite);
        const uint32_t groups  = (count + 63) / 64;
        const uint32_t groupsX = std::min(groups, 65535u);
        const uint32_t groupsY = (groups + groupsX - 1) / groupsX;
        pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
        graph.AddComputePass(std::move(pass)).RecordPassCommands([groupsX, groupsY](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(groupsX, groupsY, 1);
        });
    }
    if (generation.partial)
    {
        RHITextureCopyRegion region;
        region.size = Vec3i(generation.uniform.atlasDimensions.x);
        region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        graph.AddTransferPass("FieldRetainAtlas").CopyTexture(m_active->atlas, generation.atlas, MakeVecView(&region, 1));
    }
    else
    {
        graph.AddTransferPass("FieldInitializeAtlas").ClearTexture(generation.atlas, Color(1.0f));
    }
    RDGComputePassDesc clear;
    clear.SetShaderProgramName("DistanceFieldClearSP");
    clear.SetPassTag("FieldClearGeneration");
    BindBuild(clear, generation);
    const BuildWorkspace& workspace = *generation.workspace;
    const uint64_t        words =
        std::max<uint64_t>({HierarchyWords(m_settings.cellsPerAxis), workspace.referenceCapacity, workspace.jobCapacity,
                            uint64_t(workspace.coverageSlots) * ZEN_FIELD_COVERAGE_STATE_WORDS, workspace.bricks,
                            ActiveEntries(workspace.cells, workspace.referenceCapacity),
                            SplitSlots(workspace.referenceCapacity) * ZEN_FIELD_EMIT_SPLIT_WORDS});
    DispatchField(graph, std::move(clear), static_cast<uint32_t>((words + 255) / 256));
    if (generation.partial)
    {
        for (const char* program : {"DistanceFieldRetainSP", "DistanceFieldRecycleSP"})
        {
            RDGComputePassDesc pass;
            pass.SetShaderProgramName(program);
            pass.SetPassTag(program);
            BindBuild(pass, generation);
            DispatchField(graph, std::move(pass), (static_cast<uint32_t>(generation.workspace->cells) + 255) / 256);
        }
    }
}

void DistanceFieldScene::RecordBuild(Generation& generation)
{
    RenderGraph&   graph      = *m_device->GetCurrentFrameRDG();
    const char*    programs[] = {"DistanceFieldBatchClearSP", "DistanceFieldJobsSP",    "DistanceFieldCoverageSP",
                                 "DistanceFieldCountSP",      "DistanceFieldScanSP",    "DistanceFieldScanSumsSP",
                                 "DistanceFieldAllocateSP",   "DistanceFieldCompactSP", "DistanceFieldEmitSP",
                                 "DistanceFieldHierarchySP"};
    const uint32_t firstPhase = generation.triangleCount == 0 ? ZEN_FIELD_PHASE_HIERARCHY : ZEN_FIELD_PHASE_CLEAR_BATCH;
    // Only the GPU's current phase dispatches work, and phases are recorded in order, so one cycle
    // finishes at most one triangle batch: a batch's clear and jobs phases precede the previous
    // batch's emission. One cycle per frame tied readiness to the frame rate (New Sponza: about
    // 3,900 frames). Repeated cycles let a frame finish several batches.
    const uint32_t cycles            = NextCycleCount();
    m_recordedCycles                 = cycles;
    generation.build.triangleRange.y = m_batchCap;
    uint32_t previousPhase = ZEN_FIELD_PHASE_FAILED;
    for (uint32_t cycle = 0; cycle < cycles; ++cycle)
    {
        for (uint32_t phase = firstPhase; phase <= ZEN_FIELD_PHASE_HIERARCHY; ++phase)
        {
            const bool     heavy      = phase == ZEN_FIELD_PHASE_COVERAGE || phase == ZEN_FIELD_PHASE_COUNT
                                     || phase == ZEN_FIELD_PHASE_COMPACT || phase == ZEN_FIELD_PHASE_EMIT;
            const uint32_t dispatches = phase == ZEN_FIELD_PHASE_COVERAGE && !generation.geometry->hasCoverageMaterials ? 0u
                                      : heavy && !UsesThroughputBuild() ? m_settings.dispatchesPerFrame
                                                                        : 1u;
            for (uint32_t dispatch = 0; dispatch < dispatches; ++dispatch)
            {
                // The previous work is complete at this dependency. Advance it and prepare the
                // next dispatch together, avoiding a second serial scheduling pass per slice.
                const uint32_t jobs = UsesThroughputBuild() && heavy    ? m_dispatchLimits[phase]
                                    : phase == ZEN_FIELD_PHASE_COVERAGE ? ZEN_FIELD_COVERAGE_TRIANGLES_PER_DISPATCH
                                                                        : m_settings.jobsPerDispatch;
                const uint32_t cells =
                    UsesThroughputBuild() ? m_dispatchLimits[ZEN_FIELD_PHASE_EMIT] : m_settings.cellBatchSize;
                const glm::uvec4   schedule(phase, jobs, cells, previousPhase);
                RDGComputePassDesc prepare;
                prepare.SetShaderProgramName("DistanceFieldPrepareSP");
                prepare.SetPassTag("FieldPrepare");
                prepare.BindValue("uFieldSchedule", schedule);
                BindBuild(prepare, generation);
                DispatchField(graph, std::move(prepare), 1);
                RDGComputePassDesc work;
                work.SetShaderProgramName(programs[phase]);
                work.SetPassTag(programs[phase]);
                BindBuild(work, generation);
                if (phase == ZEN_FIELD_PHASE_JOBS || phase == ZEN_FIELD_PHASE_COVERAGE || phase == ZEN_FIELD_PHASE_COUNT
                    || phase == ZEN_FIELD_PHASE_COMPACT || phase == ZEN_FIELD_PHASE_EMIT)
                {
                    BindGeometry(work, generation);
                }
                RHIBuffer* arguments = generation.workspace->indirect;
                work.UseIndirectBuffer(arguments);
                work.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
                graph.AddComputePass(std::move(work)).RecordPassCommands([arguments](RDGPassCmdEncoder& encoder) {
                    encoder.DispatchIndirect(arguments, 0);
                });
                previousPhase = phase;
            }
        }
    }
    RDGComputePassDesc advance;
    advance.SetShaderProgramName("DistanceFieldAdvanceSP");
    advance.SetPassTag("FieldAdvance");
    advance.BindValue("uFieldSchedule", glm::uvec4(previousPhase, 0, 0, 0));
    BindBuild(advance, generation);
    DispatchField(graph, std::move(advance), 1);
    // A busy readback ring skips statistics, never construction. Completion is
    // observed later, and publication still waits for the latest submitted user.
    for (uint32_t index = 0; generation.recordedStatus < 0 && index < kStatusSlots; ++index)
    {
        BuildWorkspace::StatusSlot& slot = generation.workspace->status[index];
        if (!slot.pending)
        {
            const uint64_t controlBytes = ZEN_FIELD_CONTROL_WORDS * sizeof(uint32_t);
            graph.AddTransferPass("FieldReadBuildStatus")
                .CopyBuffer(generation.workspace->control, slot.buffer, {0, 0, controlBytes})
                .CopyBuffer(generation.workspace->counters, slot.buffer,
                            {0, controlBytes, ZEN_FIELD_COUNTER_WORDS * sizeof(uint32_t)})
                .NeverCull();
            generation.recordedStatus = static_cast<int32_t>(index);
        }
    }
}

DistanceFieldScene::State DistanceFieldScene::BuildRenderGraph(RenderScene& scene)
{
    if (!m_recorded)
    {
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (m_lastFrame != std::chrono::steady_clock::time_point{})
        {
            const double interval = std::chrono::duration<double, std::micro>(now - m_lastFrame).count();
            m_frameIntervalUs     = m_frameIntervalUs > 0.0 ? 0.5 * (m_frameIntervalUs + interval) : interval;
        }
        m_lastFrame = now;
        Poll();
        const uint64_t identity = scene.GetIdentity();
        const uint64_t revision = scene.GetGeometryRevision();
        if (m_state != State::Failed)
        {
            m_failedScene    = identity;
            m_failedRevision = revision;
        }
        UpdateDynamicVolume(scene);
        UpdateStaleRegions(scene);
        if ((m_pending && m_pending->scene != identity) || (m_active && m_active->scene != identity))
        {
            Retire(m_pending);
            Retire(m_active);
            Retire(m_local);
            RetireSharedInputs();
            m_state        = State::Pending;
            m_buildingSlot = -1;
            m_copySlot     = -1;
            m_copyRecorded = false;
            for (LocalSlot& slot : m_slots)
            {
                slot = LocalSlot{slot.core};
            }
        }
        if (m_state == State::Failed && (m_failedScene != identity || m_failedRevision != revision))
        {
            m_state          = State::Pending;
            m_failureReason  = "";
            m_failedScene    = identity;
            m_failedRevision = revision;
            for (LocalSlot& slot : m_slots)
            {
                slot.failed = false;
                slot.queued = !slot.ready;
            }
        }
        if (m_pending && m_pending->local && m_pending->request != m_localRequest)
        {
            Retire(m_pending);
        }
        if (m_local && m_local->request != m_localRequest)
        {
            Retire(m_local);
            m_copySlot = -1;
        }
        if (!LocalWorkRemains() && m_state == State::Ready && !m_pending)
        {
            RetireSharedInputs();
        }
        if (!m_pending && m_geometry && m_geometry->revision != revision)
        {
            // Local refinement can retain the old snapshot. A refresh cannot reuse it, and must
            // not wait forever for the local queue that it takes priority over to drain.
            RetireSharedInputs();
        }
        // The pool follows the published scene field, so a field that never publishes allocates none.
        const bool poolCurrent = m_poolAtlas != nullptr && m_poolSlots == m_slots.size();
        if (m_state == State::Ready && !poolCurrent)
        {
            CreateLocalPool(static_cast<uint32_t>(m_slots.size()));
        }
        const bool poolReady = m_poolAtlas != nullptr && m_poolSlots == m_slots.size();
        if (m_device->AreSubmissionsBlocked())
        {
            Fail("device submissions are blocked");
        }
        else
        {
            // A published generation older than the committed geometry is refreshed after any build
            // in flight completes; that build's snapshot stays consistent, so edits cannot starve it.
            const bool refresh = m_state == State::Ready && m_active && !m_active->local
                              && m_active->revision != scene.GetGeometryRevision(GI_STATIC);
            if (!m_pending && (m_state == State::Pending || refresh))
            {
                Start(scene);
            }
            else if (!m_pending && m_state == State::Ready && m_copySlot < 0 && poolReady)
            {
                StartNextLocal(scene);
            }
            if (m_copySlot >= 0 && m_local && poolReady && !m_copyRecorded)
            {
                RecordLocalCopy();
                m_recorded = true;
            }
            if (m_pending && m_pending->stage != BuildStage::AwaitCompletion)
            {
                if (m_pending->stage == BuildStage::Snapshot)
                {
                    RecordSnapshot(*m_pending);
                }
                else
                {
                    RecordBuild(*m_pending);
                }
                m_recorded = true;
            }
        }
    }
    UpdateStaleRegions(scene);
    UpdateLocalVolumes(scene);
    return m_state == State::Ready && m_poolAtlas == nullptr ? State::Pending : m_state;
}

bool DistanceFieldScene::IsCurrent(const RenderScene& scene) const
{
    const bool dynamicReady = !m_hasDynamic
                           || (m_detailCores.size() < m_slots.size() && m_locals.volumes[m_detailCores.size()].slot.z == 1u
                               && LocalVolumeCurrent(m_slots.back(), scene));
    return m_state == State::Ready && m_active && m_active->scene == scene.GetIdentity()
        && m_active->revision == scene.GetGeometryRevision(GI_STATIC) && dynamicReady;
}

void DistanceFieldScene::UpdateStaleRegions(const RenderScene& scene)
{
    const DistanceFieldStaleUniform previous = m_stale;
    m_stale                                  = {};
    if (m_state == State::Ready && m_active && m_active->revision != scene.GetGeometryRevision(GI_STATIC))
    {
        const HeapVector<RenderScene::ShadowCaster>& current   = scene.GetShadowCasters();
        const HeapVector<RenderScene::ShadowCaster>& published = m_active->casters;
        // Old distances reach a hit only within the hit band, a trilinear stencil and one R8 step of the
        // edited surfaces (the same guard as a local volume's authority).
        const float guard =
            m_active->uniform.gridMinCellSize.w * (2.0f * m_active->uniform.tracing.x + std::sqrt(3.0f) / 7.0f + 1.0f / 255.0f);
        HeapVector<AABB> boxes;
        bool             whole = current.size() != published.size();
        for (size_t index = 0; index < current.size() && !whole; ++index)
        {
            if (current[index].revision != published[index].revision)
            {
                for (const sg::AABB& bounds : {published[index].bounds, current[index].bounds})
                {
                    const Vec3 minimum = bounds.GetMin();
                    const Vec3 maximum = bounds.GetMax();
                    if (minimum.x <= maximum.x && minimum.y <= maximum.y && minimum.z <= maximum.z)
                    {
                        boxes.push_back(AABB(minimum - Vec3(guard), maximum + Vec3(guard)));
                    }
                }
            }
        }
        // Too many separate edits merge into their union; a changed instance list stales everything.
        if (whole || boxes.size() > ZEN_FIELD_STALE_BOXES)
        {
            Vec3 minimum(std::numeric_limits<float>::max());
            Vec3 maximum(std::numeric_limits<float>::lowest());
            for (const AABB& box : boxes)
            {
                minimum = glm::min(minimum, box.GetMin());
                maximum = glm::max(maximum, box.GetMax());
            }
            const float extent = m_active->uniform.gridMinCellSize.w * static_cast<float>(m_active->uniform.dimensions.x);
            const Vec3  fieldMinimum(m_active->uniform.gridMinCellSize);
            boxes.clear();
            boxes.push_back(whole ? AABB(fieldMinimum, fieldMinimum + Vec3(extent)) : AABB(minimum, maximum));
        }
        m_stale.count.x = static_cast<uint32_t>(boxes.size());
        for (size_t index = 0; index < boxes.size(); ++index)
        {
            m_stale.minimum[index] = Vec4(boxes[index].GetMin(), 0);
            m_stale.maximum[index] = Vec4(boxes[index].GetMax(), 0);
        }
    }
    if (std::memcmp(&previous, &m_stale, sizeof(m_stale)) != 0)
    {
        // A previously valid visibility sample must not survive a newly stale region.
        ++m_localRevision;
    }
}

void DistanceFieldScene::OnRenderGraphExecuted(RenderFrameResult result)
{
    if (m_copyRecorded)
    {
        if (result == RenderFrameResult::eSubmitted && m_local && m_copySlot >= 0)
        {
            LocalSlot& slot       = m_slots[m_copySlot];
            slot.ready            = true;
            slot.unavailableCells = m_local->unavailableCells;
            ++m_localRevision;
            slot.gridMinCellSize = m_local->uniform.gridMinCellSize;
            slot.bounds.clear();
            slot.revisions.clear();
            for (const RenderScene::ShadowCaster& caster : m_local->casters)
            {
                slot.bounds.push_back(AABB(caster.bounds.GetMin(), caster.bounds.GetMax()));
                slot.revisions.push_back(caster.revision);
            }
            // The copy reads the generation; release it after that submission's GPU work.
            m_local->retirement.Extend(m_device->CaptureResourceRetirement());
            Retire(m_local);
            m_copySlot = -1;
        }
        m_copyRecorded = false;
    }
    if (m_recorded && m_pending)
    {
        if (result == RenderFrameResult::eSubmitted)
        {
            Generation& generation = *m_pending;
            ++generation.submittedGraphs;
            generation.submittedCycles         += m_recordedCycles;
            m_lastSubmittedGraphs               = generation.submittedGraphs;
            const ResourceRetirement submitted  = m_device->CaptureResourceRetirement();
            generation.retirement.Extend(submitted);
            generation.geometry->retirement.Extend(submitted);
            generation.workspace->retirement.Extend(submitted);
            if (generation.stage == BuildStage::Snapshot)
            {
                if (!generation.geometry->snapshotSubmitted)
                {
                    generation.geometry->snapshotRetirement = submitted;
                    generation.geometry->snapshotSubmitted  = true;
                }
                // The clear pass writes every cell completed-empty with an empty hierarchy: a scene
                // without triangles is complete once that submission retires, with no GPU status.
                generation.stage =
                    generation.triangleCount == 0 && !generation.partial ? BuildStage::AwaitCompletion : BuildStage::Building;
            }
            if (generation.recordedStatus >= 0)
            {
                BuildWorkspace::StatusSlot& slot = generation.workspace->status[generation.recordedStatus];
                slot.retirement                  = submitted;
                slot.graph                       = generation.submittedGraphs;
                slot.pending                     = true;
            }
        }
        else if (result != RenderFrameResult::eDeclined)
        {
            Fail("construction graph was not submitted");
        }
        if (m_pending)
        {
            m_pending->recordedStatus = -1;
        }
    }
    m_recordedCycles = 0;
    m_recorded       = false;
}

bool DistanceFieldScene::BindTraceInputs(RDGPassDescBase& pass) const
{
    const bool ready = m_state == State::Ready && m_active && !m_device->AreSubmissionsBlocked();
    if (ready)
    {
        RHISamplerCreateInfo sampler;
        sampler.minFilter = RHISamplerFilter::eLinear;
        sampler.magFilter = RHISamplerFilter::eLinear;
        sampler.maxLod    = 0;
        pass.BindValue("uDistanceFieldData", m_active->uniform);
        pass.BindStorageBuffer("DistanceFieldCells", m_active->cells);
        pass.BindStorageBuffer("DistanceFieldHierarchy", m_active->hierarchy);
        pass.BindSampledTexture("distanceFieldAtlas", m_device->CreateSampler(sampler), m_active->atlas->GetDefaultView());
    }
    return ready;
}

void DistanceFieldScene::Destroy()
{
    if (m_device != nullptr)
    {
        ReleaseLocalPool();
        // RenderDevice defers every owned resource release through its submission history.
        // Teardown does not block or assume a CPU scope exit means GPU completion.
        if (m_active)
        {
            Release(*m_active);
        }
        if (m_pending)
        {
            Release(*m_pending);
        }
        if (m_local)
        {
            Release(*m_local);
        }
        for (UniquePtr<Generation>& generation : m_retired)
        {
            Release(*generation);
        }
        m_active.Reset();
        m_pending.Reset();
        m_local.Reset();
        ReleaseGeometry(m_geometry, false);
        ReleaseWorkspace(m_workspace, false);
        m_retired.clear();
        m_recordedCycles  = 0;
        m_state           = State::Pending;
        m_recorded        = false;
        m_failedScene     = 0;
        m_failedRevision  = 0;
    }
}
} // namespace zen::rc
