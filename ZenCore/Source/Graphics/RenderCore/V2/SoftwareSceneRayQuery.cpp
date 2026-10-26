#include "Graphics/RenderCore/V2/SoftwareSceneRayQuery.h"
#include "Graphics/RenderCore/V2/SceneRayQuery.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Utils/ThreadPool.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>

namespace zen::rc
{
namespace
{
constexpr uint32_t kBinCount      = 16;
constexpr uint32_t kLeafTriangles = 2;
static_assert(kLeafTriangles < 8);

struct TriangleBin
{
    uint32_t axis;
    float    minimum;
    float    scale;
    uint32_t split;

    uint32_t Index(const SoftwareRayTriangle& triangle) const
    {
        const float center = (triangle.a[axis] + triangle.b[axis] + triangle.c[axis]) / 3.0f;
        return std::min(static_cast<uint32_t>(std::max(0.0f, (center - minimum) * scale)), kBinCount - 1);
    }

    bool operator()(const SoftwareRayTriangle& triangle) const
    {
        return Index(triangle) <= split;
    }
};

struct TriangleBounds
{
    Vec3     minimum{std::numeric_limits<float>::max()};
    Vec3     maximum{-std::numeric_limits<float>::max()};
    uint32_t count{0};

    void Add(const Vec3& lower, const Vec3& upper, uint32_t added)
    {
        if (added != 0)
        {
            minimum  = glm::min(minimum, lower);
            maximum  = glm::max(maximum, upper);
            count   += added;
        }
    }

