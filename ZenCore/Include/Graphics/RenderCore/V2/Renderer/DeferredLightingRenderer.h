#pragma once
#include "Common/UniquePtr.h"
#include "Graphics/RenderCore/V2/RenderGraph.h"

namespace zen::sys
{
class Camera;
}

namespace zen::asset
{
struct Vertex;
using Index = uint32_t;
} // namespace zen::asset

namespace zen::rc
{
class RenderScene;
class RenderDevice;
class SkyboxRenderer;

class DeferredLightingRenderer
{
public:
    enum class RenderFlags
    {
        eBindTextures = 1 << 0
    };

    DeferredLightingRenderer(RenderDevice* renderDevice, rhi::RHIViewport* viewport);

    void Init();

    void Destroy();

    void SetRenderScene(RenderScene* renderScene)
    {
        m_scene = renderScene;
        UpdateGraphicsPassResources();
    }
    void PrepareRenderWorkload();

    void OnResize();

    RenderGraph* GetRenderGraph()
    {
        return m_rdg.Get();
    };

private:
    void PrepareTextures();

    void BuildGraphicsPasses();

    void UpdateGraphicsPassResources();

    void BuildRenderGraph();

    void AddMeshDrawNodes(RDGPassNode* pass,
                          const rhi::Rect2<int>& area,
                          const rhi::Rect2<float>& viewport);

    RenderDevice* m_renderDevice{nullptr};

    rhi::RHIViewport* m_viewport{nullptr};

    UniquePtr<RenderGraph> m_rdg;
    bool m_rebuildRDG{true};

    struct GraphicsPasses
    {
        GraphicsPass offscreen;
        GraphicsPass sceneLighting;
    } m_gfxPasses;

    RenderScene* m_scene{nullptr};

    struct
    {
        rhi::TextureHandle position;
        rhi::TextureHandle normal;
        rhi::TextureHandle albedo;
        rhi::TextureHandle metallicRoughness;
        rhi::TextureHandle emissiveOcclusion;
        rhi::TextureHandle depth;
    } m_offscreenTextures;

    rhi::SamplerHandle m_colorSampler;
    rhi::SamplerHandle m_depthSampler;
};
} // namespace zen::rc