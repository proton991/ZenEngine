#include "Graphics/RHI/RHICommon.h"
#include "Graphics/VulkanRHI/VulkanDescriptorPool.h"
#include "Graphics/VulkanRHI/VulkanDescriptorState.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"
#include "Templates/VectorView.h"
#include "Utils/Errors.h"
#include "Utils/Helpers.h"
#include <algorithm>
#include <cstring>

namespace zen
{
namespace
{
uint32_t GetResourceStride(RHIShaderResourceType type)
{
    uint32_t stride = 1;

    if (type == RHIShaderResourceType::eSamplerWithTexture ||
        type == RHIShaderResourceType::eSamplerWithTextureBuffer)
    {
        stride = 2;
    }

    return stride;
}

VkImageView GetImageView(RHIResource* pResource)
{
    VkImageView imageView = VK_NULL_HANDLE;

    if (VulkanTextureView* pView = dynamic_cast<VulkanTextureView*>(pResource))
    {
        imageView = pView->GetVkImageView();
    }
    else if (VulkanTexture* pTexture = dynamic_cast<VulkanTexture*>(pResource))
    {
        const VulkanTextureView* pDefaultView =
            dynamic_cast<const VulkanTextureView*>(pTexture->GetDefaultView());
        imageView = pDefaultView != nullptr ? pDefaultView->GetVkImageView() : VK_NULL_HANDLE;
    }

    return imageView;
}

struct DescriptorWriteBatch
{
    explicit DescriptorWriteBatch(uint32_t maxDescriptorCount) :
        writes(maxDescriptorCount),
        imageInfos(maxDescriptorCount),
        bufferInfos(maxDescriptorCount),
        bufferViews(maxDescriptorCount)
    {}

