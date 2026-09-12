#pragma once
#include "Graphics/RHI/RHIShaderParameters.h"
#include "VulkanDescriptorPool.h"

namespace zen
{
class FVulkanCommandListContext;
class VulkanPipeline;
class VulkanDescriptorPoolSetContainer;

class VulkanDescriptorSetState
{
public:
    void SetPipeline(VulkanPipeline* pPipeline);

    void SetShaderParameters(const RHIBatchedShaderParameters& parameters,
                             const RHIBindlessUse* recordedUse = nullptr);

    void FlushPendingDescriptorWrites(FVulkanCommandListContext* pContext,
                                      HeapVector<VkDescriptorSet>& outDescriptorSets,
                                      uint32_t& outFirstSet,
                                      HeapVector<uint32_t>& outDynamicOffsets);

    void Reset();

private:
    struct BindingState
    {
        RHIShaderResourceBinding srb;
        HeapVector<uint32_t> dynamicOffsets;
        uint32_t valueRange{0};
    };

    struct SetState
    {
        VkDescriptorSet vkSet{VK_NULL_HANDLE};
        VulkanDescriptorPoolSetContainer* pContainer{nullptr};
        HeapVector<BindingState> bindings;
        bool dirty{false};
    };

    VkDescriptorSet AcquireSetHandle(uint32_t setIdx,
                                     FVulkanCommandListContext* pContext,
                                     bool& outNeedsWrite);

    bool ResolveSet(uint32_t setIdx, FVulkanCommandListContext* pContext);

    bool BuildContentKey(uint32_t setIdx, VulkanDescriptorSetCache::ContentKey& outKey);

    void ClearAllSetStates();

    void InvalidateResolvedCaches();

    BindingState& FindOrAddBinding(SetState& setState,
                                   uint32_t bindingIdx,
                                   RHIShaderResourceType type);

    void WriteResourceParameter(const RHIShaderResourceParameter& param);

    bool ValidateBindingState(const BindingState& bindingState);

    bool ValidateSetState(uint32_t setIdx);

    void BuildSetUpdates(uint32_t setIndex, HeapVector<RHIShaderResourceBinding>& outUpdates);

    void SyncCacheEpoch(const VulkanDescriptorSetCache& cache);

    void BuildDescriptorSetList(FVulkanCommandListContext* pContext,
                                HeapVector<VkDescriptorSet>& outDescriptorSets,
                                uint32_t& outFirstSet,
                                HeapVector<uint32_t>& outDynamicOffsets);

    void AppendDynamicOffsetsForSet(uint32_t setIdx, HeapVector<uint32_t>& outDynamicOffsets);

    struct PackedValueBufferState
    {
        uint32_t setIdx{0};
        uint32_t bindingIdx{0};
        uint32_t blockSize{0};
        HeapVector<uint8_t> bytes;
        bool dirty{false};
    };

    void SetPackedValueParameter(uint32_t setIdx,
                                 uint32_t bindingIdx,
                                 uint32_t byteSize,
                                 const uint8_t* pData);

    void FlushPackedValueBuffers();

    PackedValueBufferState* FindOrAddPackedValueBuffer(uint32_t setIdx, uint32_t bindingIdx);

    VulkanPipeline* m_pPipeline{nullptr};

    SetState m_setStates[MAX_NUM_DESCRIPTOR_SETS];

    HeapVector<PackedValueBufferState> m_packedValueBuffers;

    uint64_t m_cacheEpoch{0};

    HeapVector<RHIShaderResourceBinding> m_updateSrbScratch;

    struct DynamicOffsetEntry
    {
        uint32_t bindingIdx{0};
        uint32_t offset{0};
    };

    HeapVector<DynamicOffsetEntry> m_dynamicOffsetScratch;
};
} // namespace zen
