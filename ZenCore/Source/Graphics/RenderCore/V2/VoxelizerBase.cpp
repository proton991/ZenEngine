#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"

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
} // namespace zen::rc