#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/RenderResource.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/ShadowMapRenderer.h"




namespace zen::rc
{
VoxelGIRenderer::VoxelGIRenderer(RenderDevice* pRenderDevice, RHIViewport* pViewport) :
    m_pRenderDevice(pRenderDevice), m_pViewport(pViewport)
{
    // m_RHI       = m_renderDevice->GetRHI();
    m_pVoxelizer = m_pRenderDevice->GetRendererServer()->RequestVoxelizer();
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

void VoxelGIRenderer::SetRenderScene(RenderScene* pScene)
{
    m_pScene = pScene;
    UpdateUniformData();
    UpdatePassResources();
}

void VoxelGIRenderer::Destroy()
{
    for (uint32_t i = 0; i < 6; i++)
    {
        m_pRenderDevice->DestroyTexture(m_textures.pVoxelMipmaps[i]);
    }
    m_pRenderDevice->DestroyTexture(m_textures.pVoxelRadiance);
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
    uint32_t voxelTexResolution = m_pVoxelizer->GetVoxelTexResolution();
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

        m_textures.pVoxelRadiance =
            m_pRenderDevice->CreateTextureStorage(texFormat, {.copyUsage = false}, "voxel_radiance");
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

        m_textures.pVoxelMipmaps[i] =
            m_pRenderDevice->CreateTextureStorage(texFormat, {.copyUsage = false}, texName);
        //        m_RHI->ChangeTextureLayout(m_renderDevice->GetCurrentUploadCmdList(),
        //                                   m_voxelTextures.mipmaps[i], RHITextureLayout::eUndefined,
        //                                   RHITextureLayout::eGeneral);
    }
    m_textures.pShadowMap =
        m_pRenderDevice->GetRendererServer()->RequestShadowMapRenderer()->GetShadowMapTexture();
}

void VoxelGIRenderer::PrepareBuffers() {}

void VoxelGIRenderer::BuildRenderGraph()
{
    m_rdg = MakeUnique<RenderGraph>("voxel_gi_rdg");
    m_rdg->Begin();


    uint32_t workgroupCount;
    // reset voxel texture
    {
        auto* pPass =
            m_rdg->AddComputePassNode(m_computePasses.pResetVoxelTexture, "reset_voxel_radiance");
        // pass->tag = "reset_voxel_texture";
        // m_rdg->DeclareTextureAccessForPass(
        //     pass, m_textures.pVoxelRadiance, RHITextureUsage::eStorage,
        //     m_renderDevice->GetTextureSubResourceRange(m_textures.pVoxelRadiance),
        //     RHIAccessMode::eReadWrite);
        workgroupCount = 1;
        m_rdg->AddComputePassDispatchNode(pPass, workgroupCount, workgroupCount, workgroupCount);
    }

    // inject radiance
    {
        auto& voxelTextures = m_pVoxelizer->GetVoxelTextures();
        // TextureHandle textures[]         = {voxelTextures.normal, voxelTextures.emissive};
        // RHITextureSubResourceRange ranges[] = {
        //     m_renderDevice->GetTextureSubResourceRange(textures[0]),
        //     m_renderDevice->GetTextureSubResourceRange(textures[1]),
        // };
        VoxelInjectRadianceSP* pShaderProgram =
            dynamic_cast<VoxelInjectRadianceSP*>(m_computePasses.pInjectRadiance->pShaderProgram);
        pShaderProgram->pushConstantsData.normalWeightedLambert = m_config.normalWeightedLambert;
        pShaderProgram->pushConstantsData.traceShadowHit        = m_config.traceShadowHit;

        auto* pPass =
            m_rdg->AddComputePassNode(m_computePasses.pInjectRadiance, "inject_voxel_radiance");
        // m_rdg->DeclareTextureAccessForPass(pass, 2, textures, RHITextureUsage::eStorage, ranges,
        //                                    RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(
        //     pass, voxelTextures.albedo, RHITextureUsage::eSampled,
        //     m_renderDevice->GetTextureSubResourceRange(voxelTextures.albedo), RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(
        //     pass, m_textures.pShadowMap, RHITextureUsage::eSampled,
        //     m_renderDevice->GetTextureSubResourceRange(m_textures.pShadowMap), RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(
        //     pass, m_textures.pVoxelRadiance, RHITextureUsage::eStorage,
        //     m_renderDevice->GetTextureSubResourceRange(m_textures.pVoxelRadiance),
        //     RHIAccessMode::eReadWrite);

        m_rdg->AddComputePassSetPushConstants(pPass, &pShaderProgram->pushConstantsData,
                                              sizeof(VoxelInjectRadianceSP::PushConstantsData));
        workgroupCount = m_pVoxelizer->GetVoxelTexResolution() / 8;
        m_rdg->AddComputePassDispatchNode(pPass, workgroupCount, workgroupCount, workgroupCount);
    }

    m_rdg->End();
}


void VoxelGIRenderer::BuildGraphicsPasses() {}

void VoxelGIRenderer::BuildComputePasses()
{
    {
        // reset voxel texture
        ComputePassBuilder builder(m_pRenderDevice);
        m_computePasses.pResetVoxelTexture = builder.SetShaderProgramName("ResetVoxelTextureSP")
                                                .SetTag("ResetVoxelTextureComp")
                                                .Build();
    }
    {
        // inject radiance
        ComputePassBuilder builder(m_pRenderDevice);
        m_computePasses.pInjectRadiance = builder.SetShaderProgramName("VoxelInjectRadianceSP")
                                             .SetTag("VoxelInjectRadianceComp")
                                             .Build();
    }
}

void VoxelGIRenderer::UpdatePassResources()
{
    // reset voxel texture
    {
        HeapVector<RHIShaderResourceBinding> set0bindings;
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eImage,
                                  m_textures.pVoxelRadiance);
        ComputePassResourceUpdater updater(m_pRenderDevice, m_computePasses.pResetVoxelTexture);
        updater.SetShaderResourceBinding(0, std::move(set0bindings)).Update();
    }
    // inject radiance pass
    {
        HeapVector<RHIShaderResourceBinding> set0bindings;
        HeapVector<RHIShaderResourceBinding> set1bindings;
        HeapVector<RHIShaderResourceBinding> set2bindings;

        auto& voxelTextures = m_pVoxelizer->GetVoxelTextures();

        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eSamplerWithTexture,
                                  m_pVoxelizer->GetVoxelSampler(), voxelTextures.pAlbedoProxy);
        ADD_SHADER_BINDING_SINGLE(set0bindings, 1, RHIShaderResourceType::eImage,
                                  voxelTextures.pNormalProxy);
        ADD_SHADER_BINDING_SINGLE(set0bindings, 2, RHIShaderResourceType::eImage,
                                  m_textures.pVoxelRadiance);
        ADD_SHADER_BINDING_SINGLE(set0bindings, 3, RHIShaderResourceType::eImage,
                                  voxelTextures.pEmissiveProxy);

