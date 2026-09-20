#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/RenderScene.h"

namespace zen::rc
{
VoxelizerBase::VoxelizerBase(RenderDevice* pRenderDevice, RHIViewport* pViewport) :
    m_pRenderDevice(pRenderDevice), m_pViewport(pViewport)
{}

bool VoxelizerBase::BeginVoxelization(RenderGraph& graph, RDGQueuePreference queuePreference)
{
    bool result{};

    if (m_needVoxelization)
    {
        // Zero also clears the packed UINT geometry accumulators. Color() has alpha 1, which
        // would incorrectly mark empty albedo voxels occupied, so clear all four channels explicitly.
        RDGTransferPassCmdRecorder reset = graph.AddTransferPass("ResetVoxelVolumes");
        // The scheduler validates clear support using the supplied queue capabilities.
        reset.SetQueuePreference(queuePreference);
        reset.ClearTexture(m_voxelTextures.pAlbedo, Color(0.0f));

        if (m_voxelTextures.pNormal != nullptr)
        {
            reset.ClearTexture(m_voxelTextures.pNormal, Color(0.0f));
        }

        if (m_voxelTextures.pEmissive != nullptr)
        {
            reset.ClearTexture(m_voxelTextures.pEmissive, Color(0.0f));
        }

        m_needVoxelization = false;
        result             = true;
    }

    return result;
}

void VoxelizerBase::PrepareTextures()
{
    {
        RHISamplerCreateInfo samplerInfo{};
        samplerInfo.magFilter = RHISamplerFilter::eLinear;
        samplerInfo.magFilter = RHISamplerFilter::eLinear;
        samplerInfo.mipFilter = RHISamplerFilter::eLinear;

        m_pVoxelSampler = m_pRenderDevice->CreateSampler(samplerInfo);
    }

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

    TextureUsageHint usageHint{.copyUsage = true};

    {
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
        TextureViewFormat proxyFormat{};
        proxyFormat.format      = DataFormat::eR8G8B8A8UNORM;
        proxyFormat.dimension   = TextureDimension::e3D;
        proxyFormat.arrayLayers = 1;
        proxyFormat.mipmaps     = 1;

        m_voxelTextures.pAlbedoView = m_pRenderDevice->CreateTextureView(
            m_voxelTextures.pAlbedo, proxyFormat, "voxel_albedo_proxy");
    }

    if (!ProducesRadianceInputs())
    {
        return;
    }

    {
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
        TextureViewFormat proxyFormat{};
        proxyFormat.format      = DataFormat::eR8G8B8A8UNORM;
        proxyFormat.dimension   = TextureDimension::e3D;
        proxyFormat.arrayLayers = 1;
        proxyFormat.mipmaps     = 1;

        m_voxelTextures.pNormalView = m_pRenderDevice->CreateTextureView(
            m_voxelTextures.pNormal, proxyFormat, "voxel_normal_proxy");
    }

    {
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
        TextureViewFormat proxyFormat{};
        proxyFormat.format      = DataFormat::eR8G8B8A8UNORM;
        proxyFormat.dimension   = TextureDimension::e3D;
        proxyFormat.arrayLayers = 1;
        proxyFormat.mipmaps     = 1;

        m_voxelTextures.pEmissiveView = m_pRenderDevice->CreateTextureView(
            m_voxelTextures.pEmissive, proxyFormat, "voxel_emissive_proxy");
    }
}

void VoxelizerBase::SetRenderScene(RenderScene* pScene)
{
    RequestVoxelization();
    m_pScene = pScene;
}

float VoxelizerBase::GetVoxelSize() const
{
    return m_pScene->GetAABB().GetMaxExtent() / m_voxelTexResolution;
}

float VoxelizerBase::GetVoxelScale() const
{
    return 1.0f / m_pScene->GetAABB().GetMaxExtent();
}

Vec3 VoxelizerBase::GetSceneMinPoint() const
{
    return m_pScene->GetAABB().GetMin();
}

void VoxelizerBase::Destroy()
{
    m_pRenderDevice->DestroyTexture(m_voxelTextures.pAlbedo);
    m_pRenderDevice->DestroyTexture(m_voxelTextures.pNormal);
    m_pRenderDevice->DestroyTexture(m_voxelTextures.pEmissive);
}
} // namespace zen::rc
