#include "Graphics/RHI/RHICommon.h"
#include "Graphics/VulkanRHI/VulkanDescriptorPool.h"
#include "Graphics/VulkanRHI/VulkanDescriptorState.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanAccelerationStructure.h"
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

    if (type == RHIShaderResourceType::eSamplerWithTexture || type == RHIShaderResourceType::eSamplerWithTextureBuffer)
    {
        stride = 2;
    }

    return stride;
}

VkImageView GetImageView(RHIResource* pResource)
{
    VkImageView imageView = VK_NULL_HANDLE;

    if (pResource != nullptr && pResource->GetResourceType() == RHIResourceType::eTextureView)
    {
        imageView = TO_VK_TEXTURE_VIEW(pResource)->GetVkImageView();
    }
    else if (pResource != nullptr && pResource->GetResourceType() == RHIResourceType::eTexture)
    {
        const VulkanTextureView* pDefaultView = TO_VK_TEXTURE_VIEW(TO_VK_TEXTURE(pResource)->GetDefaultView());

        imageView                             = pDefaultView != nullptr ? pDefaultView->GetVkImageView() : VK_NULL_HANDLE;
    }

    return imageView;
}

struct DescriptorWriteBatch
{
    explicit DescriptorWriteBatch(uint32_t maxDescriptorCount) :
        writes(maxDescriptorCount),
        imageInfos(maxDescriptorCount),
        bufferInfos(maxDescriptorCount),
        bufferViews(maxDescriptorCount),
        accelerationInfos(maxDescriptorCount),
        accelerationStructures(maxDescriptorCount)
    {}

    HeapVector<VkWriteDescriptorSet>                         writes;
    HeapVector<VkDescriptorImageInfo>                        imageInfos;
    HeapVector<VkDescriptorBufferInfo>                       bufferInfos;
    HeapVector<VkBufferView>                                 bufferViews;
    HeapVector<VkWriteDescriptorSetAccelerationStructureKHR> accelerationInfos;
    HeapVector<VkAccelerationStructureKHR>                   accelerationStructures;
    uint32_t                                                 numWrites{0};
};

