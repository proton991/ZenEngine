#pragma once
#include "VulkanHeaders.h"
#include "Templates/SmallVector.h"
#include "Utils/Helpers.h"
#include "Templates/HashMap.h"

namespace zen::rhi
{
struct VulkanDescriptorPoolKey
{
    VulkanDescriptorPoolKey() = default;

    uint32_t descriptorCount[ToUnderlying(ShaderResourceType::eMax)] = {};

    bool operator==(const VulkanDescriptorPoolKey& other) const
    {
        return memcmp(descriptorCount, other.descriptorCount, sizeof(descriptorCount)) == 0;
    }
};
} // namespace zen::rhi

namespace std
{
template <> struct hash<zen::rhi::VulkanDescriptorPoolKey>
{
    size_t operator()(const zen::rhi::VulkanDescriptorPoolKey& key) const
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

namespace zen::rhi
{
struct VulkanShader
{
    VulkanShader() = default;

    struct VertexInputInfo
    {
        SmallVector<VkVertexInputBindingDescription> vkBindings;
        SmallVector<VkVertexInputAttributeDescription> vkAttributes;
        VkPipelineVertexInputStateCreateInfo stateCI;
    } vertexInputInfo;
    std::vector<VkSpecializationMapEntry> entries{};
    VkSpecializationInfo specializationInfo{};
    VkShaderStageFlags pushConstantsStageFlags;
    SmallVector<VkPipelineShaderStageCreateInfo> stageCreateInfos;
    SmallVector<VkDescriptorSetLayout> descriptorSetLayouts;
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
    VulkanDescriptorPoolKey descriptorPoolKey{};
};

class FVulkanShader : public RHIShader
{
public:
    static FVulkanShader* CreateObject(const RHIShaderCreateInfo& createInfo);

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

    const VulkanDescriptorPoolKey& GetDescriptorPoolKey() const
    {
        return m_descriptorPoolKey;
    }

protected:
    void Init() override;

    void Destroy() override;


private:
    FVulkanShader(const RHIShaderCreateInfo& createInfo) : RHIShader(createInfo) {}

    struct VertexInputInfo
    {
        SmallVector<VkVertexInputBindingDescription> vkBindings;
        SmallVector<VkVertexInputAttributeDescription> vkAttributes;
        VkPipelineVertexInputStateCreateInfo stateCI;
    } m_vertexInputInfo;
    std::vector<VkSpecializationMapEntry> m_spcMapEntries{};
    VkSpecializationInfo m_specializationInfo{};
    VkShaderStageFlags m_pushConstantsStageFlags;
    SmallVector<VkPipelineShaderStageCreateInfo> m_stageCreateInfos;
    SmallVector<VkDescriptorSetLayout> m_descriptorSetLayouts;
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VulkanDescriptorPoolKey m_descriptorPoolKey{};
};

using VulkanDescriptorPools = HashMap<VulkanDescriptorPoolKey, HashMap<VkDescriptorPool, uint32_t>>;
using VulkanDescriptorPoolsIt = VulkanDescriptorPools::iterator;

class VulkanDescriptorPoolManager
{
public:
    explicit VulkanDescriptorPoolManager(VulkanDevice* device) : m_device(device) {}

    VkDescriptorPool GetOrCreateDescriptorPool(const VulkanDescriptorPoolKey& poolKey,
                                               VulkanDescriptorPoolsIt* iter);

    void UnRefDescriptorPool(VulkanDescriptorPoolsIt poolsIter, VkDescriptorPool pool);

private:
    VulkanDevice* m_device{nullptr};
    VulkanDescriptorPools m_pools;
};

struct VulkanDescriptorSet
{
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
    VulkanDescriptorPoolsIt iter;
};

struct VulkanPipeline
{
    VkPipeline pipeline{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
    uint32_t descriptorSetCount{0};
    VkShaderStageFlags pushConstantsStageFlags;
    //    std::vector<VulkanDescriptorSet*> descriptorSets;
    VulkanDescriptorSet* descriptorSets[8]; // todo: set size based on GPU limits
};
} // namespace zen::rhi
