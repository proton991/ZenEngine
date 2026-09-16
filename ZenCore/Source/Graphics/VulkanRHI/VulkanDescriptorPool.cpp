#include "Graphics/VulkanRHI/VulkanDescriptorPool.h"
#include "Graphics/RHI/RHICommon.h"
#include "Graphics/RHI/RHIResource.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Memory/Memory.h"
#include "Utils/Errors.h"

#include <algorithm>
#include <limits>

namespace zen
{
constexpr uint32_t kInitialSetsPerPool       = 32;
constexpr uint32_t kMaxSetsPerPool           = 512;
constexpr uint32_t kMaxPoolGrowthShift       = 4;
constexpr uint32_t kInvalidEntryIndex        = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kContanierIdleGCThreshold = 8;
constexpr uint32_t kBindlessHeapCount        = ToUnderlying(RHIBindlessHeapType::eMax);

static_assert((kInitialSetsPerPool << kMaxPoolGrowthShift) == kMaxSetsPerPool);

using DescriptorPoolSizes =
    SmallVector<VkDescriptorPoolSize, ToUnderlying(RHIShaderResourceType::eMax)>;

void BuildDescriptorPoolSizes(VulkanDevice* pDevice,
                              const VulkanDescriptorPoolKey& poolKey,
                              uint32_t maxNumSets,
                              bool updateAfterBind,
                              DescriptorPoolSizes& poolSizes)
{
    for (uint32_t resourceTypeIdx = 0; resourceTypeIdx < ToUnderlying(RHIShaderResourceType::eMax);
         ++resourceTypeIdx)
    {
        uint32_t descriptorCount = poolKey.descriptorCount[resourceTypeIdx];

        if (descriptorCount == 0)
        {
            continue;
        }

        const RHIShaderResourceType resourceType =
            static_cast<RHIShaderResourceType>(resourceTypeIdx);
        const VkDescriptorType descriptorType =
            resourceType == RHIShaderResourceType::eUniformBuffer ?
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC :
            ShaderResourceTypeToVkDescriptorType(resourceType);

        if (descriptorType == VK_DESCRIPTOR_TYPE_MAX_ENUM)
        {
            continue;
        }

        const uint32_t poolSizeIdx =
            resourceType == RHIShaderResourceType::eSamplerWithTextureBuffer ?
            ToUnderlying(RHIShaderResourceType::eTextureBuffer) :
            resourceTypeIdx;
        VkDescriptorPoolSize& poolSize = poolSizes[poolSizeIdx];
        poolSize.type                  = descriptorType;
        poolSize.descriptorCount += descriptorCount;
    }

    uint32_t numPoolSizes = 0;

    for (VkDescriptorPoolSize& poolSize : poolSizes)
    {
        if (poolSize.descriptorCount > 0)
        {
            if (updateAfterBind)
            {
                poolSize.descriptorCount =
                    std::min(poolSize.descriptorCount,
                             pDevice->GetDescriptorSetUpdateAfterBindLimit(poolSize.type));
            }

            poolSize.descriptorCount *= maxNumSets;

            if (poolSize.descriptorCount > 0)
            {
                poolSizes[numPoolSizes++] = poolSize;
            }
        }
    }

    poolSizes.resize(numPoolSizes);
}

VulkanDescriptorPool::VulkanDescriptorPool(VulkanDevice* pDevice,
                                           const VulkanDescriptorPoolKey& poolKey,
                                           uint32_t maxNumSets,
                                           bool updateAfterBind) :
    m_pDevice(pDevice), m_maxNumSets(maxNumSets), m_updateAfterBind(updateAfterBind)
{
    VERIFY_EXPR(m_pDevice != nullptr);
    VERIFY_EXPR(m_maxNumSets > 0);

    if (m_pDevice != nullptr && m_maxNumSets > 0)
    {
        DescriptorPoolSizes poolSizes(ToUnderlying(RHIShaderResourceType::eMax));
        BuildDescriptorPoolSizes(m_pDevice, poolKey, m_maxNumSets, m_updateAfterBind, poolSizes);

        VkDescriptorPoolCreateInfo poolCI{};
        InitVkStruct(poolCI, VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        poolCI.flags   = m_updateAfterBind ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0;
        poolCI.maxSets = m_maxNumSets;
        poolCI.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolCI.pPoolSizes    = poolSizes.empty() ? nullptr : poolSizes.data();

        VKCHECK(vkCreateDescriptorPool(m_pDevice->GetVkHandle(), &poolCI, nullptr, &m_vkHandle));
    }

    // TODO: record pool create metrics somewhere
}

VulkanDescriptorPool::~VulkanDescriptorPool()
{
    if (m_vkHandle != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_pDevice->GetVkHandle(), m_vkHandle, nullptr);
        m_vkHandle = VK_NULL_HANDLE;

        // TODO: record pool destroy metrics somewhere
    }
}

VkDescriptorSet VulkanDescriptorPool::Allocate(VkDescriptorSetLayout layout, uint32_t variableCount)
{
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    if (m_vkHandle != VK_NULL_HANDLE && layout != VK_NULL_HANDLE &&
        m_numAllocatedSets < m_maxNumSets)
    {
        VkDescriptorSetVariableDescriptorCountAllocateInfo variableCountInfo{};
        InitVkStruct(variableCountInfo,
                     VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO);
        variableCountInfo.descriptorSetCount = 1;
        variableCountInfo.pDescriptorCounts  = &variableCount;

        VkDescriptorSetAllocateInfo allocateInfo{};
        InitVkStruct(allocateInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        allocateInfo.pNext              = variableCount > 0 ? &variableCountInfo : nullptr;
        allocateInfo.descriptorPool     = m_vkHandle;
        allocateInfo.descriptorSetCount = 1;
        allocateInfo.pSetLayouts        = &layout;

        const VkResult result =
            vkAllocateDescriptorSets(m_pDevice->GetVkHandle(), &allocateInfo, &descriptorSet);

        if (result == VK_SUCCESS)
        {
            ++m_numAllocatedSets;
            // TODO: record set allocate metrics somewhere
        }
        else if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
        {
            VKCHECK(result);
        }
    }

    return descriptorSet;
}

void VulkanDescriptorPool::Reset()
{
    if (m_vkHandle != VK_NULL_HANDLE && m_numAllocatedSets > 0)
    {
        VKCHECK(vkResetDescriptorPool(m_pDevice->GetVkHandle(), m_vkHandle, 0));

        m_numAllocatedSets = 0;

        // TODO: record pool reset metrics here
    }
}

void VulkanDescriptorPoolSetContainer::DestroyChains(PoolChainMap& chainMap)
{
    for (std::pair<const VulkanDescriptorPoolKey, PoolChain*>& chainEntry : chainMap)
    {
        PoolChain* pChain = chainEntry.second;

        if (pChain == nullptr)
        {
            continue;
        }

        for (VulkanDescriptorPool* pPool : pChain->pools)
        {
            ZEN_DELETE(pPool);
        }

        ZEN_DELETE(pChain);
    }

    chainMap.clear();
}

VulkanDescriptorPoolSetContainer::VulkanDescriptorPoolSetContainer(VulkanDevice* pDevice) :
    m_pVulkanDevice(pDevice), m_lifetimeId(GVulkanRHI->GetLifetimeTracker().Create())
{}

VulkanDescriptorPoolSetContainer::~VulkanDescriptorPoolSetContainer()
{
    DestroyChains(m_nonUABChainMap);
    DestroyChains(m_UABChainMap);
}

bool VulkanDescriptorPoolSetContainer::CanReuse() const
{
    return GVulkanRHI->GetLifetimeTracker().IsComplete(m_lifetimeId);
}

void VulkanDescriptorPoolSetContainer::Retire()
{
    GVulkanRHI->GetLifetimeTracker().Retire(m_lifetimeId, this, [](void* resource) {
        ZEN_DELETE(static_cast<VulkanDescriptorPoolSetContainer*>(resource));
    });
}

VkDescriptorSet VulkanDescriptorPoolSetContainer::Allocate(const VulkanDescriptorPoolKey& poolKey,
                                                           bool updateAfterBind,
                                                           VkDescriptorSetLayout layout,
                                                           uint32_t variableCount)
{
    VERIFY_EXPR(layout != VK_NULL_HANDLE);
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    if (layout != VK_NULL_HANDLE)
    {
        PoolChain* pChain = AcquireChain(poolKey, updateAfterBind);

        if (pChain != nullptr)
        {
            descriptorSet = AllocateFromChain(pChain, layout, variableCount);
        }
    }

    return descriptorSet;
}

void VulkanDescriptorPoolSetContainer::ResetChains(PoolChainMap& chainMap)
{
    for (std::pair<const VulkanDescriptorPoolKey, PoolChain*>& chainEntry : chainMap)
    {
        PoolChain* pChain = chainEntry.second;

        if (pChain == nullptr)
        {
            continue;
        }

        for (VulkanDescriptorPool* pPool : pChain->pools)
        {
            pPool->Reset();
        }

        pChain->activePoolIdx = 0;
    }
}

void VulkanDescriptorPoolSetContainer::Reset()
{
    VERIFY_EXPR(CanReuse());
    ResetChains(m_nonUABChainMap);
    ResetChains(m_UABChainMap);
}

VulkanDescriptorPoolSetContainer::PoolChain* VulkanDescriptorPoolSetContainer::AcquireChain(
    const VulkanDescriptorPoolKey& poolKey,
    bool updateAfterBind)
{
    PoolChainMap& chainMap = updateAfterBind ? m_UABChainMap : m_nonUABChainMap;
    const FlatHashMap<VulkanDescriptorPoolKey, PoolChain*, VulkanDescriptorPoolKeyHasher>::iterator
        chainIt       = chainMap.find(poolKey);
    PoolChain* pChain = nullptr;

    if (chainIt != chainMap.end())
    {
        pChain = chainIt->second;
    }
    else
    {
        pChain                  = ZEN_NEW() PoolChain();
        pChain->poolKey         = poolKey;
        pChain->updateAfterBind = updateAfterBind;
        chainMap.emplace(poolKey, pChain);
    }

    return pChain;
}

VkDescriptorSet VulkanDescriptorPoolSetContainer::AllocateFromChain(PoolChain* pChain,
                                                                    VkDescriptorSetLayout layout,
                                                                    uint32_t variableCount)
{
    VERIFY_EXPR(pChain != nullptr);
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    if (pChain != nullptr)
    {
        while (pChain->activePoolIdx < pChain->pools.size() && descriptorSet == VK_NULL_HANDLE)
        {
            descriptorSet = pChain->pools[pChain->activePoolIdx]->Allocate(layout, variableCount);

            if (descriptorSet == VK_NULL_HANDLE)
            {
                ++pChain->activePoolIdx;
            }
        }

        if (descriptorSet == VK_NULL_HANDLE)
        {
            PushPoolToChain(pChain);

            if (pChain->activePoolIdx < pChain->pools.size())
            {
                descriptorSet =
                    pChain->pools[pChain->activePoolIdx]->Allocate(layout, variableCount);
            }
        }
    }

    return descriptorSet;
}

void VulkanDescriptorPoolSetContainer::PushPoolToChain(PoolChain* pChain)
{
    VERIFY_EXPR(pChain != nullptr);

    if (pChain != nullptr)
    {
        const uint32_t growthShift = std::min(pChain->numPools, kMaxPoolGrowthShift);
        const uint32_t maxNumSets  = kInitialSetsPerPool << growthShift;

        VulkanDescriptorPool* pPool = ZEN_NEW() VulkanDescriptorPool(
            m_pVulkanDevice, pChain->poolKey, maxNumSets, pChain->updateAfterBind);
        pChain->pools.push_back(pPool);
        pChain->numPools      = static_cast<uint32_t>(pChain->pools.size());
        pChain->activePoolIdx = pChain->numPools - 1;
    }
}

VulkanDescriptorSetCache::VulkanDescriptorSetCache(VulkanDescriptorPoolManager2* pPoolManager,
                                                   VulkanDevice* pVulkanDevice) :
    m_pPoolManager(pPoolManager), m_pDevice(pVulkanDevice)
{
    VERIFY_EXPR(m_pPoolManager != nullptr);
    VERIFY_EXPR(m_pDevice != nullptr);
}

VulkanDescriptorSetCache::~VulkanDescriptorSetCache()
{
    Destroy();
}

void VulkanDescriptorSetCache::BeginFrame(uint32_t frameNumber)
{
    m_currentFrame = frameNumber;
}

void VulkanDescriptorSetCache::Destroy()
{
    for (RingSlot& slot : m_ringSlots)
    {
        if (slot.pContainer != nullptr)
        {
            if (m_pPoolManager != nullptr)
            {
                m_pPoolManager->ReleaseContainer(slot.pContainer);
            }
            else
            {
                slot.pContainer->Retire();
            }
        }

        slot = {};
    }

    m_entries.clear();
    m_entryIndexMap.clear();
    m_entryHashChain.clear();
    m_numLiveSlots       = 0;
    m_nextSlotGeneration = 0;
    m_currentFrame       = 0;
    m_entryHighWaterMark = 0;
    m_numSlotRetires     = 0;
    m_pPoolManager       = nullptr;
    m_pDevice            = nullptr;
}

VkDescriptorSet VulkanDescriptorSetCache::Find(const ContentKey& key,
                                               VulkanDescriptorPoolSetContainer*& outContainer)
{
    outContainer                  = nullptr;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    const FlatHashMap<VulkanDescriptorSetCache::ContentKey, uint32_t,
                      VulkanDescriptorSetCache::ContentKeyHasher>::iterator entryIndexIt =
        m_entryIndexMap.find(key);

    if (entryIndexIt != m_entryIndexMap.end())
    {
        uint32_t entryIdx                 = entryIndexIt->second;
        uint32_t numVisitedEntries        = 0;
        const uint32_t numSearchableSlots = std::min(m_numLiveSlots, kMaxPoolLookups);

        while (entryIdx != kInvalidEntryIndex && numVisitedEntries < m_entries.size() &&
               descriptorSet == VK_NULL_HANDLE)
        {
            VERIFY_EXPR(entryIdx < m_entries.size() && entryIdx < m_entryHashChain.size());

            if (entryIdx < m_entries.size() && entryIdx < m_entryHashChain.size())
            {
                const Entry& entry = m_entries[entryIdx];

                for (uint32_t slotIdx = 0;
                     slotIdx < numSearchableSlots && descriptorSet == VK_NULL_HANDLE; ++slotIdx)
                {
                    RingSlot& slot = m_ringSlots[slotIdx];

                    if (entry.key == key && slot.generation == entry.slotGeneration)
                    {
                        descriptorSet      = entry.descriptorSet;
                        outContainer       = slot.pContainer;
                        slot.lastUsedFrame = m_currentFrame;
                    }
                }

                entryIdx = m_entryHashChain[entryIdx];
            }
            else
            {
                entryIdx = kInvalidEntryIndex;
            }

            ++numVisitedEntries;
        }
    }

    return descriptorSet;
}

VkDescriptorSet VulkanDescriptorSetCache::Insert(const ContentKey& key,
                                                 const VulkanDescriptorPoolKey& poolKey,
                                                 VkDescriptorSetLayout layout,
                                                 bool updateAfterBind,
                                                 uint32_t variableCount,
                                                 VulkanDescriptorPoolSetContainer*& outContainer)
{
    outContainer = nullptr;
    VERIFY_EXPR(layout != VK_NULL_HANDLE);
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    if (layout != VK_NULL_HANDLE)
    {
        if (m_numLiveSlots == 0 || m_ringSlots[0].numLiveEntries >= kMaxSetsPerPool)
        {
            RotateRing();
        }

        if (m_numLiveSlots > 0 && m_ringSlots[0].pContainer != nullptr)
        {
            RingSlot& slot = m_ringSlots[0];
            descriptorSet =
                slot.pContainer->Allocate(poolKey, updateAfterBind, layout, variableCount);

            if (descriptorSet != VK_NULL_HANDLE)
            {
                outContainer              = slot.pContainer;
                const uint32_t entryIdx   = static_cast<uint32_t>(m_entries.size());
                uint32_t previousEntryIdx = kInvalidEntryIndex;
                const FlatHashMap<VulkanDescriptorSetCache::ContentKey, uint32_t,
                                  VulkanDescriptorSetCache::ContentKeyHasher>::iterator
                    previousEntryIt = m_entryIndexMap.find(key);

                if (previousEntryIt != m_entryIndexMap.end())
                {
                    previousEntryIdx        = previousEntryIt->second;
                    previousEntryIt->second = entryIdx;
                }
                else
                {
                    m_entryIndexMap.emplace(key, entryIdx);
                }

                Entry entry{};
                entry.key            = key;
                entry.descriptorSet  = descriptorSet;
                entry.slotGeneration = slot.generation;
                m_entries.push_back(entry);
                m_entryHashChain.push_back(previousEntryIdx);

                ++slot.numLiveEntries;
                slot.lastUsedFrame = m_currentFrame;
                m_entryHighWaterMark =
                    std::max(m_entryHighWaterMark, static_cast<uint64_t>(m_entries.size()));
            }
        }
    }

    return descriptorSet;
}

void VulkanDescriptorSetCache::RotateRing()
{
    VERIFY_EXPR(m_pPoolManager != nullptr);
    VERIFY_EXPR(m_pDevice != nullptr);

    if (m_numLiveSlots == kRingSlots)
    {
        RetireOldestSlot();
    }

    for (uint32_t slotIdx = m_numLiveSlots; slotIdx > 0; --slotIdx)
    {
        m_ringSlots[slotIdx] = m_ringSlots[slotIdx - 1];
    }

    RingSlot& newSlot  = m_ringSlots[0];
    newSlot            = {};
    newSlot.pContainer = m_pPoolManager->AcquireDescriptorPoolSetContainer();
    newSlot.generation = ++m_nextSlotGeneration;

    if (newSlot.generation == 0)
    {
        newSlot.generation = ++m_nextSlotGeneration;
    }

    newSlot.lastUsedFrame = m_currentFrame;
    ++m_numLiveSlots;
}

void VulkanDescriptorSetCache::RetireOldestSlot()
{
    if (m_numLiveSlots > 0)
    {
        RingSlot& oldestSlot = m_ringSlots[m_numLiveSlots - 1];

        if (m_pPoolManager != nullptr)
        {
            m_pPoolManager->ReleaseContainer(oldestSlot.pContainer);
        }
        else
        {
            oldestSlot.pContainer->Retire();
        }

        oldestSlot = {};
        --m_numLiveSlots;
        ++m_numSlotRetires;

        CompactEntries();
    }
}

void VulkanDescriptorSetCache::CompactEntries()
{
    HeapVector<Entry> liveEntries;
    liveEntries.reserve(m_entries.size());

    for (const Entry& entry : m_entries)
    {
        bool generationIsLive = false;

        for (uint32_t slotIdx = 0; slotIdx < m_numLiveSlots; ++slotIdx)
        {
            if (m_ringSlots[slotIdx].generation == entry.slotGeneration)
            {
                generationIsLive = true;
                break;
            }
        }

        if (generationIsLive)
        {
            liveEntries.push_back(entry);
        }
    }

    m_entries = std::move(liveEntries);
    m_entryIndexMap.clear();
    m_entryHashChain.clear();
    m_entryHashChain.resize(m_entries.size());

    for (uint32_t slotIdx = 0; slotIdx < m_numLiveSlots; ++slotIdx)
    {
        m_ringSlots[slotIdx].numLiveEntries = 0;
    }

    for (uint32_t entryIdx = 0; entryIdx < m_entries.size(); ++entryIdx)
    {
        Entry& entry               = m_entries[entryIdx];
        m_entryHashChain[entryIdx] = kInvalidEntryIndex;
        const FlatHashMap<VulkanDescriptorSetCache::ContentKey, uint32_t,
                          VulkanDescriptorSetCache::ContentKeyHasher>::iterator
            existingEntryIndexIter = m_entryIndexMap.find(entry.key);

        if (existingEntryIndexIter == m_entryIndexMap.end())
        {
            m_entryIndexMap.emplace(entry.key, entryIdx);
        }
        else
        {
            m_entryHashChain[entryIdx]     = existingEntryIndexIter->second;
            existingEntryIndexIter->second = entryIdx;
        }

        for (uint32_t slotIdx = 0; slotIdx < m_numLiveSlots; ++slotIdx)
        {
            if (m_ringSlots[slotIdx].generation == entry.slotGeneration)
            {
                ++m_ringSlots[slotIdx].numLiveEntries;
                break;
            }
        }
    }

    if (m_entries.size() > m_entryHighWaterMark)
    {
        m_entryHighWaterMark = m_entries.size();
    }
}

VulkanDescriptorPoolManager2::VulkanDescriptorPoolManager2(VulkanDevice* pDevice) :
    m_pDevice(pDevice), m_pContentCache(nullptr)
{
    VERIFY_EXPR(m_pDevice != nullptr);

    if (m_pDevice != nullptr)
    {
        m_pContentCache = ZEN_NEW() VulkanDescriptorSetCache(this, m_pDevice);
    }
}

void VulkanDescriptorPoolManager2::BeginFrame(uint32_t frameNumber)
{
    if (m_pContentCache != nullptr)
    {
        m_pContentCache->BeginFrame(frameNumber);
    }

    ReclaimRetiredContainers();
    TickPoolSetContainers();
}

void VulkanDescriptorPoolManager2::Destroy()
{
    if (m_pContentCache != nullptr)
    {
        ZEN_DELETE(m_pContentCache);
        m_pContentCache = nullptr;
    }

    DestroyContainers();
    m_layoutIdMap.clear();
    m_nextLayoutId = 1;
}

VulkanDescriptorPoolSetContainer* VulkanDescriptorPoolManager2::AcquireDescriptorPoolSetContainer()
{
    ReclaimRetiredContainers();
    VulkanDescriptorPoolSetContainer* pContainer = nullptr;

    if (!m_freeContainers.empty())
    {
        pContainer = m_freeContainers[0].pContainer;
        m_freeContainers.pop_front();
    }
    else
    {
        pContainer = ZEN_NEW() VulkanDescriptorPoolSetContainer(m_pDevice);
    }

    m_usedContainers.push_back(pContainer);

    return pContainer;
}

void VulkanDescriptorPoolManager2::ReleaseContainer(VulkanDescriptorPoolSetContainer* pContainer)
{
    if (pContainer != nullptr)
    {
        bool containerWasUsed = false;

        for (uint32_t i = 0; i < m_usedContainers.size(); ++i)
        {
            if (m_usedContainers[i] == pContainer)
            {
                m_usedContainers[i] = m_usedContainers.back();
                m_usedContainers.pop_back();
                containerWasUsed = true;

                break;
            }
        }

        VERIFY_EXPR(containerWasUsed);

        if (containerWasUsed)
        {
            // Eviction removes lookup ownership; the shared tracker protects
            // recording and GPU lifetimes until this container can be reset.
            m_retiredContainers.push_back(pContainer);
            ReclaimRetiredContainers();
        }
    }
}

void VulkanDescriptorPoolManager2::TickPoolSetContainers()
{
    // Only containers whose recording/submission lifetimes completed reach this list.
    for (FreeContainerEntry& entry : m_freeContainers)
    {
        ++entry.idleTicks;
    }

    uint32_t numDestroyedContainers = 0;

    if (!m_freeContainers.empty() && m_freeContainers[0].idleTicks > kContanierIdleGCThreshold)
    {
        m_freeContainers[0].pContainer->Retire();

        m_freeContainers.pop_front();

        numDestroyedContainers++;
    }
}

uint32_t VulkanDescriptorPoolManager2::GetOrCreateLayoutId(
    const VkDescriptorSetLayoutCreateInfo& createInfo,
    VectorView<const VkDescriptorBindingFlags> bindingFlags)
{
    VERIFY_EXPR(bindingFlags.size() == createInfo.bindingCount);
    HeapVector<uint32_t> bindingOrder(createInfo.bindingCount);
    for (uint32_t i = 0; i < createInfo.bindingCount; ++i)
    {
        bindingOrder[i] = i;
    }
    std::sort(bindingOrder.begin(), bindingOrder.end(), [&createInfo](uint32_t a, uint32_t b) {
        return createInfo.pBindings[a].binding < createInfo.pBindings[b].binding;
    });

    LayoutKey key;
    key.data.push_back(createInfo.flags);
    key.data.push_back(createInfo.bindingCount);
    for (uint32_t i : bindingOrder)
    {
        const VkDescriptorSetLayoutBinding& binding = createInfo.pBindings[i];
        key.data.push_back(binding.binding);
        key.data.push_back(binding.descriptorType);
        key.data.push_back(binding.descriptorCount);
        key.data.push_back(binding.stageFlags);
        key.data.push_back(bindingFlags[i]);
        key.data.push_back(binding.pImmutableSamplers != nullptr);
        if (binding.pImmutableSamplers != nullptr)
        {
            for (uint32_t j = 0; j < binding.descriptorCount; ++j)
            {
                key.data.push_back(reinterpret_cast<uint64_t>(binding.pImmutableSamplers[j]));
            }
        }
    }

    const auto layoutIdIt = m_layoutIdMap.find(key);
    uint32_t layoutId     = 0;

    if (layoutIdIt != m_layoutIdMap.end())
    {
        layoutId = layoutIdIt->second;
    }
    else
    {
        VERIFY_EXPR(m_nextLayoutId != 0);

        if (m_nextLayoutId != 0)
        {
            layoutId = m_nextLayoutId++;
            m_layoutIdMap.emplace(std::move(key), layoutId);
        }
    }

    return layoutId;
}

void VulkanDescriptorPoolManager2::DestroyContainers()
{
    for (VulkanDescriptorPoolSetContainer* pContainer : m_usedContainers)
    {
        pContainer->Retire();
    }

    m_usedContainers.clear();

    for (FreeContainerEntry& entry : m_freeContainers)
    {
        entry.pContainer->Retire();
    }

    m_freeContainers.clear();

    // The shared tracker defers destruction independently of this manager,
    // including uncertain submissions that survive until queue teardown.
    for (VulkanDescriptorPoolSetContainer* pContainer : m_retiredContainers)
    {
        pContainer->Retire();
    }
    m_retiredContainers.clear();
}

void VulkanDescriptorPoolManager2::ReclaimRetiredContainers()
{
    size_t writeIndex = 0;

    for (VulkanDescriptorPoolSetContainer* pContainer : m_retiredContainers)
    {
        if (!pContainer->CanReuse())
        {
            m_retiredContainers[writeIndex++] = pContainer;
            continue;
        }

        pContainer->Reset();
        m_freeContainers.push_back({pContainer, 0});
    }

    m_retiredContainers.resize(writeIndex);
}

VkDescriptorType GetBindlessDescriptorType(RHIBindlessHeapType heapType)
{
    VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_MAX_ENUM;

    switch (heapType)
    {
        case RHIBindlessHeapType::eTexture2D:
        case RHIBindlessHeapType::eTextureCube:
            descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            break;
        case RHIBindlessHeapType::eSampler: descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; break;
        case RHIBindlessHeapType::eMax: break;
    }

    return descriptorType;
}

const VulkanTextureView* GetBindlessTextureView(const RHIResource* pResource)
{
    const VulkanTextureView* pTextureView = dynamic_cast<const VulkanTextureView*>(pResource);

    if (pTextureView == nullptr)
    {
        const VulkanTexture* pTexture = dynamic_cast<const VulkanTexture*>(pResource);

        if (pTexture != nullptr)
        {
            pTextureView = dynamic_cast<const VulkanTextureView*>(pTexture->GetDefaultView());
        }
    }

    return pTextureView;
}

RHIBindlessHeapType GetBindlessHeapType(const RHIResource* pResource)
{
    RHIBindlessHeapType heapType = RHIBindlessHeapType::eMax;

    switch (pResource->GetResourceType())
    {
        case RHIResourceType::eSampler: heapType = RHIBindlessHeapType::eSampler; break;

        case RHIResourceType::eTexture:
        case RHIResourceType::eTextureView:
        {
            const VulkanTextureView* view = GetBindlessTextureView(pResource);
            if (view == nullptr)
            {
                break;
            }
            const RHITextureType texType = view->GetTextureType();

            if (texType == RHITextureType::e2D)
            {
                heapType = RHIBindlessHeapType::eTexture2D;
            }
            else if (texType == RHITextureType::eCube)
            {
                heapType = RHIBindlessHeapType::eTextureCube;
            }

            break;
        }

        default: break;
    }

    return heapType;
}

bool IsValidBindlessResource(const RHIResource* pResource, RHIBindlessHeapType heapType)
{
    bool valid = false;

    if (heapType == RHIBindlessHeapType::eSampler)
    {
        const VulkanSampler* pSampler = dynamic_cast<const VulkanSampler*>(pResource);
        valid = pSampler != nullptr && pSampler->GetVkSampler() != VK_NULL_HANDLE;
    }
    else if (heapType == RHIBindlessHeapType::eTexture2D ||
             heapType == RHIBindlessHeapType::eTextureCube)
    {
        const VulkanTextureView* pTextureView = GetBindlessTextureView(pResource);
        valid = pTextureView != nullptr && pTextureView->GetVkImageView() != VK_NULL_HANDLE &&
            pTextureView->GetTexture()->GetBaseInfo().usageFlags.HasFlag(
                RHITextureUsageFlagBits::eSampled);
    }

    return valid;
}

bool SupportsBindlessDescriptorHeaps(VulkanDevice* pDevice)
{
    bool supported = pDevice != nullptr && pDevice->GetExtensionFlags().hasDescriptorIndexing != 0;

    if (supported)
    {
        const VkPhysicalDeviceDescriptorIndexingProperties& properties =
            pDevice->GetDescriptorIndexingProperties();
        const uint32_t sampledImageCount =
            GetBindlessHeapCapacity(RHIBindlessHeapType::eTexture2D) +
            GetBindlessHeapCapacity(RHIBindlessHeapType::eTextureCube);
        const uint32_t samplerCount    = GetBindlessHeapCapacity(RHIBindlessHeapType::eSampler);
        const uint32_t descriptorCount = sampledImageCount + samplerCount;

        supported = sampledImageCount <= properties.maxDescriptorSetUpdateAfterBindSampledImages &&
            sampledImageCount <= properties.maxPerStageDescriptorUpdateAfterBindSampledImages &&
            samplerCount <= properties.maxDescriptorSetUpdateAfterBindSamplers &&
            samplerCount <= properties.maxPerStageDescriptorUpdateAfterBindSamplers &&
            descriptorCount <= properties.maxPerStageUpdateAfterBindResources &&
            descriptorCount <= properties.maxUpdateAfterBindDescriptorsInAllPools;
    }

    return supported;
}

void VulkanBindlessDescriptorPoolManager::Init()
{
    m_pDevice                    = GVulkanRHI->GetDevice();
    const bool bindlessSupported = SupportsBindlessDescriptorHeaps(m_pDevice);

    if (bindlessSupported)
    {
        CreateGlobalBindlessDescriptorSet();

        if (m_vkSet != VK_NULL_HANDLE)
        {
            m_epoch = GVulkanRHI->GetLifetimeTracker().Create();
            m_epochs.push_back(m_epoch);
            for (uint32_t heapIdx = 0; heapIdx < kBindlessHeapCount; ++heapIdx)
            {
                const RHIBindlessHeapType heapType = static_cast<RHIBindlessHeapType>(heapIdx);
                m_slotStates[heapIdx].resize(GetBindlessHeapCapacity(heapType));
                m_heapAllocCount[heapIdx] = 0;
            }
        }
    }
}

void VulkanBindlessDescriptorPoolManager::Destroy()
{
    for (uint64_t epoch : m_epochs)
    {
        GVulkanRHI->GetLifetimeTracker().Retire(epoch);
    }
    m_epochs.clear();
    m_epoch = 0;
    m_retiredSlots.clear();
    for (uint32_t heapIdx = 0; heapIdx < kBindlessHeapCount; ++heapIdx)
    {
        m_pendingWrites[heapIdx].clear();
        for (BindlessSlotState& slot : m_slotStates[heapIdx])
        {
            if (slot.pResource != nullptr)
            {
                slot.pResource->ReleaseReference();
            }
            if (slot.pTextureOwner != nullptr)
            {
                slot.pTextureOwner->ReleaseReference();
            }
        }
        m_slotStates[heapIdx].clear();
        m_heapAllocCount[heapIdx] = 0;
    }

    if (m_pDevice != nullptr)
    {
        const VkDevice device = m_pDevice->GetVkHandle();

        if (m_vkPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(device, m_vkPool, nullptr);
        }

        if (m_vkLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, m_vkLayout, nullptr);
        }
    }

    m_vkSet    = VK_NULL_HANDLE;
    m_vkPool   = VK_NULL_HANDLE;
    m_vkLayout = VK_NULL_HANDLE;
    m_pDevice  = nullptr;
}

bool VulkanBindlessDescriptorPoolManager::RegisterBindlessResource(RHIResource* pResource,
                                                                   uint32_t slotIdx,
                                                                   RHIBindlessHandle* pOutHandle,
                                                                   uint64_t recordedEpoch)
{
    LockAuto lock(&m_mutex);
    if (pOutHandle != nullptr)
    {
        *pOutHandle = {};
    }
    CollectRetiredResourcesLocked();
    bool registered              = pResource != nullptr && m_vkSet != VK_NULL_HANDLE;
    RHIBindlessHeapType heapType = RHIBindlessHeapType::eMax;
    if (registered)
    {
        heapType = GetBindlessHeapType(pResource);
        registered =
            heapType != RHIBindlessHeapType::eMax && IsValidBindlessResource(pResource, heapType);
    }

    if (registered)
    {
        const uint32_t heapIdx  = ToUnderlying(heapType);
        const uint32_t capacity = GetBindlessHeapCapacity(heapType);
        if (slotIdx == kInvalidBindlessSlotIndex)
        {
            for (uint32_t i = 0; i < capacity; ++i)
            {
                const uint32_t candidate = (m_heapAllocCount[heapIdx] + i) % capacity;
                if (m_slotStates[heapIdx][candidate].pResource == nullptr)
                {
                    slotIdx = candidate;
                    break;
                }
            }
        }
        registered = slotIdx < capacity;
        if (registered)
        {
            BindlessSlotState& slot = m_slotStates[heapIdx][slotIdx];
            if (slot.pResource != nullptr)
            {
                const bool earlierRecording = recordedEpoch != 0 &&
                    recordedEpoch <= slot.retiredEpoch &&
                    std::binary_search(m_epochs.begin(), m_epochs.end(), recordedEpoch) &&
                    GVulkanRHI->GetLifetimeTracker().HasRecordings(recordedEpoch);
                registered = (slot.retiredEpoch == 0 || earlierRecording) &&
                    slot.resourceId == pResource->GetStableId();
            }
            else
            {
                static std::atomic<uint64_t> nextGeneration{1};
                m_pendingWrites[heapIdx].push_back({slotIdx, pResource});
                pResource->AddReference();
                slot.pResource = pResource;
                if (pResource->GetResourceType() == RHIResourceType::eTextureView)
                {
                    slot.pTextureOwner = static_cast<RHITextureView*>(pResource)->GetTexture();
                    slot.pTextureOwner->AddReference();
                }
                slot.resourceId           = pResource->GetStableId();
                slot.generation           = nextGeneration.fetch_add(1, std::memory_order_relaxed);
                m_heapAllocCount[heapIdx] = (slotIdx + 1) % capacity;
            }
            if (registered && pOutHandle != nullptr)
            {
                *pOutHandle = {heapType, slotIdx, slot.generation};
            }
        }
    }
    return registered;
}

VulkanBindlessDescriptorPoolManager::BindlessSlotState* VulkanBindlessDescriptorPoolManager::
    FindRegistration(RHIBindlessHandle handle)
{
    if (!handle.IsValid() || m_vkSet == VK_NULL_HANDLE ||
        handle.slotIndex >= GetBindlessHeapCapacity(handle.heapType))
    {
        return nullptr;
    }
    BindlessSlotState& slot = m_slotStates[ToUnderlying(handle.heapType)][handle.slotIndex];
    return slot.pResource != nullptr && slot.generation == handle.generation ? &slot : nullptr;
}

bool VulkanBindlessDescriptorPoolManager::IsRegistered(RHIBindlessHandle handle)
{
    LockAuto lock(&m_mutex);
    const BindlessSlotState* slot = FindRegistration(handle);
    return slot != nullptr && slot->retiredEpoch == 0;
}

bool VulkanBindlessDescriptorPoolManager::UnregisterBindlessResource(RHIBindlessHandle handle)
{
    LockAuto lock(&m_mutex);
    BindlessSlotState* slot = FindRegistration(handle);
    if (slot == nullptr || slot->retiredEpoch != 0 || m_epoch == UINT64_MAX)
    {
        return false;
    }
    // Reserve before changing publication state. A failed allocation leaves it active.
    m_retiredSlots.reserve(m_retiredSlots.size() + 1);
    m_epochs.reserve(m_epochs.size() + 1);
    const uint64_t nextEpoch = GVulkanRHI->GetLifetimeTracker().Create();
    m_retiredSlots.push_back(handle);
    slot->retiredEpoch = m_epoch;
    m_epoch            = nextEpoch;
    m_epochs.push_back(m_epoch);
    CollectRetiredResourcesLocked();
    return true;
}

uint64_t VulkanBindlessDescriptorPoolManager::CaptureEpoch()
{
    LockAuto lock(&m_mutex);
    uint64_t epoch = 0;
    if (m_vkSet != VK_NULL_HANDLE)
    {
        // Recording is allowed on RenderCore. Collection can destroy native resources,
        // so leave it to the RHI thread's registration/retirement sweeps.
        epoch = m_epoch;
        GVulkanRHI->GetLifetimeTracker().RetainRecording(epoch);
    }
    return epoch;
}

void VulkanBindlessDescriptorPoolManager::ReleaseEpoch(uint64_t epoch)
{
    GVulkanRHI->GetLifetimeTracker().ReleaseRecordings(MakeVecView(&epoch, 1));
}

void VulkanBindlessDescriptorPoolManager::CollectRetiredResources()
{
    LockAuto lock(&m_mutex);
    CollectRetiredResourcesLocked();
}

void VulkanBindlessDescriptorPoolManager::CollectRetiredResourcesLocked()
{
    VulkanLifetimeTracker& tracker = GVulkanRHI->GetLifetimeTracker();
    uint64_t earliestEpoch         = UINT64_MAX;
    size_t retained                = 0;
    for (uint64_t epoch : m_epochs)
    {
        const bool completed = tracker.IsComplete(epoch);
        if (completed && epoch != m_epoch)
        {
            tracker.Retire(epoch);
        }
        else
        {
            m_epochs[retained++] = epoch;
            if (!completed)
            {
                earliestEpoch = std::min(earliestEpoch, epoch);
            }
        }
    }
    m_epochs.resize(retained);
    for (auto it = m_retiredSlots.begin(); it != m_retiredSlots.end();)
    {
        BindlessSlotState* slot = FindRegistration(*it);
        if (slot != nullptr && slot->retiredEpoch < earliestEpoch)
        {
            // Unflushed writes must not outlive their retained resources or later
            // overwrite a recycled slot. Live old recordings keep their writes intact.
            HeapVector<BindlessDSWrite>& writes = m_pendingWrites[ToUnderlying(it->heapType)];
            writes.erase(std::remove_if(writes.begin(), writes.end(),
                                        [it](const BindlessDSWrite& write) {
                                            return write.slotIdx == it->slotIndex;
                                        }),
                         writes.end());
            slot->pResource->ReleaseReference();
            if (slot->pTextureOwner != nullptr)
            {
                slot->pTextureOwner->ReleaseReference();
            }
            m_heapAllocCount[ToUnderlying(it->heapType)] = it->slotIndex;
            *slot                                        = {};
            it                                           = m_retiredSlots.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void VulkanBindlessDescriptorPoolManager::Flush()
{
    LockAuto lock(&m_mutex);

    bool hasPendingWrites = false;

    for (uint32_t heapIdx = 0; heapIdx < kBindlessHeapCount; ++heapIdx)
    {
        hasPendingWrites = hasPendingWrites || !m_pendingWrites[heapIdx].empty();
    }

    if (m_vkSet != VK_NULL_HANDLE && hasPendingWrites)
    {
        WriteDescriptorSetBatch();
    }
}

VkDescriptorSetLayout VulkanBindlessDescriptorPoolManager::GetGlobalBindlessLayout() const
{
    return m_vkLayout;
}

VkDescriptorSet VulkanBindlessDescriptorPoolManager::GetGlobalBindlessSet() const
{
    return m_vkSet;
}

void VulkanBindlessDescriptorPoolManager::CreateGlobalBindlessDescriptorSet()
{
    VkDescriptorSetLayoutBinding bindings[kBindlessHeapCount]{};
    VkDescriptorBindingFlags bindingFlags[kBindlessHeapCount]{};

    for (uint32_t heapIdx = 0; heapIdx < kBindlessHeapCount; ++heapIdx)
    {
        const RHIBindlessHeapType heapType = static_cast<RHIBindlessHeapType>(heapIdx);
        bindings[heapIdx].binding          = heapIdx;
        bindings[heapIdx].descriptorType   = GetBindlessDescriptorType(heapType);
        bindings[heapIdx].descriptorCount  = GetBindlessHeapCapacity(heapType);
        bindings[heapIdx].stageFlags       = VK_SHADER_STAGE_ALL;
        bindingFlags[heapIdx]              = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{};
    InitVkStruct(bindingFlagsCI, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
    bindingFlagsCI.bindingCount  = kBindlessHeapCount;
    bindingFlagsCI.pBindingFlags = bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    InitVkStruct(layoutCI, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    layoutCI.pNext        = &bindingFlagsCI;
    layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutCI.bindingCount = kBindlessHeapCount;
    layoutCI.pBindings    = bindings;

    VkResult result =
        vkCreateDescriptorSetLayout(m_pDevice->GetVkHandle(), &layoutCI, nullptr, &m_vkLayout);
    VKCHECK(result);

    if (result == VK_SUCCESS)
    {
        const VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
             GetBindlessHeapCapacity(RHIBindlessHeapType::eTexture2D) +
                 GetBindlessHeapCapacity(RHIBindlessHeapType::eTextureCube)},
            {VK_DESCRIPTOR_TYPE_SAMPLER, GetBindlessHeapCapacity(RHIBindlessHeapType::eSampler)}};

        VkDescriptorPoolCreateInfo poolCI{};
        InitVkStruct(poolCI, VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolCI.maxSets       = 1;
        poolCI.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
        poolCI.pPoolSizes    = poolSizes;

        result = vkCreateDescriptorPool(m_pDevice->GetVkHandle(), &poolCI, nullptr, &m_vkPool);
        VKCHECK(result);
    }

    if (result == VK_SUCCESS)
    {
        VkDescriptorSetAllocateInfo allocateInfo{};
        InitVkStruct(allocateInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        allocateInfo.descriptorPool     = m_vkPool;
        allocateInfo.descriptorSetCount = 1;
        allocateInfo.pSetLayouts        = &m_vkLayout;

        result = vkAllocateDescriptorSets(m_pDevice->GetVkHandle(), &allocateInfo, &m_vkSet);
        VKCHECK(result);
    }

    if (result != VK_SUCCESS)
    {
        if (m_vkPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_pDevice->GetVkHandle(), m_vkPool, nullptr);
        }

        if (m_vkLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_pDevice->GetVkHandle(), m_vkLayout, nullptr);
        }

        m_vkSet    = VK_NULL_HANDLE;
        m_vkPool   = VK_NULL_HANDLE;
        m_vkLayout = VK_NULL_HANDLE;
    }
}

void VulkanBindlessDescriptorPoolManager::WriteDescriptorSetBatch()
{
    uint32_t numWrites = 0;

    for (uint32_t heapIdx = 0; heapIdx < kBindlessHeapCount; ++heapIdx)
    {
        numWrites += static_cast<uint32_t>(m_pendingWrites[heapIdx].size());
    }

    HeapVector<VkDescriptorImageInfo> imageInfos(numWrites);
    HeapVector<VkWriteDescriptorSet> descriptorWrites(numWrites);
    uint32_t writeIdx = 0;

    for (uint32_t heapIdx = 0; heapIdx < kBindlessHeapCount; ++heapIdx)
    {
        const RHIBindlessHeapType heapType = static_cast<RHIBindlessHeapType>(heapIdx);

        for (const BindlessDSWrite& pendingWrite : m_pendingWrites[heapIdx])
        {
            VkDescriptorImageInfo& imageInfo = imageInfos[writeIdx];

            if (heapType == RHIBindlessHeapType::eSampler)
            {
                imageInfo.sampler =
                    dynamic_cast<VulkanSampler*>(pendingWrite.pResource)->GetVkSampler();
            }
            else
            {
                imageInfo.imageView =
                    GetBindlessTextureView(pendingWrite.pResource)->GetVkImageView();
                imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }

            VkWriteDescriptorSet& descriptorWrite = descriptorWrites[writeIdx];
            InitVkStruct(descriptorWrite, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            descriptorWrite.dstSet          = m_vkSet;
            descriptorWrite.dstBinding      = heapIdx;
            descriptorWrite.dstArrayElement = pendingWrite.slotIdx;
            descriptorWrite.descriptorCount = 1;
            descriptorWrite.descriptorType  = GetBindlessDescriptorType(heapType);
            descriptorWrite.pImageInfo      = &imageInfo;
            ++writeIdx;
        }
    }

    if (numWrites > 0)
    {
        vkUpdateDescriptorSets(m_pDevice->GetVkHandle(), numWrites, descriptorWrites.data(), 0,
                               nullptr);
    }

    for (uint32_t heapIdx = 0; heapIdx < kBindlessHeapCount; ++heapIdx)
    {
        m_pendingWrites[heapIdx].clear();
    }
}
} // namespace zen
