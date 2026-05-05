#pragma once
#include <vector>
#include "VulkanExtension.h"
#include "Memory/PagedAllocator.h"
#include "Templates/HashMap.h"
#include "Templates/ObjectPool.h"
#include "Graphics/RHI/DynamicRHI.h"
#include "Graphics/RHI/RHICommands.h"
#include "Graphics/VulkanRHI/VulkanPlatformCommandList.h"
#if defined(ZEN_MACOS)
#    include "Platform/VulkanMacOSPlatform.h"
#elif defined(ZEN_WIN32)
#    include "Platform/VulkanWindowsPlatform.h"
#endif

#define ZEN_VK_API_VERSION VK_API_VERSION_1_2
#define ZEN_VK_APP_VERSION VK_MAKE_API_VERSION(0, 1, 0, 0)
#define ZEN_ENGINE_VERSION VK_MAKE_API_VERSION(0, 1, 0, 0)

namespace zen
{
class VulkanDevice;
class VulkanViewport;
class VulkanCommandBufferManager;
class VulkanDescriptorPoolManager;
class VulkanShader;
class VulkanTexture;
class VulkanBuffer;
class VulkanSampler;
class VulkanDescriptorSet;
class VulkanPipeline;
class VulkanCommandBuffer;
class VulkanMemoryAllocator;
class LegacyVulkanCommandListContext;

template <typename... RESOURCE_TYPES> struct VersatileResourceTemplate;

using VersatileResource = VersatileResourceTemplate<VulkanShader,
                                                    VulkanTexture,
                                                    VulkanSampler,
                                                    VulkanBuffer,
                                                    VulkanDescriptorSet,
                                                    VulkanPipeline,
                                                    VulkanViewport>;

class VulkanRHI : public DynamicRHI
{
public:
    VulkanRHI();

    ~VulkanRHI() override
    {
        // delete m_resourceFactory;
    }

    IRHICommandContext* GetCommandContext(RHICommandContextType contextType) override;

    IRHICommandContext* GetTransferCommandContext() override;

    LegacyRHICommandListContext* CreateLegacyCmdListContext() override;

    void WaitForLegacyCommandList(LegacyRHICommandList* pCmdList) override;

    LegacyRHICommandList* GetLegacyImmediateCommandList() override;

    void Init() override;

    void Destroy() override;

    RHIAPIType GetAPIType() override
    {
        return RHIAPIType::eVulkan;
    }

    const char* GetName() override
    {
        return "VulkanRHI";
    };

    VulkanDevice* GetDevice() const
    {
        return m_pDevice;
    }

    VkInstance GetInstance() const
    {
        return m_instance;
    }

    DataFormat GetSupportedDepthFormat() override;

    VkPhysicalDevice GetPhysicalDevice() const;

    VkDevice GetVkDevice() const;

    RHIViewport* CreateViewport(void* pWindow,
                                uint32_t width,
                                uint32_t height,
                                bool enableVSync) final;

    void DestroyViewport(RHIViewport* pViewport) final;

    void BeginDrawingViewport(RHIViewport* pViewportRHI) final;

    // Legacy immediate command-list path.
    void EndDrawingViewport(RHIViewport* pViewportRHI,
                            LegacyRHICommandListContext* pCmdListContext,
                            bool present) final;

    void EndDrawingViewport(RHIViewport* pViewportRHI,
                            RHICommandList* pCmdList,
                            bool present) final;

    // ShaderHandle CreateShader(const RHIShaderGroupInfo& shaderGroupInfo) final;

    // void DestroyShader(ShaderHandle shaderHandle) final;

    RHIShader* CreateShader(const RHIShaderCreateInfo& createInfo) final;

    void DestroyShader(RHIShader* pShader) final;

    RHIPipeline* CreatePipeline(const RHIComputePipelineCreateInfo& createInfo) final;

    RHIPipeline* CreatePipeline(const RHIGfxPipelineCreateInfo& createInfo) final;

    // PipelineHandle CreateGfxPipeline(RHIShader* shaderHandle,
    //                                  const RHIGfxPipelineStates& states,
    //                                  RenderPassHandle renderPassHandle,
    //                                  uint32_t subpass) final;

    // PipelineHandle CreateGfxPipeline(RHIShader* shaderHandle,
    //                                  const RHIGfxPipelineStates& states,
    //                                  const RHIRenderPassLayout& renderPassLayout,
    //                                  uint32_t subpass) final;

    // PipelineHandle CreateComputePipeline(RHIShader* shaderHandle) final;

    void DestroyPipeline(RHIPipeline* pPipeline) final;

    // RenderPassHandle CreateRenderPass(const RHIRenderPassLayout& renderPassLayout) final;

    // void DestroyRenderPass(RenderPassHandle renderPassHandle) final;

    // FramebufferHandle CreateFramebuffer(RenderPassHandle renderPassHandle,
    //                                     const RHIFramebufferInfo& fbInfo) final;

    // void DestroyFramebuffer(FramebufferHandle framebufferHandle) final;

    // SamplerHandle CreateSampler(const RHISamplerInfo& samplerInfo) final;
    RHISampler* CreateSampler(const RHISamplerCreateInfo& createInfo) final;

    void DestroySampler(RHISampler* pSampler) final;

    // TextureHandle CreateTexture(const TextureInfo& textureInfo) final;
    //
    // TextureHandle CreateTextureProxy(const TextureHandle& baseTexture,
    //                                  const TextureProxyInfo& textureProxyInfo) final;
    //
    // void DestroyTexture(TextureHandle textureHandle) final;

