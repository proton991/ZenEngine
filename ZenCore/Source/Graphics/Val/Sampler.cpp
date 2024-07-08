#include "Graphics/Val/Sampler.h"
#include "Common/Errors.h"

namespace zen::val
{
Sampler::Sampler(const Device& device, VkFilter filter, VkSamplerAddressMode addressMode) :
    DeviceObject(device)
{
    VkSamplerCreateInfo samplerCI{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerCI.magFilter     = filter;
    samplerCI.minFilter     = filter;
    samplerCI.addressModeU  = addressMode;
    samplerCI.addressModeV  = addressMode;
    samplerCI.addressModeW  = addressMode;
    samplerCI.mipLodBias    = 0.0f;
    samplerCI.maxAnisotropy = 1.0f;
    samplerCI.minLod        = 0.0f;
    samplerCI.maxLod        = 1.0f;
    samplerCI.borderColor   = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

    CHECK_VK_ERROR_AND_THROW(vkCreateSampler(m_device.GetHandle(), &samplerCI, nullptr, &m_handle),
                             "Failed to create sampler");
}

Sampler::Sampler(Sampler&& other) noexcept : DeviceObject(std::move(other)) {}

Sampler::~Sampler()
{
    if (m_handle != VK_NULL_HANDLE)
    {
        vkDestroySampler(m_device.GetHandle(), m_handle, nullptr);
    }
}
} // namespace zen::val