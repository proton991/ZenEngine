#pragma once
#include "RenderGraph.h"
#include "RenderDevice.h"
#include "Graphics/RHI/RHIDefs.h"
#include "Common/UniquePtr.h"

#define M_PI 3.14159265358979323846 // pi

namespace zen::rc
{
class SkyboxRenderer
{
public:
    SkyboxRenderer(RenderDevice* renderDevice, rhi::RHIViewport* viewport);

    void Init();

    void Destroy();

    void GenerateEnvCubemaps(EnvTexture* texture);

    void GenerateLutBRDF(EnvTexture* texture);

    void PrepareRenderWorkload(const rhi::TextureHandle& skyboxTexture,
                               const rhi::BufferHandle& cameraUBO);

    void OnResize();

    RenderGraph* GetRenderGraph()
    {
        return m_rdg.Get();
    };

private:
    enum CubmapTarget
    {
        IRRADIANCE      = 0,
        PREFILTERED_MAP = 1,

    };

    void PrepareTextures();

    void BuildRenderGraph();

    void BuildGraphicsPasses();

    struct SkyboxVertex
    {
        Vec3 position{0.0f, 0.0f, 0.0f};
    };

    rhi::DynamicRHI* m_RHI{nullptr};

    RenderDevice* m_renderDevice{nullptr};

    rhi::RHIViewport* m_viewport{nullptr};

    bool m_updatePassResource{true};
    bool m_rebuildRDG{true};

    struct
    {
        GraphicsPass irradiance;
        GraphicsPass prefiltered;
        GraphicsPass lutBRDF;
        GraphicsPass skybox;
    } m_gfxPasses;

    struct
    {
        rhi::TextureHandle irradiance;
        rhi::TextureHandle prefiltered;
    } m_offscreenTextures;

    UniquePtr<RenderGraph> m_rdg;

    rhi::BufferHandle m_vertexBuffer;
    rhi::BufferHandle m_indexBuffer;

    struct
    {
        rhi::SamplerHandle cubemapSampler;
        rhi::SamplerHandle lutBRDFSampler;
    } m_samplers;

    struct PushConstantIrradiance
    {
        glm::mat4 mvp;
        float deltaPhi   = 2.0f * static_cast<float>(M_PI) / 180.0f;
        float deltaTheta = 0.5f * static_cast<float>(M_PI) / 64.0f;
    } m_pcIrradiance;

    struct PushConstantPrefilterEnv
    {
        glm::mat4 mvp;
        float roughness;
        uint32_t numSamples = 32u;
    } m_pcPrefilterEnv;

    const rhi::DataFormat cIrradianceFormat  = rhi::DataFormat::eR32G32B32A32SFloat;
    const rhi::DataFormat cPrefilteredFormat = rhi::DataFormat::eR16G16B16A16SFloat;

    const std::vector<SkyboxVertex> cSkyboxVertices = {
        {Vec3(-1.0f, 1.0f, -1.0f)}, // Front face
        {Vec3(1.0f, 1.0f, -1.0f)},  {Vec3(1.0f, -1.0f, -1.0f)}, {Vec3(-1.0f, -1.0f, -1.0f)},

        {Vec3(-1.0f, 1.0f, 1.0f)}, // Back face
        {Vec3(1.0f, 1.0f, 1.0f)},   {Vec3(1.0f, -1.0f, 1.0f)},  {Vec3(-1.0f, -1.0f, 1.0f)}};

    const std::vector<uint32_t> cSkyboxIndices = {
        0, 1, 2, 2, 3, 0, // Front face
        5, 4, 7, 7, 6, 5, // Back face
        4, 0, 3, 3, 7, 4, // Left face
        1, 5, 6, 6, 2, 1, // Right face
        4, 5, 1, 1, 0, 4, // Top face
        3, 2, 6, 6, 7, 3  // Bottom face
    };
};
} // namespace zen::rc