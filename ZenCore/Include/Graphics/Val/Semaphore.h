#pragma once
#include <vulkan/vulkan.h>
#include "DeviceObject.h"

namespace zen::val
{
class Semaphore : public DeviceObject<VkSemaphore, VK_OBJECT_TYPE_SEMAPHORE>
{
public:
    Semaphore(Device& device);
    ~Semaphore();
};
} // namespace zen::val