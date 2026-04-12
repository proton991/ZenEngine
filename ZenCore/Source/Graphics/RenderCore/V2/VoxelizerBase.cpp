#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/RenderScene.h"

namespace zen::rc
{
VoxelizerBase::VoxelizerBase(RenderDevice* pRenderDevice, RHIViewport* pViewport) :
    m_pRenderDevice(pRenderDevice), m_pViewport(pViewport)
{}

void VoxelizerBase::PrepareTextures()
{

    {
        RHISamplerCreateInfo samplerInfo{};
        samplerInfo.magFilter = RHISamplerFilter::eLinear;
        samplerInfo.magFilter = RHISamplerFilter::eLinear;
        samplerInfo.mipFilter = RHISamplerFilter::eLinear;

        m_pVoxelSampler = m_pRenderDevice->CreateSampler(samplerInfo);
    }

    // offscreen depth texture sampler
    {
        RHISamplerCreateInfo samplerInfo{};
        samplerInfo.borderColor = RHISamplerBorderColor::eFloatOpaqueWhite;
        samplerInfo.minFilter   = RHISamplerFilter::eLinear;
        samplerInfo.magFilter   = RHISamplerFilter::eLinear;
        samplerInfo.mipFilter   = RHISamplerFilter::eLinear;
        samplerInfo.repeatU     = RHISamplerRepeatMode::eRepeat;
        samplerInfo.repeatV     = RHISamplerRepeatMode::eRepeat;
        samplerInfo.repeatW     = RHISamplerRepeatMode::eRepeat;
        samplerInfo.borderColor = RHISamplerBorderColor::eFloatOpaqueWhite;

        m_pColorSampler = m_pRenderDevice->CreateSampler(samplerInfo);
    }
    TextureUsageHint usageHint{.copyUsage = false};
    {
        // INIT_TEXTURE_INFO(texInfo, RHITextureType::e3D, DataFormat::eR8UNORM,
        //                   m_voxelTexResolution, m_voxelTexResolution, m_voxelTexResolution, 1, 1,
        //                   SampleCount::e1, "voxel_static_flag", RHITextureUsageFlagBits::eStorage,
        //                   RHITextureUsageFlagBits::eSampled);
        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::e3D;
        texFormat.format      = DataFormat::eR8UNORM;
        texFormat.width       = m_voxelTexResolution;
        texFormat.height      = m_voxelTexResolution;
        texFormat.depth       = m_voxelTexResolution;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        m_voxelTextures.pStaticFlag =
            m_pRenderDevice->CreateTextureStorage(texFormat, usageHint, "voxel_static_flag");
    }
    {
        // INIT_TEXTURE_INFO(texInfo, RHITextureType::e3D, m_voxelTexFormat, m_voxelTexResolution,
        //                   m_voxelTexResolution, m_voxelTexResolution, 1, 1, SampleCount::e1,
        //                   "voxel_albedo", RHITextureUsageFlagBits::eStorage,
        //                   RHITextureUsageFlagBits::eSampled);
        // texInfo.mutableFormat  = true;
        // m_voxelTextures.albedo = m_renderDevice->CreateTexture(texInfo);

        TextureFormat texFormat{};
        texFormat.dimension     = TextureDimension::e3D;
        texFormat.format        = m_voxelTexFormat;
        texFormat.width         = m_voxelTexResolution;
        texFormat.height        = m_voxelTexResolution;
        texFormat.depth         = m_voxelTexResolution;
        texFormat.arrayLayers   = 1;
        texFormat.mipmaps       = 1;
        texFormat.mutableFormat = true;

        m_voxelTextures.pAlbedo =
            m_pRenderDevice->CreateTextureStorage(texFormat, usageHint, "voxel_albedo");
    }
    {
        // TextureProxyInfo textureProxyInfo{};
        // textureProxyInfo.type        = RHITextureType::e3D;
        // textureProxyInfo.arrayLayers = 1;
        // textureProxyInfo.mipmaps     = 1;
        // textureProxyInfo.format      = DataFormat::eR8G8B8A8UNORM;
        // textureProxyInfo.name        = "voxel_albedo_proxy";
        // m_voxelTextures.albedoProxy =
        //     m_renderDevice->CreateTextureProxy(m_voxelTextures.albedo, textureProxyInfo);

        TextureProxyFormat proxyFormat{};
        proxyFormat.format      = DataFormat::eR8G8B8A8UNORM;
        proxyFormat.dimension   = TextureDimension::e3D;
        proxyFormat.arrayLayers = 1;
        proxyFormat.mipmaps     = 1;

        m_voxelTextures.pAlbedoProxy = m_pRenderDevice->CreateTextureProxy(
            m_voxelTextures.pAlbedo, proxyFormat, "voxel_albedo_proxy");
    }
    {
        // INIT_TEXTURE_INFO(texInfo, RHITextureType::e3D, m_voxelTexFormat, m_voxelTexResolution,
        //                   m_voxelTexResolution, m_voxelTexResolution, 1, 1, SampleCount::e1,
        //                   "voxel_normal", RHITextureUsageFlagBits::eStorage,
        //                   RHITextureUsageFlagBits::eSampled);
        // texInfo.mutableFormat  = true;
        // m_voxelTextures.normal = m_renderDevice->CreateTexture(texInfo);

        TextureFormat texFormat{};
        texFormat.dimension     = TextureDimension::e3D;
        texFormat.format        = m_voxelTexFormat;
        texFormat.width         = m_voxelTexResolution;
        texFormat.height        = m_voxelTexResolution;
        texFormat.depth         = m_voxelTexResolution;
        texFormat.arrayLayers   = 1;
        texFormat.mipmaps       = 1;
        texFormat.mutableFormat = true;

        m_voxelTextures.pNormal =
            m_pRenderDevice->CreateTextureStorage(texFormat, usageHint, "voxel_normal");
    }
    {
        // TextureProxyInfo textureProxyInfo{};
        // textureProxyInfo.type        = RHITextureType::e3D;
        // textureProxyInfo.arrayLayers = 1;
        // textureProxyInfo.mipmaps     = 1;
        // textureProxyInfo.format      = DataFormat::eR8G8B8A8UNORM;
        // textureProxyInfo.name        = "voxel_normal_proxy";
        // m_voxelTextures.normalProxy =
        //     m_renderDevice->CreateTextureProxy(m_voxelTextures.normal, textureProxyInfo);
        TextureProxyFormat proxyFormat{};
        proxyFormat.format      = DataFormat::eR8G8B8A8UNORM;
        proxyFormat.dimension   = TextureDimension::e3D;
        proxyFormat.arrayLayers = 1;
        proxyFormat.mipmaps     = 1;

        m_voxelTextures.pNormalProxy = m_pRenderDevice->CreateTextureProxy(
            m_voxelTextures.pNormal, proxyFormat, "voxel_normal_proxy");
    }
    {
        // INIT_TEXTURE_INFO(texInfo, RHITextureType::e3D, m_voxelTexFormat, m_voxelTexResolution,
        //                   m_voxelTexResolution, m_voxelTexResolution, 1, 1, SampleCount::e1,
        //                   "voxel_emissive", RHITextureUsageFlagBits::eStorage,
        //                   RHITextureUsageFlagBits::eSampled);
        // texInfo.mutableFormat    = true;
        // m_voxelTextures.emissive = m_renderDevice->CreateTexture(texInfo);
        TextureFormat texFormat{};
        texFormat.dimension     = TextureDimension::e3D;
        texFormat.format        = m_voxelTexFormat;
        texFormat.width         = m_voxelTexResolution;
        texFormat.height        = m_voxelTexResolution;
        texFormat.depth         = m_voxelTexResolution;
        texFormat.arrayLayers   = 1;
        texFormat.mipmaps       = 1;
        texFormat.mutableFormat = true;

        m_voxelTextures.pEmissive =
            m_pRenderDevice->CreateTextureStorage(texFormat, usageHint, "voxel_emissive");
    }
    {
        TextureProxyFormat proxyFormat{};
        proxyFormat.format      = DataFormat::eR8G8B8A8UNORM;
        proxyFormat.dimension   = TextureDimension::e3D;
        proxyFormat.arrayLayers = 1;
        proxyFormat.mipmaps     = 1;

        m_voxelTextures.pEmissiveProxy = m_pRenderDevice->CreateTextureProxy(
            m_voxelTextures.pNormal, proxyFormat, "voxel_emissive_proxy");

        // TextureProxyInfo textureProxyInfo{};
        // textureProxyInfo.type        = RHITextureType::e3D;
        // textureProxyInfo.arrayLayers = 1;
        // textureProxyInfo.mipmaps     = 1;
        // textureProxyInfo.format      = DataFormat::eR8G8B8A8UNORM;
        // textureProxyInfo.name        = "voxel_emissive_proxy";
        // m_voxelTextures.emissiveProxy =
        //     m_renderDevice->CreateTextureProxy(m_voxelTextures.emissive, textureProxyInfo);
    }
}

void VoxelizerBase::SetRenderScene(RenderScene* pScene)
{
    m_pScene       = pScene;
    m_sceneExtent = m_pScene->GetAABB().GetMaxExtent();
    m_voxelSize   = m_sceneExtent / static_cast<float>(m_voxelTexResolution);
    m_voxelScale  = 1.0f / m_sceneExtent;

    UpdateUniformData();
    UpdatePassResources();
}

Vec3 VoxelizerBase::GetSceneMinPoint() const
{
    return m_pScene->GetAABB().GetMin();
}

void VoxelizerBase::Destroy()
{
    m_pRenderDevice->DestroyTexture(m_voxelTextures.pAlbedoProxy);
    m_pRenderDevice->DestroyTexture(m_voxelTextures.pNormalProxy);
    m_pRenderDevice->DestroyTexture(m_voxelTextures.pEmissiveProxy);

    m_pRenderDevice->DestroyTexture(m_voxelTextures.pStaticFlag);
    m_pRenderDevice->DestroyTexture(m_voxelTextures.pAlbedo);
    m_pRenderDevice->DestroyTexture(m_voxelTextures.pNormal);
    m_pRenderDevice->DestroyTexture(m_voxelTextures.pEmissive);
}
} // namespace zen::rc