#pragma once
#include "RHIResource.h"
#include "RHIDefs.h"

namespace zen
{
enum class GraphicsAPIType
{
    eVulkan,
    Count
};

enum class CommandBufferLevel
{
    PRIMARY   = 0,
    SECONDARY = 1
};

class DynamicRHI
{
public:
    virtual ~DynamicRHI() = default;

    virtual void Init() = 0;

    virtual void Destroy() = 0;

    virtual GraphicsAPIType GetAPIType() = 0;

    virtual const char* GetName() = 0;

    virtual SwapchainHandle CreateSwapchain(SurfaceHandle surfaceHandle, bool enableVSync) = 0;

    virtual Status ResizeSwapchain(SwapchainHandle swapchainHandle) = 0;

    virtual void DestroySwapchain(SwapchainHandle swapchainHandle) = 0;

    virtual CommandPoolHandle CreateCommandPool(uint32_t queueFamilyIndex) = 0;

    virtual void ResetCommandPool(CommandPoolHandle commandPoolHandle) = 0;

    virtual void DestroyCommandPool(CommandPoolHandle commandPoolHandle) = 0;

    virtual CommandBufferHandle GetOrCreateCommandBuffer(CommandPoolHandle  cmdPoolHandle,
                                                         CommandBufferLevel level) = 0;

    virtual RHISamplerPtr CreateSampler(const RHISamplerSpec& samplerSpec) = 0;
};
} // namespace zen