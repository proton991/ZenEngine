#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/RenderResource.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "SceneGraph/Camera.h"
#include "Templates/SmallVector.h"

#define IRRADIANCE_DIM  64
#define PREFILTERED_DIM 512

namespace zen::rc
{
static const SmallVector<Mat4, 6> cMatrices = {
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
    m_pVertexBuffer = m_pRenderDevice->CreateVertexBuffer(
        vbSize, reinterpret_cast<const uint8_t*>(cSkyboxVertices.data()));

    m_pIndexBuffer =
        m_pRenderDevice->CreateIndexBuffer(cSkyboxIndices.size() * sizeof(uint32_t),
                                           reinterpret_cast<const uint8_t*>(cSkyboxIndices.data()));

    PrepareTextures();
}

void SkyboxRenderer::Destroy()
{
    m_pRenderDevice->DestroyTexture(m_offscreenTextures.pIrradiance);
    m_pRenderDevice->DestroyTexture(m_offscreenTextures.pPrefiltered);
}

void SkyboxRenderer::PrepareTextures()
{
    TextureUsageHint usageHint{.copyUsage = true};

    {
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
        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::e2D;
        texFormat.format      = cPrefilteredFormat;
        texFormat.width       = PREFILTERED_DIM;
        texFormat.height      = PREFILTERED_DIM;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        m_offscreenTextures.pPrefiltered = m_pRenderDevice->CreateTextureColorRT(
            texFormat, usageHint, "env_prefiltered_offscreen");
    }
}

void SkyboxRenderer::BuildRenderGraph()
{
    RenderGraph* pRDG = m_pRenderDevice->GetCurrentFrameRDG();
    VERIFY_EXPR(pRDG != nullptr && m_pScene != nullptr);
    m_pRecordedPreprocessEnvTexture = nullptr;

    if (EnvTexture* pTexture = m_pPendingPreprocessEnvTexture)
    {
        RHIGfxPipelineStates pso{};
        pso.primitiveType      = RHIDrawPrimitiveType::eTriangleList;
        pso.rasterizationState = {};
        pso.depthStencilState  = RHIGfxPipelineDepthStencilState::Create(
            false, false, RHIDepthCompareOperator::eLessOrEqual);
        pso.multiSampleState = {};
        pso.colorBlendState.AddAttachment();
        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

        for (uint32_t target = 0; target <= PREFILTERED_MAP; ++target)
        {
            const bool irradiance = target == IRRADIANCE;
            RHITexture* pCubemap  = irradiance ? pTexture->pIrradiance : pTexture->pPrefiltered;
            RHITexture* pOffscreen =
                irradiance ? m_offscreenTextures.pIrradiance : m_offscreenTextures.pPrefiltered;
            const uint32_t dim     = irradiance ? IRRADIANCE_DIM : PREFILTERED_DIM;
            const uint32_t numMips = pCubemap->GetNumMipmaps();
            const NameID targetName =
                irradiance ? "irradiance_cubemap_gen" : "prefiltered_cubemap_gen";

            for (uint32_t mip = 0; mip < numMips; ++mip)
            {
                const uint32_t extent = std::max(1u, dim >> mip);

                for (uint32_t face = 0; face < 6; ++face)
                {
                    RDGGraphicsPassDesc desc{};
                    desc.SetShaderProgramName(irradiance ? "EnvMapIrradianceSP" :
                                                           "EnvMapPrefilteredSP");
                    desc.AddColorOutput(pOffscreen);
                    desc.SetPipelineStates(pso);

                    const NameID tag(
                        fmt::format("{}_mip_{}_face_{}", targetName.CStr(), mip, face));
                    desc.SetPassTag(tag);
                    desc.SetRenderArea(0, 0, extent, extent);

                    desc.BindVertexBuffer(m_pVertexBuffer);
                    desc.BindIndexBuffer(m_pIndexBuffer);
                    desc.BindSampledTexture("samplerEnv", pTexture->pPrefilteredSampler,
                                            pTexture->pSkybox->GetDefaultView());

                    const Mat4 mvp = glm::perspective(glm::half_pi<float>(), 1.0f, 0.1f, 512.0f) *
                        cMatrices[face];
                    const uint32_t count = static_cast<uint32_t>(cSkyboxIndices.size());

                    if (irradiance)
                    {
                        PushConstantIrradiance constants{};
                        constants.mvp = mvp;

                        pRDG->AddGraphicsPass(std::move(desc))
                            .RecordPassCommands([constants, count](RDGPassCmdEncoder& encoder) {
                                encoder.SetPushConstants(constants);
                                encoder.DrawIndexed(count, 1, 0, 0, 0);
                            });
                    }
                    else
                    {
                        PushConstantPrefilterEnv constants{};
                        constants.mvp       = mvp;
                        constants.roughness = static_cast<float>(mip) / std::max(1u, numMips - 1);

                        pRDG->AddGraphicsPass(std::move(desc))
                            .RecordPassCommands([constants, count](RDGPassCmdEncoder& encoder) {
                                encoder.SetPushConstants(constants);
                                encoder.DrawIndexed(count, 1, 0, 0, 0);
                            });
                    }

                    RHITextureCopyRegion region{};
                    region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
                    region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
                    region.dstSubresources.baseArrayLayer = face;
                    region.dstSubresources.mipmap         = mip;
                    region.size                           = {extent, extent, 1};

                    pRDG->AddTransferPass(NameID(fmt::format("{}_copy", tag.CStr())))
                        .CopyTexture(pOffscreen, pCubemap, MakeVecView(&region, 1));
                }
            }
        }

        {
            RHIGfxPipelineStates pso{};
            pso.primitiveType      = RHIDrawPrimitiveType::eTriangleList;
            pso.rasterizationState = {};
            pso.depthStencilState  = RHIGfxPipelineDepthStencilState::Create(
                false, false, RHIDepthCompareOperator::eLessOrEqual);
            pso.multiSampleState = {};
            pso.colorBlendState.AddAttachment();
            pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

            RDGGraphicsPassDesc lut{};
            lut.SetShaderProgramName("EnvMapBRDFLutGenSP");
            lut.AddColorOutput(pTexture->pLutBRDF);
            lut.SetPipelineStates(pso);
            lut.SetRenderArea(0, 0, pTexture->pLutBRDF->GetWidth(),
                              pTexture->pLutBRDF->GetHeight());
            lut.SetPassTag("GenlutBRDF");

            pRDG->AddGraphicsPass(std::move(lut))
                .RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Draw(3, 1); });
        }

