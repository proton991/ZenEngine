#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/RenderScene.h"

namespace zen::rc
{
void VoxelizerBase::PrepareTextures()
{
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
}

void VoxelizerBase::SetRenderScene(RenderScene* scene)
{
    m_scene       = scene;
    m_sceneExtent = m_scene->GetAABB().GetMaxExtent();
    m_voxelSize   = m_sceneExtent / static_cast<float>(m_voxelTexResolution);

    UpdateUniformData();
    UpdatePassResources();
}
} // namespace zen::rc