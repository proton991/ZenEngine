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

SkyboxRenderer::SkyboxRenderer(RenderDevice* renderDevice, rhi::RHIViewport* viewport) :
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

    m_rdg = MakeUnique<RenderGraph>();

    PrepareTextures();

    BuildGraphicsPasses();
}

void SkyboxRenderer::Destroy()
{
    m_renderDevice->DestroyTexture(m_offscreenTextures.irradiance);
    m_renderDevice->DestroyTexture(m_offscreenTextures.prefiltered);
}

void SkyboxRenderer::PrepareTextures()
{
    TextureUsageHint usageHint{.copyUsage = true};
    {
        // TextureInfo offscreenTexInfo{};
        // offscreenTexInfo.width  = IRRADIANCE_DIM;
        // offscreenTexInfo.height = IRRADIANCE_DIM;
        // offscreenTexInfo.format = cIrradianceFormat;
        // offscreenTexInfo.usageFlags.SetFlags(TextureUsageFlagBits::eColorAttachment,
        //                                      TextureUsageFlagBits::eTransferSrc);
        // offscreenTexInfo.type = TextureType::e2D;
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
        // offscreenTexInfo.usageFlags.SetFlags(TextureUsageFlagBits::eColorAttachment,
        //                                      TextureUsageFlagBits::eTransferSrc);
        // offscreenTexInfo.type = TextureType::e2D;
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
    std::vector<rhi::RenderPassClearValue> clearValues(2);
    clearValues[0].color   = {0.0f, 0.0f, 0.2f, 0.0f};
    clearValues[1].depth   = 1.0f;
    clearValues[1].stencil = 0;
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

    auto* pass = m_rdg->AddGraphicsPassNode(m_gfxPasses.skybox, area, clearValues, "skybox_draw");
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
    using namespace zen::rhi;
    GfxPipelineStates pso{};
    pso.primitiveType      = DrawPrimitiveType::eTriangleList;
    pso.rasterizationState = {};
    pso.depthStencilState =
        GfxPipelineDepthStencilState::Create(false, false, CompareOperator::eLessOrEqual);
    pso.multiSampleState = {};
    pso.colorBlendState  = GfxPipelineColorBlendState::CreateDisabled(1);
    pso.dynamicStates.push_back(DynamicState::eScissor);
    pso.dynamicStates.push_back(DynamicState::eViewPort);

    {
        GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.irradiance = builder.SetShaderProgramName("EnvMapIrradianceSP")
                                     .SetNumSamples(SampleCount::e1)
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
            builder.SetShaderProgramName("EnvMapPrefilteredSP")
                .SetNumSamples(SampleCount::e1)
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

        pso.primitiveType               = DrawPrimitiveType::eTriangleList;
        pso.rasterizationState          = {};
        pso.rasterizationState.cullMode = PolygonCullMode::eFront;
        pso.depthStencilState =
            GfxPipelineDepthStencilState::Create(true, false, CompareOperator::eLessOrEqual);
        pso.multiSampleState = {};
        pso.colorBlendState  = GfxPipelineColorBlendState::CreateDisabled(1);
        pso.dynamicStates.push_back(DynamicState::eScissor);
        pso.dynamicStates.push_back(DynamicState::eViewPort);

        GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.skybox = builder.SetShaderProgramName("SkyboxRenderSP")
                                 .SetNumSamples(SampleCount::e1)
                                 // offscreen texture
                                 .AddViewportColorRT(m_viewport)
                                 .SetViewportDepthStencilRT(m_viewport, RenderTargetLoadOp::eClear,
                                                            RenderTargetStoreOp::eStore)
                                 .SetPipelineState(pso)
                                 .SetFramebufferInfo(m_viewport)
                                 .SetTag("SkyboxDraw")
                                 .Build();
    }
}

void SkyboxRenderer::GenerateEnvCubemaps(EnvTexture* texture)
{
    using namespace zen::rhi;

    for (uint32_t target = 0; target < PREFILTERED_MAP + 1; target++)
    {
        TextureRD* cubemapTexture;
        TextureRD* offscreenTexture;
        uint32_t dim;
        DataFormat format;
        GraphicsPass* gfxPass;
        std::string targetName = "";
        if (target == IRRADIANCE)
        {
            gfxPass          = &m_gfxPasses.irradiance;
            offscreenTexture = m_offscreenTextures.irradiance;
            format           = cIrradianceFormat;
            dim              = IRRADIANCE_DIM;
            targetName       = "irradiance_cubemap_gen";
        }
        else
        {
            gfxPass          = &m_gfxPasses.prefiltered;
            offscreenTexture = m_offscreenTextures.prefiltered;
            format           = cPrefilteredFormat;
            dim              = PREFILTERED_DIM;
            targetName       = "prefiltered_cubemap_gen";
        }

        TextureInfo textureInfo{};
        textureInfo.format      = format;
        textureInfo.width       = dim;
        textureInfo.height      = dim;
        textureInfo.type        = TextureType::eCube;
        textureInfo.depth       = 1;
        textureInfo.arrayLayers = 6;
        textureInfo.mipmaps     = CalculateTextureMipLevels(dim);
        textureInfo.usageFlags.SetFlag(TextureUsageFlagBits::eTransferDst);
        textureInfo.usageFlags.SetFlag(TextureUsageFlagBits::eSampled);

        const uint32_t numMips = textureInfo.mipmaps;

        SamplerInfo samplerInfo{};
        samplerInfo.minFilter     = SamplerFilter::eLinear;
        samplerInfo.magFilter     = SamplerFilter::eLinear;
        samplerInfo.repeatU       = SamplerRepeatMode::eClampToEdge;
        samplerInfo.repeatV       = SamplerRepeatMode::eClampToEdge;
        samplerInfo.repeatW       = SamplerRepeatMode::eClampToEdge;
        samplerInfo.minLod        = 0.0f;
        samplerInfo.maxLod        = static_cast<float>(numMips);
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.borderColor   = SamplerBorderColor::eFloatOpaqueBlack;

        m_samplers.cubemapSampler = m_renderDevice->CreateSampler(samplerInfo);

        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::eCube;
        texFormat.format      = format;
        texFormat.width       = dim;
        texFormat.height      = dim;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 6;
        texFormat.mipmaps     = CalculateTextureMipLevels(dim);

        cubemapTexture =
            m_renderDevice->CreateTextureSampled(texFormat, {.copyUsage = true}, targetName);

        // offscreen
        {
            std::vector<ShaderResourceBinding> textureBindings;
            ADD_SHADER_BINDING_SINGLE(textureBindings, 0, ShaderResourceType::eSamplerWithTexture,
                                      m_samplers.cubemapSampler, texture->skybox->GetHandle());

            GraphicsPassResourceUpdater updater(m_renderDevice, gfxPass);
            updater.SetShaderResourceBinding(0, textureBindings).Update();

            UniquePtr<RenderGraph> rdg = MakeUnique<RenderGraph>();
            rdg->Begin();
            std::vector<RenderPassClearValue> clearValues(1);
            clearValues[0].color = {0.0f, 0.0f, 0.2f, 0.0f};

            Rect2i area;
            area.minX = 0;
            area.minY = 0;
            area.maxX = static_cast<int>(dim);
            area.maxY = static_cast<int>(dim);

            // 1st pass: render to offscreen texture (6 faces)
            // auto* pass = rdg->AddGraphicsPassNode(*gfxPass, area, clearValues, targetName);
            // // rdg->DeclareTextureAccessForPass(pass, offscreenTexture, TextureUsage::eColorAttachment,
            // //                                  m_RHI->GetTextureSubResourceRange(offscreenTexture),
            // //                                  rhi::AccessMode::eReadWrite);
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
                    auto* pass = rdg->AddGraphicsPassNode(*gfxPass, area, clearValues, passTag);
                    // rdg->DeclareTextureAccessForPass(
                    //     pass, offscreenTexture, TextureUsage::eColorAttachment,
                    //     m_RHI->GetTextureSubResourceRange(offscreenTexture),
                    //     rhi::AccessMode::eReadWrite);
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
                    TextureCopyRegion copyRegion{};
                    copyRegion.srcSubresources.aspect.SetFlag(TextureAspectFlagBits::eColor);
                    copyRegion.srcOffset = {0, 0, 0};
                    copyRegion.dstSubresources.aspect.SetFlag(TextureAspectFlagBits::eColor);
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
            //     cubemapTexture->GetHandle(), TextureLayout::eShaderReadOnly);

            if (target == IRRADIANCE)
            {
                texture->irradiance        = cubemapTexture;
                texture->irradianceSampler = m_samplers.cubemapSampler;
                m_renderDevice->GetRHIDebug()->SetTextureDebugName(texture->irradiance->GetHandle(),
                                                                   "EnvIrradiance");
            }
            else
            {
                texture->prefiltered        = cubemapTexture;
                texture->prefilteredSampler = m_samplers.cubemapSampler;
                m_renderDevice->GetRHIDebug()->SetTextureDebugName(
                    texture->prefiltered->GetHandle(), "EnvPrefiltered");
            }
        }
    }
}

void SkyboxRenderer::GenerateLutBRDF(EnvTexture* texture)
{
    using namespace zen::rhi;

    const uint32_t dim      = 512;
    const DataFormat format = DataFormat::eR16G16SFloat;

    // TextureInfo textureInfo{};
    // textureInfo.format = format;
    // textureInfo.width  = dim;
    // textureInfo.height = dim;
    // textureInfo.type   = TextureType::e2D;
    // textureInfo.usageFlags.SetFlag(TextureUsageFlagBits::eColorAttachment);
    // textureInfo.usageFlags.SetFlag(TextureUsageFlagBits::eSampled);
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

    SamplerInfo samplerInfo{};
    samplerInfo.minFilter     = SamplerFilter::eLinear;
    samplerInfo.magFilter     = SamplerFilter::eLinear;
    samplerInfo.repeatU       = SamplerRepeatMode::eClampToEdge;
    samplerInfo.repeatV       = SamplerRepeatMode::eClampToEdge;
    samplerInfo.repeatW       = SamplerRepeatMode::eClampToEdge;
    samplerInfo.minLod        = 0.0f;
    samplerInfo.maxLod        = 1.0f;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor   = SamplerBorderColor::eFloatOpaqueWhite;

    m_samplers.lutBRDFSampler = m_renderDevice->CreateSampler(samplerInfo);
    texture->lutBRDFSampler   = m_samplers.lutBRDFSampler;

    // build lutBRDF gen pass
    GfxPipelineStates pso{};
    pso.primitiveType      = DrawPrimitiveType::eTriangleList;
    pso.rasterizationState = {};
    pso.depthStencilState =
        GfxPipelineDepthStencilState::Create(false, false, CompareOperator::eLessOrEqual);
    pso.multiSampleState = {};
    pso.colorBlendState  = GfxPipelineColorBlendState::CreateDisabled(1);
    pso.dynamicStates.push_back(DynamicState::eScissor);
    pso.dynamicStates.push_back(DynamicState::eViewPort);

    GraphicsPassBuilder builder(m_renderDevice);
    m_gfxPasses.lutBRDF = builder.SetShaderProgramName("EnvMapBRDFLutGenSP")
                              .SetNumSamples(SampleCount::e1)
                              // offscreen texture
                              .AddColorRenderTarget(texture->lutBRDF)
                              .SetPipelineState(pso)
                              .SetFramebufferInfo(m_viewport, dim, dim)
                              .SetTag("GenlutBRDF")
                              .Build();

    // build rdg
    UniquePtr<RenderGraph> rdg = MakeUnique<RenderGraph>();
    rdg->Begin();
    std::vector<RenderPassClearValue> clearValues(1);
    clearValues[0].color = {0.0f, 0.0f, 0.2f, 0.0f};

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
    auto* pass = rdg->AddGraphicsPassNode(m_gfxPasses.lutBRDF, area, clearValues, "lut_brdf_gen");
    // rdg->DeclareTextureAccessForPass(pass, texture->lutBRDF, TextureUsage::eColorAttachment,
    //                                  m_RHI->GetTextureSubResourceRange(texture->lutBRDF),
    //                                  rhi::AccessMode::eReadWrite);
    rdg->AddGraphicsPassSetScissorNode(pass, area);
    rdg->AddGraphicsPassSetViewportNode(pass, vp);
    rdg->AddGraphicsPassDrawNode(pass, 3, 1);
    rdg->End();

    m_renderDevice->ExecuteImmediate(m_viewport, rdg.Get());

    // m_renderDevice->GetCurrentUploadCmdList()->ChangeTextureLayout(texture->lutBRDF->GetHandle(),
    //                                                                TextureLayout::eShaderReadOnly);
}

void SkyboxRenderer::PrepareRenderWorkload()
{
    if (m_rebuildRDG)
    {
        BuildRenderGraph();
        m_rebuildRDG = false;
    }
    m_gfxPasses.skybox.shaderProgram->UpdateUniformBuffer("uCameraData",
                                                          m_scene->GetCameraUniformData(), 0);
}


void SkyboxRenderer::UpdateGraphicsPassResources()
{
    using namespace zen::rhi;
    std::vector<ShaderResourceBinding> bufferBindings;
    std::vector<ShaderResourceBinding> textureBindings;
    ADD_SHADER_BINDING_SINGLE(
        bufferBindings, 0, ShaderResourceType::eUniformBuffer,
        m_gfxPasses.skybox.shaderProgram->GetUniformBufferHandle("uCameraData"))

    ADD_SHADER_BINDING_SINGLE(textureBindings, 0, ShaderResourceType::eSamplerWithTexture,
                              m_samplers.cubemapSampler,
                              m_scene->GetEnvTexture().skybox->GetHandle())

    GraphicsPassResourceUpdater updater(m_renderDevice, &m_gfxPasses.skybox);
    updater.SetShaderResourceBinding(0, bufferBindings)
        .SetShaderResourceBinding(1, textureBindings)
        .Update();
}

void SkyboxRenderer::OnResize()
{
    m_rebuildRDG = true;
    // update graphics pass
    m_renderDevice->UpdateGraphicsPassOnResize(m_gfxPasses.skybox, m_viewport);
}
} // namespace zen::rc