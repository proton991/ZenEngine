#pragma once
#include <algorithm>
#include <atomic>
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

    // The manager owns one reference; recorded workloads retain additional references
    // until completion or a definite rejection. References may outlive the manager.
    uint32_t AddRef()
    {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t Release();

    uint32_t GetRefCount() const
    {
        return m_refCount.load(std::memory_order_acquire);
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

    std::atomic_uint32_t m_refCount{1};
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

    uint64_t GetEpoch() const
    {
        return m_nextSlotGeneration;
    }

private:
    static constexpr uint32_t kMaxPoolLookups     = 2;
    static constexpr uint32_t kRingSlots          = kMaxPoolLookups + 1;
    static constexpr uint32_t kMaxSetsPerPool     = 512;

    struct ContentKey
    {
        uint32_t layoutId{0};
        // Canonical binding metadata followed by every resource slot, including nulls.
        HeapVector<uint64_t> bindings;

        bool operator==(const ContentKey& other) const noexcept
        {
            return layoutId == other.layoutId && bindings.size() == other.bindings.size() &&
                std::equal(bindings.begin(), bindings.end(), other.bindings.begin());
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
            for (uint64_t value : contentKey.bindings)
            {
                util::HashCombine(hash, value);
            }

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

    VkDescriptorSet Find(const ContentKey& key, VulkanDescriptorPoolSetContainer*& outContainer);

    VkDescriptorSet Insert(const ContentKey& key,
                           const VulkanDescriptorPoolKey& poolKey,
                           VkDescriptorSetLayout layout,
                           bool updateAfterBind,
                           uint32_t variableCount,
                           VulkanDescriptorPoolSetContainer*& outContainer);

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

    // Remove from the cache; reset/reuse waits for every retaining workload.
    void ReleaseContainer(VulkanDescriptorPoolSetContainer* pContainer);

    void TickPoolSetContainers();

    VulkanDescriptorSetCache* GetContentCache() const
    {
        return m_pContentCache;
    }

    uint32_t GetOrCreateLayoutId(const VkDescriptorSetLayoutCreateInfo& createInfo,
                                 VectorView<const VkDescriptorBindingFlags> bindingFlags);

private:
    void DestroyContainers();

    void ReclaimRetiredContainers();

    struct FreeContainerEntry
    {
        VulkanDescriptorPoolSetContainer* pContainer{nullptr};
        uint32_t idleTicks{0};
    };

    struct LayoutKey
    {
        HeapVector<uint64_t> data;

        bool operator==(const LayoutKey& other) const
        {
            return data.size() == other.data.size() &&
                std::equal(data.begin(), data.end(), other.data.begin());
        }
    };

    struct LayoutKeyHasher
    {
        size_t operator()(const LayoutKey& key) const
        {
            size_t hash = 0;
            for (uint64_t value : key.data)
            {
                util::HashCombine(hash, value);
            }
            return hash;
        }
    };

    VulkanDevice* m_pDevice{nullptr};

    HeapVector<VulkanDescriptorPoolSetContainer*> m_usedContainers;

    HeapVector<FreeContainerEntry> m_freeContainers;

    HeapVector<VulkanDescriptorPoolSetContainer*> m_retiredContainers;

    VulkanDescriptorSetCache* m_pContentCache;

    FlatHashMap<LayoutKey, uint32_t, LayoutKeyHasher> m_layoutIdMap;
    uint32_t m_nextLayoutId{1};
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

class VulkanBindlessUse final : public RHIBindlessUse
{
public:
    VulkanBindlessUse(uint64_t owner, uint64_t epoch) : owner(owner), epoch(epoch) {}
    const uint64_t owner;
    const uint64_t epoch;
};

class VulkanBindlessDescriptorPoolManager
{
public:
    VulkanBindlessDescriptorPoolManager() = default;

    void Init();

    void Destroy();

    // Published slots remain immutable until explicitly retired and all earlier
    // uses finish. recordedUse permits idempotent playback of an old registration.
    bool RegisterBindlessResource(RHIResource* pResource,
                                  uint32_t slotIdx,
                                  RHIBindlessHandle* pOutHandle     = nullptr,
                                  const RHIBindlessUse* recordedUse = nullptr);
    bool UnregisterBindlessResource(RHIBindlessHandle handle);
    bool IsRegistered(RHIBindlessHandle handle);
    void CollectRetiredResources();
    RefCountPtr<RHIBindlessUse> CaptureUse();

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
        RHIResource* pResource{nullptr};
        RHITexture* pTextureOwner{nullptr};
        uint64_t generation{0};
        uint64_t retiredEpoch{0};
    };

    void CollectRetiredResourcesLocked();
    BindlessSlotState* FindRegistration(RHIBindlessHandle handle);

    void CreateGlobalBindlessDescriptorSet();

    void WriteDescriptorSetBatch();

    VulkanDevice* m_pDevice{nullptr};

    VkDescriptorSet m_vkSet{VK_NULL_HANDLE};
    VkDescriptorPool m_vkPool{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_vkLayout{VK_NULL_HANDLE};

    uint32_t m_heapAllocCount[ToUnderlying(RHIBindlessHeapType::eMax)]{};

    uint64_t m_ownerId{0};
    uint64_t m_epoch{1};
    HeapVector<RefCountPtr<VulkanBindlessUse>> m_uses;
    HeapVector<RHIBindlessHandle> m_retiredSlots;

    HeapVector<BindlessDSWrite> m_pendingWrites[ToUnderlying(RHIBindlessHeapType::eMax)];
    HeapVector<BindlessSlotState> m_slotStates[ToUnderlying(RHIBindlessHeapType::eMax)];

    Mutex m_mutex;
};
} // namespace zen
