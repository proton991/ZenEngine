#pragma once
#include "Graphics/RHI/RHICommon.h"
#include "Graphics/RHI/RHIResource.h"
#include "Templates/HeapVector.h"
#include "Templates/SmallVector.h"
#include "VulkanPipeline.h"
#include "Templates/FlatHashMap.h"
#include "Utils/Mutex.h"
#include "Utils/Helpers.h"

namespace zen
{
class VulkanDevice;
class VulkanDescriptorPoolManager2;
class VulkanQueue;

struct VulkanDescriptorPoolKeyHasher
{
    size_t operator()(const VulkanDescriptorPoolKey& poolKey) const noexcept
    {
        size_t hash = 0;

        for (uint32_t descriptorCount : poolKey.descriptorCount)
        {
            util::HashCombine(hash, descriptorCount);
        }

        return hash;
    }
};

class VulkanDescriptorPool
{
public:
    VulkanDescriptorPool(VulkanDevice* pDevice,
                         const VulkanDescriptorPoolKey& poolKey,
                         uint32_t maxNumSets,
                         bool updateAfterBind);

    ~VulkanDescriptorPool();

    VulkanDescriptorPool(const VulkanDescriptorPool&) = delete;

    VulkanDescriptorPool& operator=(const VulkanDescriptorPool&) = delete;

    VkDescriptorSet Allocate(VkDescriptorSetLayout layout, uint32_t variableCount);

    void Reset();

private:
    VulkanDevice* m_pDevice{nullptr};
    VkDescriptorPool m_vkHandle{VK_NULL_HANDLE};
    uint32_t m_maxNumSets{0};
    uint32_t m_numAllocatedSets{0};
    bool m_updateAfterBind{false};
};

class VulkanDescriptorPoolSetContainer
{
public:
    explicit VulkanDescriptorPoolSetContainer(VulkanDevice* pDevice) : m_pVulkanDevice(pDevice) {};

    ~VulkanDescriptorPoolSetContainer();

    VulkanDescriptorPoolSetContainer(const VulkanDescriptorPoolSetContainer&) = delete;

    VulkanDescriptorPoolSetContainer& operator=(const VulkanDescriptorPoolSetContainer&) = delete;

    VkDescriptorSet Allocate(const VulkanDescriptorPoolKey& poolKey,
                             bool updateAfterBind,
                             VkDescriptorSetLayout layout,
                             uint32_t variableCount);

    void Reset();

    void SetAcquireEpoch(uint64_t epoch)
    {
        m_acquireEpoch = epoch;
    }

    uint64_t GetAcquireEpoch()
    {
        return m_acquireEpoch;
    }

private:
    // A growable chain of VulkanDescriptorPool for ONE pool size.
    // Size grows at index, 32, 64, 128...
    struct PoolChain
    {
        VulkanDescriptorPoolKey poolKey;
        SmallVector<VulkanDescriptorPool*> pools;
        uint32_t activePoolIdx{0};
        uint32_t numPools{0};
        bool updateAfterBind{false};
    };

    PoolChain* AcquireChain(const VulkanDescriptorPoolKey& poolKey, bool updateAfterBind);

    VkDescriptorSet AllocateFromChain(PoolChain* pChain,
                                      VkDescriptorSetLayout layout,
                                      uint32_t variableCount);

    void PushPoolToChain(PoolChain* pChain);

    using PoolChainMap =
        FlatHashMap<VulkanDescriptorPoolKey, PoolChain*, VulkanDescriptorPoolKeyHasher>;

    VulkanDevice* m_pVulkanDevice{nullptr};

    static void DestroyChains(PoolChainMap& chainMap);

    static void ResetChains(PoolChainMap& chainMap);

    PoolChainMap m_nonUABChainMap;
    PoolChainMap m_UABChainMap;

    uint64_t m_acquireEpoch{0};
};

class VulkanDescriptorSetCache
{
public:
    VulkanDescriptorSetCache(VulkanDescriptorPoolManager2* pPoolManager,
                             VulkanDevice* pVulkanDevice);

    ~VulkanDescriptorSetCache();

    VulkanDescriptorSetCache(const VulkanDescriptorSetCache&) = delete;

    VulkanDescriptorSetCache& operator=(const VulkanDescriptorSetCache&) = delete;

    void BeginFrame(uint32_t frameNumber);

    void Destroy();

private:
    static constexpr uint32_t kMaxPoolLookups     = 2;
    static constexpr uint32_t kRingSlots          = kMaxPoolLookups + 1;
    static constexpr uint32_t kMaxSetsPerPool     = 512;
    static constexpr uint32_t kGCFrameDelayMargin = 1;

    struct ContentKey
    {
        uint32_t layoutId{0};
        size_t resourceBindingsHash{0};

        bool operator==(const ContentKey& other) const noexcept
        {
            return layoutId == other.layoutId && resourceBindingsHash == other.resourceBindingsHash;
        }

        bool operator!=(const ContentKey& other) const noexcept
        {
            return !(*this == other);
        }
    };

    struct ContentKeyHasher
    {
        size_t operator()(const ContentKey& contentKey) const noexcept
        {
            size_t hash = 0;
            util::HashCombine(hash, contentKey.layoutId);
            util::HashCombine(hash, contentKey.resourceBindingsHash);

            return hash;
        }
    };
    struct Entry
    {
        ContentKey key;
        VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
        uint64_t slotGeneration{0};
    };

