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

SkyboxRenderer::SkyboxRenderer(RenderDevice* renderDevice, RHIViewport* viewport) :
    m_renderDevice(renderDevice), m_viewport(viewport)
{}

void SkyboxRenderer::Init()
{
    uint32_t vbSize = cSkyboxVertices.size() * sizeof(SkyboxVertex);
    m_vertexBuffer  = m_renderDevice->CreateVertexBuffer(
        vbSize, reinterpret_cast<const uint8_t*>(cSkyboxVertices.data()));

    m_indexBuffer =
        m_renderDevice->CreateIndexBuffer(cSkyboxIndices.size() * sizeof(uint32_t),
                                          reinterpret_cast<const uint8_t*>(cSkyboxIndices.data()));

    m_rdg = MakeUnique<RenderGraph>("skybox_draw_rdg");

    PrepareTextures();

    BuildGraphicsPasses();
}

void SkyboxRenderer::Destroy() {}

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

        m_offscreenTextures.irradiance =
            m_renderDevice->CreateTextureColorRT(texFormat, usageHint, "env_irradiance_offscreen");
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

        m_offscreenTextures.prefiltered =
            m_renderDevice->CreateTextureColorRT(texFormat, usageHint, "env_prefiltered_offscreen");
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
    area.maxX = static_cast<int>(m_viewport->GetWidth());
    area.maxY = static_cast<int>(m_viewport->GetHeight());

    Rect2<float> vp;
    vp.minX = 0.0f;
    vp.minY = 0.0f;
    vp.maxX = static_cast<float>(m_viewport->GetWidth());
    vp.maxY = static_cast<float>(m_viewport->GetHeight());

    auto* pass = m_rdg->AddGraphicsPassNode(m_gfxPasses.skybox, "skybox_draw");
    m_rdg->AddGraphicsPassSetScissorNode(pass, area);
    m_rdg->AddGraphicsPassSetViewportNode(pass, vp);
    m_rdg->AddGraphicsPassBindVertexBufferNode(pass, m_vertexBuffer, {0});
    m_rdg->AddGraphicsPassBindIndexBufferNode(pass, m_indexBuffer, DataFormat::eR32UInt);
    // draw skybox model
    m_rdg->AddGraphicsPassDrawIndexedNode(pass, cSkyboxIndices.size(), 1, 0, 0, 0);
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
        GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.irradiance = builder
                                     .SetShaderProgramName("EnvMapIrradianceSP")
                                     // .SetNumSamples(SampleCount::e1)
                                     // offscreen texture
                                     .AddColorRenderTarget(m_offscreenTextures.irradiance)
                                     .SetPipelineState(pso)
                                     .SetFramebufferInfo(m_viewport, IRRADIANCE_DIM, IRRADIANCE_DIM)
                                     .SetTag("EnvIrradiance")
                                     .Build();
    }

    {
        GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.prefiltered =
            builder
                .SetShaderProgramName("EnvMapPrefilteredSP")
                // .SetNumSamples(SampleCount::e1)
                // offscreen texture
                .AddColorRenderTarget(m_offscreenTextures.prefiltered)
                .SetPipelineState(pso)
                .SetFramebufferInfo(m_viewport, PREFILTERED_DIM, PREFILTERED_DIM)
                .SetTag("EnvPrefilter")
                .Build();
    }
    {
        // build skybox draw pass
        pso = {};

        pso.primitiveType               = RHIDrawPrimitiveType::eTriangleList;
        pso.rasterizationState          = {};
        pso.rasterizationState.cullMode = RHIPolygonCullMode::eFront;
        pso.depthStencilState           = RHIGfxPipelineDepthStencilState::Create(
            true, false, RHIDepthCompareOperator::eLessOrEqual);
        pso.multiSampleState = {};
        pso.colorBlendState.AddAttachment();
        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

        GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.skybox =
            builder
                .SetShaderProgramName("SkyboxRenderSP")
                // .SetNumSamples(SampleCount::e1)
                // offscreen texture
                .AddViewportColorRT(m_viewport)
                .SetViewportDepthStencilRT(m_viewport, RHIRenderTargetLoadOp::eClear,
                                           RHIRenderTargetStoreOp::eStore)
                .SetPipelineState(pso)
                .SetFramebufferInfo(m_viewport)
                .SetTag("SkyboxDraw")
                .Build();
    }
}

