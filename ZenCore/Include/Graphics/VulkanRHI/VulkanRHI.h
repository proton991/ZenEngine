#pragma once
#include "Graphics/RHI/RHICommandList.h"
#include "Templates/HeapVector.h"
#include "VulkanExtension.h"
#include "VulkanLifetimeTracker.h"
#include "Memory/PagedAllocator.h"
#include "Templates/HashMap.h"
#include "Templates/ObjectPool.h"
#include "Templates/VectorView.h"
#include "Graphics/RHI/DynamicRHI.h"
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
class VulkanDescriptorPoolManager2;
class VulkanBindlessDescriptorPoolManager;
class VulkanUniformBufferAllocator;
class VulkanShader;
class VulkanTexture;
class VulkanTextureView;
class VulkanBuffer;
class VulkanSampler;
class VulkanPipeline;
class VulkanCommandBuffer;
class VulkanMemoryAllocator;

template <typename... RESOURCE_TYPES> struct VersatileResourceTemplate;

using VersatileResource = VersatileResourceTemplate<VulkanShader,
                                                    VulkanTexture,
                                                    VulkanTextureView,
                                                    VulkanSampler,
                                                    VulkanBuffer,
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

    void Init() override;

    void Destroy() override;

    void BeginFrame() override;

    RHIAPIType GetAPIType() override
    {
        return RHIAPIType::eVulkan;
    }

    NameID GetName() override
    {
        static const NameID name("VulkanRHI");

        return name;
    }

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

    RHITextureView* CreateTextureView(RHITexture* pBaseTexture,
                                      const RHITextureViewCreateInfo& createInfo) final;

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

    // void DestroyDescriptorSet(RHIDescriptorSet* pDescriptorSet) final;

    // void UpdateDescriptorSet(DescriptorSetHandle descriptorSetHandle,
    //                          const HeapVector<RHIShaderResourceBinding>& resourceBindings) final;

    void FinalizeCommandLists(VectorView<RHICommandList*> cmdLists,
                              HeapVector<RHIPlatformCommandList*>& outCommandLists) final;

    void SubmitPlatformCommandLists(VectorView<RHIPlatformCommandList*> commandLists) final;

    RHISubmissionResult FlushAllGPUCommands() final;

    void BlockSubmissions()
    {
        m_submissionBlocked = true;
    }

    bool AreSubmissionsBlocked() const final
    {
        return m_submissionBlocked;
    }

    bool IsTransferQueueSharedWithGraphics() const final;

    uint64_t GetLastSubmittedSerial(RHICommandContextType contextType) const final;

    uint64_t GetLastCompletedSerial(RHICommandContextType contextType) final;

    bool WaitForSubmission(RHICommandContextType contextType,
                           uint64_t submissionSerial,
                           uint64_t timeoutNS = UINT64_MAX) final;

    void WaitDeviceIdle() final;

    const RHIGPUInfo& QueryGPUInfo() const final;

    RHITextureCopyCapabilities GetTextureCopyCapabilities(DataFormat format) const final;

    RHIQueueCopyCapabilities GetQueueCopyCapabilities(RHICommandContextType type) const final;

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

    PagedAllocator<VersatileResource>& GetResourceAllocator()
    {
        return m_resourceAllocator;
    }

    VulkanDescriptorPoolManager2* GetDescriptorPoolManager2() const
    {
        return m_pDescriptorPoolManager2;
    }

    VulkanBindlessDescriptorPoolManager* GetBindlessDescriptorPoolManager() const
    {
        return m_pBindlessDescriptorPoolManager;
    }

    RHIBindlessHandle RegisterBindlessResource(
        RHIResource* pResource,
        uint32_t slotIndex = kInvalidBindlessSlotIndex) override;
    bool UnregisterBindlessResource(RHIBindlessHandle handle) override;
    bool IsBindlessResourceRegistered(RHIBindlessHandle handle) override;
    void CollectRetiredBindlessResources() override;

    VulkanUniformBufferAllocator* GetUniformBufferAllocator() const
    {
        return m_pUniformBufferAllocator;
    }

    VulkanLifetimeTracker& GetLifetimeTracker()
    {
        return m_lifetimeTracker;
    }

    InstanceExtensionFlags& GetInstanceExtensionFlags()
    {
        return m_instanceExtensionFlags;
    }

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

    HeapVector<NameID> m_instanceLayers;
    HeapVector<NameID> m_instanceExtensions;

    InstanceExtensionFlags m_instanceExtensionFlags{};

    VulkanDevice* m_pDevice{nullptr};

    RHIGPUInfo m_gpuInfo{};

    VulkanViewport* m_pCurrentViewport{nullptr};

    VulkanDescriptorPoolManager2* m_pDescriptorPoolManager2{nullptr};
    VulkanBindlessDescriptorPoolManager* m_pBindlessDescriptorPoolManager{nullptr};
    VulkanUniformBufferAllocator* m_pUniformBufferAllocator{nullptr};

    // allocator for memory
    // VulkanMemoryAllocator* m_vkMemAllocator{nullptr};
    // allocators for resources
    PagedAllocator<VersatileResource> m_resourceAllocator;

    // HashMap<RHIShader*, VulkanPipeline*> m_shaderPipelines;

    // Debug-only mirror of layouts requested through explicit barriers. RDG/RenderDevice resource
    // state is the authority for deciding whether a transition is needed.
    HashMap<VkImage, VkImageLayout> m_imageLayoutCache;

    HashMap<uint32_t, VkRenderPass> m_renderPassCache;

    HashMap<uint32_t, VkFramebuffer> m_framebufferCache;

    ObjectPool<VulkanPlatformCommandList> m_platformCommandListPool;

    HeapVector<VulkanPlatformCommandList*> m_pendingPlatformCmdLists;
    bool m_submissionBlocked{false};
    VulkanLifetimeTracker m_lifetimeTracker;
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
