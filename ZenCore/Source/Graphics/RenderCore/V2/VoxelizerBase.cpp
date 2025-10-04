#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/RenderScene.h"

namespace zen::rc
{
VoxelizerBase::VoxelizerBase(RenderDevice* renderDevice, rhi::RHIViewport* viewport) :
    m_renderDevice(renderDevice), m_viewport(viewport)
{}

void VoxelizerBase::PrepareTextures()
{
    using namespace zen::rhi;
    {
        rhi::SamplerInfo samplerInfo{};
        samplerInfo.magFilter = rhi::SamplerFilter::eLinear;
        samplerInfo.magFilter = rhi::SamplerFilter::eLinear;
        samplerInfo.mipFilter = rhi::SamplerFilter::eLinear;

        m_voxelSampler = m_renderDevice->CreateSampler(samplerInfo);
    }

    // offscreen depth texture sampler
    {
        rhi::SamplerInfo samplerInfo{};
        samplerInfo.borderColor = rhi::SamplerBorderColor::eFloatOpaqueWhite;
        samplerInfo.minFilter   = rhi::SamplerFilter::eLinear;
        samplerInfo.magFilter   = rhi::SamplerFilter::eLinear;
        samplerInfo.mipFilter   = rhi::SamplerFilter::eLinear;
        samplerInfo.repeatU     = rhi::SamplerRepeatMode::eRepeat;
        samplerInfo.repeatV     = rhi::SamplerRepeatMode::eRepeat;
        samplerInfo.repeatW     = rhi::SamplerRepeatMode::eRepeat;
        samplerInfo.borderColor = rhi::SamplerBorderColor::eFloatOpaqueWhite;

        m_colorSampler = m_renderDevice->CreateSampler(samplerInfo);
    }
    TextureUsageHint usageHint{.copyUsage = false};
    {
        // INIT_TEXTURE_INFO(texInfo, rhi::TextureType::e3D, DataFormat::eR8UNORM,
        //                   m_voxelTexResolution, m_voxelTexResolution, m_voxelTexResolution, 1, 1,
        //                   SampleCount::e1, "voxel_static_flag", TextureUsageFlagBits::eStorage,
        //                   TextureUsageFlagBits::eSampled);
        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::e3D;
        texFormat.format      = DataFormat::eR8UNORM;
        texFormat.width       = m_voxelTexResolution;
        texFormat.height      = m_voxelTexResolution;
        texFormat.depth       = m_voxelTexResolution;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        m_voxelTextures.staticFlag =
            m_renderDevice->CreateTextureStorage(texFormat, usageHint, "voxel_static_flag");
    }
    {
        // INIT_TEXTURE_INFO(texInfo, rhi::TextureType::e3D, m_voxelTexFormat, m_voxelTexResolution,
        //                   m_voxelTexResolution, m_voxelTexResolution, 1, 1, SampleCount::e1,
        //                   "voxel_albedo", TextureUsageFlagBits::eStorage,
        //                   TextureUsageFlagBits::eSampled);
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

        m_voxelTextures.albedo =
            m_renderDevice->CreateTextureStorage(texFormat, usageHint, "voxel_albedo");
    }
    {
        // rhi::TextureProxyInfo textureProxyInfo{};
        // textureProxyInfo.type        = rhi::TextureType::e3D;
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

        m_voxelTextures.albedoProxy = m_renderDevice->CreateTextureProxy(
            m_voxelTextures.albedo, proxyFormat, "voxel_albedo_proxy");
    }
    {
        // INIT_TEXTURE_INFO(texInfo, rhi::TextureType::e3D, m_voxelTexFormat, m_voxelTexResolution,
        //                   m_voxelTexResolution, m_voxelTexResolution, 1, 1, SampleCount::e1,
        //                   "voxel_normal", TextureUsageFlagBits::eStorage,
        //                   TextureUsageFlagBits::eSampled);
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

        m_voxelTextures.normal =
            m_renderDevice->CreateTextureStorage(texFormat, usageHint, "voxel_normal");
    }
    {
        // rhi::TextureProxyInfo textureProxyInfo{};
        // textureProxyInfo.type        = rhi::TextureType::e3D;
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

        m_voxelTextures.normalProxy = m_renderDevice->CreateTextureProxy(
            m_voxelTextures.normal, proxyFormat, "voxel_normal_proxy");
    }
    {
        // INIT_TEXTURE_INFO(texInfo, rhi::TextureType::e3D, m_voxelTexFormat, m_voxelTexResolution,
        //                   m_voxelTexResolution, m_voxelTexResolution, 1, 1, SampleCount::e1,
        //                   "voxel_emissive", TextureUsageFlagBits::eStorage,
        //                   TextureUsageFlagBits::eSampled);
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

        m_voxelTextures.emissive =
            m_renderDevice->CreateTextureStorage(texFormat, usageHint, "voxel_emissive");
    }
    {
        TextureProxyFormat proxyFormat{};
        proxyFormat.format      = DataFormat::eR8G8B8A8UNORM;
        proxyFormat.dimension   = TextureDimension::e3D;
        proxyFormat.arrayLayers = 1;
        proxyFormat.mipmaps     = 1;

        m_voxelTextures.emissiveProxy = m_renderDevice->CreateTextureProxy(
            m_voxelTextures.normal, proxyFormat, "voxel_emissive_proxy");

        // rhi::TextureProxyInfo textureProxyInfo{};
        // textureProxyInfo.type        = rhi::TextureType::e3D;
        // textureProxyInfo.arrayLayers = 1;
        // textureProxyInfo.mipmaps     = 1;
        // textureProxyInfo.format      = DataFormat::eR8G8B8A8UNORM;
        // textureProxyInfo.name        = "voxel_emissive_proxy";
        // m_voxelTextures.emissiveProxy =
        //     m_renderDevice->CreateTextureProxy(m_voxelTextures.emissive, textureProxyInfo);
    }
}

void VoxelizerBase::SetRenderScene(RenderScene* scene)
{
    m_scene       = scene;
    m_sceneExtent = m_scene->GetAABB().GetMaxExtent();
    m_voxelSize   = m_sceneExtent / static_cast<float>(m_voxelTexResolution);
    m_voxelScale  = 1.0f / m_sceneExtent;

    UpdateUniformData();
    UpdatePassResources();
}

Vec3 VoxelizerBase::GetSceneMinPoint() const
{
    return m_scene->GetAABB().GetMin();
}

void VoxelizerBase::Destroy()
{
    m_renderDevice->DestroyTexture(m_voxelTextures.albedoProxy);
    m_renderDevice->DestroyTexture(m_voxelTextures.normalProxy);
    m_renderDevice->DestroyTexture(m_voxelTextures.emissiveProxy);

    m_renderDevice->DestroyTexture(m_voxelTextures.staticFlag);
    m_renderDevice->DestroyTexture(m_voxelTextures.albedo);
    m_renderDevice->DestroyTexture(m_voxelTextures.normal);
    m_renderDevice->DestroyTexture(m_voxelTextures.emissive);
}
} // namespace zen::rc