void SkyboxRenderer::GenerateEnvCubemaps(EnvTexture* texture)
{


    for (uint32_t target = 0; target < PREFILTERED_MAP + 1; target++)
    {
        RHITexture* cubemapTexture;
        RHITexture* offscreenTexture;
        uint32_t dim;
        DataFormat format;
        GraphicsPass* gfxPass;
        std::string targetName = "";
        if (target == IRRADIANCE)
        {
            gfxPass          = m_gfxPasses.irradiance;
            offscreenTexture = m_offscreenTextures.irradiance;
            format           = cIrradianceFormat;
            dim              = IRRADIANCE_DIM;
            targetName       = "irradiance_cubemap_gen";
        }
        else
        {
            gfxPass          = m_gfxPasses.prefiltered;
            offscreenTexture = m_offscreenTextures.prefiltered;
            format           = cPrefilteredFormat;
            dim              = PREFILTERED_DIM;
            targetName       = "prefiltered_cubemap_gen";
        }

        // TextureInfo textureInfo{};
        // textureInfo.format      = format;
        // textureInfo.width       = dim;
        // textureInfo.height      = dim;
        // textureInfo.type        = RHITextureType::eCube;
        // textureInfo.depth       = 1;
        // textureInfo.arrayLayers = 6;
        // textureInfo.mipmaps     = CalculateTextureMipLevels(dim);
        // textureInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);
        // textureInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);
        //
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

        m_samplers.cubemapSampler = m_renderDevice->CreateSampler(samplerInfo);

        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::eCube;
        texFormat.format      = format;
        texFormat.width       = dim;
        texFormat.height      = dim;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 6;
        texFormat.mipmaps     = numMips;

        cubemapTexture =
            m_renderDevice->CreateTextureSampled(texFormat, {.copyUsage = true}, targetName);

        // offscreen
        {
            HeapVector<RHIShaderResourceBinding> textureBindings;
            ADD_SHADER_BINDING_SINGLE(textureBindings, 0,
                                      RHIShaderResourceType::eSamplerWithTexture,
                                      m_samplers.cubemapSampler, texture->skybox);

            GraphicsPassResourceUpdater updater(m_renderDevice, gfxPass);
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

            // 1st pass: render to offscreen texture (6 faces)
            // auto* pass = rdg->AddGraphicsPassNode(*gfxPass, area, clearValues, targetName);
            // // rdg->DeclareTextureAccessForPass(pass, offscreenTexture, RHITextureUsage::eColorAttachment,
            // //                                  m_RHI->GetTextureSubResourceRange(offscreenTexture),
            // //                                  RHIAccessMode::eReadWrite);
            // rdg->AddGraphicsPassSetScissorNode(pass, area);
            // rdg->AddGraphicsPassBindVertexBufferNode(pass, m_vertexBuffer, {0});
            // rdg->AddGraphicsPassBindIndexBufferNode(pass, m_indexBuffer, DataFormat::eR32UInt);

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
                    auto* pass = rdg->AddGraphicsPassNode(gfxPass, passTag);
                    // rdg->DeclareTextureAccessForPass(
                    //     pass, offscreenTexture, RHITextureUsage::eColorAttachment,
                    //     m_RHI->GetTextureSubResourceRange(offscreenTexture),
                    //     RHIAccessMode::eReadWrite);
                    rdg->AddGraphicsPassSetScissorNode(pass, area);
                    rdg->AddGraphicsPassSetViewportNode(pass, vp);
                    rdg->AddGraphicsPassBindVertexBufferNode(pass, m_vertexBuffer, {0});
                    rdg->AddGraphicsPassBindIndexBufferNode(pass, m_indexBuffer,
                                                            DataFormat::eR32UInt);
                    if (target == IRRADIANCE)
                    {
                        m_pcIrradiance.mvp =
                            glm::perspective(static_cast<float>((M_PI / 2.0)), 1.0f, 0.1f, 512.0f) *
                            cMatrices[f];
                        rdg->AddGraphicsPassSetPushConstants(pass, &m_pcIrradiance,
                                                             sizeof(PushConstantIrradiance));
                    }
                    else
                    {
                        m_pcPrefilterEnv.mvp =
                            glm::perspective(static_cast<float>((M_PI / 2.0)), 1.0f, 0.1f, 512.0f) *
                            cMatrices[f];
                        m_pcPrefilterEnv.roughness =
                            static_cast<float>(m) / static_cast<float>(numMips - 1);
                        rdg->AddGraphicsPassSetPushConstants(pass, &m_pcPrefilterEnv,
                                                             sizeof(PushConstantPrefilterEnv));
                    }

                    // draw skybox model
                    rdg->AddGraphicsPassDrawIndexedNode(pass, cSkyboxIndices.size(), 1, 0, 0, 0);

                    // 2nd pass: copy to env texture cubemap
                    RHITextureCopyRegion copyRegion{};
                    copyRegion.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
                    copyRegion.srcOffset = {0, 0, 0};
                    copyRegion.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
                    copyRegion.dstSubresources.baseArrayLayer = f;
                    copyRegion.dstSubresources.mipmap         = m;
                    copyRegion.dstOffset                      = {0, 0, 0};
                    copyRegion.size                           = {vp.Width(), vp.Height(), 1};

                    rdg->AddTextureCopyNode(offscreenTexture, cubemapTexture, copyRegion);
                }
            }

            rdg->End();

            m_renderDevice->ExecuteImmediate(m_viewport, rdg.Get());

            // m_renderDevice->GetCurrentUploadCmdList()->ChangeTextureLayout(
            //     cubemapTexture, RHITextureLayout::eShaderReadOnly);

            if (target == IRRADIANCE)
            {
                texture->irradiance        = cubemapTexture;
                texture->irradianceSampler = m_samplers.cubemapSampler;
                m_renderDevice->GetRHIDebug()->SetTextureDebugName(texture->irradiance,
                                                                   "EnvIrradiance");
            }
            else
            {
                texture->prefiltered        = cubemapTexture;
                texture->prefilteredSampler = m_samplers.cubemapSampler;
                m_renderDevice->GetRHIDebug()->SetTextureDebugName(texture->prefiltered,
                                                                   "EnvPrefiltered");
            }
        }
    }
    // free texture resources
    m_renderDevice->DestroyTexture(m_offscreenTextures.irradiance);
    m_renderDevice->DestroyTexture(m_offscreenTextures.prefiltered);
}

