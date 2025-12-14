#pragma once
#include "RHICommon.h"
#include "RHIResource.h"
#include "RHIDefs.h"

namespace zen
{
class RHICommandList;
class RHICommandListContext;

class DynamicRHI
{
public:
    static DynamicRHI* Create(RHIAPIType type);

    virtual ~DynamicRHI() = default;

    virtual void Init() = 0;

    virtual void Destroy() = 0;

    virtual RHICommandListContext* CreateCmdListContext() = 0;

    virtual void WaitForCommandList(RHICommandList* cmdList) = 0;

    virtual RHICommandList* GetImmediateCommandList() = 0;

    virtual RHIAPIType GetAPIType() = 0;

    virtual const char* GetName() = 0;

    virtual DataFormat GetSupportedDepthFormat() = 0;

    virtual RHIViewport* CreateViewport(void* pWindow,
                                        uint32_t width,
                                        uint32_t height,
                                        bool enableVSync) = 0;

    virtual void DestroyViewport(RHIViewport* viewport) = 0;

    virtual void BeginDrawingViewport(RHIViewport* viewport) = 0;

    virtual void EndDrawingViewport(RHIViewport* viewport,
                                    RHICommandListContext* cmdListContext,
                                    bool present) = 0;

    // virtual ShaderHandle CreateShader(const RHIShaderGroupInfo& shaderGroupInfo) = 0;

    virtual RHIShader* CreateShader(const RHIShaderCreateInfo& createInfo) = 0;

    // virtual void DestroyShader(ShaderHandle shaderHandle) = 0;

    virtual void DestroyShader(RHIShader* shader) = 0;

    virtual RHIPipeline* CreatePipeline(const RHIComputePipelineCreateInfo& createInfo) = 0;

    virtual RHIPipeline* CreatePipeline(const RHIGfxPipelineCreateInfo& createInfo) = 0;

    // virtual RHIPipeline* CreateGfxPipeline(RHIShader* shaderHandle,
    //                                        const RHIGfxPipelineStates& states,
    //                                        RenderPassHandle renderPassHandle,
    //                                        uint32_t subpass) = 0;

    // virtual RHIPipeline* CreateGfxPipeline(RHIShader* shaderHandle,
    //                                        const RHIGfxPipelineStates& states,
    //                                        const RHIRenderPassLayout& renderPassLayout,
    //                                        uint32_t subpass) = 0;

    // virtual RHIPipeline* CreateComputePipeline(RHIShader* shaderHandle) = 0;

    virtual void DestroyPipeline(RHIPipeline* pipeline) = 0;

    virtual RenderPassHandle CreateRenderPass(const RHIRenderPassLayout& renderPassLayout) = 0;

    virtual void DestroyRenderPass(RenderPassHandle renderPassHandle) = 0;

    virtual FramebufferHandle CreateFramebuffer(RenderPassHandle renderPassHandle,
                                                const RHIFramebufferInfo& fbInfo) = 0;

    virtual void DestroyFramebuffer(FramebufferHandle framebufferHandle) = 0;

    virtual RHISampler* CreateSampler(const RHISamplerCreateInfo& createInfo) = 0;
    // {
    //     return m_resourceFactory->CreateSampler(samplerInfo);
    // }

    virtual void DestroySampler(RHISampler* sampler) = 0;

    virtual RHITexture* CreateTexture(const RHITextureCreateInfo& createInfo) = 0;
    // {
    //     return m_resourceFactory->CreateTexture(textureInfo);
    // }
    //
    // virtual TextureHandle CreateTextureProxy(const TextureHandle& baseTexture,
    //                                          const TextureProxyInfo& textureProxyInfo) = 0;

    // virtual void DestroyTexture(TextureHandle textureHandle) = 0;

    virtual void DestroyTexture(RHITexture* texture) = 0;

    // virtual DataFormat GetTextureFormat(TextureHandle textureHandle) = 0;

    // virtual RHITextureSubResourceRange GetTextureSubResourceRange(TextureHandle textureHandle) = 0;

    // virtual BufferHandle CreateBuffer(uint32_t size,
    //                                   BitField<RHIBufferUsageFlagBits> usageFlags,
    //                                   RHIBufferAllocateType allocateType) = 0;
    //

    virtual RHIBuffer* CreateBuffer(const RHIBufferCreateInfo& createInfo) = 0;

    virtual void DestroyBuffer(RHIBuffer* pBuffer) = 0;
    //
    // virtual uint8_t* MapBuffer(BufferHandle bufferHandle) = 0;
    //
    // virtual void UnmapBuffer(BufferHandle bufferHandle) = 0;
    //
    // virtual void DestroyBuffer(BufferHandle bufferHandle) = 0;
    //
    // virtual void SetBufferTexelFormat(BufferHandle bufferHandle, DataFormat format) = 0;

    // virtual DescriptorSetHandle CreateDescriptorSet(RHIShader* shaderHandle, uint32_t setIndex) = 0;

    virtual void DestroyDescriptorSet(RHIDescriptorSet* pDescriptorSet) = 0;

    // virtual void UpdateDescriptorSet(
    //     DescriptorSetHandle descriptorSetHandle,
    //     const std::vector<RHIShaderResourceBinding>& resourceBindings) = 0;

    virtual void SubmitAllGPUCommands() = 0;

    virtual void WaitDeviceIdle() = 0;

    virtual const RHIGPUInfo& QueryGPUInfo() const = 0;

    RHIResourceFactory* GetResourceFactory() const
    {
        return m_resourceFactory;
    }

protected:
    RHIResourceFactory* m_resourceFactory{nullptr};
};

// Global instance pointer
} // namespace zen
extern zen::DynamicRHI* GDynamicRHI;