    struct RingSlot
    {
        VulkanDescriptorPoolSetContainer* pContainer{nullptr};
        uint64_t generation{0};
        uint64_t lastUsedFrame{0};
        uint32_t numLiveEntries{0};
    };

    VkDescriptorSet Find(const ContentKey& key);

    VkDescriptorSet Insert(const ContentKey& key,
                           const VulkanDescriptorPoolKey& poolKey,
                           VkDescriptorSetLayout layout,
                           bool updateAfterBind,
                           uint32_t variableCount);

    void RotateRing();

    void RetireOldestSlot();

    void CompactEntries();

    VulkanDescriptorPoolManager2* m_pPoolManager{nullptr};
    VulkanDevice* m_pDevice{nullptr};

    HeapVector<Entry> m_entries;

    // ContentKey -> first entry index
    FlatHashMap<ContentKey, uint32_t, ContentKeyHasher> m_entryIndexMap;

    // Resolve entry collisions
    HeapVector<uint32_t> m_entryHashChain;

    RingSlot m_ringSlots[kRingSlots];
    uint32_t m_numLiveSlots{0};
    uint64_t m_nextSlotGeneration{0};
    uint64_t m_currentFrame{0};

    uint64_t m_gcFrameDelay{0};

    uint64_t m_entryHighWaterMark{0};
    uint64_t m_numSlotRetires{0};

    friend class VulkanDescriptorSetState;
};

class VulkanDescriptorPoolManager2
{
public:
    VulkanDescriptorPoolManager2(VulkanDevice* pDevice);

    void BeginFrame(uint32_t frameNumber);

    void Destroy();

    VulkanDescriptorPoolSetContainer* AcquireDescriptorPoolSetContainer();

    void ReleaseContainer(VulkanDescriptorPoolSetContainer* pContainer);

    void RetireContainer(VulkanDescriptorPoolSetContainer* pContainer, VulkanQueue* pQueue);

    void AssignReteireSerial(VulkanQueue* pQueue, uint64_t serial);

    void TickPoolSetContainers();

    VulkanDescriptorSetCache* GetContentCache() const
    {
        return m_pContentCache;
    }

    uint32_t GetOrCreateLayoutId(size_t layoutHash);

private:
    void DestroyContainers();

    void ReclaimCompletedContainers();

    struct FreeContainerEntry
    {
        VulkanDescriptorPoolSetContainer* pContainer{nullptr};
        uint32_t idleTicks{0};
    };

    struct PendingContainerEntry
    {
        VulkanDescriptorPoolSetContainer* pContainer{nullptr};
        VulkanQueue* pQueue{nullptr};
        uint64_t serial{0};
        uint32_t numFramesAwaitingSubmission{0};
    };

    VulkanDevice* m_pDevice{nullptr};

    HeapVector<VulkanDescriptorPoolSetContainer*> m_usedContainers;

    HeapVector<FreeContainerEntry> m_freeContainers;

    HeapVector<PendingContainerEntry> m_awaitingSubmissionContainers;

    HeapVector<PendingContainerEntry> m_awaitingCompletionContainers;

    VulkanDescriptorSetCache* m_pContentCache;

    // content hash -> layoutId
    FlatHashMap<size_t, uint32_t> m_layoutIdMap{0};
    uint32_t m_nextLayoutId{1};

    uint64_t m_nextAcquireEpoch{1};
};

inline constexpr uint32_t kBindlessHeapCapacity[ToUnderlying(RHIBindlessHeapType::eMax)] = {
    2048, // eTexture2D
    64,   // eTextureCube
    16    // eSampler
};

inline constexpr uint32_t GetBindlessHeapCapacity(RHIBindlessHeapType heapType)
{
    const uint32_t heapIdx = ToUnderlying(heapType);
    return heapIdx < ToUnderlying(RHIBindlessHeapType::eMax) ? kBindlessHeapCapacity[heapIdx] : 0;
}

class VulkanBindlessDescriptorPoolManager
{
public:
    VulkanBindlessDescriptorPoolManager() = default;

    void Init();

    void Destroy();

    bool RegisterBindlessResource(RHIResource* pResource, uint32_t slotIdx);

    // Write all registered bindless resources in batch, call WriteDescriptorSetBatch
    void Flush();

    VkDescriptorSetLayout GetGlobalBindlessLayout() const;

    VkDescriptorSet GetGlobalBindlessSet() const;

private:
    struct BindlessDSWrite
    {
        uint32_t slotIdx{0};
        RHIResource* pResource{nullptr};
    };

    struct BindlessSlotState
    {
        uint64_t resourceId{0};
        uint32_t resourceGeneration{0};
    };

    void CreateGlobalBindlessDescriptorSet();

    void WriteDescriptorSetBatch();

    VulkanDevice* m_pDevice{nullptr};

    VkDescriptorSet m_vkSet{VK_NULL_HANDLE};
    VkDescriptorPool m_vkPool{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_vkLayout{VK_NULL_HANDLE};

    uint32_t m_heapAllocCount[ToUnderlying(RHIBindlessHeapType::eMax)]{};

    HeapVector<BindlessDSWrite> m_pendingWrites[ToUnderlying(RHIBindlessHeapType::eMax)];
    HeapVector<BindlessSlotState> m_slotStates[ToUnderlying(RHIBindlessHeapType::eMax)];

    Mutex m_mutex;
};
} // namespace zen
