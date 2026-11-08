#pragma once
#include "Graphics/RenderCore/V2/Renderer/ForwardDrawCache.h"
#include "Graphics/RenderCore/V2/FrameBufferRing.h"
#include "Graphics/RenderCore/V2/RenderGraph/RDGDefs.h"

namespace zen::rc
{
class RenderDevice;
class RenderGraph;

struct ForwardTriangleData
{
    Vec4       center{0.0f};
    glm::uvec4 identity{0}; // Node, material, source first index, culling cluster.
};
static_assert(sizeof(ForwardTriangleData) == 32);

bool SupportsForwardGPUDraws(const RHIGPUInfo& gpu, uint32_t count);

struct ForwardGPUResult
{
    RDGBuffer keys;
    RDGBuffer indices;
    RDGBuffer arguments;
    bool      splitCore{false};
    explicit  operator bool() const
    {
        return indices && arguments;
    }
};

struct ForwardClusterBounds
{
    Vec4 minimum;
    Vec4 maximum;
};
static_assert(sizeof(ForwardClusterBounds) == 32);

RDGTexture AddForwardOpaqueHiZ(RenderDevice& device, RenderGraph& graph, RHITexture* depth, uint32_t width, uint32_t height);
RDGBuffer  AddForwardClusterVisibility(RenderDevice& device,
                                       RenderGraph&  graph,
                                       RDGBuffer     clusters,
                                       uint32_t      count,
                                       RDGTexture    pyramid,
                                       const Mat4&   projectionView);

// Exact triangle order. Persistent keys are published only after submission;
// the graph's resource history orders their reuse after preceding GPU readers.
class ForwardGPUDraws
{
public:
    ForwardGPUResult Build(RenderDevice&      device,
                           RenderGraph&       graph,
                           const RenderScene& scene,
                           ForwardDrawCache&  draws,
                           const Mat4&        view);
    void             Emit(RenderDevice&           device,
                          RenderGraph&            graph,
                          const ForwardGPUResult& result,
                          RHITexture*             depth,
                          uint32_t                width,
                          uint32_t                height,
                          const Mat4&             projectionView);
    void             OnExecuted(bool submitted);
    void             Destroy(RenderDevice& device);
    RHIBuffer*       GetTriangles() const
    {
        return m_triangles.GetBuffer();
    }

private:
    bool                             Prepare(RenderDevice& device, const RenderScene& scene, ForwardDrawCache& draws);
    void                             PrepareClusters(const RenderScene& scene);
    ForwardSceneKey                  m_preparedKey;
    ForwardDrawKey                   m_sortedKey;
    ForwardDrawKey                   m_recordedKey;
    HeapVector<ForwardTriangleData>  m_contents;
    HeapVector<ForwardClusterBounds> m_clusterContents;
    FrameBufferRing                  m_clusters;
    FrameBufferRing                  m_triangles;
    FrameBufferRing                  m_depthRows;
    RHIBuffer*                       m_keys{nullptr};
    bool                             m_supported{false};
    bool                             m_splitCore{false};
    bool                             m_valid{false};
    bool                             m_recorded{false};
};
} // namespace zen::rc