        m_pRecordedPreprocessEnvTexture = pTexture;
        m_pPendingPreprocessEnvTexture  = nullptr;
    }

    RHIGfxPipelineStates pso{};

    pso.primitiveType               = RHIDrawPrimitiveType::eTriangleList;
    pso.rasterizationState          = {};
    pso.rasterizationState.cullMode = RHIPolygonCullMode::eFront;
    pso.depthStencilState =
        RHIGfxPipelineDepthStencilState::Create(true, false, RHIDepthCompareOperator::eLessOrEqual);
    pso.multiSampleState = {};
    pso.colorBlendState.AddAttachment();
    pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

    RDGGraphicsPassDesc desc{};
    desc.SetShaderProgramName("SkyboxRenderSP");
    desc.AddColorOutput(m_pViewport->GetColorBackBuffer());
    desc.AddDepthStencilOutput(m_pViewport->GetDepthStencilBackBuffer(),
                               RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore);
    desc.SetPipelineStates(pso);
    desc.SetRenderArea(0, 0, m_pViewport->GetWidth(), m_pViewport->GetHeight());
    desc.SetPassTag("SkyboxDraw");

    desc.BindVertexBuffer(m_pVertexBuffer);
    desc.BindIndexBuffer(m_pIndexBuffer);

    const EnvTexture& env = m_pScene->GetEnvTexture();
    desc.BindSampledTexture("samplerEnv", env.pPrefilteredSampler, env.pSkybox->GetDefaultView());
    desc.BindValue("uCameraData", m_pScene->GetCameraUniformData(), sizeof(sg::CameraUniformData));
    desc.BindValue("uSceneData", m_pScene->GetSceneUniformData(), sizeof(SceneUniformData));

    pRDG->AddGraphicsPass(std::move(desc))
        .RecordPassCommands(
            [count = static_cast<uint32_t>(cSkyboxIndices.size())](RDGPassCmdEncoder& encoder) {
                encoder.DrawIndexed(count, 1, 0, 0, 0);
            });
}

void SkyboxRenderer::OnRenderGraphExecuted(bool succeeded)
{
    if (!succeeded && m_pPendingPreprocessEnvTexture == nullptr)
    {
        m_pPendingPreprocessEnvTexture = m_pRecordedPreprocessEnvTexture;
    }

    m_pRecordedPreprocessEnvTexture = nullptr;
}

void SkyboxRenderer::PreprocessEnvTexture(EnvTexture* pTexture)
{
    VERIFY_EXPR(pTexture != nullptr);
    PrepareEnvCubemaps(pTexture);
    PrepareLutBRDF(pTexture);
    m_pPendingPreprocessEnvTexture = pTexture;
}

void SkyboxRenderer::PrepareEnvCubemaps(EnvTexture* pTexture)
{
    for (uint32_t target = 0; target <= PREFILTERED_MAP; ++target)
    {
        const bool irradiance  = target == IRRADIANCE;
        const uint32_t dim     = irradiance ? IRRADIANCE_DIM : PREFILTERED_DIM;
        const uint32_t numMips = RHITexture::CalculateTextureMipLevels(dim);

        RHISamplerCreateInfo samplerInfo{};
        samplerInfo.minFilter     = RHISamplerFilter::eLinear;
        samplerInfo.magFilter     = RHISamplerFilter::eLinear;
        samplerInfo.mipFilter     = RHISamplerFilter::eLinear;
        samplerInfo.repeatU       = RHISamplerRepeatMode::eClampToEdge;
        samplerInfo.repeatV       = RHISamplerRepeatMode::eClampToEdge;
        samplerInfo.repeatW       = RHISamplerRepeatMode::eClampToEdge;
        samplerInfo.maxLod        = static_cast<float>(numMips);
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.borderColor   = RHISamplerBorderColor::eFloatOpaqueBlack;

        RHISampler* pSampler = m_pRenderDevice->CreateSampler(samplerInfo);

        TextureFormat format{};
        format.dimension   = TextureDimension::eCube;
        format.format      = irradiance ? cIrradianceFormat : cPrefilteredFormat;
        format.width       = dim;
        format.height      = dim;
        format.arrayLayers = 6;
        format.mipmaps     = numMips;

        RHITexture* pCubemap = m_pRenderDevice->CreateTextureSampled(
            format, {.copyUsage = true}, irradiance ? "env_irradiance" : "env_prefiltered");

        if (irradiance)
        {
            pTexture->pIrradiance        = pCubemap;
            pTexture->pIrradianceSampler = pSampler;
        }
        else
        {
            pTexture->pPrefiltered        = pCubemap;
            pTexture->pPrefilteredSampler = pSampler;
        }
    }
}

void SkyboxRenderer::PrepareLutBRDF(EnvTexture* pTexture)
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

    pTexture->pLutBRDFSampler = m_pRenderDevice->CreateSampler(samplerInfo);
}

} // namespace zen::rc