    float Cost() const
    {
        const Vec3 extent = count != 0 ? maximum - minimum : Vec3(0);
        return static_cast<float>(count) * (extent.x * extent.y + extent.x * extent.z + extent.y * extent.z);
    }
};

struct TriangleOrder
{
    uint32_t axis;
    bool     operator()(const SoftwareRayTriangle& first, const SoftwareRayTriangle& second) const
    {
        return first.a[axis] + first.b[axis] + first.c[axis] < second.a[axis] + second.b[axis] + second.c[axis];
    }
};

// Both partition operations check cancellation inside their scans: stopping a
// large Debug build must not wait for a whole uninterruptible STL partition.
uint32_t PartitionTriangles(HeapVector<SoftwareRayTriangle>& triangles,
                            uint32_t                         first,
                            uint32_t                         count,
                            const TriangleBin&               partition,
                            const std::atomic<bool>&         cancelled)
{
    uint32_t end = first + count;
    while (first < end && !cancelled.load(std::memory_order_relaxed))
    {
        while (first < end && partition(triangles[first]) && !cancelled.load(std::memory_order_relaxed))
        {
            ++first;
        }
        while (first < end && !partition(triangles[end - 1]) && !cancelled.load(std::memory_order_relaxed))
        {
            --end;
        }
        if (first < end && !cancelled.load(std::memory_order_relaxed))
        {
            std::swap(triangles[first++], triangles[--end]);
        }
    }
    return first;
}

void SelectMedian(HeapVector<SoftwareRayTriangle>& triangles,
                  uint32_t                         first,
                  uint32_t                         count,
                  uint32_t                         axis,
                  const std::atomic<bool>&         cancelled)
{
    const uint32_t      middle = first + count / 2;
    const TriangleOrder order{axis};
    uint32_t            end = first + count;
    while (first < end && !cancelled.load(std::memory_order_relaxed))
    {
        const SoftwareRayTriangle pivot = triangles[first + (end - first) / 2];
        uint32_t                  lower = first, index = first, upper = end;
        while (index < upper && !cancelled.load(std::memory_order_relaxed))
        {
            if (order(triangles[index], pivot))
            {
                std::swap(triangles[lower++], triangles[index++]);
            }
            else if (order(pivot, triangles[index]))
            {
                std::swap(triangles[index], triangles[--upper]);
            }
            else
            {
                ++index;
            }
        }
        if (middle < lower)
        {
            end = lower;
        }
        else if (middle >= upper)
        {
            first = upper;
        }
        else
        {
            first = end;
        }
    }
}

bool BuildNodes(HeapVector<SoftwareRayNode>&     nodes,
                HeapVector<SoftwareRayTriangle>& triangles,
                uint32_t                         first,
                uint32_t                         count,
                const std::atomic<bool>&         cancelled,
                uint32_t                         depth = 0)
{
    bool            ready = !cancelled.load(std::memory_order_relaxed);
    const uint32_t  index = static_cast<uint32_t>(nodes.size());
    SoftwareRayNode node;
    Vec3            minimum(std::numeric_limits<float>::max());
    Vec3            maximum(-std::numeric_limits<float>::max());
    Vec3            centerMinimum = minimum;
    Vec3            centerMaximum = maximum;
    for (uint32_t i = first; i < first + count && ready; ++i)
    {
        const SoftwareRayTriangle& triangle = triangles[i];
        minimum           = glm::min(minimum, glm::min(Vec3(triangle.a), glm::min(Vec3(triangle.b), Vec3(triangle.c))));
        maximum           = glm::max(maximum, glm::max(Vec3(triangle.a), glm::max(Vec3(triangle.b), Vec3(triangle.c))));
        const Vec3 center = (Vec3(triangle.a) + Vec3(triangle.b) + Vec3(triangle.c)) / 3.0f;
        centerMinimum     = glm::min(centerMinimum, center);
        centerMaximum     = glm::max(centerMaximum, center);
        ready             = !cancelled.load(std::memory_order_relaxed);
    }
    if (count != 0)
    {
        // Conservative bounds cover CPU/GPU transform rounding and planar boxes.
        const Vec3 padding =
            glm::max(glm::abs(minimum), glm::abs(maximum)) * (8.0f * std::numeric_limits<float>::epsilon()) + Vec3(1e-7f);
        node.minimum = minimum - padding;
        node.maximum = maximum + padding;
    }
    node.firstAndCount = count <= kLeafTriangles ? (first << 3u) | count : 0u;
    nodes.push_back(node);
    if (ready && count > kLeafTriangles)
    {
        const Vec3  extent = centerMaximum - centerMinimum;
        TriangleBin best{};
        float       bestCost   = std::numeric_limits<float>::max();
        bool        splitFound = false;
        // Surface-area splits reduce overlapping child bounds. Cap skewed build
        // depth, then use median splits; traversal still visits every candidate.
        if (depth < 32)
        {
            for (uint32_t axis = 0; axis < 3 && ready; ++axis)
            {
                if (extent[axis] > 0.0f)
                {
                    const TriangleBin partition{axis, centerMinimum[axis], static_cast<float>(kBinCount) / extent[axis], 0};
                    std::array<TriangleBounds, kBinCount> bins{};
                    for (uint32_t i = first; i < first + count && ready; ++i)
                    {
                        const SoftwareRayTriangle& triangle = triangles[i];
                        bins[partition.Index(triangle)].Add(
                            glm::min(Vec3(triangle.a), glm::min(Vec3(triangle.b), Vec3(triangle.c))),
                            glm::max(Vec3(triangle.a), glm::max(Vec3(triangle.b), Vec3(triangle.c))), 1);
                        ready = !cancelled.load(std::memory_order_relaxed);
                    }
                    std::array<TriangleBounds, kBinCount> right{};
                    TriangleBounds                        suffix;
                    for (uint32_t bin = kBinCount; bin-- > 0;)
                    {
                        suffix.Add(bins[bin].minimum, bins[bin].maximum, bins[bin].count);
                        right[bin] = suffix;
                    }
                    TriangleBounds left;
                    for (uint32_t bin = 0; bin + 1 < kBinCount; ++bin)
                    {
                        left.Add(bins[bin].minimum, bins[bin].maximum, bins[bin].count);
                        const float cost = left.Cost() + right[bin + 1].Cost();
                        if (left.count != 0 && right[bin + 1].count != 0 && cost < bestCost)
                        {
                            best       = partition;
                            best.split = bin;
                            bestCost   = cost;
                            splitFound = true;
                        }
                    }
                }
            }
        }
        uint32_t middle = first + count / 2;
        if (ready && splitFound)
        {
            middle = PartitionTriangles(triangles, first, count, best, cancelled);
        }
        else if (ready)
        {
            const uint32_t axis = extent.x >= extent.y && extent.x >= extent.z ? 0u : extent.y >= extent.z ? 1u : 2u;
            SelectMedian(triangles, first, count, axis, cancelled);
        }
        ready = ready && !cancelled.load(std::memory_order_relaxed)
             && BuildNodes(nodes, triangles, first, middle - first, cancelled, depth + 1)
             && BuildNodes(nodes, triangles, middle, first + count - middle, cancelled, depth + 1);
    }
    nodes[index].escape = static_cast<uint32_t>(nodes.size());
    return ready && !cancelled.load(std::memory_order_relaxed);
}
struct SoftwareRayDraw
{
    SceneMeshDraw range;
    Mat4          transform;
    uint32_t      mask;
};

struct SoftwareRayBuild
{
    // These two values are compared only by the owner; the worker never accesses a scene.
    const RenderScene*              scene{nullptr};
    uint64_t                        revision{0};
    HeapVector<asset::Vertex>       vertices;
    HeapVector<uint32_t>            indices;
    HeapVector<sg::MaterialData>    materials;
    HeapVector<SoftwareRayDraw>     draws;
    HeapVector<SoftwareRayNode>     nodes;
    HeapVector<SoftwareRayTriangle> triangles;
    std::atomic<bool>               cancelled{false};
    bool                            ready{false};
};

SharedPtr<SoftwareRayBuild, MultiThreadCounter> SnapshotBuild(const RenderScene& scene)
{
    SharedPtr<SoftwareRayBuild, MultiThreadCounter> build(new SoftwareRayBuild);
    build->scene    = &scene;
    build->revision = scene.GetGeometryRevision();
    // Enumerate compact primitive ranges instead of millions of triangle records.
    const HeapVector<SceneMeshDraw> draws = SnapshotSceneDraws(scene, GI_ALL, true, false);
    build->draws.reserve(draws.size());
    for (const SceneMeshDraw& draw : draws)
    {
        build->draws.push_back({draw, scene.GetInstanceTransform(draw.nodeIndex), scene.GetInstanceMask(draw.nodeIndex)});
    }
    return build;
}

void BuildScene(SoftwareRayBuild&                   build,
                const HeapVector<asset::Vertex>&    vertices,
                const HeapVector<uint32_t>&         indices,
                const HeapVector<sg::MaterialData>& materials)
{
    bool ready = !build.cancelled.load(std::memory_order_relaxed);
    for (size_t drawIndex = 0; drawIndex < build.draws.size() && ready; ++drawIndex)
    {
        const SoftwareRayDraw& draw  = build.draws[drawIndex];
        const SceneMeshDraw&   range = draw.range;
        ready = range.firstIndex <= indices.size() && range.indexCount <= indices.size() - range.firstIndex
             && range.indexCount % 3 == 0;
        for (uint32_t offset = 0; offset < range.indexCount && ready; offset += 3)
        {
            ready = !build.cancelled.load(std::memory_order_relaxed);
            if (ready)
            {
                const uint32_t index = range.firstIndex + offset;
                const uint32_t a = indices[index], b = indices[index + 1], c = indices[index + 2];
                ready = a < vertices.size() && b < vertices.size() && c < vertices.size();
                // Match BLAS filtering: coverage edits bump the geometry revision.
                const bool occludes =
                    ready
                    && (range.materialIndex >= materials.size()
                        || MaterialMayOcclude(materials[range.materialIndex],
                                              std::max({vertices[a].color.a, vertices[b].color.a, vertices[c].color.a})));
                if (occludes)
                {
                    const SoftwareRayTriangle triangle{draw.transform * Vec4(Vec3(vertices[a].pos), 1),
                                                       draw.transform * Vec4(Vec3(vertices[b].pos), 1),
                                                       draw.transform * Vec4(Vec3(vertices[c].pos), 1),
                                                       glm::uvec4(index, range.nodeIndex, range.materialIndex, draw.mask)};
                    for (uint32_t axis = 0; axis < 3; ++axis)
                    {
                        ready &= std::isfinite(triangle.a[axis]) && std::isfinite(triangle.b[axis])
                              && std::isfinite(triangle.c[axis]);
                    }
                    build.triangles.push_back(triangle);
                }
            }
        }
    }
    // The byte limit also keeps offsets inside the 29-bit triangle-index field.
    ready = ready && build.triangles.size() <= std::numeric_limits<uint32_t>::max() / sizeof(SoftwareRayTriangle)
         && build.triangles.size() <= std::numeric_limits<uint32_t>::max() / (2 * sizeof(SoftwareRayNode));
    if (ready)
    {
        ready = BuildNodes(build.nodes, build.triangles, 0, static_cast<uint32_t>(build.triangles.size()), build.cancelled);
        if (build.triangles.empty())
        {
            build.triangles.push_back(SoftwareRayTriangle{});
        }
    }
    build.ready = ready && !build.cancelled.load(std::memory_order_relaxed);
}

void BuildSnapshot(SharedPtr<SoftwareRayBuild, MultiThreadCounter> build)
{
    BuildScene(*build, build->vertices, build->indices, build->materials);
    // Release input copies on the worker once construction is finished.
    build->vertices  = {};
    build->indices   = {};
    build->materials = {};
    build->draws     = {};
}
} // namespace

struct SoftwareSceneRayQuery::AsyncPreparation
{
    ThreadPool<void, uint32_t>                      worker;
    SharedPtr<SoftwareRayBuild, MultiThreadCounter> build;
    std::future<void>                               completion;
};

SoftwareSceneRayQuery::SoftwareSceneRayQuery(RenderDevice* device) : m_device(device) {}

SoftwareSceneRayQuery::~SoftwareSceneRayQuery()
{
    // The pool drains at object destruction; ordinary cancellation never waits.
    CancelPreparation();
}

void SoftwareSceneRayQuery::CancelPreparation()
{
    if (m_preparation && m_preparation->build)
    {
        m_preparation->build->cancelled.store(true, std::memory_order_relaxed);
        if (m_preparation->completion.valid()
            && m_preparation->completion.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            m_preparation->completion.get();
            m_preparation->build.Reset();
        }
    }
}

bool SoftwareSceneRayQuery::Publish(const HeapVector<SoftwareRayNode>&     nodes,
                                    const HeapVector<SoftwareRayTriangle>& triangles,
                                    uint64_t                               revision)
{
    uint64_t nodeBytes = 0, triangleBytes = 0;
    bool ready = ValidateGIStorageBuffer(nodes.size(), sizeof(SoftwareRayNode), m_device->GetGPUInfo(), nodeBytes)
                  == GIResourceStatus::eSuccess
              && ValidateGIStorageBuffer(triangles.size(), sizeof(SoftwareRayTriangle), m_device->GetGPUInfo(), triangleBytes)
                     == GIResourceStatus::eSuccess;
    if (ready)
    {
        // Construction reorders both payloads. Preserve ring allocations and their
        // submission retirement, but publish metadata only after both uploads succeed.
        m_nodeBuffers.MarkDirty(0, static_cast<uint32_t>(nodeBytes));
        m_triangleBuffers.MarkDirty(0, static_cast<uint32_t>(triangleBytes));
        ready = m_nodeBuffers.Update(*m_device, nodes.data(), static_cast<uint32_t>(nodeBytes), "software_ray_nodes")
             && m_triangleBuffers.Update(*m_device, triangles.data(), static_cast<uint32_t>(triangleBytes),
                                         "software_ray_triangles");
        if (ready)
        {
            m_revision  = revision;
            m_nodeCount = static_cast<uint32_t>(nodes.size());
            m_triangleCount =
                triangles.size() == 1 && triangles[0].record.w == 0 ? 0u : static_cast<uint32_t>(triangles.size());
            m_placeholder = false;
        }
    }
    return ready;
}

bool SoftwareSceneRayQuery::Rebuild(RenderScene& scene)
{
    SharedPtr<SoftwareRayBuild, MultiThreadCounter> build = SnapshotBuild(scene);
    BuildScene(*build, scene.GetVertices(), scene.GetIndices(), scene.GetMaterialsData());
    return build->ready && Publish(build->nodes, build->triangles, build->revision);
}

bool SoftwareSceneRayQuery::Prepare(RenderScene& scene)
{
    CancelPreparation();
    if (m_scene != &scene)
    {
        Destroy();
        m_scene = &scene;
    }
    bool ready = !scene.HasUncommittedVertices() && !m_device->AreSubmissionsBlocked();
    if (ready && (!HoldsSceneGeometry() || m_revision != scene.GetGeometryRevision()))
    {
        ready = Rebuild(scene);
    }
    if (!ready)
    {
        LOGE("Software scene ray-query preparation failed; refusing stale geometry");
        Destroy();
    }
    return ready && IsReady();
}

SoftwareSceneRayQuery::PreparationResult SoftwareSceneRayQuery::PrepareAsync(RenderScene& scene)
{
    if (m_scene != &scene)
    {
        Destroy();
        m_scene = &scene;
    }
    PreparationResult result = PreparationResult::Pending;
    if (scene.HasUncommittedVertices() || m_device->AreSubmissionsBlocked())
    {
        result = PreparationResult::Failed;
    }
    else if (HoldsSceneGeometry() && m_revision == scene.GetGeometryRevision())
    {
        CancelPreparation();
        result = PreparationResult::Ready;
    }
    else
    {
        if (!m_preparation)
        {
            m_preparation.Reset(new AsyncPreparation);
            m_preparation->worker.Resize(1);
        }
        // Scene changes do not abandon a build: one that restarted on every change would
        // never finish while the scene animates.
        if (m_preparation->build && m_preparation->build->scene != &scene)
        {
            CancelPreparation();
        }
        if (m_preparation->completion.valid()
            && m_preparation->completion.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            m_preparation->completion.get();
            const SoftwareRayBuild& build = *m_preparation->build;
            if (!build.cancelled.load(std::memory_order_relaxed))
            {
                // A scene that changed during the build is likely still changing, and a new
                // snapshot would be outdated again by the time it finished. Build its current
                // geometry here instead, as each changed frame does once frames trace the BVH.
                const bool current = build.revision == scene.GetGeometryRevision();
                result = (current ? build.ready && Publish(build.nodes, build.triangles, build.revision) : Rebuild(scene))
                           ? PreparationResult::Ready
                           : PreparationResult::Failed;
            }
            m_preparation->build.Reset();
        }
        // A cancelled build still owns its snapshot until it completes. Do not queue
        // another snapshot each frame while the single worker is stopping that build.
        if (result == PreparationResult::Pending && !m_preparation->completion.valid())
        {
            m_preparation->build = SnapshotBuild(scene);
            // Only asynchronous construction needs bulk copies. HeapVector uses memcpy
            // for these trivially-copyable payloads; the worker owns their lifetime.
            m_preparation->build->vertices  = scene.GetVertices();
            m_preparation->build->indices   = scene.GetIndices();
            m_preparation->build->materials = scene.GetMaterialsData();
            m_preparation->completion       = m_preparation->worker.Push(&BuildSnapshot, m_preparation->build);
        }
    }
    // Hardware shaders still declare software traversal bindings while the switch
    // is pending. Scene replacement may have retired their previous placeholders.
    if (result == PreparationResult::Pending && !PrepareUnused())
    {
        result = PreparationResult::Failed;
    }
    if (result == PreparationResult::Failed)
    {
        LOGE("Software scene ray-query preparation failed; refusing stale geometry");
        Destroy();
    }
    return result;
}

bool SoftwareSceneRayQuery::IsPreparing() const
{
    return m_preparation && m_preparation->completion.valid();
}

bool SoftwareSceneRayQuery::PrepareUnused()
{
    bool ready = !m_device->AreSubmissionsBlocked();
    if (ready && (m_nodeBuffers.GetBuffer() == nullptr || m_triangleBuffers.GetBuffer() == nullptr))
    {
        // A zero node count makes every traversal miss without reading these records.
        const SoftwareRayNode     node{};
        const SoftwareRayTriangle triangle{};
        ready = m_nodeBuffers.Update(*m_device, &node, sizeof(node), "software_ray_nodes_unused")
             && m_triangleBuffers.Update(*m_device, &triangle, sizeof(triangle), "software_ray_triangles_unused");
        if (ready)
        {
            m_nodeCount     = 0;
            m_triangleCount = 0;
            m_placeholder   = true;
        }
    }
    if (!ready)
    {
        Destroy();
    }
    return ready && IsReady();
}

bool SoftwareSceneRayQuery::IsReady() const
{
    return m_nodeBuffers.GetBuffer() != nullptr && m_triangleBuffers.GetBuffer() != nullptr
        && !m_device->AreSubmissionsBlocked();
}

void SoftwareSceneRayQuery::BindTraversalInputs(RDGPassDescBase& pass) const
{
    pass.BindValue("uSoftwareRayData", glm::uvec4(m_nodeCount, 0, 0, 0));
    pass.BindStorageBuffer("SoftwareRayNodes", m_nodeBuffers.GetBuffer());
    pass.BindStorageBuffer("SoftwareRayTriangles", m_triangleBuffers.GetBuffer());
}

void SoftwareSceneRayQuery::BindInputs(RDGPassDescBase& pass, RenderScene& scene, bool forwardMaterialBound) const
{
    BindTraversalInputs(pass);
    BindSceneRayGeometry(pass, *m_device, scene, forwardMaterialBound);
}

void SoftwareSceneRayQuery::OnRenderGraphExecuted(RenderFrameResult result)
{
    // These buffers are prepared by staging uploads, independently of frame acceptance.
    if (result == RenderFrameResult::eFailed || result == RenderFrameResult::eBlocked)
    {
        Destroy();
    }
}

void SoftwareSceneRayQuery::Destroy()
{
    CancelPreparation();
    m_nodeBuffers.Destroy(*m_device);
    m_triangleBuffers.Destroy(*m_device);
    m_scene         = nullptr;
    m_revision      = 0;
    m_nodeCount     = 0;
    m_triangleCount = 0;
    m_placeholder   = false;
}
} // namespace zen::rc
