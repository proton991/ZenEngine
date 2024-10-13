#pragma once
#include <vector>
#include "VulkanHeaders.h"
#include "VulkanExtension.h"
#include "Common/PagedAllocator.h"
#include "Common/HashMap.h"
#include "Graphics/RHI/DynamicRHI.h"
#include "Graphics/RHI/RHICommands.h"
#if defined(ZEN_MACOS)
#    include "Platform/VulkanMacOSPlatform.h"
#elif defined(ZEN_WIN32)
#    include "Platform/VulkanWindowsPlatform.h"
#endif

#define ZEN_VK_API_VERSION VK_API_VERSION_1_2
#define ZEN_VK_APP_VERSION VK_MAKE_API_VERSION(0, 1, 0, 0)
#define ZEN_ENGINE_VERSION VK_MAKE_API_VERSION(0, 1, 0, 0)

namespace zen::rhi
{
class VulkanDevice;
class VulkanViewport;
class VulkanCommandBufferManager;
class VulkanDescriptorPoolManager;
struct VulkanShader;
struct VulkanTexture;
struct VulkanBuffer;
struct VulkanDescriptorSet;
struct VulkanPipeline;
class VulkanCommandBuffer;
class VulkanMemoryAllocator;

template <typename... RESOURCE_TYPES> struct VersatileResourceTemplate;

using VersatileResource = VersatileResourceTemplate<VulkanShader,
                                                    VulkanTexture,
                                                    VulkanBuffer,
                                                    VulkanDescriptorSet,
                                                    VulkanPipeline>;

class VulkanRHI : public DynamicRHI
{
public:
    VulkanRHI();
    ~VulkanRHI() override = default;

    RHICommandListContext* CreateCmdListContext() override;

    void WaitForCommandList(RHICommandList* cmdList) override;

    void Init() override;
    void Destroy() override;

    GraphicsAPIType GetAPIType() override
    {
        return GraphicsAPIType::eVulkan;
    }

    const char* GetName() override
    {
        return "VulkanRHI";
    };

    VulkanDevice* GetDevice() const
    {
        return m_device;
    }

    VkInstance GetInstance() const
    {
        return m_instance;
    }

    VkPhysicalDevice GetPhysicalDevice() const;

    VkDevice GetVkDevice() const;

    RHIViewport* CreateViewport(void* windowPtr,
                                uint32_t width,
                                uint32_t height,
                                bool enableVSync) final;

    void DestroyViewport(RHIViewport* viewport) final;

    void BeginDrawingViewport(RHIViewport* viewportRHI) final;

    void EndDrawingViewport(RHIViewport* viewportRHI,
                            RHICommandListContext* cmdListContext,
                            bool present) final;

    ShaderHandle CreateShader(const ShaderGroupInfo& shaderGroupInfo) final;

    void DestroyShader(ShaderHandle shaderHandle) final;

    InstanceExtensionFlags& GetInstanceExtensionFlags()
    {
        return m_instanceExtensionFlags;
    }

    PipelineHandle CreateGfxPipeline(ShaderHandle shaderHandle,
                                     const GfxPipelineStates& states,
                                     RenderPassHandle renderPassHandle,
                                     uint32_t subpass) final;

    void DestroyPipeline(PipelineHandle pipelineHandle) final;

    RenderPassHandle CreateRenderPass(const RenderPassLayout& renderPassLayout) final;

    void DestroyRenderPass(RenderPassHandle renderPassHandle) final;

    FramebufferHandle CreateFramebuffer(RenderPassHandle renderPassHandle,
                                        const FramebufferInfo& fbInfo) final;

    void DestroyFramebuffer(FramebufferHandle framebufferHandle) final;

    SamplerHandle CreateSampler(const SamplerInfo& samplerInfo) final;

    void DestroySampler(SamplerHandle samplerHandle) final;

    TextureHandle CreateTexture(const TextureInfo& textureInfo) final;

    void DestroyTexture(TextureHandle textureHandle) final;

    BufferHandle CreateBuffer(uint32_t size,
                              BitField<BufferUsageFlagBits> usageFlags,
                              BufferAllocateType allocateType) final;

    uint8_t* MapBuffer(BufferHandle bufferHandle) final;

    void UnmapBuffer(BufferHandle bufferHandle) final;

    void DestroyBuffer(BufferHandle bufferHandle) final;

    void SetBufferTexelFormat(BufferHandle bufferHandle, DataFormat format) final;

    DescriptorSetHandle CreateDescriptorSet(ShaderHandle shaderHandle, uint32_t setIndex) final;

    void DestroyDescriptorSet(DescriptorSetHandle descriptorSetHandle) final;

    void UpdateDescriptorSet(DescriptorSetHandle descriptorSetHandle,
                             const std::vector<ShaderResourceBinding>& resourceBindings) final;

    void SubmitAllGPUCommands() final;

    void ChangeTextureLayout(RHICommandList* cmdList,
                             TextureHandle textureHandle,
                             TextureLayout oldLayout,
                             TextureLayout newLayout) final;

    void ChangeImageLayout(VulkanCommandBuffer* cmdBuffer,
                           VkImage image,
                           VkImageLayout srcLayout,
                           VkImageLayout dstLayout,
                           const VkImageSubresourceRange& range);

    void WaitDeviceIdle() final;

protected:
    void CreateInstance();

private:
    void SetupInstanceLayers(VulkanInstanceExtensionArray& instanceExtensions);
    void SetupInstanceExtensions(VulkanInstanceExtensionArray& instanceExtensions);
    void PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& dbgMessengerCI);

    void SelectGPU();

    VkInstance m_instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT m_messenger{VK_NULL_HANDLE};

    std::vector<const char*> m_instanceLayers;
    std::vector<const char*> m_instanceExtensions;

    InstanceExtensionFlags m_instanceExtensionFlags{};

    VulkanDevice* m_device{nullptr};

    VulkanViewport* m_currentViewport{nullptr};

    std::vector<RHICommandListContext*> m_cmdListContexts;
    VulkanDescriptorPoolManager* m_descriptorPoolManager{nullptr};

    // allocator for memory
    VulkanMemoryAllocator* m_vkMemAllocator{nullptr};
    // allocators for resrouces
    PagedAllocator<VersatileResource> m_resourceAllocator;

    HashMap<ShaderHandle, VulkanPipeline*> m_shaderPipelines;
};
} // namespace zen::rhi