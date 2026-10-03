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
#include "Graphics/RHI/RHIGPUFrameTiming.h"
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

    // Clears GVulkanRHI when it still points to this backend.
    ~VulkanRHI() override;

    IRHICommandContext* GetCommandContext(RHICommandContextType contextType) override;

    IRHICommandContext* GetTransferCommandContext() override;

    RHIExecutionCounterStorage& GetExecutionCounterStorage()
    {
        return m_executionCounters;
    }

    RHIExecutionCounters GetExecutionCounters() const override
    {
        return m_executionCounters.Read();
    }

    bool PrepareSubmissionDependencies(IRHICommandContext*                       context,
                                       VectorView<const RHISubmissionDependency> dependencies) override;

    void Init() override;

    void Destroy() override;

    void BeginFrame() override;

    void BeginGPUFrameTiming(const RHIGPUFrameTimingPtr& timing) override;

    void EndGPUFrameTiming(const RHIGPUFrameTimingPtr& timing, bool succeeded) override;

    RHIGPUTimingPtr RegisterNativeGPUFrameRecording(bool included);

    void ReleaseNativeGPUFrameRecording();

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

    RHIViewport* CreateViewport(void* pWindow, uint32_t width, uint32_t height, bool enableVSync) final;

    void DestroyViewport(RHIViewport* pViewport) final;

    RHIShader* CreateShader(const RHIShaderCreateInfo& createInfo) final;

    void DestroyShader(RHIShader* pShader) final;

    RHIPipeline* CreatePipeline(const RHIComputePipelineCreateInfo& createInfo) final;

    RHIPipeline* CreatePipeline(const RHIGfxPipelineCreateInfo& createInfo) final;

    void DestroyPipeline(RHIPipeline* pPipeline) final;

    RHISampler* CreateSampler(const RHISamplerCreateInfo& createInfo) final;

    void DestroySampler(RHISampler* pSampler) final;

    RHITexture* CreateTexture(const RHITextureCreateInfo& createInfo) final;

    RHITextureView* CreateTextureView(RHITexture* pBaseTexture, const RHITextureViewCreateInfo& createInfo) final;

    void DestroyTexture(RHITexture* pTexture) final;

    RHIBuffer* CreateBuffer(const RHIBufferCreateInfo& createInfo) final;

    void DestroyBuffer(RHIBuffer* pBuffer) final;

    RHIStatus FinalizeCommandLists(VectorView<RHICommandList*>          cmdLists,
                                   HeapVector<RHIPlatformCommandList*>& outCommandLists) final;

    void SubmitPlatformCommandLists(VectorView<RHIPlatformCommandList*> commandLists) final;

    RHISubmissionResult FlushAllGPUCommands() final;

    void BlockSubmissions(
        RHIError error = MakeRHIError(RHIErrorCode::eBackendFailure, "Vulkan submissions blocked", __FILE__, __LINE__))
    {
        m_deviceLost |= error.code == RHIErrorCode::eDeviceLost;

        if (!m_terminalError.IsFailure())
        {
            m_terminalError = error;
        }

        m_submissionBlocked = true;
    }

    RHIError GetTerminalError() const override
    {
        return m_terminalError;
    }

    bool HasDeviceLoss() const override
    {
        return m_deviceLost;
    }

    RHIError GetLastSubmissionError() const override
    {
        return m_submissionError.IsFailure() ? m_submissionError : m_terminalError;
    }

    void SetSubmissionError(RHIError error)
    {
        m_submissionError = error;
    }

    bool AreSubmissionsBlocked() const final
    {
        return m_submissionBlocked;
    }

    RHIQueueCapabilities GetQueueCapabilities() const final;

    uint64_t GetLastSubmittedSerial(RHICommandContextType contextType) const final;

    uint64_t QueryLastCompletedSerial(RHICommandContextType contextType) final;

    bool WaitForCompletion(RHICommandContextType contextType, uint64_t submissionSerial, uint64_t timeoutNS = UINT64_MAX) final;

    void WaitDeviceIdle() final;

    const RHIGPUInfo& QueryGPUInfo() const final;

    RHIGPUMemoryStats GetGPUMemoryStats() const final;

    RHITextureCopyCapabilities GetTextureCopyCapabilities(DataFormat format) const final;

    RHIQueueCopyCapabilities GetQueueCopyCapabilities(RHICommandContextType type) const final;

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

    RHIBindlessHandle RegisterBindlessResource(RHIResource* pResource, uint32_t slotIndex = kInvalidBindlessSlotIndex) override;

    bool UnregisterBindlessResource(RHIBindlessHandle handle) override;

    bool IsBindlessResourceRegistered(RHIBindlessHandle handle) override;

    void CollectRetiredBindlessResources() override;

    bool ResetBindlessResources() override;

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

    RHIExecutionCounterStorage m_executionCounters;

    void PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& dbgMessengerCI);

    void SelectGPU();

    VulkanPlatformCommandList* AcquirePlatformCommandList();

    void ReleasePlatformCommandList(VulkanPlatformCommandList* pCommandList);

    void DestroyPlatformCommandListPool();

    VkInstance               m_instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT m_messenger{VK_NULL_HANDLE};

    HeapVector<NameID> m_instanceLayers;
    HeapVector<NameID> m_instanceExtensions;

    InstanceExtensionFlags m_instanceExtensionFlags{};

    VulkanDevice* m_pDevice{nullptr};

    RHIGPUInfo m_gpuInfo{};

    VulkanDescriptorPoolManager2*        m_pDescriptorPoolManager2{nullptr};
    VulkanBindlessDescriptorPoolManager* m_pBindlessDescriptorPoolManager{nullptr};
    VulkanUniformBufferAllocator*        m_pUniformBufferAllocator{nullptr};

    PagedAllocator<VersatileResource> m_resourceAllocator;

    ObjectPool<VulkanPlatformCommandList> m_platformCommandListPool;

    HeapVector<VulkanPlatformCommandList*> m_pendingPlatformCmdLists;
    bool                                   m_submissionBlocked{false};
    RHIError                               m_terminalError{};
    RHIError                               m_submissionError{};
    bool                                   m_deviceLost{false};
    RHIGPUFrameTimingPtr                   m_gpuFrameTiming;
    uint32_t                               m_pendingNativeRecordings{0};
    VulkanLifetimeTracker                  m_lifetimeTracker;
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
extern VulkanRHI*             GVulkanRHI;
} // namespace zen
