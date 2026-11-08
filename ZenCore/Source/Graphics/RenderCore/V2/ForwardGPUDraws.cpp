#include "Graphics/RenderCore/V2/Renderer/ForwardGPUDraws.h"
#include "Graphics/RenderCore/V2/Renderer/ForwardDepthSort.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include <algorithm>
#include <bit>
#include <cfloat>

namespace zen::rc
{
bool SupportsForwardGPUDraws(const RHIGPUInfo& gpu, uint32_t count)
{
    return SupportsForwardDepthSort(gpu, count) && gpu.maxVertexOutputComponents >= 68 && gpu.maxFragmentInputComponents >= 68
        && gpu.maxDrawIndirectCount != 0 && uint64_t(count) * sizeof(ForwardTriangleData) <= gpu.maxStorageBufferRange;
}

namespace
{
uint32_t MortonCoordinate(uint32_t value)
{
    value = (value | (value << 16u)) & 0x030000ffu;
    value = (value | (value << 8u)) & 0x0300f00fu;
    value = (value | (value << 4u)) & 0x030c30c3u;
    value = (value | (value << 2u)) & 0x09249249u;
    return value;
}
uint32_t SpatialKey(Vec3 point, Vec3 minimum, Vec3 maximum)
{
    const Vec3       extent = glm::max(maximum - minimum, Vec3(1e-10f));
    const glm::uvec3 position(glm::clamp((point - minimum) / extent * 1023.0f, Vec3(0.0f), Vec3(1023.0f)));
    return MortonCoordinate(position.x) | (MortonCoordinate(position.y) << 1u) | (MortonCoordinate(position.z) << 2u);
}
} // namespace

void ForwardGPUDraws::PrepareClusters(const RenderScene& scene)
{
    HeapVector<Vec3> centers;
    centers.reserve(m_contents.size());
    Vec3 minimum(FLT_MAX);
    Vec3 maximum(-FLT_MAX);
    for (const ForwardTriangleData& triangle : m_contents)
    {
        const Vec3 center(scene.GetInstanceTransform(triangle.identity.x) * triangle.center);
        centers.push_back(center);
        minimum = glm::min(minimum, center);
        maximum = glm::max(maximum, center);
    }
    HeapVector<glm::uvec2> spatial;
    spatial.reserve(centers.size());
    for (uint32_t index = 0; index < centers.size(); ++index)
    {
        spatial.emplace_back(SpatialKey(centers[index], minimum, maximum), index);
    }
    std::sort(spatial.begin(), spatial.end(), [](const glm::uvec2& left, const glm::uvec2& right) {
        return left.x != right.x ? left.x < right.x : left.y < right.y;
    });
    m_clusterContents.clear();
    constexpr uint32_t clusterSize = 64;
    for (uint32_t first = 0; first < spatial.size(); first += clusterSize)
    {
        ForwardClusterBounds bounds{Vec4(FLT_MAX), Vec4(-FLT_MAX)};
        for (uint32_t index = first; index < std::min(first + clusterSize, uint32_t(spatial.size())); ++index)
        {
            ForwardTriangleData& triangle = m_contents[spatial[index].y];
            triangle.identity.w           = static_cast<uint32_t>(m_clusterContents.size());
            const Mat4& model             = scene.GetInstanceTransform(triangle.identity.x);
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                const uint32_t vertex = scene.GetIndices()[triangle.identity.z + corner];
                const Vec4     point  = model * Vec4(Vec3(scene.GetVertices()[vertex].pos), 1.0f);
                bounds.minimum        = glm::min(bounds.minimum, point);
                bounds.maximum        = glm::max(bounds.maximum, point);
            }
        }
        m_clusterContents.push_back(bounds);
    }
    m_clusters.MarkDirty(0, static_cast<uint32_t>(m_clusterContents.size() * sizeof(ForwardClusterBounds)));
}

bool ForwardGPUDraws::Prepare(RenderDevice& device, const RenderScene& scene, ForwardDrawCache& draws)
{
    draws.Prepare(scene);
    if (m_preparedKey != draws.preparedKey)
    {
        m_preparedKey = draws.preparedKey;
        m_valid       = false;
        m_contents.clear();
        const RHIGPUInfo& gpu = device.GetGPUInfo();
        m_supported           = draws.items.size() <= kForwardSortMaxKeys
                             && SupportsForwardGPUDraws(gpu, static_cast<uint32_t>(draws.items.size()))
                             && scene.GetVertexBuffer()->GetRequiredSize() <= gpu.maxStorageBufferRange
                             && scene.GetIndexBuffer()->GetRequiredSize() <= gpu.maxStorageBufferRange;
        if (m_supported)
        {
            m_splitCore = scene.GetMaterialsData()[draws.items.front().draw.materialIndex].textureSampling.z > 0.0f;
            for (const ForwardDrawItem& item : draws.items)
            {
                const sg::MaterialData& material = scene.GetMaterialsData()[item.draw.materialIndex];
                m_supported =
                    item.sorted && IsBasicForwardMaterial(material) && (material.textureSampling.z > 0.0f) == m_splitCore;
                if (!m_supported)
                {
                    break;
                }
            }
        }
        if (m_supported)
        {
            m_contents.reserve(draws.items.size());
            for (const ForwardDrawItem& item : draws.items)
            {
                m_contents.push_back({Vec4(item.center, 1.0f),
                                      glm::uvec4(item.draw.nodeIndex, item.draw.materialIndex, item.draw.firstIndex, 0)});
            }
            PrepareClusters(scene);
            m_triangles.MarkDirty(0, static_cast<uint32_t>(m_contents.size() * sizeof(ForwardTriangleData)));
        }
    }
    return m_supported
        && m_triangles.Update(device, m_contents.data(), static_cast<uint32_t>(m_contents.size() * sizeof(ForwardTriangleData)),
                              "forward_triangles")
        && m_clusters.Update(device, m_clusterContents.data(),
                             static_cast<uint32_t>(m_clusterContents.size() * sizeof(ForwardClusterBounds)),
                             "forward_clusters");
}

