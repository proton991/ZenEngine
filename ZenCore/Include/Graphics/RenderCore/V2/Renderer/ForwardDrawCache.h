#pragma once

#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"

namespace zen::rc
{
// A translucent draw before view ordering. A blend triangle keeps its corner indices so
// ordering need not revisit the scene's index buffer.
struct ForwardDrawItem
{
    SceneMeshDraw draw;
    Vec3          center{0.0f}; // Node space.
    uint32_t      triangle[3]{0, 0, 0};
    bool          sorted{false};
};

struct ForwardDepthKey
{
    uint32_t depth;
    uint32_t item;
};

struct ForwardSortedDraw
{
    SceneMeshDraw draw;
    bool          sorted{false};
    uint32_t      sortedFirst{0};
};

struct ForwardDrawBatch
{
    uint32_t         firstDraw{0};
    uint32_t         drawCount{0};
    sg::MeshTopology topology{sg::MeshTopology::Triangles};
    bool             writeDepth{false};
    bool             sorted{false};
    bool             basicPBR{false};
    bool             splitCore{false};
};

struct ForwardSceneKey
{
    uint64_t scene{0};
    uint64_t geometryRevision{0};
    uint64_t surfaceRevision{0};
    uint64_t renderables{0};

    bool operator==(const ForwardSceneKey&) const = default;
};

struct ForwardDrawKey : ForwardSceneKey
{
    Mat4 view{1.0f};

    bool operator==(const ForwardDrawKey&) const = default;
};

bool NeedsForwardDepthOrder(const ForwardDrawKey& previous, const ForwardDrawKey& next);

// Classification, node merging and triangle centers depend on committed geometry and
// materials; only depth order depends on the camera view, and neither on lighting or
// projection. A viewing-direction change reorders the prepared items without rebuilding
// them; translation/roll and rotations below 0.05 degrees reuse the last order.
// Preserve original per-triangle entries for the failed-index-upload fallback.
struct ForwardDrawCache
{
    ForwardSceneKey               preparedKey;
    ForwardDrawKey                key;
    HeapVector<SceneMeshDraw>     opaque[3];
    HeapVector<ForwardDrawItem>   items;
    HeapVector<ForwardDepthKey>   depthKeys;
    HeapVector<ForwardDepthKey>   depthScratch;
    HeapVector<ForwardSortedDraw> translucent;
    HeapVector<uint32_t>          indices;
    HeapVector<SceneMeshDraw>     draws;
    HeapVector<ForwardDrawBatch>  batches;
    bool                          hasBlend{false};
    bool                          indicesChanged{false};

    // True means the view order was rebuilt; false reuses the previous draw snapshot.
    bool Update(const RenderScene& scene, const Mat4& view);
    bool Prepare(const RenderScene& scene);
    void Clear();
};

bool IsForwardTranslucent(const sg::MaterialData& material);
bool IsBasicForwardMaterial(const sg::MaterialData& material);
bool SupportsDeferredOpaque(const HeapVector<sg::MaterialData>& materials);
} // namespace zen::rc
