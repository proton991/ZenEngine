#pragma once
#include "Utils/UniquePtr.h"
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

    DeferredLightingRenderer(RenderDevice* renderDevice, RHIViewport* viewport);

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

    void AddMeshDrawNodes(RDGPassNode* pass, const Rect2<int>& area, const Rect2<float>& viewport);

    RenderDevice* m_renderDevice{nullptr};

    RHIViewport* m_viewport{nullptr};

    UniquePtr<RenderGraph> m_rdg;
    bool m_rebuildRDG{true};

    struct GraphicsPasses
    {
        GraphicsPass* offscreen;
        GraphicsPass* sceneLighting;
    } m_gfxPasses;

    RenderScene* m_scene{nullptr};

    // struct
    // {
    //     TextureHandle position;
    //     TextureHandle normal;
    //     TextureHandle albedo;
    //     TextureHandle metallicRoughness;
    //     TextureHandle emissiveOcclusion;
    //     TextureHandle depth;
    // } m_offscreenTextures;

    struct
    {
        RHITexture* position{nullptr};
        RHITexture* normal{nullptr};
        RHITexture* albedo{nullptr};
        RHITexture* metallicRoughness{nullptr};
        RHITexture* emissiveOcclusion{nullptr};
        RHITexture* depth{nullptr};
    } m_offscreenTextures;

    RHISampler* m_colorSampler;
    RHISampler* m_depthSampler;
};
} // namespace zen::rc