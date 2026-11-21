#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/RenderCoreDefs.h"
#include "Math/Math.h"

namespace zen::rc
{
class RenderDevice;
class RenderScene;
class VoxelGIRenderer;
class WorldRadianceCache;
struct HybridGIUniformData;

class GlossyReflections
{
public:
    explicit GlossyReflections(RenderDevice* device);
    ~GlossyReflections();
    bool     Prepare(glm::uvec2 extent);
    void     RecordClear(RenderGraph& graph) const;
    void     Record(VoxelGIRenderer&           voxelGI,
                    RenderScene&               scene,
                    const WorldRadianceCache&  cache,
                    const HybridGIUniformData& receiver,
                    RHISampler*                sampler,
                    bool                       reset);
    void     RecordTrace(VoxelGIRenderer&           voxelGI,
                         RenderScene&               scene,
                         const WorldRadianceCache&  cache,
                         const HybridGIUniformData& receiver,
                         RHISampler*                sampler,
                         RDGBuffer                  raw,
                         RDGTexture                 specular) const;
    void     BindGather(RDGPassDescBase& pass) const;
    void     OnRenderGraphExecuted(RenderFrameResult result);
    void     Destroy();
    uint64_t GetHistoryBytes() const;

private:
    void          Bind(RDGPassDescBase&           pass,
                       const HybridGIUniformData& receiver,
                       RHISampler*                sampler,
                       const RenderScene&         scene,
                       uint32_t                   filterPass = 0) const;
    void          Dispatch(RDGComputePassDesc&& pass, glm::uvec2 extent) const;
    void          BindTrace(RDGPassDescBase&           pass,
                            VoxelGIRenderer&           voxelGI,
                            RenderScene&               scene,
                            const WorldRadianceCache&  cache,
                            const HybridGIUniformData& receiver,
                            RHISampler*                sampler,
                            RDGBuffer                  raw,
                            RDGTexture                 specular) const;
    RenderDevice* m_device;
    RHIBuffer*    m_history[2]{};
    RDGBuffer     m_filtered;
    glm::uvec2    m_extent{0};
    uint32_t      m_current{0};
    bool          m_valid{false};
    bool          m_recorded{false};
    struct Uniforms
    {
        glm::uvec4 extent{0};
        Vec4       control{0};
    } m_uniforms;
};
} // namespace zen::rc
