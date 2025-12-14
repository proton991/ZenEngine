#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/RenderResource.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/ShadowMapRenderer.h"




namespace zen::rc
{
VoxelGIRenderer::VoxelGIRenderer(RenderDevice* renderDevice, RHIViewport* viewport) :
    m_renderDevice(renderDevice), m_viewport(viewport)
{
    // m_RHI       = m_renderDevice->GetRHI();
    m_voxelizer = m_renderDevice->GetRendererServer()->RequestVoxelizer();
}

void VoxelGIRenderer::Init()
{
    m_config.injectFirstBounce     = true;
    m_config.traceShadowCones      = true;
    m_config.normalWeightedLambert = true;
    m_config.traceShadowHit        = 0.5f;

    PrepareTextures();

    PrepareBuffers();

    BuildGraphicsPasses();

    BuildComputePasses();
}

void VoxelGIRenderer::SetRenderScene(RenderScene* scene)
{
    m_scene = scene;
    UpdateUniformData();
    UpdatePassResources();
}

void VoxelGIRenderer::Destroy()
{
    for (uint32_t i = 0; i < 6; i++)
    {
        m_renderDevice->DestroyTexture(m_textures.voxelMipmaps[i]);
    }
    m_renderDevice->DestroyTexture(m_textures.voxelRadiance);
}

void VoxelGIRenderer::PrepareRenderWorkload()
{
    if (m_rebuildRDG)
    {
        BuildRenderGraph();
        m_rebuildRDG = false;
    }
    UpdateUniformData();
}

void VoxelGIRenderer::OnResize()
{
    m_rebuildRDG = true;
    // m_renderDevice->UpdateGraphicsPassOnResize(m_gfxPasses.voxelDraw, m_viewport);
}

void VoxelGIRenderer::PrepareTextures()
{
    DataFormat voxelTexFormat   = DataFormat::eR8G8B8A8UNORM;
    uint32_t voxelTexResolution = m_voxelizer->GetVoxelTexResolution();
    {
        // INIT_TEXTURE_INFO(texInfo, RHITextureType::e3D, voxelTexFormat, voxelTexResolution,
        //                   voxelTexResolution, voxelTexResolution, 1, 1, SampleCount::e1,
        //                   "voxel_radiance", RHITextureUsageFlagBits::eStorage,
        //                   RHITextureUsageFlagBits::eSampled);
        TextureFormat texFormat{};
        texFormat.format      = voxelTexFormat;
        texFormat.dimension   = TextureDimension::e3D;
        texFormat.width       = voxelTexResolution;
        texFormat.height      = voxelTexResolution;
        texFormat.depth       = voxelTexResolution;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        m_textures.voxelRadiance =
            m_renderDevice->CreateTextureStorage(texFormat, {.copyUsage = false}, "voxel_radiance");
    }
    const auto halfDim = voxelTexResolution / 2;
    for (uint32_t i = 0; i < 6; i++)
    {
        const auto texName = "voxel_mipmap_face_" + std::to_string(i);
        // INIT_TEXTURE_INFO(texInfo, RHITextureType::e3D, voxelTexFormat, halfDim, halfDim, halfDim,
        //                   CalculateTextureMipLevels(halfDim, halfDim, halfDim), 1, SampleCount::e1,
        //                   texName, RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eSampled);
        TextureFormat texFormat{};
        texFormat.format    = voxelTexFormat;
        texFormat.dimension = TextureDimension::e3D;
        texFormat.width     = halfDim;
        texFormat.height    = halfDim;
        texFormat.depth     = halfDim;
        texFormat.mipmaps   = RHITexture::CalculateTextureMipLevels(halfDim, halfDim, halfDim);

        m_textures.voxelMipmaps[i] =
            m_renderDevice->CreateTextureStorage(texFormat, {.copyUsage = false}, texName);
        //        m_RHI->ChangeTextureLayout(m_renderDevice->GetCurrentUploadCmdList(),
        //                                   m_voxelTextures.mipmaps[i], RHITextureLayout::eUndefined,
        //                                   RHITextureLayout::eGeneral);
    }
    m_textures.shadowMap =
        m_renderDevice->GetRendererServer()->RequestShadowMapRenderer()->GetShadowMapTexture();
}

void VoxelGIRenderer::PrepareBuffers() {}

void VoxelGIRenderer::BuildRenderGraph()
{
    m_rdg = MakeUnique<RenderGraph>("voxel_gi_rdg");
    m_rdg->Begin();


    uint32_t workgroupCount;
    // reset voxel texture
    {
        auto* pass =
            m_rdg->AddComputePassNode(m_computePasses.resetVoxelTexture, "reset_voxel_radiance");
        // pass->tag = "reset_voxel_texture";
        // m_rdg->DeclareTextureAccessForPass(
        //     pass, m_textures.voxelRadiance, RHITextureUsage::eStorage,
        //     m_renderDevice->GetTextureSubResourceRange(m_textures.voxelRadiance),
        //     RHIAccessMode::eReadWrite);
        workgroupCount = 1;
        m_rdg->AddComputePassDispatchNode(pass, workgroupCount, workgroupCount, workgroupCount);
    }

    // inject radiance
    {
        auto& voxelTextures = m_voxelizer->GetVoxelTextures();
        // TextureHandle textures[]         = {voxelTextures.normal, voxelTextures.emissive};
        // RHITextureSubResourceRange ranges[] = {
        //     m_renderDevice->GetTextureSubResourceRange(textures[0]),
        //     m_renderDevice->GetTextureSubResourceRange(textures[1]),
        // };
        VoxelInjectRadianceSP* shaderProgram =
            dynamic_cast<VoxelInjectRadianceSP*>(m_computePasses.injectRadiance->shaderProgram);
        shaderProgram->pushConstantsData.normalWeightedLambert = m_config.normalWeightedLambert;
        shaderProgram->pushConstantsData.traceShadowHit        = m_config.traceShadowHit;

        auto* pass =
            m_rdg->AddComputePassNode(m_computePasses.injectRadiance, "inject_voxel_radiance");
        // m_rdg->DeclareTextureAccessForPass(pass, 2, textures, RHITextureUsage::eStorage, ranges,
        //                                    RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(
        //     pass, voxelTextures.albedo, RHITextureUsage::eSampled,
        //     m_renderDevice->GetTextureSubResourceRange(voxelTextures.albedo), RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(
        //     pass, m_textures.shadowMap, RHITextureUsage::eSampled,
        //     m_renderDevice->GetTextureSubResourceRange(m_textures.shadowMap), RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(
        //     pass, m_textures.voxelRadiance, RHITextureUsage::eStorage,
        //     m_renderDevice->GetTextureSubResourceRange(m_textures.voxelRadiance),
        //     RHIAccessMode::eReadWrite);

        m_rdg->AddComputePassSetPushConstants(pass, &shaderProgram->pushConstantsData,
                                              sizeof(VoxelInjectRadianceSP::PushConstantsData));
        workgroupCount = m_voxelizer->GetVoxelTexResolution() / 8;
        m_rdg->AddComputePassDispatchNode(pass, workgroupCount, workgroupCount, workgroupCount);
    }

    m_rdg->End();
}


void VoxelGIRenderer::BuildGraphicsPasses() {}

void VoxelGIRenderer::BuildComputePasses()
{
    {
        // reset voxel texture
        ComputePassBuilder builder(m_renderDevice);
        m_computePasses.resetVoxelTexture = builder.SetShaderProgramName("ResetVoxelTextureSP")
                                                .SetTag("ResetVoxelTextureComp")
                                                .Build();
    }
    {
        // inject radiance
        ComputePassBuilder builder(m_renderDevice);
        m_computePasses.injectRadiance = builder.SetShaderProgramName("VoxelInjectRadianceSP")
                                             .SetTag("VoxelInjectRadianceComp")
                                             .Build();
    }
}

void VoxelGIRenderer::UpdatePassResources()
{
    // reset voxel texture
    {
        std::vector<RHIShaderResourceBinding> set0bindings;
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eImage,
                                  m_textures.voxelRadiance);
        ComputePassResourceUpdater updater(m_renderDevice, m_computePasses.resetVoxelTexture);
        updater.SetShaderResourceBinding(0, set0bindings).Update();
    }
    // inject radiance pass
    {
        std::vector<RHIShaderResourceBinding> set0bindings;
        std::vector<RHIShaderResourceBinding> set1bindings;
        std::vector<RHIShaderResourceBinding> set2bindings;

        auto& voxelTextures = m_voxelizer->GetVoxelTextures();

        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eSamplerWithTexture,
                                  m_voxelizer->GetVoxelSampler(), voxelTextures.albedoProxy);
        ADD_SHADER_BINDING_SINGLE(set0bindings, 1, RHIShaderResourceType::eImage,
                                  voxelTextures.normalProxy);
        ADD_SHADER_BINDING_SINGLE(set0bindings, 2, RHIShaderResourceType::eImage,
                                  m_textures.voxelRadiance);
        ADD_SHADER_BINDING_SINGLE(set0bindings, 3, RHIShaderResourceType::eImage,
                                  voxelTextures.emissiveProxy);

