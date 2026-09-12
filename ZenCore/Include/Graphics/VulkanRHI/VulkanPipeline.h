#pragma once

#include "Graphics/RHI/RHICommon.h"
#include "Templates/SmallVector.h"
#include "Templates/HashMap.h"
#include "Utils/Helpers.h"
#include "Graphics/RHI/RHIResource.h"
#include "VulkanTypes.h"

namespace zen
{
struct VulkanDescriptorPoolKey
{
    VulkanDescriptorPoolKey() = default;

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
class VulkanDevice;
class VulkanShader : public RHIShader
{
public:
    static VulkanShader* CreateObject(const RHIShaderCreateInfo& createInfo);

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

    struct VertexInputInfo
    {
        SmallVector<VkVertexInputBindingDescription> vkBindings;
        SmallVector<VkVertexInputAttributeDescription> vkAttributes;
        VkPipelineVertexInputStateCreateInfo stateCI;
    } m_vertexInputInfo;
    HeapVector<VkSpecializationMapEntry> m_spcMapEntries{};
    VkSpecializationInfo m_specializationInfo{};
    VkShaderStageFlags m_pushConstantsStageFlags;
    SmallVector<VkPipelineShaderStageCreateInfo> m_stageCreateInfos;

    struct DescriptorSetInfo
    {
        VulkanDescriptorPoolKey poolKey{};
        uint32_t layoutId{0};
        uint32_t variableCount{0};
        bool ownsLayout{false};
    };

    SmallVector<DescriptorSetInfo, MAX_NUM_DESCRIPTOR_SETS> m_descriptorSetInfos;

    SmallVector<VkDescriptorSetLayout, MAX_NUM_DESCRIPTOR_SETS> m_descriptorSetLayouts;

    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    bool m_hasGlobalBindlessSet{false};
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

    VkPipelineLayout GetVkPipelineLayout() const
    {
        return TO_VK_SHADER(m_pShader)->GetVkPipelineLayout();
    }

    VkShaderStageFlags GetPushConstantsStageFlags() const
    {
        return m_pushConstantsStageFlags;
    }

protected:
    void Init() override;

    void Destroy() override;

private:
    friend struct VulkanDescriptorStateTestAccess;

    VulkanPipeline(const RHIGfxPipelineCreateInfo& createInfo) : RHIPipeline(createInfo) {}

    VulkanPipeline(const RHIComputePipelineCreateInfo& createInfo) : RHIPipeline(createInfo) {}

    void InitGraphics();

    void InitCompute();

    VkPipeline m_vkPipeline{VK_NULL_HANDLE};
    VkPipelineBindPoint m_bindPoint;
    VkShaderStageFlags m_pushConstantsStageFlags;
};
} // namespace zen