bool ResolveDescriptorBinding(const RHIShaderResourceBinding& binding,
                              uint32_t                        valueRange,
                              VectorView<const uint32_t>      offsets,
                              VkDescriptorSet                 set,
                              DescriptorWriteBatch*           batch)
{
    const uint32_t stride = GetResourceStride(binding.type);
    const uint32_t count  = static_cast<uint32_t>(binding.resources.size()) / stride;
    bool           valid  = binding.type < RHIShaderResourceType::eMax && count > 0 && binding.resources.size() % stride == 0;
    for (uint32_t element = 0; valid && element < count; ++element)
    {
        const uint32_t index     = element * stride;
        RHIResource*   resource  = binding.resources[index];
        RHIResource*   auxiliary = stride == 2 ? binding.resources[index + 1] : nullptr;
        if (resource == nullptr && auxiliary == nullptr && count > 1)
        {
            continue;
        }
        VkDescriptorImageInfo      image{};
        VkDescriptorBufferInfo     bufferInfo{};
        VkBufferView               view      = VK_NULL_HANDLE;
        VkAccelerationStructureKHR structure = VK_NULL_HANDLE;
        VkWriteDescriptorSet       write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet          = set;
        write.dstBinding      = binding.binding;
        write.dstArrayElement = element;
        write.descriptorCount = 1;
        write.descriptorType  = ShaderResourceTypeToVkDescriptorType(binding.type);
        switch (binding.type)
        {
            case RHIShaderResourceType::eAccelerationStructure:
                valid = resource != nullptr && resource->GetResourceType() == RHIResourceType::eAccelerationStructure;
                if (valid)
                {
                    VulkanAccelerationStructure* acceleration = static_cast<VulkanAccelerationStructure*>(resource);
                    structure                                 = acceleration->GetVkHandle();
                    valid = acceleration->GetType() == RHIAccelerationStructureType::eTopLevel && structure != VK_NULL_HANDLE;
                }
                break;
            case RHIShaderResourceType::eSampler:
            case RHIShaderResourceType::eSamplerWithTexture:
            {
                VulkanSampler* sampler = TryVulkanSampler(resource);
                image.sampler          = sampler != nullptr ? sampler->GetVkSampler() : VK_NULL_HANDLE;
                valid                  = image.sampler != VK_NULL_HANDLE;
                if (binding.type == RHIShaderResourceType::eSamplerWithTexture)
                {
                    image.imageView   = GetImageView(auxiliary);
                    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    valid             = valid && image.imageView != VK_NULL_HANDLE;
                }
                write.pImageInfo = &image;
                break;
            }
            case RHIShaderResourceType::eTexture:
            case RHIShaderResourceType::eImage:
            case RHIShaderResourceType::eInputAttachment:
                image.imageView   = GetImageView(resource);
                image.imageLayout = binding.type == RHIShaderResourceType::eImage ? VK_IMAGE_LAYOUT_GENERAL
                                                                                  : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                valid             = image.imageView != VK_NULL_HANDLE;
                write.pImageInfo  = &image;
                break;
            case RHIShaderResourceType::eTextureBuffer:
            case RHIShaderResourceType::eSamplerWithTextureBuffer:
            case RHIShaderResourceType::eImageBuffer:
            {
                VulkanBuffer* buffer   = TryVulkanBuffer(stride == 2 ? auxiliary : resource);
                view                   = buffer != nullptr ? buffer->GetVkBufferView() : VK_NULL_HANDLE;
                valid                  = view != VK_NULL_HANDLE && (stride != 2 || TryVulkanSampler(resource) != nullptr);
                write.pTexelBufferView = &view;
                break;
            }
            case RHIShaderResourceType::eUniformBuffer:
            case RHIShaderResourceType::eStorageBuffer:
            {
                VulkanBuffer* buffer = TryVulkanBuffer(resource);
                valid                = buffer != nullptr;
                if (valid)
                {
                    bufferInfo.buffer         = buffer->GetVkBuffer();
                    bufferInfo.range          = valueRange > 0 ? valueRange : buffer->GetRequiredSize();
                    const uint64_t     offset = index < offsets.size() ? offsets[index] : 0;
                    const VkDeviceSize alignment =
                        GVulkanRHI->GetDevice()->GetPhysicalDeviceProperties().limits.minUniformBufferOffsetAlignment;
                    valid = bufferInfo.buffer != VK_NULL_HANDLE && bufferInfo.range > 0 && offset <= buffer->GetRequiredSize()
                         && bufferInfo.range <= buffer->GetRequiredSize() - offset && (offset == 0 || offset % alignment == 0);
                }
                write.pBufferInfo = &bufferInfo;
                break;
            }
            default: valid = false; break;
        }
        if (valid && batch != nullptr)
        {
            const uint32_t slot = batch->numWrites++;
            VERIFY_EXPR(slot < batch->writes.size() && set != VK_NULL_HANDLE);
            batch->imageInfos[slot]             = image;
            batch->bufferInfos[slot]            = bufferInfo;
            batch->bufferViews[slot]            = view;
            batch->accelerationStructures[slot] = structure;
            if (write.pImageInfo != nullptr)
            {
                write.pImageInfo = &batch->imageInfos[slot];
            }
            if (write.pBufferInfo != nullptr)
            {
                write.pBufferInfo = &batch->bufferInfos[slot];
            }
            if (write.pTexelBufferView != nullptr)
            {
                write.pTexelBufferView = &batch->bufferViews[slot];
            }
            if (structure != VK_NULL_HANDLE)
            {
                VkWriteDescriptorSetAccelerationStructureKHR& info = batch->accelerationInfos[slot];
                info                            = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
                info.accelerationStructureCount = 1;
                info.pAccelerationStructures    = &batch->accelerationStructures[slot];
                write.pNext                     = &info;
            }
            batch->writes[slot] = write;
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

        // Per-set state covers exactly the bound shader's layout; the device bounds that count.
        const VulkanShader* shader = pPipeline != nullptr ? TO_CVK_SHADER(pPipeline->GetShader()) : nullptr;

        m_setStates.resize(shader != nullptr ? shader->GetNumDescriptorSetLayouts() : 0);

        m_packedValueBuffers.clear();

        m_cacheRevision          = 0;

        m_pPipeline              = pPipeline;

        m_parametersValid        = true;

        m_lastWorkloadGeneration = 0;
    }
}

bool VulkanDescriptorSetState::SetShaderParameters(RHIShaderParameterView parameters,
                                                   uint64_t               recordedEpoch,
                                                   uint64_t               transaction)
{
    VulkanBindlessDescriptorPoolManager* manager = GVulkanRHI->GetBindlessDescriptorPoolManager();

    bool valid                                   = m_parametersValid && m_pPipeline != nullptr;

    for (const RHIShaderValueParameter& parameter : parameters.GetValueParams())
    {
        valid &= parameter.set < m_setStates.size();
    }

    for (const RHIShaderResourceParameter& parameter : parameters.GetResourceParams())
    {
        valid &= parameter.set < m_setStates.size()
              && (parameter.bufferOffset == 0 || parameter.resourceType == RHIShaderResourceType::eUniformBuffer);
    }

    const VectorView<const RHIShaderResourceParameter> bindless = parameters.GetBindlessParams();

    for (const RHIShaderResourceParameter& parameter : bindless)
    {
        valid &= parameter.bufferOffset == 0;
    }

    if (valid && !bindless.empty())
    {
        valid = manager->CanRegisterBindlessResources(bindless, recordedEpoch, transaction);
    }

    if (valid)
    {
        for (const RHIShaderValueParameter& parameter : parameters.GetValueParams())
        {
            const VectorView<const uint8_t> bytes = parameters.GetValueBytes(parameter);

            if (!bytes.empty())
            {
                SetPackedValueParameter(parameter.set, parameter.binding, parameter.byteSize, bytes.data());
            }
        }

        for (const RHIShaderResourceParameter& parameter : parameters.GetResourceParams())
        {
            WriteResourceParameter(parameter);
        }

        for (const RHIShaderResourceParameter& parameter : bindless)
        {
            valid &= manager->RegisterBindlessResource(parameter.pResource, parameter.arrayIndex, nullptr, recordedEpoch,
                                                       transaction);
        }
    }

    m_parametersValid &= valid;

    return m_parametersValid;
}

bool VulkanDescriptorSetState::FlushPendingDescriptorWrites(FVulkanCommandListContext*   pContext,
                                                            HeapVector<VkDescriptorSet>& outDescriptorSets,
                                                            uint32_t&                    outFirstSet,
                                                            HeapVector<uint32_t>&        outDynamicOffsets)
{
    bool valid = m_parametersValid && pContext != nullptr && m_pPipeline != nullptr;

    if (valid)
    {
        VulkanShader* shader = TO_VK_SHADER(m_pPipeline->GetShader());

        if (shader != nullptr && shader->HasGlobalBindlessSet())
        {
            pContext->RecordCurrentBindlessEpoch();

            GVulkanRHI->GetBindlessDescriptorPoolManager()->Flush();
        }

        const VulkanDescriptorSetCache* cache         = GVulkanRHI->GetDescriptorPoolManager2()->GetContentCache();

        bool reuse                                    = m_lastContext == pContext && m_lastWorkloadGeneration != 0
                                                     && m_lastWorkloadGeneration == pContext->GetWorkloadGeneration()
                                                     && m_lastCacheRevision == cache->GetRevision();

        const VulkanUniformBufferAllocator* allocator = GVulkanRHI->GetUniformBufferAllocator();

        for (const SetState& set : m_setStates)
        {
            reuse &= !set.dirty;

            for (const BindingState& binding : set.bindings)
            {
                reuse &= binding.uniformBlockId == 0
                      || allocator->GetBlockGeneration(binding.uniformBlockId) == binding.uniformGeneration;
            }
        }

        for (const PackedValueBufferState& value : m_packedValueBuffers)
        {
            reuse &= !value.dirty;
        }

        if (reuse)
        {
            outDescriptorSets = m_resolvedSets;

            outDynamicOffsets = m_resolvedOffsets;

            outFirstSet       = m_resolvedFirstSet;
        }
        else
        {
            outDescriptorSets.clear();

            outDynamicOffsets.clear();

            outFirstSet = 0;

            valid       = FlushPackedValueBuffers();

            if (valid)
            {
                valid = BuildDescriptorSetList(pContext, outDescriptorSets, outFirstSet, outDynamicOffsets);
            }

            m_resolvedSets           = outDescriptorSets;

            m_resolvedOffsets        = outDynamicOffsets;

            m_resolvedFirstSet       = outFirstSet;

            m_lastContext            = pContext;

            m_lastWorkloadGeneration = pContext->GetWorkloadGeneration();

            m_lastCacheRevision      = cache->GetRevision();
        }
    }

    if (!valid)
    {
        outDescriptorSets.clear();

        outDynamicOffsets.clear();

        outFirstSet              = 0;

        m_lastWorkloadGeneration = 0;
    }

    return valid;
}

void VulkanDescriptorSetState::Reset()
{
    m_parametersValid = true;

    ClearAllSetStates();

    m_setStates.clear();

    m_packedValueBuffers.clear();


    m_cacheRevision          = 0;

    m_pPipeline              = nullptr;

    m_lastWorkloadGeneration = 0;

    m_resolvedSets.clear();

    m_resolvedOffsets.clear();
}

VkDescriptorSet VulkanDescriptorSetState::AcquireSetHandle(uint32_t                   setIdx,
                                                           FVulkanCommandListContext* pContext,
                                                           bool&                      outNeedsWrite)
{
    VkDescriptorSet descriptorSet  = VK_NULL_HANDLE;

    outNeedsWrite                  = false;

    m_setStates[setIdx].pContainer = nullptr;

    if (m_pPipeline != nullptr)
    {
        VulkanShader* pShader = TO_VK_SHADER(m_pPipeline->GetShader());

        if (setIdx == kGlobalBindlessHeapIndex && pShader->HasGlobalBindlessSet())
        {
            outNeedsWrite = false;

            descriptorSet = GVulkanRHI->GetBindlessDescriptorPoolManager()->GetGlobalBindlessSet();
        }
        else
        {
            VERIFY_EXPR(setIdx != kGlobalBindlessHeapIndex || pShader->HasGlobalBindlessSet());
            VulkanDescriptorSetCache::ContentKey contentKey{};

            VulkanDescriptorSetCache* pCache = GVulkanRHI->GetDescriptorPoolManager2()->GetContentCache();

            if (pCache != nullptr && BuildContentKey(setIdx, contentKey))
            {
                descriptorSet = pCache->Find(contentKey, m_setStates[setIdx].pContainer);

                if (descriptorSet != VK_NULL_HANDLE)
                {
                    outNeedsWrite = false;
                }
                else
                {
                    descriptorSet =
                        pCache->Allocate(pShader->GetDescriptorPoolKey(setIdx), pShader->GetDescriptorSetLayoutHandle(setIdx),
                                         m_setStates[setIdx].pContainer);

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

    if (setIdx < m_setStates.size() && ValidateSetState(setIdx))
    {
        SetState& setState = m_setStates[setIdx];

        if (setState.dirty || setState.vkSet == VK_NULL_HANDLE)
        {
            bool needsWrite = false;

            setState.vkSet  = AcquireSetHandle(setIdx, pContext, needsWrite);

            if (setState.vkSet != VK_NULL_HANDLE && needsWrite)
            {
                uint32_t maxDescriptorWrites = 0;
                for (const BindingState& binding : setState.bindings)
                {
                    maxDescriptorWrites +=
                        static_cast<uint32_t>(binding.srb.resources.size()) / GetResourceStride(binding.srb.type);
                }
                DescriptorWriteBatch writeBatch(maxDescriptorWrites);
                bool                 valid = true;
                for (const BindingState& binding : setState.bindings)
                {
                    valid = valid
                         && ResolveDescriptorBinding(binding.srb, binding.valueRange, binding.dynamicOffsets, setState.vkSet,
                                                     &writeBatch);
                }
                VulkanDescriptorSetCache::ContentKey key{};
                valid = valid && BuildContentKey(setIdx, key);
                if (valid)
                {
                    if (writeBatch.numWrites > 0)
                    {
                        vkUpdateDescriptorSets(GVulkanRHI->GetVkDevice(), writeBatch.numWrites, writeBatch.writes.data(), 0,
                                               nullptr);
                    }
                    GVulkanRHI->GetDescriptorPoolManager2()->GetContentCache()->Publish(key, setState.vkSet,
                                                                                        setState.pContainer);
                }
                else
                {
                    setState.vkSet      = VK_NULL_HANDLE;
                    setState.pContainer = nullptr;
                }
            }
        }

        resolved       = setState.vkSet != VK_NULL_HANDLE;

        setState.dirty = false;
    }

    return resolved;
}

bool VulkanDescriptorSetState::BuildContentKey(uint32_t setIdx, VulkanDescriptorSetCache::ContentKey& outKey)
{
    bool complete = false;

    outKey        = {};

    if (m_pPipeline != nullptr && setIdx < m_setStates.size())
    {
        VulkanShader* pShader = TO_VK_SHADER(m_pPipeline->GetShader());

        if (pShader != nullptr && setIdx < pShader->GetNumDescriptorSetLayouts())
        {
            complete        = true;

            outKey.layoutId = pShader->GetDescriptorSetLayoutId(setIdx);

            HeapVector<const BindingState*> bindingOrder;

            for (const BindingState& binding : m_setStates[setIdx].bindings)
            {
                bindingOrder.push_back(&binding);
            }

            std::sort(bindingOrder.begin(), bindingOrder.end(),
                      [](const BindingState* a, const BindingState* b) { return a->srb.binding < b->srb.binding; });

            outKey.bindings.push_back(bindingOrder.size());

            for (const BindingState* binding : bindingOrder)
            {
                outKey.bindings.push_back(binding->srb.binding);

                outKey.bindings.push_back(ToUnderlying(binding->srb.type));

                outKey.bindings.push_back(binding->valueRange);

                outKey.bindings.push_back(binding->srb.resources.size());

                // Dynamic offsets are bind-time state, not descriptor contents.
                for (const RHIResource* resource : binding->srb.resources)
                {
                    outKey.bindings.push_back(resource != nullptr ? resource->GetStableId() : 0);
                }
            }
        }
    }

    return complete;
}

void VulkanDescriptorSetState::ClearAllSetStates()
{
    for (SetState& setState : m_setStates)
    {
        setState.vkSet      = VK_NULL_HANDLE;

        setState.pContainer = nullptr;

        setState.bindings.clear();

        setState.dirty = false;
    }

    m_packedValueBuffers.clear();
}

void VulkanDescriptorSetState::InvalidateResolvedCaches()
{
    for (SetState& setState : m_setStates)
    {
        setState.vkSet      = VK_NULL_HANDLE;

        setState.pContainer = nullptr;
    }
}

VulkanDescriptorSetState::BindingState& VulkanDescriptorSetState::FindOrAddBinding(SetState&             setState,
                                                                                   uint32_t              bindingIdx,
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

    VERIFY_EXPR(param.set < m_setStates.size());

    if (m_pPipeline != nullptr && param.set < m_setStates.size())
    {
        if (param.bufferOffset != 0 && param.resourceType != RHIShaderResourceType::eUniformBuffer)
        {
            m_parametersValid = false;

            LOGE("Only uniform-buffer parameters accept a buffer offset");
        }

        SetState& setState         = m_setStates[param.set];

        BindingState& bindingState = FindOrAddBinding(setState, param.binding, param.resourceType);

        if (param.resourceType == RHIShaderResourceType::eUniformBuffer)
        {
            // Each external UBO element has its own offset, defaulting to zero.
            // Pending packed bytes must not override these explicit bindings.
            if (bindingState.dynamicOffsets.size() <= param.arrayIndex)
            {
                bindingState.dynamicOffsets.resize(param.arrayIndex + 1);
            }

            bindingState.dynamicOffsets[param.arrayIndex] = param.bufferOffset;

            bindingState.uniformBlockId                   = 0;

            bindingState.uniformGeneration                = 0;

            const RHIShaderResourceDescriptor* srd = m_pPipeline->GetShader()->GetSRDByLocation(param.set, param.binding);

            bindingState.valueRange                = srd != nullptr ? srd->blockSize : 0;

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

bool VulkanDescriptorSetState::ValidateBindingState(const BindingState& binding)
{
    const bool valid =
        ResolveDescriptorBinding(binding.srb, binding.valueRange, binding.dynamicOffsets, VK_NULL_HANDLE, nullptr);
    if (!valid)
    {
        LOGE("Invalid Vulkan descriptor binding resources");
    }
    return valid;
}

bool VulkanDescriptorSetState::ValidateSetState(uint32_t setIdx)
{
    VulkanShader* shader = m_pPipeline != nullptr ? TO_VK_SHADER(m_pPipeline->GetShader()) : nullptr;

    bool valid           = shader != nullptr && setIdx < m_setStates.size() && setIdx < shader->GetNumDescriptorSetLayouts();

    if (valid && !(setIdx == kGlobalBindlessHeapIndex && shader->HasGlobalBindlessSet()))
    {
        const HeapVector<VulkanDescriptorSetState::BindingState>& bindings = m_setStates[setIdx].bindings;

        for (const VulkanDescriptorSetState::BindingState& binding : bindings)
        {
            const RHIShaderResourceDescriptor* descriptor  = shader->GetSRDByLocation(setIdx, binding.srb.binding);

            valid                                         &= descriptor != nullptr && descriptor->type == binding.srb.type
                                                          && binding.srb.resources.size() / GetResourceStride(binding.srb.type) <= descriptor->arraySize;

            valid                                         &= ValidateBindingState(binding);
        }

        for (RHIShaderResourceDescriptor const& descriptor : (*shader->GetSRDTable())[setIdx])
        {
            if (descriptor.arraySize == 1)
            {
                valid &= std::any_of(bindings.begin(), bindings.end(),
                                     [bindingIndex = descriptor.binding](const BindingState& binding) {
                                         return binding.srb.binding == bindingIndex;
                                     });
            }
        }
    }

    if (!valid)
    {
        LOGE("Invalid Vulkan descriptor set bindings");
    }

    return valid;
}

void VulkanDescriptorSetState::SyncCacheRevision(const VulkanDescriptorSetCache& cache)
{
    if (m_cacheRevision != cache.GetRevision())
    {
        InvalidateResolvedCaches();

        m_cacheRevision = cache.GetRevision();
    }
}

bool VulkanDescriptorSetState::BuildDescriptorSetList(FVulkanCommandListContext*   pContext,
                                                      HeapVector<VkDescriptorSet>& outDescriptorSets,
                                                      uint32_t&                    outFirstSet,
                                                      HeapVector<uint32_t>&        outDynamicOffsets)
{
    VulkanShader* pShader = TO_VK_SHADER(m_pPipeline->GetShader());

    bool allSetsResolved  = pShader != nullptr;

    if (pShader != nullptr)
    {
        const VulkanDescriptorSetCache* cache = GVulkanRHI->GetDescriptorPoolManager2()->GetContentCache();

        VERIFY_EXPR(cache != nullptr);

        SyncCacheRevision(*cache);

        const uint32_t numSets = pShader->GetNumDescriptorSetLayouts();

        VERIFY_EXPR(numSets == m_setStates.size());

        uint32_t firstUsed = numSets;

        uint32_t lastUsed  = 0;

        bool anyUsed       = false;

        for (uint32_t setIdx = 0; setIdx < numSets; setIdx++)
        {
            const SetState& setState = m_setStates[setIdx];

            const bool declaredSet   = !(*pShader->GetSRDTable())[setIdx].empty();

            // Resolve declared sets even without parameters: scalar bindings must be present,
            // and partially-bound arrays still need a native set. Only empty layout gaps may be skipped.
            if (!declaredSet && setState.vkSet == VK_NULL_HANDLE && setState.bindings.empty())
            {
                continue;
            }

            firstUsed = std::min(firstUsed, setIdx);

            lastUsed  = setIdx;

            anyUsed   = true;
        }

        if (anyUsed)
        {
            outFirstSet                       = firstUsed;

            const uint32_t descriptorSetCount = lastUsed - firstUsed + 1;

            outDescriptorSets.resize(descriptorSetCount);

            for (uint32_t i = firstUsed; i <= lastUsed; i++)
            {
                // Resolving an earlier set can rotate the cache. Check again before
                // dereferencing a resolved handle/container from a previous draw.
                SyncCacheRevision(*cache);

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
                    if (setState.pContainer != nullptr)
                    {
                        // Retain on every bind, including unchanged bindings in a new
                        // workload. Queue retirement releases this ownership.
                        pContext->RecordDescriptorPool(setState.pContainer);
                    }

                    for (const BindingState& binding : setState.bindings)
                    {
                        pContext->RecordUniformBufferBlock(binding.uniformBlockId);
                    }

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

    return allSetsResolved;
}

void VulkanDescriptorSetState::AppendDynamicOffsetsForSet(uint32_t setIdx, HeapVector<uint32_t>& outDynamicOffsets)
{
    const VulkanShader* shader = TO_CVK_SHADER(m_pPipeline->GetShader());

    if (setIdx < m_setStates.size() && shader != nullptr)
    {
        for (const VulkanShader::DynamicOffsetSlot& slot : shader->GetDynamicOffsetSlots(setIdx))
        {
            const BindingState* binding = nullptr;

            for (const BindingState& state : m_setStates[setIdx].bindings)
            {
                if (state.srb.binding == slot.binding)
                {
                    binding = &state;

                    break;
                }
            }

            for (uint32_t element = 0; element < slot.count; ++element)
            {
                outDynamicOffsets.push_back(
                    binding != nullptr && element < binding->dynamicOffsets.size() ? binding->dynamicOffsets[element] : 0);
            }
        }
    }
}

void VulkanDescriptorSetState::SetPackedValueParameter(uint32_t       setIdx,
                                                       uint32_t       bindingIdx,
                                                       uint32_t       byteSize,
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

bool VulkanDescriptorSetState::FlushPackedValueBuffers()
{
    VulkanUniformBufferAllocator* pAllocator = GVulkanRHI->GetUniformBufferAllocator();

    bool valid                               = pAllocator != nullptr;

    if (pAllocator != nullptr)
    {
        for (PackedValueBufferState& bufferState : m_packedValueBuffers)
        {
            SetState& setState = m_setStates[bufferState.setIdx];

            BindingState& bindingState =
                FindOrAddBinding(setState, bufferState.bindingIdx, RHIShaderResourceType::eUniformBuffer);

            // Cached values retain CPU bytes, not transient buffer ownership. Refresh
            // before resolving descriptors if the slot was reused or its tail was trimmed.
            if (bindingState.uniformBlockId != 0
                && pAllocator->GetBlockGeneration(bindingState.uniformBlockId) != bindingState.uniformGeneration)
            {
                bufferState.dirty = true;

                // The old native buffer may already have been destroyed. Do not leave
                // a dangling resource in descriptor resolution if the upload must retry.
                bindingState.srb.resources.clear();

                bindingState.uniformBlockId    = 0;

                bindingState.uniformGeneration = 0;

                setState.dirty                 = true;
            }

            if (bufferState.dirty && bufferState.blockSize > 0)
            {
                VulkanUniformBufferBlock block = pAllocator->Alloc(bufferState.blockSize);

                if (!block.IsValid())
                {
                    valid = false;

                    break;
                }

                if (block.IsValid())
                {
                    std::memcpy(block.pMapped, bufferState.bytes.data(), bufferState.blockSize);

                    const bool bufferChanged = bindingState.srb.resources.size() != 1
                                            || bindingState.srb.resources[0] != block.pBuffer
                                            || bindingState.uniformGeneration != block.generation
                                            || bindingState.valueRange != bufferState.blockSize;

                    bindingState.srb.resources.clear();

                    bindingState.srb.resources.push_back(block.pBuffer);

                    bindingState.dynamicOffsets.resize(1);

                    bindingState.dynamicOffsets[0]  = block.offset;

                    bindingState.valueRange         = bufferState.blockSize;

                    bindingState.uniformBlockId     = block.blockId;

                    bindingState.uniformGeneration  = block.generation;

                    setState.dirty                 |= bufferChanged;

                    bufferState.dirty               = false;
                }
            }
        }
    }

    return valid;
}

VulkanDescriptorSetState::PackedValueBufferState* VulkanDescriptorSetState::FindOrAddPackedValueBuffer(uint32_t setIdx,
                                                                                                       uint32_t bindingIdx)
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
        const RHIShaderResourceDescriptor* pSRD = m_pPipeline->GetShader()->GetSRDByLocation(setIdx, bindingIdx);

        if (pSRD == nullptr || pSRD->type != RHIShaderResourceType::eUniformBuffer || pSRD->arraySize != 1 || pSRD->bindless)
        {
            m_parametersValid = false;

            LOGE("Packed uniform values require a single uniform-buffer descriptor");
        }
        else
        {
            PackedValueBufferState& bufferState = m_packedValueBuffers.emplace_back();

            bufferState.setIdx                  = setIdx;

            bufferState.bindingIdx              = bindingIdx;

            bufferState.blockSize               = pSRD->blockSize;

            bufferState.bytes.resize(pSRD->blockSize);

            pBufferState = &bufferState;
        }
    }

    return pBufferState;
}
} // namespace zen
