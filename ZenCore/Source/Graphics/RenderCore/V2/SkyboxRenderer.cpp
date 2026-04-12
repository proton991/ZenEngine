#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "Graphics/RenderCore/V2/RenderResource.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/RenderScene.h"

#define IRRADIANCE_DIM  64
#define PREFILTERED_DIM 512

namespace zen::rc
{
static const std::vector<Mat4> cMatrices = {
    glm::rotate(glm::rotate(Mat4(1.0f), glm::radians(90.0f), Vec3(0.0f, 1.0f, 0.0f)),
                glm::radians(180.0f),
                Vec3(1.0f, 0.0f, 0.0f)),
    glm::rotate(glm::rotate(Mat4(1.0f), glm::radians(-90.0f), Vec3(0.0f, 1.0f, 0.0f)),
                glm::radians(180.0f),
                Vec3(1.0f, 0.0f, 0.0f)),
    glm::rotate(Mat4(1.0f), glm::radians(-90.0f), Vec3(1.0f, 0.0f, 0.0f)),
    glm::rotate(Mat4(1.0f), glm::radians(90.0f), Vec3(1.0f, 0.0f, 0.0f)),
    glm::rotate(Mat4(1.0f), glm::radians(180.0f), Vec3(1.0f, 0.0f, 0.0f)),
    glm::rotate(Mat4(1.0f), glm::radians(180.0f), Vec3(0.0f, 0.0f, 1.0f)),
};

SkyboxRenderer::SkyboxRenderer(RenderDevice* pRenderDevice, RHIViewport* pViewport) :
    m_pRenderDevice(pRenderDevice), m_pViewport(pViewport)
{}

void SkyboxRenderer::Init()
{
    uint32_t vbSize = cSkyboxVertices.size() * sizeof(SkyboxVertex);
    m_pVertexBuffer  = m_pRenderDevice->CreateVertexBuffer(
        vbSize, reinterpret_cast<const uint8_t*>(cSkyboxVertices.data()));

    m_pIndexBuffer =
        m_pRenderDevice->CreateIndexBuffer(cSkyboxIndices.size() * sizeof(uint32_t),
                                          reinterpret_cast<const uint8_t*>(cSkyboxIndices.data()));

    m_rdg = MakeUnique<RenderGraph>("skybox_draw_rdg");

    PrepareTextures();

    BuildGraphicsPasses();
}

void SkyboxRenderer::Destroy()
{
    // free texture resources
    m_pRenderDevice->DestroyTexture(m_offscreenTextures.pIrradiance);
    m_pRenderDevice->DestroyTexture(m_offscreenTextures.pPrefiltered);
}

void SkyboxRenderer::PrepareTextures()
{
    TextureUsageHint usageHint{.copyUsage = true};
    {
        // TextureInfo offscreenTexInfo{};
        // offscreenTexInfo.width  = IRRADIANCE_DIM;
        // offscreenTexInfo.height = IRRADIANCE_DIM;
        // offscreenTexInfo.format = cIrradianceFormat;
        // offscreenTexInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eColorAttachment,
        //                                      RHITextureUsageFlagBits::eTransferSrc);
        // offscreenTexInfo.type = RHITextureType::e2D;
        // offscreenTexInfo.name = "EnvIrradianceOffscreen";

        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::e2D;
        texFormat.format      = cIrradianceFormat;
        texFormat.width       = IRRADIANCE_DIM;
        texFormat.height      = IRRADIANCE_DIM;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        m_offscreenTextures.pIrradiance =
            m_pRenderDevice->CreateTextureColorRT(texFormat, usageHint, "env_irradiance_offscreen");
    }
    {
        // TextureInfo offscreenTexInfo{};
        // offscreenTexInfo.width  = PREFILTERED_DIM;
        // offscreenTexInfo.height = PREFILTERED_DIM;
        // offscreenTexInfo.format = cPrefilteredFormat;
        // offscreenTexInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eColorAttachment,
        //                                      RHITextureUsageFlagBits::eTransferSrc);
        // offscreenTexInfo.type = RHITextureType::e2D;
        // offscreenTexInfo.name = "EnvPrefilteredOffscreen";
        //
        // m_offscreenTextures.prefiltered = m_renderDevice->CreateTexture(offscreenTexInfo);

        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::e2D;
        texFormat.format      = cPrefilteredFormat;
        texFormat.width       = PREFILTERED_DIM;
        texFormat.height      = PREFILTERED_DIM;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        m_offscreenTextures.pPrefiltered =
            m_pRenderDevice->CreateTextureColorRT(texFormat, usageHint, "env_prefiltered_offscreen");
    }
}

void SkyboxRenderer::BuildRenderGraph()
{
    // build rdg
    m_rdg->Begin();
    // std::vector<RHIRenderPassClearValue> clearValues(2);
    // clearValues[0].color   = {0.0f, 0.0f, 0.2f, 0.0f};
    // clearValues[1].depth   = 1.0f;
    // clearValues[1].stencil = 0;
    Rect2i area;
    area.minX = 0;
    area.minY = 0;
    area.maxX = static_cast<int>(m_pViewport->GetWidth());
    area.maxY = static_cast<int>(m_pViewport->GetHeight());

    Rect2<float> vp;
    vp.minX = 0.0f;
    vp.minY = 0.0f;
    vp.maxX = static_cast<float>(m_pViewport->GetWidth());
    vp.maxY = static_cast<float>(m_pViewport->GetHeight());

    auto* pPass = m_rdg->AddGraphicsPassNode(m_gfxPasses.pSkybox, "skybox_draw");
    m_rdg->AddGraphicsPassSetScissorNode(pPass, area);
    m_rdg->AddGraphicsPassSetViewportNode(pPass, vp);
    m_rdg->AddGraphicsPassBindVertexBufferNode(pPass, m_pVertexBuffer, {0});
    m_rdg->AddGraphicsPassBindIndexBufferNode(pPass, m_pIndexBuffer, DataFormat::eR32UInt);
    // draw skybox model
    m_rdg->AddGraphicsPassDrawIndexedNode(pPass, cSkyboxIndices.size(), 1, 0, 0, 0);
    m_rdg->End();
}

void SkyboxRenderer::BuildGraphicsPasses()
{
    RHIGfxPipelineStates pso{};
    pso.primitiveType      = RHIDrawPrimitiveType::eTriangleList;
    pso.rasterizationState = {};
    pso.depthStencilState  = RHIGfxPipelineDepthStencilState::Create(
        false, false, RHIDepthCompareOperator::eLessOrEqual);
    pso.multiSampleState = {};
    pso.colorBlendState.AddAttachment();
    pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

    {
        GraphicsPassBuilder builder(m_pRenderDevice);
        m_gfxPasses.pIrradiance = builder
                                     .SetShaderProgramName("EnvMapIrradianceSP")
                                     // .SetNumSamples(SampleCount::e1)
                                     // offscreen texture
                                     .AddColorRenderTarget(m_offscreenTextures.pIrradiance)
                                     .SetPipelineState(pso)
                                     .SetFramebufferInfo(m_pViewport, IRRADIANCE_DIM, IRRADIANCE_DIM)
                                     .SetTag("EnvIrradiance")
                                     .Build();
    }

    {
        GraphicsPassBuilder builder(m_pRenderDevice);
        m_gfxPasses.pPrefiltered =
            builder
                .SetShaderProgramName("EnvMapPrefilteredSP")
                // .SetNumSamples(SampleCount::e1)
                // offscreen texture
                .AddColorRenderTarget(m_offscreenTextures.pPrefiltered)
                .SetPipelineState(pso)
                .SetFramebufferInfo(m_pViewport, PREFILTERED_DIM, PREFILTERED_DIM)
                .SetTag("EnvPrefilter")
                .Build();
    }
    {
        // build skybox draw pPass
        pso = {};

        pso.primitiveType               = RHIDrawPrimitiveType::eTriangleList;
        pso.rasterizationState          = {};
        pso.rasterizationState.cullMode = RHIPolygonCullMode::eFront;
        pso.depthStencilState           = RHIGfxPipelineDepthStencilState::Create(
            true, false, RHIDepthCompareOperator::eLessOrEqual);
        pso.multiSampleState = {};
        pso.colorBlendState.AddAttachment();
        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

        GraphicsPassBuilder builder(m_pRenderDevice);
        m_gfxPasses.pSkybox =
            builder
                .SetShaderProgramName("SkyboxRenderSP")
                // .SetNumSamples(SampleCount::e1)
                // offscreen texture
                .AddViewportColorRT(m_pViewport)
                .SetViewportDepthStencilRT(m_pViewport, RHIRenderTargetLoadOp::eClear,
                                           RHIRenderTargetStoreOp::eStore)
                .SetPipelineState(pso)
                .SetFramebufferInfo(m_pViewport)
                .SetTag("SkyboxDraw")
                .Build();
    }
}

void SkyboxRenderer::PreprocessEnvTexture(EnvTexture* pTexture)
{
    HeapVector<UniquePtr<RenderGraph>> outRenderGraphs;

    GenerateEnvCubemaps(pTexture, outRenderGraphs);
    GenerateLutBRDF(pTexture, outRenderGraphs);

    m_pRenderDevice->ExecuteImmediate(outRenderGraphs);
}

void SkyboxRenderer::GenerateEnvCubemaps(EnvTexture* pTexture,
                                         HeapVector<UniquePtr<RenderGraph>>& outRDGs)
{
    for (uint32_t target = 0; target < PREFILTERED_MAP + 1; target++)
    {
        RHITexture* pCubemapTexture;
        RHITexture* pOffscreenTexture;
        uint32_t dim;
        DataFormat format;
        GraphicsPass* pGfxPass;
        std::string targetName  = "";
        std::string textureName = "";

        if (target == IRRADIANCE)
        {
            pGfxPass          = m_gfxPasses.pIrradiance;
            pOffscreenTexture = m_offscreenTextures.pIrradiance;
            format           = cIrradianceFormat;
            dim              = IRRADIANCE_DIM;
            targetName       = "irradiance_cubemap_gen";
            textureName      = "env_irradiance";
        }
        else
        {
            pGfxPass          = m_gfxPasses.pPrefiltered;
            pOffscreenTexture = m_offscreenTextures.pPrefiltered;
            format           = cPrefilteredFormat;
            dim              = PREFILTERED_DIM;
            targetName       = "prefiltered_cubemap_gen";
            textureName      = "env_prefiltered";
        }

        const uint32_t numMips = RHITexture::CalculateTextureMipLevels(dim);

        RHISamplerCreateInfo samplerInfo{};
        samplerInfo.minFilter     = RHISamplerFilter::eLinear;
        samplerInfo.magFilter     = RHISamplerFilter::eLinear;
        samplerInfo.repeatU       = RHISamplerRepeatMode::eClampToEdge;
        samplerInfo.repeatV       = RHISamplerRepeatMode::eClampToEdge;
        samplerInfo.repeatW       = RHISamplerRepeatMode::eClampToEdge;
        samplerInfo.minLod        = 0.0f;
        samplerInfo.maxLod        = static_cast<float>(numMips);
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.borderColor   = RHISamplerBorderColor::eFloatOpaqueBlack;

        m_samplers.pCubemapSampler = m_pRenderDevice->CreateSampler(samplerInfo);

        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::eCube;
        texFormat.format      = format;
        texFormat.width       = dim;
        texFormat.height      = dim;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 6;
        texFormat.mipmaps     = numMips;

        pCubemapTexture =
            m_pRenderDevice->CreateTextureSampled(texFormat, {.copyUsage = true}, textureName);

        // offscreen
        {
            HeapVector<RHIShaderResourceBinding> textureBindings;
            ADD_SHADER_BINDING_SINGLE(textureBindings, 0,
                                      RHIShaderResourceType::eSamplerWithTexture,
                                      m_samplers.pCubemapSampler, pTexture->pSkybox);

            GraphicsPassResourceUpdater updater(m_pRenderDevice, pGfxPass);
            updater.SetShaderResourceBinding(0, std::move(textureBindings)).Update();

            UniquePtr<RenderGraph> rdg = MakeUnique<RenderGraph>("env_cubmap_gen_rdg");
            rdg->Begin();
            // std::vector<RHIRenderPassClearValue> clearValues(1);
            // clearValues[0].color = {0.0f, 0.0f, 0.2f, 0.0f};

            Rect2i area;
            area.minX = 0;
            area.minY = 0;
            area.maxX = static_cast<int>(dim);
            area.maxY = static_cast<int>(dim);

            // 1st pPass: render to offscreen texture (6 faces)
            // auto* pPass = rdg->AddGraphicsPassNode(*gfxPass, area, clearValues, targetName);
            // // rdg->DeclareTextureAccessForPass(pPass, offscreenTexture, RHITextureUsage::eColorAttachment,
            // //                                  m_RHI->GetTextureSubResourceRange(offscreenTexture),
            // //                                  RHIAccessMode::eReadWrite);
            // rdg->AddGraphicsPassSetScissorNode(pPass, area);
            // rdg->AddGraphicsPassBindVertexBufferNode(pPass, m_vertexBuffer, {0});
            // rdg->AddGraphicsPassBindIndexBufferNode(pPass, m_indexBuffer, DataFormat::eR32UInt);

            for (uint32_t m = 0; m < numMips; m++)
            {
                for (uint32_t f = 0; f < 6; f++)
                {
                    Rect2<float> vp;
                    vp.minX = 0.0f;
                    vp.minY = 0.0f;
                    vp.maxX = static_cast<float>(dim * std::pow(0.5f, m));
                    vp.maxY = static_cast<float>(dim * std::pow(0.5f, m));

                    std::string passTag =
                        targetName + "_mip_" + std::to_string(m) + "_face_" + std::to_string(f);
                    auto* pPass = rdg->AddGraphicsPassNode(pGfxPass, passTag);
                    // rdg->DeclareTextureAccessForPass(
                    //     pPass, offscreenTexture, RHITextureUsage::eColorAttachment,
                    //     m_RHI->GetTextureSubResourceRange(offscreenTexture),
                    //     RHIAccessMode::eReadWrite);
                    rdg->AddGraphicsPassSetScissorNode(pPass, area);
                    rdg->AddGraphicsPassSetViewportNode(pPass, vp);
                    rdg->AddGraphicsPassBindVertexBufferNode(pPass, m_pVertexBuffer, {0});
                    rdg->AddGraphicsPassBindIndexBufferNode(pPass, m_pIndexBuffer,
                                                            DataFormat::eR32UInt);
                    if (target == IRRADIANCE)
                    {
                        m_pcIrradiance.mvp =
                            glm::perspective(static_cast<float>((M_PI / 2.0)), 1.0f, 0.1f, 512.0f) *
                            cMatrices[f];
                        rdg->AddGraphicsPassSetPushConstants(pPass, &m_pcIrradiance,
                                                             sizeof(PushConstantIrradiance));
                    }
                    else
                    {
                        m_pcPrefilterEnv.mvp =
                            glm::perspective(static_cast<float>((M_PI / 2.0)), 1.0f, 0.1f, 512.0f) *
                            cMatrices[f];
                        m_pcPrefilterEnv.roughness =
                            static_cast<float>(m) / static_cast<float>(numMips - 1);
                        rdg->AddGraphicsPassSetPushConstants(pPass, &m_pcPrefilterEnv,
                                                             sizeof(PushConstantPrefilterEnv));
                    }

                    // draw skybox model
                    rdg->AddGraphicsPassDrawIndexedNode(pPass, cSkyboxIndices.size(), 1, 0, 0, 0);

                    // 2nd pPass: copy to env texture cubemap
                    RHITextureCopyRegion copyRegion{};
                    copyRegion.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
                    copyRegion.srcOffset = {0, 0, 0};
                    copyRegion.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
                    copyRegion.dstSubresources.baseArrayLayer = f;
                    copyRegion.dstSubresources.mipmap         = m;
                    copyRegion.dstOffset                      = {0, 0, 0};
                    copyRegion.size                           = {vp.Width(), vp.Height(), 1};

                    rdg->AddTextureCopyNode(pOffscreenTexture, pCubemapTexture, copyRegion);
                }
            }

            rdg->End();

            outRDGs.emplace_back(rdg);

            // m_renderDevice->ExecuteImmediate(m_viewport, rdg.Get());

            // m_renderDevice->GetCurrentUploadCmdList()->ChangeTextureLayout(
            //     cubemapTexture, RHITextureLayout::eShaderReadOnly);

            if (target == IRRADIANCE)
            {
                pTexture->pIrradiance        = pCubemapTexture;
                pTexture->pIrradianceSampler = m_samplers.pCubemapSampler;
                m_pRenderDevice->GetRHIDebug()->SetTextureDebugName(pTexture->pIrradiance,
                                                                   "EnvIrradiance");
            }
            else
            {
                pTexture->pPrefiltered        = pCubemapTexture;
                pTexture->pPrefilteredSampler = m_samplers.pCubemapSampler;
                m_pRenderDevice->GetRHIDebug()->SetTextureDebugName(pTexture->pPrefiltered,
                                                                   "EnvPrefiltered");
            }
        }
    }
}

void SkyboxRenderer::GenerateLutBRDF(EnvTexture* pTexture,
                                     HeapVector<UniquePtr<RenderGraph>>& outRDGs)
{
    const uint32_t dim = 512;

    TextureFormat texFormat{};
    texFormat.format      = DataFormat::eR16G16SFloat;
    texFormat.dimension   = TextureDimension::e2D;
    texFormat.width       = dim;
    texFormat.height      = dim;
    texFormat.depth       = 1;
    texFormat.arrayLayers = 1;
    texFormat.mipmaps     = 1;

    pTexture->pLutBRDF =
        m_pRenderDevice->CreateTextureColorRT(texFormat, {.copyUsage = false}, "env_lut_brdf");

    RHISamplerCreateInfo samplerInfo{};
    samplerInfo.minFilter     = RHISamplerFilter::eLinear;
    samplerInfo.magFilter     = RHISamplerFilter::eLinear;
    samplerInfo.repeatU       = RHISamplerRepeatMode::eClampToEdge;
    samplerInfo.repeatV       = RHISamplerRepeatMode::eClampToEdge;
    samplerInfo.repeatW       = RHISamplerRepeatMode::eClampToEdge;
    samplerInfo.minLod        = 0.0f;
    samplerInfo.maxLod        = 1.0f;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor   = RHISamplerBorderColor::eFloatOpaqueWhite;

    m_samplers.pLutBRDFSampler = m_pRenderDevice->CreateSampler(samplerInfo);
    pTexture->pLutBRDFSampler   = m_samplers.pLutBRDFSampler;

    // build lutBRDF gen pPass
    RHIGfxPipelineStates pso{};
    pso.primitiveType      = RHIDrawPrimitiveType::eTriangleList;
    pso.rasterizationState = {};
    pso.depthStencilState  = RHIGfxPipelineDepthStencilState::Create(
        false, false, RHIDepthCompareOperator::eLessOrEqual);
    pso.multiSampleState = {};
    pso.colorBlendState.AddAttachment();
    pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

    GraphicsPassBuilder builder(m_pRenderDevice);
    m_gfxPasses.pLutBRDF = builder
                              .SetShaderProgramName("EnvMapBRDFLutGenSP")
                              // .SetNumSamples(SampleCount::e1)
                              // offscreen texture
                              .AddColorRenderTarget(pTexture->pLutBRDF)
                              .SetPipelineState(pso)
                              .SetFramebufferInfo(m_pViewport, dim, dim)
                              .SetTag("GenlutBRDF")
                              .Build();

    // build rdg
    UniquePtr<RenderGraph> rdg = MakeUnique<RenderGraph>("lut_brdf_ge_rdg");
    rdg->Begin();
    // std::vector<RHIRenderPassClearValue> clearValues(1);
    // clearValues[0].color = {0.0f, 0.0f, 0.2f, 0.0f};

    Rect2i area;
    area.minX = 0;
    area.minY = 0;
    area.maxX = static_cast<int>(dim);
    area.maxY = static_cast<int>(dim);

    Rect2<float> vp;
    vp.minX    = 0.0f;
    vp.minY    = 0.0f;
    vp.maxX    = static_cast<float>(dim);
    vp.maxY    = static_cast<float>(dim);
    auto* pPass = rdg->AddGraphicsPassNode(m_gfxPasses.pLutBRDF, "lut_brdf_gen");
    // rdg->DeclareTextureAccessForPass(pPass, texture->lutBRDF, RHITextureUsage::eColorAttachment,
    //                                  m_RHI->GetTextureSubResourceRange(texture->lutBRDF),
    //                                  RHIAccessMode::eReadWrite);
    rdg->AddGraphicsPassSetScissorNode(pPass, area);
    rdg->AddGraphicsPassSetViewportNode(pPass, vp);
    rdg->AddGraphicsPassDrawNode(pPass, 3, 1);
    rdg->End();

    outRDGs.emplace_back(rdg);

    // m_renderDevice->ExecuteImmediate(m_viewport, rdg.Get());

    // m_renderDevice->GetCurrentUploadCmdList()->ChangeTextureLayout(texture->lutBRDF,
    //                                                                RHITextureLayout::eShaderReadOnly);
}

void SkyboxRenderer::PrepareRenderWorkload()
{
    if (m_rebuildRDG)
    {
        BuildRenderGraph();
        m_rebuildRDG = false;
    }
    m_gfxPasses.pSkybox->pShaderProgram->UpdateUniformBuffer("uCameraData",
                                                           m_pScene->GetCameraUniformData(), 0);
}


void SkyboxRenderer::UpdateGraphicsPassResources()
{

    HeapVector<RHIShaderResourceBinding> bufferBindings;
    HeapVector<RHIShaderResourceBinding> textureBindings;
    ADD_SHADER_BINDING_SINGLE(
        bufferBindings, 0, RHIShaderResourceType::eUniformBuffer,
        m_gfxPasses.pSkybox->pShaderProgram->GetUniformBufferHandle("uCameraData"))

    ADD_SHADER_BINDING_SINGLE(textureBindings, 0, RHIShaderResourceType::eSamplerWithTexture,
                              m_samplers.pCubemapSampler, m_pScene->GetEnvTexture().pSkybox)

    GraphicsPassResourceUpdater updater(m_pRenderDevice, m_gfxPasses.pSkybox);
    updater.SetShaderResourceBinding(0, std::move(bufferBindings))
        .SetShaderResourceBinding(1, std::move(textureBindings))
        .Update();
}

void SkyboxRenderer::OnResize()
{
    m_rebuildRDG = true;
    // update graphics pPass
    m_pRenderDevice->UpdateGraphicsPassOnResize(m_gfxPasses.pSkybox, m_pViewport);
}
} // namespace zen::rc