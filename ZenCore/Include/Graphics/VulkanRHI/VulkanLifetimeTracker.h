#pragma once
#include "Templates/FlatHashMap.h"
#include "Templates/HeapVector.h"
#include "Templates/SmallVector.h"
#include "Templates/VectorView.h"
#include "Utils/Mutex.h"

namespace zen
{
class VulkanQueue;

// Shared bookkeeping for resources referenced by CPU recordings and GPU submissions.
// Owners keep IDs; workloads keep one ID per recording. No tracker pointers escape.
class VulkanLifetimeTracker
{
public:
    using DestroyResource = void (*)(void*);

    uint64_t Create();
    void RetainRecording(uint64_t id);
    void ReleaseRecordings(VectorView<const uint64_t> ids);
    void SubmitRecordings(VectorView<const uint64_t> ids,
                          const VulkanQueue* pQueue,
                          uint64_t serial);

    bool HasRecordings(uint64_t id);
    bool IsComplete(uint64_t id);

    // Drop owner membership. Optional resource destruction waits for every recording
    // and accepted submission, and runs outside the tracker lock.
    void Retire(uint64_t id, void* pResource = nullptr, DestroyResource destroy = nullptr);
    void Collect();

    // Queue/device teardown is terminal, including uncertain failed submissions.
    void RemoveQueue(const VulkanQueue* pQueue);
    void Destroy();

    size_t GetTrackedCount();

private:
    struct QueueSerial
    {
        const VulkanQueue* pQueue;
        uint64_t serial;
    };

    struct Entry
    {
        uint64_t recordings{0};
        SmallVector<QueueSerial, 3> submissions;
        void* pResource{nullptr};
        DestroyResource destroy{nullptr};
        bool retired{false};

        bool IsComplete() const;
    };

    FlatHashMap<uint64_t, Entry> m_entries;
    HeapVector<uint64_t> m_retired;
    Mutex m_mutex;
};
} // namespace zen