        // set-1 bindings
        ShadowMapRenderer* shadowMapRenderer =
            m_renderDevice->GetRendererServer()->RequestShadowMapRenderer();
        ADD_SHADER_BINDING_SINGLE(set1bindings, 0, RHIShaderResourceType::eSamplerWithTexture,
                                  shadowMapRenderer->GetColorSampler(), m_textures.shadowMap);
        // set-2 bindings
        ADD_SHADER_BINDING_SINGLE(
            set2bindings, 0, RHIShaderResourceType::eUniformBuffer,
            m_computePasses.injectRadiance->shaderProgram->GetUniformBufferHandle("uLightInfo"));
        // ADD_SHADER_BINDING_SINGLE(
        //     set2bindings, 1, RHIShaderResourceType::eUniformBuffer,
        //     m_computePasses.injectRadiance.shaderProgram->GetUniformBufferHandle("uSceneInfo"));

        ComputePassResourceUpdater updater(m_renderDevice, m_computePasses.injectRadiance);
        updater.SetShaderResourceBinding(0, set0bindings)
            .SetShaderResourceBinding(1, set1bindings)
            .SetShaderResourceBinding(2, set2bindings)
            .Update();
    }
}

void VoxelGIRenderer::UpdateUniformData()
{
    {
        VoxelInjectRadianceSP* shaderProgram =
            dynamic_cast<VoxelInjectRadianceSP*>(m_computePasses.injectRadiance->shaderProgram);

        shaderProgram->pushConstantsData.volumeDimension = m_voxelizer->GetVoxelTexResolution();
        shaderProgram->pushConstantsData.voxelSize       = m_voxelizer->GetVoxelSize();
        shaderProgram->pushConstantsData.voxelScale      = m_voxelizer->GetVoxelScale();
        shaderProgram->pushConstantsData.worldMinPoint   = m_voxelizer->GetSceneMinPoint();
        // todo: light info should be added by user or imported from scene file
        auto& lightInfo = shaderProgram->lightInfo;

        // lightInfo.lightTypeCount[DIRECTIONAL_LIGHT] = 1;
        // lightInfo.lightTypeCount[POINT_LIGHT]       = 0;
        // lightInfo.lightTypeCount[DIRECTIONAL_LIGHT] = 0;

        Light dirLight;
        dirLight.direction       = Vec3(-1.0f, -1.0f, -1.0f);
        dirLight.diffuse         = Vec3(1.0f, 1.0f, 1.0f);
        dirLight.shadowingMethod = 2;

        lightInfo.directionalLight[0] = dirLight;
        lightInfo.lightViewProjection = Mat4(1.0f);
        // shaderProgram->UpdateUniformBuffer("uSceneInfo", shaderProgram->GetSceneInfoData(), 0);
        shaderProgram->UpdateUniformBuffer("uLightInfo", shaderProgram->GetLightInfoData(), 0);
    }
}
} // namespace zen::rc