#pragma once
#include "DeviceObject.h"

namespace zen::val
{
class Sampler : public DeviceObject<VkSampler, VK_OBJECT_TYPE_SAMPLER>
{
public:
    explicit Sampler(const Device& device,
                     VkFilter filter                  = VK_FILTER_LINEAR,
                     VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT);

    Sampler(Sampler&& other) noexcept;

    ~Sampler();
};
} // namespace zen::val