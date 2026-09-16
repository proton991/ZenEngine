#pragma once
#include "Graphics/VulkanRHI/VulkanRHI.h"

namespace zen::test
{
// Set zero is reserved for the global heap in every Vulkan pipeline layout.
inline constexpr uint32_t kLocalResourceSet = kGlobalBindlessHeapIndex + 1;

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
