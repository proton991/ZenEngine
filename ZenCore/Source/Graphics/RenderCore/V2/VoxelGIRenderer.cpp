#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/RenderResource.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/Renderer/ShadowMapRenderer.h"

namespace zen::rc
{
VoxelGIRenderer::VoxelGIRenderer(RenderDevice* pRenderDevice,
                                 RHIViewport* pViewport,
                                 VoxelizerBase* pVoxelizer,
                                 ShadowMapRenderer* pShadowMapRenderer) :
    m_pRenderDevice(pRenderDevice),
    m_pViewport(pViewport),
    m_pVoxelizer(pVoxelizer),
    m_pShadowMapRenderer(pShadowMapRenderer)
{}

void VoxelGIRenderer::Init()
{
    if (m_initialized || m_pVoxelizer == nullptr || !m_pVoxelizer->ProducesRadianceInputs() ||
        m_pShadowMapRenderer == nullptr)
    {
        return;
    }

    m_config.injectFirstBounce     = true;
    m_config.traceShadowCones      = true;
    m_config.normalWeightedLambert = true;
    m_config.traceShadowHit        = 0.5f;

    PrepareTextures();

    m_initialized = true;
}

void VoxelGIRenderer::SetRenderScene(RenderScene* pScene)
{
    m_pScene = pScene;
}

void VoxelGIRenderer::Destroy()
{
    m_pRenderDevice->DestroyTexture(m_textures.pVoxelRadiance);
    m_textures    = {};
    m_initialized = false;
}

void VoxelGIRenderer::PrepareTextures()
{
    DataFormat voxelTexFormat   = DataFormat::eR8G8B8A8UNORM;
    uint32_t voxelTexResolution = m_pVoxelizer->GetVoxelTexResolution();

    {
        TextureFormat texFormat{};
        texFormat.format      = voxelTexFormat;
        texFormat.dimension   = TextureDimension::e3D;
        texFormat.width       = voxelTexResolution;
        texFormat.height      = voxelTexResolution;
        texFormat.depth       = voxelTexResolution;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        m_textures.pVoxelRadiance = m_pRenderDevice->CreateTextureStorage(
            texFormat, {.copyUsage = false}, "voxel_radiance");
    }
}

void VoxelGIRenderer::BuildRenderGraph()
{
    if (!m_initialized)
    {
        return;
    }

    RenderGraph* pRDG = m_pRenderDevice->GetCurrentFrameRDG();
    VERIFY_EXPR(pRDG != nullptr);

    const uint32_t groups = (m_pVoxelizer->GetVoxelTexResolution() + 7) / 8;

    RDGComputePassDesc reset{};
    reset.SetShaderProgramName("ResetVoxelTextureSP");
    reset.SetPassTag("ResetVoxelTextureComp");

    reset.BindStorageImage("voxelTexture", m_textures.pVoxelRadiance->GetDefaultView());

    pRDG->AddComputePass(std::move(reset)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(groups, groups, groups);
    });

    const VoxelTextures& textures = m_pVoxelizer->GetVoxelTextures();

    RDGComputePassDesc inject{};
    inject.SetShaderProgramName("VoxelInjectRadianceSP");
    inject.SetPassTag("VoxelInjectRadianceComp");

    inject.BindSampledTexture("voxelAlbedo", m_pVoxelizer->GetVoxelSampler(), textures.pAlbedoView);
    inject.BindStorageImage("voxelNormal", textures.pNormalView);
    inject.BindStorageImage("voxelRadiance", m_textures.pVoxelRadiance->GetDefaultView());
    inject.BindStorageImage("voxelEmissive", textures.pEmissiveView);
    inject.BindSampledTexture("shadowMap", m_pShadowMapRenderer->GetColorSampler(),
                              m_pShadowMapRenderer->GetShadowMapTexture()->GetDefaultView());

    VoxelInjectRadianceSP::LightInfo lightInfo{};
    Light light;
    light.direction               = Vec3(-1.0f, -1.0f, -1.0f);
    light.diffuse                 = Vec3(1.0f);
    light.shadowingMethod         = 2;
    lightInfo.directionalLight[0] = light;
    lightInfo.lightViewProjection = Mat4(1.0f);

    inject.BindValue("uLightInfo", lightInfo);

    VoxelInjectRadianceSP::PushConstantsData constants{};
    constants.normalWeightedLambert = m_config.normalWeightedLambert;
    constants.traceShadowHit        = m_config.traceShadowHit;
    constants.volumeDimension       = m_pVoxelizer->GetVoxelTexResolution();
    constants.voxelSize             = m_pVoxelizer->GetVoxelSize();
    constants.voxelScale            = m_pVoxelizer->GetVoxelScale();
    constants.worldMinPoint         = m_pVoxelizer->GetSceneMinPoint();

    pRDG->AddComputePass(std::move(inject))
        .RecordPassCommands([constants, groups](RDGPassCmdEncoder& encoder) {
            encoder.SetPushConstants(constants);
            encoder.Dispatch(groups, groups, groups);
        });
}

} // namespace zen::rc
