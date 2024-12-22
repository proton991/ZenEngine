#pragma once
#include "RHICommon.h"
#include "RHIResource.h"
#include "RHIDefs.h"

namespace zen::rhi
{
class RHICommandList;
class RHICommandListContext;

enum class GraphicsAPIType
{
    eVulkan,
    Count
};

class DynamicRHI
{
public:
    virtual ~DynamicRHI() = default;

    virtual void Init() = 0;

    virtual void Destroy() = 0;

    virtual RHICommandListContext* CreateCmdListContext() = 0;

    virtual void WaitForCommandList(RHICommandList* cmdList) = 0;

    virtual GraphicsAPIType GetAPIType() = 0;

    virtual const char* GetName() = 0;

    virtual DataFormat GetSupportedDepthFormat() = 0;

    virtual RHIViewport* CreateViewport(void* windowPtr,
                                        uint32_t width,
                                        uint32_t height,
                                        bool enableVSync) = 0;

    virtual void DestroyViewport(RHIViewport* viewport) = 0;

    virtual void BeginDrawingViewport(RHIViewport* viewport) = 0;

    virtual void EndDrawingViewport(RHIViewport* viewport,
                                    RHICommandListContext* cmdListContext,
                                    bool present) = 0;

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
                                                const FramebufferInfo& fbInfo) = 0;

    virtual void DestroyFramebuffer(FramebufferHandle framebufferHandle) = 0;

    virtual SamplerHandle CreateSampler(const SamplerInfo& samplerInfo) = 0;

    virtual void DestroySampler(SamplerHandle samplerHandle) = 0;

    virtual TextureHandle CreateTexture(const TextureInfo& textureInfo) = 0;

    virtual void DestroyTexture(TextureHandle textureHandle) = 0;

    virtual DataFormat GetTextureFormat(TextureHandle textureHandle) = 0;

    virtual TextureSubResourceRange GetTextureSubResourceRange(TextureHandle textureHandle) = 0;

    virtual BufferHandle CreateBuffer(uint32_t size,
                                      BitField<BufferUsageFlagBits> usageFlags,
                                      BufferAllocateType allocateType) = 0;

    virtual uint8_t* MapBuffer(BufferHandle bufferHandle) = 0;

    virtual void UnmapBuffer(BufferHandle bufferHandle) = 0;

    virtual void DestroyBuffer(BufferHandle bufferHandle) = 0;

    virtual void SetBufferTexelFormat(BufferHandle bufferHandle, DataFormat format) = 0;

    virtual DescriptorSetHandle CreateDescriptorSet(ShaderHandle shaderHandle,
                                                    uint32_t setIndex) = 0;

    virtual void DestroyDescriptorSet(DescriptorSetHandle descriptorSetHandle) = 0;

    virtual void UpdateDescriptorSet(
        DescriptorSetHandle descriptorSetHandle,
        const std::vector<ShaderResourceBinding>& resourceBindings) = 0;

    virtual void ChangeTextureLayout(RHICommandList* cmdList,
                                     TextureHandle textureHandle,
                                     TextureLayout oldLayout,
                                     TextureLayout newLayout) = 0;

    virtual void SubmitAllGPUCommands() = 0;

    virtual void WaitDeviceIdle() = 0;

    virtual size_t GetUniformBufferAlignment() = 0;

    virtual size_t GetStorageBufferAlignment() = 0;

    // todo: move to debug util
    virtual void SetRenderPassDebugName(RenderPassHandle renderPassHandle,
                                        const std::string& debugName) = 0;
};
} // namespace zen::rhi