ForwardGPUResult ForwardGPUDraws::Build(RenderDevice&      device,
                                        RenderGraph&       graph,
                                        const RenderScene& scene,
                                        ForwardDrawCache&  draws,
                                        const Mat4&        view)
{
    ForwardGPUResult result;
    if (Prepare(device, scene, draws))
    {
        const uint32_t       count  = static_cast<uint32_t>(m_contents.size());
        const uint32_t       groups = (count + kForwardSortGroupSize - 1u) / kForwardSortGroupSize;
        const ForwardDrawKey next{draws.preparedKey, view};
        const bool           rebuild = !m_valid || NeedsForwardDepthOrder(m_sortedKey, next);
        bool                 ready   = !rebuild;
        RDGBuffer            keys;
        if (rebuild)
        {
            m_valid = false;
            HeapVector<Vec4> rows(scene.GetRenderableNodes().size());
            for (const sg::Node* node : scene.GetRenderableNodes())
            {
                const uint32_t index     = node->GetRenderableIndex();
                const Mat4     modelView = view * scene.GetInstanceTransform(index);
                rows[index]              = Vec4(modelView[0][2], modelView[1][2], modelView[2][2], modelView[3][2]);
            }
            const uint32_t rowBytes = static_cast<uint32_t>(rows.size() * sizeof(Vec4));
            m_depthRows.MarkDirty(0, rowBytes);
            const bool     uploaded = m_depthRows.Update(device, rows.data(), rowBytes, "forward_depth_rows");
            const uint64_t bytes    = uint64_t(count) * sizeof(glm::uvec2);
            if (m_keys == nullptr || m_keys->GetRequiredSize() != bytes)
            {
                device.DestroyBuffer(m_keys);
                RHIBufferCreateInfo description;
                description.size         = bytes;
                description.allocateType = RHIBufferAllocateType::eGPU;
                description.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
                description.tag = "forward_depth_keys";
                m_keys          = device.CreateBuffer(description);
            }
            if (uploaded && m_keys != nullptr)
            {
                keys = graph.GetResourceManager()->ImportBuffer(m_keys, RDGImportContents::eUndefined);
                RDGComputePassDesc generate;
                generate.SetShaderProgramName("ForwardDepthKeysSP");
                generate.SetPassTag("ForwardDepthKeys");
                generate.BindStorageBuffer("ForwardTriangles", m_triangles.GetBuffer());
                generate.BindStorageBuffer("ForwardDepthRows", m_depthRows.GetBuffer());
                generate.BindStorageBuffer("ForwardDepthKeys", keys, RDGContentGuarantee::eFullWrite);
                graph.AddComputePass(std::move(generate)).RecordPassCommands([count, groups](RDGPassCmdEncoder& encoder) {
                    encoder.SetPushConstants(count);
                    encoder.Dispatch(groups, 1, 1);
                });
                keys          = AddForwardDepthSort(device, graph, keys, count);
                ready         = bool(keys);
                m_recorded    = ready;
                m_recordedKey = next;
            }
        }
        else
        {
            keys = graph.GetResourceManager()->ImportBuffer(m_keys);
        }
        if (ready)
        {
            RDGBufferDesc description;
            description.name = "forward_gpu_indices";
            description.size = uint64_t(count) * 3u * sizeof(uint32_t);
            description.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eIndexBuffer);
            result.indices   = graph.GetResourceManager()->CreateBuffer(description);
            description.name = "forward_gpu_arguments";
            description.size = 5u * sizeof(uint32_t);
            description.usageFlags.Clear();
            description.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eIndirectBuffer);
            result.arguments = graph.GetResourceManager()->CreateBuffer(description);
            result.splitCore = m_splitCore;
            result.keys      = keys;
        }
    }
    m_recorded = m_recorded && bool(result);
    return result;
}

void ForwardGPUDraws::OnExecuted(bool submitted)
{
    if (m_recorded)
    {
        m_valid = submitted;
        if (submitted)
        {
            m_sortedKey = m_recordedKey;
        }
    }
    m_recorded = false;
}

void ForwardGPUDraws::Destroy(RenderDevice& device)
{
    m_triangles.Destroy(device);
    m_clusters.Destroy(device);
    m_clusterContents.clear();
    m_depthRows.Destroy(device);
    device.DestroyBuffer(m_keys);
    m_keys = nullptr;
    m_contents.clear();
    m_preparedKey = {};
    m_sortedKey   = {};
    m_recordedKey = {};
    m_supported   = false;
    m_valid       = false;
    m_recorded    = false;
}
} // namespace zen::rc
