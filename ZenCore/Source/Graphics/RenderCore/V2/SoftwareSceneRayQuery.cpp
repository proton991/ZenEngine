#include "Graphics/RenderCore/V2/SoftwareSceneRayQuery.h"
#include "Graphics/RenderCore/V2/SceneRayQuery.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include <algorithm>
#include <array>
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

void BuildNodes(HeapVector<SoftwareRayNode>&     nodes,
                HeapVector<SoftwareRayTriangle>& triangles,
                uint32_t                         first,
                uint32_t                         count,
                uint32_t                         depth = 0)
{
    const uint32_t  index = static_cast<uint32_t>(nodes.size());
    SoftwareRayNode node;
    Vec3            minimum(std::numeric_limits<float>::max());
    Vec3            maximum(-std::numeric_limits<float>::max());
    Vec3            centerMinimum = minimum;
    Vec3            centerMaximum = maximum;
    for (uint32_t i = first; i < first + count; ++i)
    {
        const SoftwareRayTriangle& triangle = triangles[i];
        minimum           = glm::min(minimum, glm::min(Vec3(triangle.a), glm::min(Vec3(triangle.b), Vec3(triangle.c))));
        maximum           = glm::max(maximum, glm::max(Vec3(triangle.a), glm::max(Vec3(triangle.b), Vec3(triangle.c))));
        const Vec3 center = (Vec3(triangle.a) + Vec3(triangle.b) + Vec3(triangle.c)) / 3.0f;
        centerMinimum     = glm::min(centerMinimum, center);
        centerMaximum     = glm::max(centerMaximum, center);
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
    if (count > kLeafTriangles)
    {
        const Vec3  extent = centerMaximum - centerMinimum;
        TriangleBin best{};
        float       bestCost   = std::numeric_limits<float>::max();
        bool        splitFound = false;
        // Surface-area splits reduce overlapping child bounds. Cap skewed build
        // depth, then use median splits; traversal still visits every candidate.
        if (depth < 32)
        {
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                if (extent[axis] > 0.0f)
                {
                    const TriangleBin partition{axis, centerMinimum[axis], static_cast<float>(kBinCount) / extent[axis], 0};
                    std::array<TriangleBounds, kBinCount> bins{};
                    for (uint32_t i = first; i < first + count; ++i)
                    {
                        const SoftwareRayTriangle& triangle = triangles[i];
                        bins[partition.Index(triangle)].Add(
                            glm::min(Vec3(triangle.a), glm::min(Vec3(triangle.b), Vec3(triangle.c))),
                            glm::max(Vec3(triangle.a), glm::max(Vec3(triangle.b), Vec3(triangle.c))), 1);
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
        if (splitFound)
        {
            middle = static_cast<uint32_t>(std::partition(triangles.begin() + first, triangles.begin() + first + count, best)
                                           - triangles.begin());
        }
        else
        {
            const uint32_t axis = extent.x >= extent.y && extent.x >= extent.z ? 0u : extent.y >= extent.z ? 1u : 2u;
            std::nth_element(triangles.begin() + first, triangles.begin() + middle, triangles.begin() + first + count,
                             TriangleOrder{axis});
        }
        BuildNodes(nodes, triangles, first, middle - first, depth + 1);
        BuildNodes(nodes, triangles, middle, first + count - middle, depth + 1);
    }
    nodes[index].escape = static_cast<uint32_t>(nodes.size());
}
} // namespace

bool SoftwareSceneRayQuery::Prepare(RenderScene& scene)
{
    if (m_scene != &scene)
    {
        Destroy();
        m_scene = &scene;
    }
    bool ready = !scene.HasUncommittedVertices() && !m_device->AreSubmissionsBlocked();
    if (ready && (m_nodes == nullptr || m_placeholder || m_revision != scene.GetGeometryRevision()))
    {
        const HeapVector<glm::uvec4>     records = scene.BuildTriangleRecords();
        HeapVector<SoftwareRayTriangle>  triangles;
        const HeapVector<asset::Vertex>& vertices = scene.GetVertices();
        const HeapVector<uint32_t>&      indices  = scene.GetIndices();
        for (const glm::uvec4& record : records)
        {
            if (record.w != 0 && ready)
            {
                ready = record.x <= indices.size() && indices.size() - record.x >= 3;
                if (ready)
                {
                    const uint32_t a = indices[record.x], b = indices[record.x + 1], c = indices[record.x + 2];
                    ready = a < vertices.size() && b < vertices.size() && c < vertices.size();
                    if (ready)
                    {
                        const Mat4&               transform = scene.GetInstanceTransform(record.y);
                        const SoftwareRayTriangle triangle{transform * Vec4(Vec3(vertices[a].pos), 1),
                                                           transform * Vec4(Vec3(vertices[b].pos), 1),
                                                           transform * Vec4(Vec3(vertices[c].pos), 1), record};
                        for (uint32_t axis = 0; axis < 3; ++axis)
                        {
                            ready &= std::isfinite(triangle.a[axis]) && std::isfinite(triangle.b[axis])
                                  && std::isfinite(triangle.c[axis]);
                        }
                        triangles.push_back(triangle);
                    }
                }
            }
        }
        HeapVector<SoftwareRayNode> nodes;
        // The 32-bit triangle-buffer byte limit also keeps offsets inside the
        // node encoding's 29-bit triangle-index field.
        ready = ready && triangles.size() <= std::numeric_limits<uint32_t>::max() / sizeof(SoftwareRayTriangle)
             && triangles.size() <= std::numeric_limits<uint32_t>::max() / (2 * sizeof(SoftwareRayNode));
        if (ready)
        {
            BuildNodes(nodes, triangles, 0, static_cast<uint32_t>(triangles.size()));
            if (triangles.empty())
            {
                triangles.push_back(SoftwareRayTriangle{});
            }
            uint64_t nodeBytes = 0, triangleBytes = 0;
            ready =
                ValidateGIStorageBuffer(nodes.size(), sizeof(SoftwareRayNode), m_device->GetGPUInfo(), nodeBytes)
                    == GIResourceStatus::eSuccess
                && ValidateGIStorageBuffer(triangles.size(), sizeof(SoftwareRayTriangle), m_device->GetGPUInfo(), triangleBytes)
                       == GIResourceStatus::eSuccess;
        }
        if (ready)
        {
            RHIBuffer* nodeBuffer =
                m_device->CreateStorageBuffer(static_cast<uint32_t>(nodes.size() * sizeof(SoftwareRayNode)),
                                              reinterpret_cast<const uint8_t*>(nodes.data()), "software_ray_nodes");
            RHIBuffer* triangleBuffer =
                m_device->CreateStorageBuffer(static_cast<uint32_t>(triangles.size() * sizeof(SoftwareRayTriangle)),
                                              reinterpret_cast<const uint8_t*>(triangles.data()), "software_ray_triangles");
            ready = nodeBuffer != nullptr && triangleBuffer != nullptr;
            // DestroyBuffer retires old storage after all submitted readers finish.
            m_device->DestroyBuffer(m_nodes);
            m_device->DestroyBuffer(m_triangles);
            m_nodes       = nodeBuffer;
            m_triangles   = triangleBuffer;
            m_revision    = scene.GetGeometryRevision();
            m_nodeCount   = static_cast<uint32_t>(nodes.size());
            m_placeholder = false;
        }
    }
    if (!ready)
    {
        LOGE("Software scene ray-query preparation failed; refusing stale geometry");
        Destroy();
    }
    return ready && IsReady();
}

bool SoftwareSceneRayQuery::PrepareUnused()
{
    if (!m_placeholder || m_nodes == nullptr || m_triangles == nullptr)
    {
        Destroy();
        // A zero node count makes every traversal miss without reading these records.
        const SoftwareRayNode     node{};
        const SoftwareRayTriangle triangle{};
        m_nodes =
            m_device->CreateStorageBuffer(sizeof(node), reinterpret_cast<const uint8_t*>(&node), "software_ray_nodes_unused");
        m_triangles   = m_device->CreateStorageBuffer(sizeof(triangle), reinterpret_cast<const uint8_t*>(&triangle),
                                                      "software_ray_triangles_unused");
        m_placeholder = true;
    }
    return IsReady();
}

bool SoftwareSceneRayQuery::IsReady() const
{
    return m_nodes != nullptr && m_triangles != nullptr && !m_device->AreSubmissionsBlocked();
}

void SoftwareSceneRayQuery::BindTraversalInputs(RDGPassDescBase& pass) const
{
    pass.BindValue("uSoftwareRayData", glm::uvec4(m_nodeCount, 0, 0, 0));
    pass.BindStorageBuffer("SoftwareRayNodes", m_nodes);
    pass.BindStorageBuffer("SoftwareRayTriangles", m_triangles);
}

void SoftwareSceneRayQuery::BindInputs(RDGPassDescBase& pass, RenderScene& scene, bool forwardMaterialBound) const
{
    BindTraversalInputs(pass);
    BindSceneRayGeometry(pass, *m_device, scene, forwardMaterialBound);
}

void SoftwareSceneRayQuery::OnRenderGraphExecuted(bool succeeded)
{
    if (!succeeded)
    {
        Destroy();
    }
}

void SoftwareSceneRayQuery::Destroy()
{
    m_device->DestroyBuffer(m_nodes);
    m_device->DestroyBuffer(m_triangles);
    m_nodes     = nullptr;
    m_triangles = nullptr;
    m_scene     = nullptr;
    m_revision    = 0;
    m_nodeCount   = 0;
    m_placeholder = false;
}
} // namespace zen::rc
