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

    DeferredLightingRenderer(RenderDevice* pRenderDevice, RHIViewport* pViewport);

    void Init();

    void Destroy();

    void SetRenderScene(RenderScene* pRenderScene)
    {
        m_pScene = pRenderScene;
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

    void AddMeshDrawNodes(RDGPassNode* pPass, const Rect2<int>& area, const Rect2<float>& viewport);

    RenderDevice* m_pRenderDevice{nullptr};

    RHIViewport* m_pViewport{nullptr};

    UniquePtr<RenderGraph> m_rdg;
    bool m_rebuildRDG{true};

    struct GraphicsPasses
    {
        GraphicsPass* pOffscreen;
        GraphicsPass* pSceneLighting;
    } m_gfxPasses;

    RenderScene* m_pScene{nullptr};

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
        RHITexture* pPosition{nullptr};
        RHITexture* pNormal{nullptr};
        RHITexture* pAlbedo{nullptr};
        RHITexture* pMetallicRoughness{nullptr};
        RHITexture* pEmissiveOcclusion{nullptr};
        RHITexture* pDepth{nullptr};
    } m_offscreenTextures;

    RHISampler* m_pColorSampler;
    RHISampler* m_pDepthSampler;
};
} // namespace zen::rc