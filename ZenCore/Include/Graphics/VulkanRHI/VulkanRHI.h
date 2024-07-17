#pragma once
#include <vector>
#include "VulkanHeaders.h"
#include "VulkanExtension.h"
#include "Common/PagedAllocator.h"
#include "Graphics/RHI/DynamicRHI.h"
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
class VulkanCommandBufferManager;
class VulkanDescriptorPoolManager;
struct VulkanShader;
struct VulkanTexture;
struct VulkanBuffer;
struct VulkanDescriptorSet;
class VulkanMemoryAllocator;

template <typename... RESOURCE_TYPES> struct VersatileResourceTemplate;

using VersatileResource =
    VersatileResourceTemplate<VulkanShader, VulkanTexture, VulkanBuffer, VulkanDescriptorSet>;

class VulkanRHI : public DynamicRHI
{
public:
    VulkanRHI();
    ~VulkanRHI() override = default;

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

    VkInstance GetInstance() const
    {
        return m_instance;
    }

    VkPhysicalDevice GetPhysicalDevice() const;

    VkDevice GetVkDevice() const;

    VulkanCommandBufferManager* GetCmdBufferManager() const
    {
        return m_cmdBufferManager;
    }

    SwapchainHandle CreateSwapchain(SurfaceHandle surfaceHandle, bool enableVSync) final;

    Status ResizeSwapchain(SwapchainHandle swapchainHandle) final;

    void DestroySwapchain(SwapchainHandle swapchainHandle) final;

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
                                        const RenderTargetInfo& RTInfo) final;

    void DestroyFramebuffer(FramebufferHandle framebufferHandle) final;

    SamplerHandle CreateSampler(const SamplerInfo& samplerInfo) final;

    void DestroySampler(SamplerHandle samplerHandle) final;

    TextureHandle CreateTexture(const TextureInfo& textureInfo) final;

    void DestroyTexture(TextureHandle textureHandle) final;

    BufferHandle CreateBuffer(uint32_t size,
                              BitField<BufferUsageFlagBits> usageFlags,
                              BufferAllocateType allocateType) final;

    void DestroyBuffer(BufferHandle bufferHandle) final;

    void SetBufferTexelFormat(BufferHandle bufferHandle, DataFormat format) final;

    DescriptorSetHandle CreateDescriptorSet(ShaderHandle shaderHandle, uint32_t setIndex) final;

    void DestroyDescriptorSet(DescriptorSetHandle descriptorSetHandle) final;

    void UpdateDescriptorSet(DescriptorSetHandle descriptorSetHandle,
                             const std::vector<ShaderResourceBinding>& resourceBindings) final;

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

    VulkanCommandBufferManager* m_cmdBufferManager{nullptr};
    VulkanDescriptorPoolManager* m_descriptorPoolManager{nullptr};

    // allocator for memory
    VulkanMemoryAllocator* m_vkMemAllocator{nullptr};
    // allocators for resrouces
    PagedAllocator<VersatileResource> m_resourceAllocator;
    // PagedAllocator<VulkanShader> m_shaderAllocator;
    // PagedAllocator<VulkanTexture> m_textureAllocator;
    // PagedAllocator<VulkanBuffer> m_bufferAllocator;
    // PagedAllocator<VulkanDescriptorSet> m_descriptorSetAllocator;
};
} // namespace zen::rhi