void SkyboxRenderer::GenerateLutBRDF(EnvTexture* texture)
{


    const uint32_t dim      = 512;
    const DataFormat format = DataFormat::eR16G16SFloat;

    // TextureInfo textureInfo{};
    // textureInfo.format = format;
    // textureInfo.width  = dim;
    // textureInfo.height = dim;
    // textureInfo.type   = RHITextureType::e2D;
    // textureInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eColorAttachment);
    // textureInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);
    //
    // texture->lutBRDF = m_RHI->CreateTexture(textureInfo);

    TextureFormat texFormat{};
    texFormat.format      = DataFormat::eR16G16SFloat;
    texFormat.dimension   = TextureDimension::e2D;
    texFormat.width       = dim;
    texFormat.height      = dim;
    texFormat.depth       = 1;
    texFormat.arrayLayers = 1;
    texFormat.mipmaps     = 1;

    texture->lutBRDF =
        m_renderDevice->CreateTextureColorRT(texFormat, {.copyUsage = false}, "env_lut_brdf");

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

    m_samplers.lutBRDFSampler = m_renderDevice->CreateSampler(samplerInfo);
    texture->lutBRDFSampler   = m_samplers.lutBRDFSampler;

    // build lutBRDF gen pass
    RHIGfxPipelineStates pso{};
    pso.primitiveType      = RHIDrawPrimitiveType::eTriangleList;
    pso.rasterizationState = {};
    pso.depthStencilState  = RHIGfxPipelineDepthStencilState::Create(
        false, false, RHIDepthCompareOperator::eLessOrEqual);
    pso.multiSampleState = {};
    pso.colorBlendState.AddAttachment();
    pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

    GraphicsPassBuilder builder(m_renderDevice);
    m_gfxPasses.lutBRDF = builder
                              .SetShaderProgramName("EnvMapBRDFLutGenSP")
                              // .SetNumSamples(SampleCount::e1)
                              // offscreen texture
                              .AddColorRenderTarget(texture->lutBRDF)
                              .SetPipelineState(pso)
                              .SetFramebufferInfo(m_viewport, dim, dim)
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
    auto* pass = rdg->AddGraphicsPassNode(m_gfxPasses.lutBRDF, "lut_brdf_gen");
    // rdg->DeclareTextureAccessForPass(pass, texture->lutBRDF, RHITextureUsage::eColorAttachment,
    //                                  m_RHI->GetTextureSubResourceRange(texture->lutBRDF),
    //                                  RHIAccessMode::eReadWrite);
    rdg->AddGraphicsPassSetScissorNode(pass, area);
    rdg->AddGraphicsPassSetViewportNode(pass, vp);
    rdg->AddGraphicsPassDrawNode(pass, 3, 1);
    rdg->End();

    m_renderDevice->ExecuteImmediate(m_viewport, rdg.Get());

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
    m_gfxPasses.skybox->shaderProgram->UpdateUniformBuffer("uCameraData",
                                                           m_scene->GetCameraUniformData(), 0);
}


void SkyboxRenderer::UpdateGraphicsPassResources()
{

    HeapVector<RHIShaderResourceBinding> bufferBindings;
    HeapVector<RHIShaderResourceBinding> textureBindings;
    ADD_SHADER_BINDING_SINGLE(
        bufferBindings, 0, RHIShaderResourceType::eUniformBuffer,
        m_gfxPasses.skybox->shaderProgram->GetUniformBufferHandle("uCameraData"))

    ADD_SHADER_BINDING_SINGLE(textureBindings, 0, RHIShaderResourceType::eSamplerWithTexture,
                              m_samplers.cubemapSampler, m_scene->GetEnvTexture().skybox)

    GraphicsPassResourceUpdater updater(m_renderDevice, m_gfxPasses.skybox);
    updater.SetShaderResourceBinding(0, std::move(bufferBindings))
        .SetShaderResourceBinding(1, std::move(textureBindings))
        .Update();
}

void SkyboxRenderer::OnResize()
{
    m_rebuildRDG = true;
    // update graphics pass
    m_renderDevice->UpdateGraphicsPassOnResize(m_gfxPasses.skybox, m_viewport);
}
} // namespace zen::rc