#pragma once

#include "Graphics/RHI/RHICommon.h"
#include "Templates/SmallVector.h"
#include "Templates/HashMap.h"
#include "Utils/Helpers.h"
#include "Graphics/RHI/RHIResource.h"
#include "VulkanHeaders.h"

namespace zen
{
struct VulkanDescriptorPoolKey
{
    VulkanDescriptorPoolKey()                                           = default;

    uint32_t descriptorCount[ToUnderlying(RHIShaderResourceType::eMax)] = {};

    bool operator==(const VulkanDescriptorPoolKey& other) const
    {
        return memcmp(descriptorCount, other.descriptorCount, sizeof(descriptorCount)) == 0;
    }
};
} // namespace zen

namespace std
{
template <> struct hash<zen::VulkanDescriptorPoolKey>
{
    size_t operator()(const zen::VulkanDescriptorPoolKey& key) const noexcept
    {
        size_t seed = 0;

        for (uint32_t count : key.descriptorCount)
        {
            zen::util::HashCombine(seed, count);
        }

        return seed;
    }
};
} // namespace std

namespace zen
{
// Uniform-buffer descriptors of one pipeline layout. Every reflected uniform buffer is a
// dynamic descriptor; array elements count individually. Bindless descriptors are
// excluded: they are checked against the update-after-bind limits instead.
struct VulkanUniformBufferUsage
{
    uint32_t dynamicCount{0};
    uint32_t maxPerStageCount{0};
};

VulkanUniformBufferUsage CountUniformBufferDescriptors(const RHIShaderResourceDescriptorTable& srdTable);

bool UniformBuffersFitLimits(const VulkanUniformBufferUsage& usage, const VkPhysicalDeviceLimits& limits);

class VulkanDevice;
class VulkanShader : public RHIShader
{
public:
    static VulkanShader* CreateObject(const RHIShaderCreateInfo& createInfo);

    struct DynamicOffsetSlot
    {
        uint32_t binding{0};
        uint32_t count{0};
    };

    VectorView<const DynamicOffsetSlot> GetDynamicOffsetSlots(uint32_t set) const
    {
        return m_dynamicOffsetSlots[set];
    }

    uint32_t GetNumShaderStages() const
    {
        return m_stageCreateInfos.size();
    }

    const VkPipelineShaderStageCreateInfo* GetStageCreateInfoData() const
    {
        return m_stageCreateInfos.data();
    }

    VkPipelineLayout GetVkPipelineLayout() const
    {
        return m_pipelineLayout;
    }

    const VkDescriptorSetLayout* GetDescriptorSetLayoutData() const
    {
        return m_descriptorSetLayouts.data();
    }

    uint32_t GetNumDescriptorSetLayouts() const
    {
        return m_descriptorSetLayouts.size();
    }

    VkShaderStageFlags GetPushConstantsStageFlags() const
    {
        return m_pushConstantsStageFlags;
    }

    const VkPipelineVertexInputStateCreateInfo* GetVertexInputStateCreateInfoData() const
    {
        return &m_vertexInputInfo.stateCI;
    }

    const VulkanDescriptorPoolKey& GetDescriptorPoolKey(uint32_t setIdx) const
    {
        VERIFY_EXPR(setIdx < m_descriptorSetInfos.size());

        return m_descriptorSetInfos[setIdx].poolKey;
    }

    uint32_t GetDescriptorSetLayoutId(uint32_t setIdx) const
    {
        return m_descriptorSetInfos[setIdx].layoutId;
    }

    VkDescriptorSetLayout GetDescriptorSetLayoutHandle(uint32_t setIdx) const
    {
        return m_descriptorSetLayouts[setIdx];
    }

    uint32_t GetDescriptorSetVariableCount(uint32_t setIdx) const
    {
        return m_descriptorSetInfos[setIdx].variableCount;
    }

    bool HasGlobalBindlessSet() const
    {
        return m_hasGlobalBindlessSet;
    }

protected:
    void Init() override;

    void Destroy() override;

private:
    VulkanShader(const RHIShaderCreateInfo& createInfo) : RHIShader(createInfo) {}

    bool LoadSpirvFiles();

    void InitNativeObjects(const RHIShaderGroupInfo& sgInfo);

    struct VertexInputInfo
    {
        SmallVector<VkVertexInputBindingDescription>   vkBindings;
        SmallVector<VkVertexInputAttributeDescription> vkAttributes;
        VkPipelineVertexInputStateCreateInfo           stateCI;
    } m_vertexInputInfo;
    HeapVector<VkSpecializationMapEntry>         m_spcMapEntries{};
    HeapVector<uint32_t>                         m_specializationData{};
    VkSpecializationInfo                         m_specializationInfo{};
    VkShaderStageFlags                           m_pushConstantsStageFlags;
    SmallVector<VkPipelineShaderStageCreateInfo> m_stageCreateInfos;

    struct DescriptorSetInfo
    {
        VulkanDescriptorPoolKey poolKey{};
        uint32_t                layoutId{0};
        uint32_t                variableCount{0};
        bool                    ownsLayout{false};
    };

    SmallVector<DescriptorSetInfo, MAX_NUM_DESCRIPTOR_SETS> m_descriptorSetInfos;

    SmallVector<VkDescriptorSetLayout, MAX_NUM_DESCRIPTOR_SETS> m_descriptorSetLayouts;

    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    bool             m_hasGlobalBindlessSet{false};
    // False when the reflected layout exceeds device descriptor limits; no native objects exist.
    bool m_fitsDeviceLimits{false};

    SmallVector<DynamicOffsetSlot> m_dynamicOffsetSlots[MAX_NUM_DESCRIPTOR_SETS];
};

class VulkanPipeline : public RHIPipeline
{
public:
    static VulkanPipeline* CreateObject(const RHIGfxPipelineCreateInfo& createInfo);

    static VulkanPipeline* CreateObject(const RHIComputePipelineCreateInfo& createInfo);

    VkPipelineBindPoint GetVkPipelineBindPoint() const
    {
        return m_bindPoint;
    }

    VkPipeline GetVkPipeline() const
    {
        return m_vkPipeline;
    }

    bool UsesDynamicState(RHIDynamicState state) const
    {
        return m_gfxStates.dynamicStates.enabledStates.Test(ToUnderlying(state));
    }

    VkPipelineLayout GetVkPipelineLayout() const
    {
        return static_cast<VulkanShader*>(m_pShader)->GetVkPipelineLayout();
    }

    VkShaderStageFlags GetPushConstantsStageFlags() const
    {
        return m_pushConstantsStageFlags;
    }

protected:
    void Init() override;

    void Destroy() override;

private:
    VulkanPipeline(const RHIGfxPipelineCreateInfo& createInfo) : RHIPipeline(createInfo) {}

    VulkanPipeline(const RHIComputePipelineCreateInfo& createInfo) : RHIPipeline(createInfo) {}

    void InitGraphics(const RHIRenderingLayout& layout);

    void InitCompute();

    VkPipeline          m_vkPipeline{VK_NULL_HANDLE};
    VkPipelineBindPoint m_bindPoint{VK_PIPELINE_BIND_POINT_GRAPHICS};
    VkShaderStageFlags  m_pushConstantsStageFlags{0};
    bool                m_initialized{false};
};
} // namespace zen
