#include "Graphics/RenderCore/V2/Renderer/ForwardDrawCache.h"
#include <algorithm>
#include <bit>
#include <cstring>

namespace zen::rc
{
namespace
{
constexpr uint32_t kDepthRadixBits    = 11;
constexpr uint32_t kDepthRadixBuckets = 1u << kDepthRadixBits;
constexpr uint32_t kDepthRadixPasses  = 3;

// Integer keys order like the floats. Adding zero folds -0 into +0, which compare equal.
uint32_t DepthSortKey(float depth)
{
    const uint32_t bits = std::bit_cast<uint32_t>(depth + 0.0f);
    return (bits & 0x80000000u) != 0 ? ~bits : bits | 0x80000000u;
}

uint32_t DepthRadixBucket(const ForwardDepthKey& key, uint32_t pass)
{
    return (key.depth >> (pass * kDepthRadixBits)) & (kDepthRadixBuckets - 1);
}

// Stable, so equal depths keep scene order.
void SortForwardDepths(HeapVector<ForwardDepthKey>& keys, HeapVector<ForwardDepthKey>& scratch)
{
    uint32_t offsets[kDepthRadixPasses][kDepthRadixBuckets]{};
    for (const ForwardDepthKey& key : keys)
    {
        for (uint32_t pass = 0; pass < kDepthRadixPasses; ++pass)
        {
            ++offsets[pass][DepthRadixBucket(key, pass)];
        }
    }
    scratch.resize(keys.size());
    for (uint32_t pass = 0; pass < kDepthRadixPasses; ++pass)
    {
        uint32_t first = 0;
        for (uint32_t& offset : offsets[pass])
        {
            const uint32_t count  = offset;
            offset                = first;
            first                += count;
        }
        for (const ForwardDepthKey& key : keys)
        {
            scratch[offsets[pass][DepthRadixBucket(key, pass)]++] = key;
        }
        std::swap(keys, scratch);
    }
}

bool SameForwardNodeData(const sg::NodeData& left, const sg::NodeData& right)
{
    return std::memcmp(&left, &right, sizeof(sg::NodeData)) == 0;
}

uint32_t BlendNode(const sg::Node& node, HeapVector<const sg::Node*>& representatives)
{
    uint32_t index = node.GetRenderableIndex();
    bool     found = false;
    for (const sg::Node* representative : representatives)
    {
        if (SameForwardNodeData(node.GetData(), representative->GetData()))
        {
            index = representative->GetRenderableIndex();
            found = true;
            break;
        }
    }
    if (!found)
    {
        representatives.push_back(&node);
    }
    return index;
}

void PrepareForwardDraws(ForwardDrawCache& cache, const RenderScene& scene)
{
    for (HeapVector<SceneMeshDraw>& topology : cache.opaque)
    {
        topology.clear();
    }
    cache.items.clear();
    cache.hasBlend = false;
    HeapVector<const sg::Node*>      representatives;
    const HeapVector<uint32_t>&      sourceIndices = scene.GetIndices();
    const HeapVector<asset::Vertex>& vertices      = scene.GetVertices();
    for (const sg::Node* node : scene.GetRenderableNodes())
    {
        const uint32_t instance = node->GetRenderableIndex();
        if (scene.GetInstanceMask(instance) != 0)
        {
            uint32_t blendNode         = instance;
            bool     preparedBlendNode = false;
            for (const sg::SubMesh* mesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                SceneMeshDraw draw{instance,      mesh->GetMaterial()->index, mesh->GetIndexCount(), mesh->GetFirstIndex(), 0,
                                   mesh->topology};
                const sg::MaterialData& material = scene.GetMaterialsData()[draw.materialIndex];
                if (!IsForwardTranslucent(material))
                {
                    cache.opaque[static_cast<uint32_t>(draw.topology)].push_back(draw);
                }
                else if (material.surfaceProperties.y == static_cast<float>(sg::AlphaMode::Blend)
                         && draw.topology == sg::MeshTopology::Triangles)
                {
                    // Ordinary BLEND triangles ignore node identity in forward receiver matching.
                    // Equal complete node records preserve position, normals, orientation and thickness.
                    // Split materials retain their identity to reuse only their own core's shadows.
                    if (material.textureSampling.z <= 0.0f && !preparedBlendNode)
                    {
                        blendNode         = BlendNode(*node, representatives);
                        preparedBlendNode = true;
                    }
                    draw.nodeIndex = material.textureSampling.z > 0.0f ? instance : blendNode;
                    for (uint32_t index = 0; index + 2 < draw.indexCount; index += 3)
                    {
                        ForwardDrawItem item{draw};
                        item.draw.firstIndex += index;
                        item.draw.indexCount  = 3;
                        item.sorted           = true;
                        Vec3 center(0.0f);
                        for (uint32_t corner = 0; corner < 3; ++corner)
                        {
                            item.triangle[corner]  = sourceIndices[draw.firstIndex + index + corner];
                            center                += Vec3(vertices[item.triangle[corner]].pos);
                        }
                        item.center = center / 3.0f;
                        cache.items.push_back(item);
                        cache.hasBlend = true;
                    }
                }
                else
                {
                    cache.items.push_back({draw, mesh->GetAABB().GetCenter()});
                }
            }
        }
    }
}

void BuildForwardBatches(ForwardDrawCache& cache, const RenderScene& scene)
{
    cache.draws.clear();
    cache.batches.clear();
    HeapVector<uint8_t> basicMaterials;
    basicMaterials.reserve(scene.GetMaterialsData().size());
    for (const sg::MaterialData& material : scene.GetMaterialsData())
    {
        basicMaterials.push_back(IsBasicForwardMaterial(material) ? 1 : 0);
    }
    for (const ForwardSortedDraw& entry : cache.translucent)
    {
        SceneMeshDraw draw = entry.draw;
        const bool    writeDepth =
            scene.GetMaterialsData()[draw.materialIndex].surfaceProperties.y != static_cast<float>(sg::AlphaMode::Blend);
        const bool basicPBR  = basicMaterials[draw.materialIndex] != 0;
        const bool splitCore = draw.topology == sg::MeshTopology::Triangles
                            && scene.GetMaterialsData()[draw.materialIndex].textureSampling.z > 0.0f;
        if (cache.batches.empty() || cache.batches.back().topology != draw.topology
            || cache.batches.back().writeDepth != writeDepth || cache.batches.back().sorted != entry.sorted
            || cache.batches.back().basicPBR != basicPBR || cache.batches.back().splitCore != splitCore)
        {
            cache.batches.push_back(
                {static_cast<uint32_t>(cache.draws.size()), 0, draw.topology, writeDepth, entry.sorted, basicPBR, splitCore});
        }
        ForwardDrawBatch& batch = cache.batches.back();
        if (entry.sorted)
        {
            draw.firstIndex = entry.sortedFirst;
        }
        if (entry.sorted && batch.drawCount != 0 && cache.draws.back().nodeIndex == draw.nodeIndex
            && cache.draws.back().materialIndex == draw.materialIndex
            && cache.draws.back().firstIndex + cache.draws.back().indexCount == draw.firstIndex)
        {
            cache.draws.back().indexCount += draw.indexCount;
        }
        else
        {
            cache.draws.push_back(draw);
            ++batch.drawCount;
        }
    }
}

void OrderForwardDraws(ForwardDrawCache& cache, const RenderScene& scene, const Mat4& view)
{
    cache.depthKeys.resize(cache.items.size());
    uint32_t node = UINT32_MAX;
    Mat4     modelView(1.0f);
    for (uint32_t index = 0; index < cache.items.size(); ++index)
    {
        const ForwardDrawItem& item = cache.items[index];
        if (item.draw.nodeIndex != node)
        {
            node      = item.draw.nodeIndex;
            modelView = view * scene.GetInstanceTransform(node);
        }
        cache.depthKeys[index] = {DepthSortKey((modelView * Vec4(item.center, 1.0f)).z), index};
    }
    SortForwardDepths(cache.depthKeys, cache.depthScratch);
    cache.translucent.clear();
    cache.translucent.reserve(cache.items.size());
    HeapVector<uint32_t> sortedIndices;
    sortedIndices.reserve(cache.indices.size());
    for (const ForwardDepthKey& key : cache.depthKeys)
    {
        const ForwardDrawItem& item = cache.items[key.item];
        ForwardSortedDraw      entry{item.draw, item.sorted};
        if (item.sorted)
        {
            entry.sortedFirst = static_cast<uint32_t>(sortedIndices.size());
            for (uint32_t corner : item.triangle)
            {
                sortedIndices.push_back(corner);
            }
        }
        cache.translucent.push_back(entry);
    }
    cache.indicesChanged = sortedIndices.size() != cache.indices.size()
                        || !std::equal(sortedIndices.begin(), sortedIndices.end(), cache.indices.begin());
    if (cache.indicesChanged)
    {
        cache.indices = std::move(sortedIndices);
    }
    BuildForwardBatches(cache, scene);
}
} // namespace

bool IsForwardTranslucent(const sg::MaterialData& material)
{
    return material.surfaceProperties.y == static_cast<float>(sg::AlphaMode::Blend) || material.sheenColorTransmission.w > 0.0f;
}

bool IsBasicForwardMaterial(const sg::MaterialData& material)
{
    // Validate the coefficients too: runtime MaterialData edits can leave the
    // authored feature flag unchanged. Never specialize away an active lobe.
    return material.materialProperties.w <= 0.5f && material.surfaceProperties.w <= 0.5f
        && material.clearcoatSheenSpecular.x == 0.0f && material.sheenColorTransmission == Vec4(0.0f)
        && material.volumeIridescence.z == 0.0f && material.iridescenceAnisotropy.z == 0.0f
        && material.diffuseTransmissionColorFactor.w == 0.0f && material.volumeScatterColorRetroreflection.w == 0.0f;
}

bool SupportsDeferredOpaque(const HeapVector<sg::MaterialData>& materials)
{
    bool supported = true;
    for (const sg::MaterialData& material : materials)
    {
        supported = supported && (material.materialProperties.w <= 0.5f || IsForwardTranslucent(material));
    }
    return supported;
}

bool NeedsForwardDepthOrder(const ForwardDrawKey& previous, const ForwardDrawKey& next)
{
    // Translation adds the same depth to every item, and camera roll does not
    // change depth. Only compare the viewing direction. Anchor the tolerance to
    // the last sort so a sequence of small rotations cannot keep stale order.
    constexpr float directionTolerance = 0.0008726646f; // 0.05 degrees for a rigid view.
    const Vec3      directionDelta(next.view[0][2] - previous.view[0][2], next.view[1][2] - previous.view[1][2],
                                   next.view[2][2] - previous.view[2][2]);
    return static_cast<const ForwardSceneKey&>(previous) != next
        || !(glm::dot(directionDelta, directionDelta) <= directionTolerance * directionTolerance);
}

bool ForwardDrawCache::Update(const RenderScene& scene, const Mat4& view)
{
    const ForwardDrawKey next{
        {scene.GetIdentity(), scene.GetGeometryRevision(), scene.GetSurfaceRevision(), scene.GetRenderableNodes().size()},
        view};
    const bool rebuild = NeedsForwardDepthOrder(key, next);
    indicesChanged     = false;
    if (rebuild)
    {
        Prepare(scene);
        OrderForwardDraws(*this, scene, view);
        key = next;
    }
    return rebuild;
}

bool ForwardDrawCache::Prepare(const RenderScene& scene)
{
    const ForwardSceneKey next{scene.GetIdentity(), scene.GetGeometryRevision(), scene.GetSurfaceRevision(),
                               scene.GetRenderableNodes().size()};
    const bool            changed = preparedKey != next;
    if (changed)
    {
        PrepareForwardDraws(*this, scene);
        preparedKey = next;
    }
    return changed;
}

void ForwardDrawCache::Clear()
{
    key         = {};
    preparedKey = {};
    for (HeapVector<SceneMeshDraw>& topology : opaque)
    {
        topology.clear();
    }
    items.clear();
    depthKeys.clear();
    depthScratch.clear();
    translucent.clear();
    indices.clear();
    draws.clear();
    batches.clear();
    hasBlend       = false;
    indicesChanged = false;
}
} // namespace zen::rc