    HeapVector<VkWriteDescriptorSet> writes;
    HeapVector<VkDescriptorImageInfo> imageInfos;
    HeapVector<VkDescriptorBufferInfo> bufferInfos;
    HeapVector<VkBufferView> bufferViews;
    uint32_t numWrites{0};
    uint32_t numImageInfos{0};
    uint32_t numBufferInfos{0};
    uint32_t numBufferViews{0};
};

bool AppendDescriptorBindingWrites(VkDescriptorSet descriptorSet,
                                   const RHIShaderResourceBinding& binding,
                                   uint32_t valueRange,
                                   DescriptorWriteBatch& batch)
{
    bool valid = descriptorSet != VK_NULL_HANDLE && !binding.resources.empty();

    if (valid)
    {
        const uint32_t resourceStride = GetResourceStride(binding.type);
        const uint32_t descriptorCount =
            static_cast<uint32_t>(binding.resources.size()) / resourceStride;
        valid = descriptorCount > 0 && binding.resources.size() % resourceStride == 0;

        for (uint32_t descriptorIdx = 0; descriptorIdx < descriptorCount; ++descriptorIdx)
        {
            const uint32_t resourceIdx = descriptorIdx * resourceStride;
            RHIResource* pResource     = binding.resources[resourceIdx];
            RHIResource* pAuxResource =
                resourceStride == 2 ? binding.resources[resourceIdx + 1] : nullptr;
            const bool descriptorIsPopulated = pResource != nullptr || pAuxResource != nullptr;

            if (descriptorIsPopulated)
            {
                VERIFY_EXPR(batch.numWrites < batch.writes.size());
                VkWriteDescriptorSet& write = batch.writes[batch.numWrites];
                write                       = {};
                InitVkStruct(write, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
                write.dstSet          = descriptorSet;
                write.dstBinding      = binding.binding;
                write.dstArrayElement = descriptorIdx;
                write.descriptorCount = 1;

                bool descriptorIsValid = true;

                switch (binding.type)
                {
                    case RHIShaderResourceType::eSampler:
                    {
                        VulkanSampler* pSampler          = TO_VK_SAMPLER(pResource);
                        VkDescriptorImageInfo& imageInfo = batch.imageInfos[batch.numImageInfos++];
                        imageInfo                        = {};
                        imageInfo.sampler =
                            pSampler != nullptr ? pSampler->GetVkSampler() : VK_NULL_HANDLE;
                        descriptorIsValid    = imageInfo.sampler != VK_NULL_HANDLE;
                        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                        write.pImageInfo     = &imageInfo;
                    }
                    break;

                    case RHIShaderResourceType::eTexture:
                    case RHIShaderResourceType::eImage:
                    case RHIShaderResourceType::eInputAttachment:
                    {
                        VkDescriptorImageInfo& imageInfo = batch.imageInfos[batch.numImageInfos++];
                        imageInfo                        = {};
                        imageInfo.imageView              = GetImageView(pResource);
                        imageInfo.imageLayout = binding.type == RHIShaderResourceType::eImage ?
                            VK_IMAGE_LAYOUT_GENERAL :
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        descriptorIsValid     = imageInfo.imageView != VK_NULL_HANDLE;
                        write.descriptorType  = binding.type == RHIShaderResourceType::eImage ?
                            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
                            binding.type == RHIShaderResourceType::eInputAttachment ?
                            VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT :
                            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                        write.pImageInfo      = &imageInfo;
                    }
                    break;

                    case RHIShaderResourceType::eSamplerWithTexture:
                    {
                        VulkanSampler* pSampler          = TO_VK_SAMPLER(pResource);
                        VkDescriptorImageInfo& imageInfo = batch.imageInfos[batch.numImageInfos++];
                        imageInfo                        = {};
                        imageInfo.sampler =
                            pSampler != nullptr ? pSampler->GetVkSampler() : VK_NULL_HANDLE;
                        imageInfo.imageView   = GetImageView(pAuxResource);
                        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        descriptorIsValid     = imageInfo.sampler != VK_NULL_HANDLE &&
                            imageInfo.imageView != VK_NULL_HANDLE;
                        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        write.pImageInfo     = &imageInfo;
                    }
                    break;

                    case RHIShaderResourceType::eTextureBuffer:
                    case RHIShaderResourceType::eSamplerWithTextureBuffer:
                    case RHIShaderResourceType::eImageBuffer:
                    {
                        RHIResource* pBufferResource = pResource;

                        if (binding.type == RHIShaderResourceType::eSamplerWithTextureBuffer)
                        {
                            descriptorIsValid = TO_VK_SAMPLER(pResource) != nullptr;
                            pBufferResource   = pAuxResource;
                        }

                        VulkanBuffer* pBuffer    = TO_VK_BUFFER(pBufferResource);
                        VkBufferView& bufferView = batch.bufferViews[batch.numBufferViews++];
                        bufferView =
                            pBuffer != nullptr ? pBuffer->GetVkBufferView() : VK_NULL_HANDLE;
                        descriptorIsValid    = descriptorIsValid && bufferView != VK_NULL_HANDLE;
                        write.descriptorType = binding.type == RHIShaderResourceType::eImageBuffer ?
                            VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER :
                            VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
                        write.pTexelBufferView = &bufferView;
                    }
                    break;

                    case RHIShaderResourceType::eUniformBuffer:
                    case RHIShaderResourceType::eStorageBuffer:
                    {
                        VulkanBuffer* pBuffer = TO_VK_BUFFER(pResource);
                        VkDescriptorBufferInfo& bufferInfo =
                            batch.bufferInfos[batch.numBufferInfos++];
                        bufferInfo = {};

                        if (pBuffer != nullptr)
                        {
                            bufferInfo.buffer = pBuffer->GetVkBuffer();
                            bufferInfo.range =
                                valueRange > 0 ? valueRange : pBuffer->GetRequiredSize();
                        }

                        descriptorIsValid = pBuffer != nullptr &&
                            bufferInfo.buffer != VK_NULL_HANDLE && bufferInfo.range > 0;
                        write.descriptorType =
                            binding.type == RHIShaderResourceType::eUniformBuffer ?
                            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC :
                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                        write.pBufferInfo = &bufferInfo;
                    }
                    break;

                    default: descriptorIsValid = false; break;
                }

                valid = valid && descriptorIsValid;

                if (descriptorIsValid)
                {
                    ++batch.numWrites;
                }
            }
        }
    }

    return valid;
}
} // namespace

void VulkanDescriptorSetState::SetPipeline(VulkanPipeline* pPipeline)
{
    if (m_pPipeline != pPipeline)
    {
        ClearAllSetStates();
        m_packedValueBuffers.clear();
        m_containerEpoch = 0;
        m_pPipeline      = pPipeline;
    }
}

void VulkanDescriptorSetState::SetShaderParameters(const RHIBatchedShaderParameters& parameters)
{
    for (const RHIShaderValueParameter& parameter : parameters.GetValueParams())
    {
        const VectorView<const uint8_t> bytes = parameters.GetValueBytes(parameter);

        if (!bytes.empty())
        {
            SetPackedValueParameter(parameter.set, parameter.binding, parameter.byteSize,
                                    bytes.data());
        }
    }

    for (const RHIShaderResourceParameter& parameter : parameters.GetResourceParams())
    {
        WriteResourceParameter(parameter);
    }

    for (const RHIShaderResourceParameter& parameter : parameters.GetBindlessParams())
    {
        GVulkanRHI->GetBindlessDescriptorPoolManager()->RegisterBindlessResource(
            parameter.pResource, parameter.arrayIndex);
    }
}

void VulkanDescriptorSetState::FlushPendingDescriptorWrites(
    FVulkanCommandListContext* pContext,
    HeapVector<VkDescriptorSet>& outDescriptorSets,
    uint32_t& outFirstSet,
    HeapVector<uint32_t>& outDynamicOffsets)
{
    outDescriptorSets.clear();
    outDynamicOffsets.clear();
    outFirstSet = 0;

    VERIFY_EXPR(pContext != nullptr);
    VERIFY_EXPR(m_pPipeline != nullptr);

    VulkanShader* pShader = TO_VK_SHADER(m_pPipeline->GetShader());

    if (pShader != nullptr && pShader->HasGlobalBindlessSet())
    {
        GVulkanRHI->GetBindlessDescriptorPoolManager()->Flush();
    }

    FlushPackedValueBuffers();

    BuildDescriptorSetList(pContext, outDescriptorSets, outFirstSet, outDynamicOffsets);
}

void VulkanDescriptorSetState::Reset()
{
    ClearAllSetStates();
    m_packedValueBuffers.clear();
    m_updateSrbScratch.clear();
    m_dynamicOffsetScratch.clear();
    m_containerEpoch = 0;
    m_pPipeline      = nullptr;
}

VkDescriptorSet VulkanDescriptorSetState::AcquireSetHandle(uint32_t setIdx,
                                                           FVulkanCommandListContext* pContext,
                                                           bool& outNeedsWrite)
{
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    outNeedsWrite                 = false;

    if (m_pPipeline != nullptr)
    {
        VulkanShader* pShader      = TO_VK_SHADER(m_pPipeline->GetShader());
        uint32_t variableCount     = pShader->GetDescriptorSetVariableCount(setIdx);
        const bool updateAfterBind = variableCount > 0;

        if (setIdx == kGlobalBindlessHeapIndex && pShader->HasGlobalBindlessSet())
        {
            outNeedsWrite = false;
            descriptorSet = GVulkanRHI->GetBindlessDescriptorPoolManager()->GetGlobalBindlessSet();
        }
        else
        {
            VulkanDescriptorSetCache::ContentKey contentKey{};

            VulkanDescriptorSetCache* pCache =
                GVulkanRHI->GetDescriptorPoolManager2()->GetContentCache();

            if (pCache != nullptr && BuildContentKey(setIdx, contentKey))
            {
                descriptorSet = pCache->Find(contentKey);

                if (descriptorSet != VK_NULL_HANDLE)
                {
                    outNeedsWrite = false;
                }
                else
                {
                    descriptorSet =
                        pCache->Insert(contentKey, pShader->GetDescriptorPoolKey(setIdx),
                                       pShader->GetDescriptorSetLayoutHandle(setIdx),
                                       updateAfterBind, variableCount);

                    outNeedsWrite = true;
                }
            }
        }
    }

    return descriptorSet;
}

bool VulkanDescriptorSetState::ResolveSet(uint32_t setIdx, FVulkanCommandListContext* pContext)
{
    bool resolved = false;

    if (setIdx < MAX_NUM_DESCRIPTOR_SETS && ValidateSetState(setIdx))
    {
        SetState& setState = m_setStates[setIdx];

        if (setState.dirty || setState.vkSet == VK_NULL_HANDLE)
        {
            bool needsWrite = false;
            setState.vkSet  = AcquireSetHandle(setIdx, pContext, needsWrite);

            if (setState.vkSet != VK_NULL_HANDLE && needsWrite)
            {
                BuildSetUpdates(setIdx, m_updateSrbScratch);
                uint32_t maxDescriptorWrites = 0;

                for (const RHIShaderResourceBinding& update : m_updateSrbScratch)
                {
                    maxDescriptorWrites += static_cast<uint32_t>(update.resources.size()) /
                        GetResourceStride(update.type);
                }

                DescriptorWriteBatch writeBatch(maxDescriptorWrites);

                for (const RHIShaderResourceBinding& update : m_updateSrbScratch)
                {
                    uint32_t valueRange = 0;

                    for (const BindingState& bindingState : setState.bindings)
                    {
                        if (bindingState.srb.binding == update.binding)
                        {
                            valueRange = bindingState.valueRange;
                        }
                    }

                    AppendDescriptorBindingWrites(setState.vkSet, update, valueRange, writeBatch);
                }

                if (writeBatch.numWrites > 0)
                {
                    vkUpdateDescriptorSets(GVulkanRHI->GetVkDevice(), writeBatch.numWrites,
                                           writeBatch.writes.data(), 0, nullptr);
                }
            }
        }

        resolved       = setState.vkSet != VK_NULL_HANDLE;
        setState.dirty = false;
    }

    return resolved;
}

bool VulkanDescriptorSetState::BuildContentKey(uint32_t setIdx,
                                               VulkanDescriptorSetCache::ContentKey& outKey)
{
    bool complete = false;
    outKey        = {};

    if (m_pPipeline != nullptr && setIdx < MAX_NUM_DESCRIPTOR_SETS)
    {
        VulkanShader* pShader = TO_VK_SHADER(m_pPipeline->GetShader());

        outKey.layoutId = pShader->GetDescriptorSetLayoutId(setIdx);

        if (pShader != nullptr && setIdx < pShader->GetNumDescriptorSetLayouts())
        {
            complete            = true;
            size_t resourceHash = 0;

            {
                for (const BindingState& bindingState : m_setStates[setIdx].bindings)
                {
                    util::HashCombine(resourceHash, bindingState.srb.binding);
                    util::HashCombine(resourceHash, bindingState.srb.type);
                    util::HashCombine(resourceHash, bindingState.valueRange);

                    for (RHIResource* pResource : bindingState.srb.resources)
                    {
                        if (pResource != nullptr)
                        {
                            util::HashCombine(resourceHash, pResource->GetStableId());
                            util::HashCombine(resourceHash, pResource->GetGenerationId());
                        }
                    }
                }

                outKey.resourceBindingsHash = resourceHash;
            }
        }
    }

    return complete;
}

void VulkanDescriptorSetState::ClearAllSetStates()
{
    for (SetState& setState : m_setStates)
    {
        setState.vkSet = VK_NULL_HANDLE;
        setState.bindings.clear();
        setState.dirty = false;
    }

    m_packedValueBuffers.clear();
}

void VulkanDescriptorSetState::InvalidateResolvedCaches()
{
    for (SetState& setState : m_setStates)
    {
        setState.vkSet = VK_NULL_HANDLE;
    }
}

VulkanDescriptorSetState::BindingState& VulkanDescriptorSetState::FindOrAddBinding(
    SetState& setState,
    uint32_t bindingIdx,
    RHIShaderResourceType type)
{
    BindingState* pBindingState = nullptr;

    for (BindingState& bindingState : setState.bindings)
    {
        if (bindingState.srb.binding == bindingIdx)
        {
            pBindingState = &bindingState;
        }
    }

    if (pBindingState == nullptr)
    {
        BindingState& bindingState = setState.bindings.emplace_back();
        // todo: maintain order (0, 1, 2, 4...) while adding bindings
        bindingState.srb.binding = bindingIdx;
        bindingState.srb.type    = type;
        pBindingState            = &bindingState;
    }
    else
    {
        VERIFY_EXPR(pBindingState->srb.type == type);
    }

    return *pBindingState;
}

void VulkanDescriptorSetState::WriteResourceParameter(const RHIShaderResourceParameter& param)
{
    VERIFY_EXPR(m_pPipeline != nullptr);
    VERIFY_EXPR(param.set < MAX_NUM_DESCRIPTOR_SETS);

    if (m_pPipeline != nullptr && param.set < MAX_NUM_DESCRIPTOR_SETS)
    {
        SetState& setState         = m_setStates[param.set];
        BindingState& bindingState = FindOrAddBinding(setState, param.binding, param.resourceType);

        if (param.resourceType == RHIShaderResourceType::eUniformBuffer)
        {
            // A physical UBO starts at offset zero, even when this pipeline previously used
            // packed values at the same binding. Pending packed bytes must not override it.
            bindingState.dynamicOffset = 0;
            const RHIShaderResourceDescriptor* srd =
                m_pPipeline->GetShader()->GetSRDByLocation(param.set, param.binding);
            bindingState.valueRange = srd != nullptr ? srd->blockSize : 0;

            for (VulkanDescriptorSetState::PackedValueBufferState& packed : m_packedValueBuffers)
            {
                if (packed.setIdx == param.set && packed.bindingIdx == param.binding)
                {
                    packed.dirty = false;
                }
            }
        }

        const uint32_t resourceStride = GetResourceStride(param.resourceType);
        const uint32_t baseIndex      = param.arrayIndex * resourceStride;
        const uint32_t resourceCount  = baseIndex + resourceStride;

        if (bindingState.srb.resources.size() < resourceCount)
        {
            bindingState.srb.resources.resize(resourceCount);
        }

        if (resourceStride == 2)
        {
            // Batched parameters carry the texture/buffer first; descriptor bindings store
            // combined resources as sampler, texture/buffer pairs.
            bindingState.srb.resources[baseIndex]     = param.pAuxResource;
            bindingState.srb.resources[baseIndex + 1] = param.pResource;
        }
        else
        {
            bindingState.srb.resources[baseIndex] = param.pResource;
        }

        setState.dirty = true;
    }
}

bool VulkanDescriptorSetState::ValidateBindingState(const BindingState& bindingState)
{
    const RHIShaderResourceBinding& binding = bindingState.srb;
    const uint32_t stride                   = GetResourceStride(binding.type);
    bool valid = binding.type < RHIShaderResourceType::eMax && !binding.resources.empty() &&
        binding.resources.size() % stride == 0;

    if (valid)
    {
        for (size_t i = 0; i < binding.resources.size(); i += stride)
        {
            RHIResource* resource  = binding.resources[i];
            RHIResource* auxiliary = stride == 2 ? binding.resources[i + 1] : nullptr;

            // Array layouts permit unpopulated elements, but never half of a sampler/image pair.
            if (resource == nullptr && auxiliary == nullptr && binding.resources.size() > stride)
            {
                continue;
            }

            switch (binding.type)
            {
                case RHIShaderResourceType::eSampler:
                    valid &= TO_VK_SAMPLER(resource) != nullptr;
                    break;
                case RHIShaderResourceType::eTexture:
                case RHIShaderResourceType::eImage:
                case RHIShaderResourceType::eInputAttachment:
                    valid &= GetImageView(resource) != VK_NULL_HANDLE;
                    break;
                case RHIShaderResourceType::eSamplerWithTexture:
                    valid &= TO_VK_SAMPLER(resource) != nullptr &&
                        GetImageView(auxiliary) != VK_NULL_HANDLE;
                    break;

                case RHIShaderResourceType::eTextureBuffer:
                case RHIShaderResourceType::eImageBuffer:
                case RHIShaderResourceType::eSamplerWithTextureBuffer:
                {
                    VulkanBuffer* buffer = TO_VK_BUFFER(stride == 2 ? auxiliary : resource);
                    valid &= buffer != nullptr && buffer->GetVkBufferView() != VK_NULL_HANDLE;

                    if (stride == 2)
                    {
                        valid &= TO_VK_SAMPLER(resource) != nullptr;
                    }

                    break;
                }

                case RHIShaderResourceType::eUniformBuffer:
                case RHIShaderResourceType::eStorageBuffer:
                {
                    VulkanBuffer* buffer = TO_VK_BUFFER(resource);

                    if (buffer == nullptr)
                    {
                        valid = false;
                        break;
                    }

                    const uint64_t range = bindingState.valueRange > 0 ? bindingState.valueRange :
                                                                         buffer->GetRequiredSize();
                    valid &= buffer->GetVkBuffer() != VK_NULL_HANDLE && range > 0 &&
                        uint64_t(bindingState.dynamicOffset) + range <= buffer->GetRequiredSize();
                    break;
                }

                default: valid = false; break;
            }
        }
    }

    VERIFY_EXPR_MSG(valid, "Invalid Vulkan descriptor binding resources");

    return valid;
}

bool VulkanDescriptorSetState::ValidateSetState(uint32_t setIdx)
{
    VulkanShader* shader =
        m_pPipeline != nullptr ? TO_VK_SHADER(m_pPipeline->GetShader()) : nullptr;
    bool valid = shader != nullptr && setIdx < MAX_NUM_DESCRIPTOR_SETS &&
        setIdx < shader->GetNumDescriptorSetLayouts();

    if (valid && !(setIdx == kGlobalBindlessHeapIndex && shader->HasGlobalBindlessSet()))
    {
        const HeapVector<VulkanDescriptorSetState::BindingState>& bindings =
            m_setStates[setIdx].bindings;

        for (const VulkanDescriptorSetState::BindingState& binding : bindings)
        {
            const RHIShaderResourceDescriptor* descriptor =
                shader->GetSRDByLocation(setIdx, binding.srb.binding);
            valid &= descriptor != nullptr && descriptor->type == binding.srb.type &&
                binding.srb.resources.size() / GetResourceStride(binding.srb.type) <=
                    (descriptor->bindless ? shader->GetDescriptorSetVariableCount(setIdx) :
                                            descriptor->arraySize);
            valid &= ValidateBindingState(binding);
        }

        for (RHIShaderResourceDescriptor const& descriptor : (*shader->GetSRDTable())[setIdx])
        {
            if (!descriptor.bindless && descriptor.arraySize == 1)
            {
                valid &=
                    std::any_of(bindings.begin(), bindings.end(),
                                [bindingIndex = descriptor.binding](const BindingState& binding) {
                                    return binding.srb.binding == bindingIndex;
                                });
            }
        }
    }

    VERIFY_EXPR_MSG(valid, "Invalid Vulkan descriptor set bindings");

    return valid;
}

void VulkanDescriptorSetState::BuildSetUpdates(uint32_t setIndex,
                                               HeapVector<RHIShaderResourceBinding>& outUpdates)
{
    outUpdates.clear();

    if (setIndex < MAX_NUM_DESCRIPTOR_SETS)
    {
        for (const BindingState& bindingState : m_setStates[setIndex].bindings)
        {
            if (ValidateBindingState(bindingState))
            {
                outUpdates.push_back(bindingState.srb);
            }
        }

        std::sort(outUpdates.begin(), outUpdates.end(),
                  [](const RHIShaderResourceBinding& lhs, const RHIShaderResourceBinding& rhs) {
                      return lhs.binding < rhs.binding;
                  });
    }
}

VulkanDescriptorPoolSetContainer* VulkanDescriptorSetState::SyncContainerEpoch(
    FVulkanCommandListContext* pContext)
{
    VulkanDescriptorPoolSetContainer* pContainer = nullptr;

    if (pContext != nullptr)
    {
        pContainer = pContext->AcquireDescriptorPoolSetContainer();

        if (pContainer != nullptr)
        {
            const uint64_t acquireEpoch = pContainer->GetAcquireEpoch();

            if (m_containerEpoch != acquireEpoch)
            {
                InvalidateResolvedCaches();
                m_containerEpoch = acquireEpoch;
            }
        }
    }

    return pContainer;
}

void VulkanDescriptorSetState::BuildDescriptorSetList(
    FVulkanCommandListContext* pContext,
    HeapVector<VkDescriptorSet>& outDescriptorSets,
    uint32_t& outFirstSet,
    HeapVector<uint32_t>& outDynamicOffsets)
{
    VulkanShader* pShader = TO_VK_SHADER(m_pPipeline->GetShader());

    if (pShader != nullptr)
    {
        const uint32_t numSets = pShader->GetNumDescriptorSetLayouts();

        uint32_t firstUsed = MAX_NUM_DESCRIPTOR_SETS;
        uint32_t lastUsed  = 0;
        bool anyUsed       = false;

        for (uint32_t setIdx = 0; setIdx < numSets; setIdx++)
        {
            const SetState& setState = m_setStates[setIdx];

            if (setState.vkSet == VK_NULL_HANDLE && setState.bindings.empty())
            {
                continue;
            }

            firstUsed = std::min(firstUsed, setIdx);
            lastUsed  = setIdx;
            anyUsed   = true;
        }

        if (anyUsed)
        {
            outFirstSet = firstUsed;

            const uint32_t descriptorSetCount = lastUsed - firstUsed + 1;
            outDescriptorSets.resize(descriptorSetCount);

            bool allSetsResolved = true;

            for (uint32_t i = firstUsed; i <= lastUsed; i++)
            {
                SetState& setState     = m_setStates[i];
                const bool needResolve = setState.dirty || setState.vkSet == VK_NULL_HANDLE;

                if (needResolve && !ResolveSet(i, pContext))
                {
                    LOGE("[VulkanRHI][VulkanDescriptorSetState]: ResolveSet failed!");
                    allSetsResolved = false;
                    break;
                }

                if (allSetsResolved)
                {
                    outDescriptorSets[i - firstUsed] = setState.vkSet;
                    AppendDynamicOffsetsForSet(i, outDynamicOffsets);
                }
            }

            if (!allSetsResolved)
            {
                outDescriptorSets.clear();
                outDynamicOffsets.clear();
                outFirstSet = 0;
            }
        }
    }
}

void VulkanDescriptorSetState::AppendDynamicOffsetsForSet(uint32_t setIdx,
                                                          HeapVector<uint32_t>& outDynamicOffsets)
{
    m_dynamicOffsetScratch.clear();
    VulkanShader* pShader = TO_VK_SHADER(m_pPipeline->GetShader());

    if (setIdx < MAX_NUM_DESCRIPTOR_SETS && pShader != nullptr)
    {
        const RHIShaderResourceDescriptorTable* pSRDTable = pShader->GetSRDTable();

        for (const RHIShaderResourceDescriptor& srd : (*pSRDTable)[setIdx])
        {
            if (srd.type != RHIShaderResourceType::eUniformBuffer)
            {
                continue;
            }

            // @note: currently only support dynamic UBO
            //        UBOs are dynamic by default in current implementation
            uint32_t offset = 0;

            for (const BindingState& bindingState : m_setStates[setIdx].bindings)
            {
                if (bindingState.srb.binding == srd.binding)
                {
                    offset = bindingState.dynamicOffset;
                    break;
                }
            }

            m_dynamicOffsetScratch.push_back({srd.binding, offset});
        }

        std::stable_sort(m_dynamicOffsetScratch.begin(), m_dynamicOffsetScratch.end(),
                         [](const DynamicOffsetEntry& lhs, const DynamicOffsetEntry& rhs) {
                             return lhs.bindingIdx < rhs.bindingIdx;
                         });

        for (const DynamicOffsetEntry& dynamicOffset : m_dynamicOffsetScratch)
        {
            outDynamicOffsets.push_back(dynamicOffset.offset);
        }
    }
}

void VulkanDescriptorSetState::SetPackedValueParameter(uint32_t setIdx,
                                                       uint32_t bindingIdx,
                                                       uint32_t byteSize,
                                                       const uint8_t* pData)
{
    VERIFY_EXPR(pData != nullptr);

    if (pData != nullptr && byteSize > 0)
    {
        PackedValueBufferState* pBufferState = FindOrAddPackedValueBuffer(setIdx, bindingIdx);

        if (pBufferState != nullptr)
        {
            if (pBufferState->bytes.size() < byteSize)
            {
                pBufferState->bytes.resize(byteSize);
            }

            std::memcpy(pBufferState->bytes.data(), pData, byteSize);
            pBufferState->dirty = true;
        }
    }
}

void VulkanDescriptorSetState::FlushPackedValueBuffers()
{
    VulkanUniformBufferAllocator* pAllocator = GVulkanRHI->GetUniformBufferAllocator();
    VERIFY_EXPR(pAllocator != nullptr);

    if (pAllocator != nullptr)
    {
        for (PackedValueBufferState& bufferState : m_packedValueBuffers)
        {
            if (bufferState.dirty && bufferState.blockSize > 0)
            {
                VulkanUniformBufferBlock block = pAllocator->Alloc(bufferState.blockSize);
                VERIFY_EXPR(block.IsValid());

                if (block.IsValid())
                {
                    std::memcpy(block.pMapped, bufferState.bytes.data(), bufferState.blockSize);

                    SetState& setState         = m_setStates[bufferState.setIdx];
                    BindingState& bindingState = FindOrAddBinding(
                        setState, bufferState.bindingIdx, RHIShaderResourceType::eUniformBuffer);

                    const bool bufferChanged = bindingState.srb.resources.size() != 1 ||
                        bindingState.srb.resources[0] != block.pBuffer ||
                        bindingState.valueRange != bufferState.blockSize;
                    bindingState.srb.resources.clear();
                    bindingState.srb.resources.push_back(block.pBuffer);
                    bindingState.dynamicOffset = block.offset;
                    bindingState.valueRange    = bufferState.blockSize;
                    setState.dirty |= bufferChanged;
                    bufferState.dirty = false;
                }
            }
        }
    }
}

VulkanDescriptorSetState::PackedValueBufferState* VulkanDescriptorSetState::
    FindOrAddPackedValueBuffer(uint32_t setIdx, uint32_t bindingIdx)
{
    PackedValueBufferState* pBufferState = nullptr;

    for (PackedValueBufferState& bufferState : m_packedValueBuffers)
    {
        if (bufferState.setIdx == setIdx && bufferState.bindingIdx == bindingIdx)
        {
            pBufferState = &bufferState;
            break;
        }
    }

    if (pBufferState == nullptr)
    {
        const RHIShaderResourceDescriptor* pSRD =
            m_pPipeline->GetShader()->GetSRDByLocation(setIdx, bindingIdx);
        VERIFY_EXPR(pSRD != nullptr);

        PackedValueBufferState& bufferState = m_packedValueBuffers.emplace_back();
        bufferState.setIdx                  = setIdx;
        bufferState.bindingIdx              = bindingIdx;
        bufferState.blockSize               = pSRD->blockSize;
        bufferState.bytes.resize(pSRD->blockSize);

        pBufferState = &bufferState;
    }

    return pBufferState;
}
} // namespace zen
