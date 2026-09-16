#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"

namespace zen::rc
{
class RenderScene;
class RenderDevice;

class SkyboxRenderer
{
public:
    SkyboxRenderer(RenderDevice* pRenderDevice, RHIViewport* pViewport);

    void Init();

    void BuildRenderGraph();

    // Restore one-time work when the frame is rejected before submission.
    void OnRenderGraphExecuted(bool succeeded);

    void Destroy();

    void PreprocessEnvTexture(EnvTexture* pTexture);

    void SetRenderScene(RenderScene* pRenderScene)
    {
        m_pScene = pRenderScene;
    }

private:
    enum CubmapTarget
    {
        IRRADIANCE      = 0,
        PREFILTERED_MAP = 1,

    };

    void PrepareTextures();

    void PrepareEnvCubemaps(EnvTexture* pTexture);

    void PrepareLutBRDF(EnvTexture* pTexture);

    struct SkyboxVertex
    {
        Vec3 position{0.0f, 0.0f, 0.0f};
    };

    RenderDevice* m_pRenderDevice{nullptr};

    RHIViewport* m_pViewport{nullptr};

    RenderScene* m_pScene{nullptr};

    EnvTexture* m_pPendingPreprocessEnvTexture{nullptr};
    EnvTexture* m_pRecordedPreprocessEnvTexture{nullptr};

    struct
    {
        RHITexture* pIrradiance{nullptr};
        RHITexture* pPrefiltered{nullptr};
    } m_offscreenTextures;

    RHIBuffer* m_pVertexBuffer;
    RHIBuffer* m_pIndexBuffer;

    struct PushConstantIrradiance
    {
        glm::mat4 mvp;
        float deltaPhi   = 2.0f * glm::pi<float>() / 180.0f;
        float deltaTheta = 0.5f * glm::pi<float>() / 64.0f;
    };

    struct PushConstantPrefilterEnv
    {
        glm::mat4 mvp;
        float roughness;
        uint32_t numSamples = 32u;
    };

    const DataFormat cIrradianceFormat  = DataFormat::eR32G32B32A32SFloat;
    const DataFormat cPrefilteredFormat = DataFormat::eR16G16B16A16SFloat;

    const HeapVector<SkyboxVertex> cSkyboxVertices = {
        {Vec3(-1.0f, 1.0f, -1.0f)}, // Front face
        {Vec3(1.0f, 1.0f, -1.0f)},  {Vec3(1.0f, -1.0f, -1.0f)}, {Vec3(-1.0f, -1.0f, -1.0f)},

        {Vec3(-1.0f, 1.0f, 1.0f)}, // Back face
        {Vec3(1.0f, 1.0f, 1.0f)},   {Vec3(1.0f, -1.0f, 1.0f)},  {Vec3(-1.0f, -1.0f, 1.0f)}};

    const HeapVector<uint32_t> cSkyboxIndices = {
        0, 1, 2, 2, 3, 0, // Front face
        5, 4, 7, 7, 6, 5, // Back face
        4, 0, 3, 3, 7, 4, // Left face
        1, 5, 6, 6, 2, 1, // Right face
        4, 5, 1, 1, 0, 4, // Top face
        3, 2, 6, 6, 7, 3  // Bottom face
    };
};
} // namespace zen::rc
