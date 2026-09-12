#pragma once
#include "Graphics/VulkanRHI/VulkanRHI.h"

namespace zen::test
{
// Used only by the explicitly invoked VulkanRHIIntegrationTest executable.
// Objects are initialized through the same public entry points as the renderer.
class VulkanSession
{
public:
    VulkanSession()
    {
        GDynamicRHI = &rhi;
        rhi.Init();
    }

    ~VulkanSession()
    {
        rhi.Destroy();
        GDynamicRHI = nullptr;
        GVulkanRHI  = nullptr;
    }

    VulkanRHI rhi;
};

} // namespace zen::test
