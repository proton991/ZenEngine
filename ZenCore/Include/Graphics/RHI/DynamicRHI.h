#pragma once
#include "RHICommon.h"
#include "RHIResource.h"
#include "RHIDefs.h"

namespace zen::rhi
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

    virtual CommandBufferHandle GetOrCreateCommandBuffer(CommandPoolHandle cmdPoolHandle,
                                                         CommandBufferLevel level) = 0;

    virtual ShaderHandle CreateShader(const ShaderGroupInfo& shaderGroupInfo) = 0;

    virtual void DestroyShader(ShaderHandle shaderHandle) = 0;

    virtual PipelineHandle CreateGfxPipeline(ShaderHandle shaderHandle,
                                             const GfxPipelineStates& states,
                                             RenderPassHandle renderPassHandle,
                                             uint32_t subpass) = 0;

    virtual void DestroyPipeline(PipelineHandle pipelineHandle) = 0;

    virtual RenderPassHandle CreateRenderPass(const RenderPassLayout& renderPassLayout) = 0;

    virtual void DestroyRenderPass(RenderPassHandle renderPassHandle) = 0;

    virtual FramebufferHandle CreateFramebuffer(RenderPassHandle renderPassHandle,
                                                const RenderTargetInfo& RTInfo) = 0;

    virtual void DestroyFramebuffer(FramebufferHandle framebufferHandle) = 0;

    virtual TextureHandle CreateTexture(const TextureInfo& textureInfo) = 0;

    virtual void DestroyTexture(TextureHandle textureHandle) = 0;
};
} // namespace zen::rhi