        // set-1 bindings
        ShadowMapRenderer* pShadowMapRenderer =
            m_pRenderDevice->GetRendererServer()->RequestShadowMapRenderer();
        ADD_SHADER_BINDING_SINGLE(set1bindings, 0, RHIShaderResourceType::eSamplerWithTexture,
                                  pShadowMapRenderer->GetColorSampler(), m_textures.pShadowMap);
        // set-2 bindings
        ADD_SHADER_BINDING_SINGLE(
            set2bindings, 0, RHIShaderResourceType::eUniformBuffer,
            m_computePasses.pInjectRadiance->pShaderProgram->GetUniformBufferHandle("uLightInfo"));
        // ADD_SHADER_BINDING_SINGLE(
        //     set2bindings, 1, RHIShaderResourceType::eUniformBuffer,
        //     m_computePasses.pInjectRadiance.shaderProgram->GetUniformBufferHandle("uSceneInfo"));

        ComputePassResourceUpdater updater(m_pRenderDevice, m_computePasses.pInjectRadiance);
        updater.SetShaderResourceBinding(0, std::move(set0bindings))
            .SetShaderResourceBinding(1, std::move(set1bindings))
            .SetShaderResourceBinding(2, std::move(set2bindings))
            .Update();
    }
}

void VoxelGIRenderer::UpdateUniformData()
{
    {
        VoxelInjectRadianceSP* pShaderProgram =
            dynamic_cast<VoxelInjectRadianceSP*>(m_computePasses.pInjectRadiance->pShaderProgram);

        pShaderProgram->pushConstantsData.volumeDimension = m_pVoxelizer->GetVoxelTexResolution();
        pShaderProgram->pushConstantsData.voxelSize       = m_pVoxelizer->GetVoxelSize();
        pShaderProgram->pushConstantsData.voxelScale      = m_pVoxelizer->GetVoxelScale();
        pShaderProgram->pushConstantsData.worldMinPoint   = m_pVoxelizer->GetSceneMinPoint();
        // todo: light info should be added by user or imported from scene file
        auto& lightInfo = pShaderProgram->lightInfo;

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
        pShaderProgram->UpdateUniformBuffer("uLightInfo", pShaderProgram->GetLightInfoData(), 0);
    }
}
} // namespace zen::rc