    RHITexture* CreateTexture(const RHITextureCreateInfo& createInfo) final;

    RHITexture* CreateTextureProxy(const RHITexture* pBaseTexture,
                                   const RHITextureProxyCreateInfo& proxyInfo) final;

    void DestroyTexture(RHITexture* pTexture) final;

    // DataFormat GetTextureFormat(TextureHandle textureHandle) final;
    //
    // RHITextureSubResourceRange GetTextureSubResourceRange(TextureHandle textureHandle) final;

    // BufferHandle CreateBuffer(uint32_t size,
    //                           BitField<RHIBufferUsageFlagBits> usageFlags,
    //                           RHIBufferAllocateType allocateType) final;
    //
    // uint8_t* MapBuffer(BufferHandle bufferHandle) final;
    //
    // void UnmapBuffer(BufferHandle bufferHandle) final;
    //
    // void DestroyBuffer(BufferHandle bufferHandle) final;
    //

    RHIBuffer* CreateBuffer(const RHIBufferCreateInfo& createInfo) final;

    void DestroyBuffer(RHIBuffer* pBuffer) final;
    //
    // void SetBufferTexelFormat(BufferHandle bufferHandle, DataFormat format) final;

    // DescriptorSetHandle CreateDescriptorSet(RHIShader* shaderHandle, uint32_t setIndex) final;

    void DestroyDescriptorSet(RHIDescriptorSet* pDescriptorSet) final;

    // void UpdateDescriptorSet(DescriptorSetHandle descriptorSetHandle,
    //                          const HeapVector<RHIShaderResourceBinding>& resourceBindings) final;

    void FinalizeCommandLists(VectorView<RHICommandList*> cmdLists,
                              HeapVector<RHIPlatformCommandList*>& outCommandLists) final;

    void SubmitPlatformCommandLists(VectorView<RHIPlatformCommandList*> commandLists) final;

    void SubmitAllGPUCommands() final;

    void WaitDeviceIdle() final;

    const RHIGPUInfo& QueryGPUInfo() const final;

    void UpdateImageLayout(VkImage image, VkImageLayout newLayout);

    void RemoveImageLayout(VkImage image);

    VkImageLayout GetImageCurrentLayout(VkImage image);

    VkRenderPass GetOrCreateRenderPass(const RHIRenderingLayout* pRenderingLayout);

    VkFramebuffer GetOrCreateFramebuffer(const RHIRenderingLayout* pRenderingLayout,
                                         VkRenderPass renderPass);

    VulkanViewport* GetCurrentViewport() const
    {
        return m_pCurrentViewport;
    }

    auto& GetResourceAllocator()
    {
        return m_resourceAllocator;
    }

    VulkanDescriptorPoolManager* GetDescriptorPoolManager() const
    {
        return m_pDescriptorPoolManager;
    }

    InstanceExtensionFlags& GetInstanceExtensionFlags()
    {
        return m_instanceExtensionFlags;
    }

    LegacyVulkanCommandListContext* GetLegacyImmediateCmdContext() const;

protected:
    void CreateInstance();

private:
    void SetupInstanceLayers(VulkanInstanceExtensionArray& instanceExtensions);
    void SetupInstanceExtensions(VulkanInstanceExtensionArray& instanceExtensions);
    void PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& dbgMessengerCI);

    void SelectGPU();

    VulkanPlatformCommandList* AcquirePlatformCommandList();

    void ReleasePlatformCommandList(VulkanPlatformCommandList* pCommandList);

    void DestroyPlatformCommandListPool();

    VkInstance m_instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT m_messenger{VK_NULL_HANDLE};

    HeapVector<const char*> m_instanceLayers;
    HeapVector<const char*> m_instanceExtensions;

    InstanceExtensionFlags m_instanceExtensionFlags{};

    VulkanDevice* m_pDevice{nullptr};

    RHIGPUInfo m_gpuInfo{};

    VulkanViewport* m_pCurrentViewport{nullptr};

    HeapVector<LegacyRHICommandListContext*> m_legacyCmdListContexts;
    VulkanDescriptorPoolManager* m_pDescriptorPoolManager{nullptr};

    // allocator for memory
    // VulkanMemoryAllocator* m_vkMemAllocator{nullptr};
    // allocators for resources
    PagedAllocator<VersatileResource> m_resourceAllocator;

    // HashMap<RHIShader*, VulkanPipeline*> m_shaderPipelines;

    // used when ChangeTextureLayout or AddPipelineBarrier is called,
    // primarily applied outside the RenderGraph.
    HashMap<VkImage, VkImageLayout> m_imageLayoutCache;

    HashMap<uint32_t, VkRenderPass> m_renderPassCache;

    HashMap<uint32_t, VkFramebuffer> m_framebufferCache;

    ObjectPool<VulkanPlatformCommandList> m_platformCommandListPool;
};

class VulkanResourceFactory : public RHIResourceFactory
{
public:
    RHIBuffer* CreateBuffer(const RHIBufferCreateInfo& createInfo) final;

    RHITexture* CreateTexture(const RHITextureCreateInfo& createInfo) final;

    RHISampler* CreateSampler(const RHISamplerCreateInfo& createInfo) final;

    RHIShader* CreateShader(const RHIShaderCreateInfo& createInfo) final;

    RHIPipeline* CreatePipeline(const RHIComputePipelineCreateInfo& createInfo) final;

    RHIPipeline* CreatePipeline(const RHIGfxPipelineCreateInfo& createInfo) final;
};

extern VulkanMemoryAllocator* GVkMemAllocator;
extern VulkanRHI* GVulkanRHI;
} // namespace zen
