#include "Graphics/VulkanRHI/VulkanLifetimeTracker.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include <algorithm>
#include <atomic>

namespace zen
{
void VulkanLifetimeTracker::DestroyResources(VectorView<const RetiredResource> resources)
{
    for (const RetiredResource& resource : resources)
    {
        resource.first(resource.second);
    }
}

bool VulkanLifetimeTracker::Entry::IsComplete() const
{
    return recordings == 0 &&
        std::all_of(submissions.begin(), submissions.end(), [](const QueueSerial& submission) {
               return submission.pQueue->GetLastCompletedSerial() >= submission.serial;
           });
}

uint64_t VulkanLifetimeTracker::Create()
{
    // Ordered, process-wide IDs also identify bindless retirement boundaries and
    // cannot alias IDs from an earlier owner or tracker initialization.
    static std::atomic<uint64_t> nextId{1};
    LockAuto lock(&m_mutex);
    const uint64_t id = nextId.fetch_add(1, std::memory_order_relaxed);
    m_entries.try_emplace(id);
    return id;
}

void VulkanLifetimeTracker::RetainRecording(uint64_t id)
{
    if (id != 0)
    {
        LockAuto lock(&m_mutex);
        auto it = m_entries.find(id);
        VERIFY_EXPR(it != m_entries.end() && !it->second.retired);
        if (it != m_entries.end() && !it->second.retired)
        {
            ++it->second.recordings;
        }
    }
}

void VulkanLifetimeTracker::ReleaseRecordings(VectorView<const uint64_t> ids)
{
    LockAuto lock(&m_mutex);
    for (uint64_t id : ids)
    {
        auto it = m_entries.find(id);
        // Owners may already have been destroyed during terminal device teardown.
        if (it != m_entries.end())
        {
            VERIFY_EXPR(it->second.recordings > 0);
            if (it->second.recordings > 0)
            {
                --it->second.recordings;
            }
        }
    }
}

void VulkanLifetimeTracker::SubmitRecordings(VectorView<const uint64_t> ids,
                                             const VulkanQueue* pQueue,
                                             uint64_t serial)
{
    VERIFY_EXPR(pQueue != nullptr && serial > 0);
    if (pQueue != nullptr && serial > 0)
    {
        LockAuto lock(&m_mutex);
        for (uint64_t id : ids)
        {
            auto it = m_entries.find(id);
            VERIFY_EXPR(it != m_entries.end() && it->second.recordings > 0);
            if (it == m_entries.end() || it->second.recordings == 0)
            {
                continue;
            }
            Entry& entry = it->second;
            auto submission =
                std::find_if(entry.submissions.begin(), entry.submissions.end(),
                             [pQueue](const QueueSerial& value) { return value.pQueue == pQueue; });
            if (submission == entry.submissions.end())
            {
                entry.submissions.push_back({pQueue, serial});
            }
            else
            {
                submission->serial = std::max(submission->serial, serial);
            }
            --entry.recordings;
        }
    }
}

bool VulkanLifetimeTracker::HasRecordings(uint64_t id)
{
    LockAuto lock(&m_mutex);
    auto it = m_entries.find(id);
    return it != m_entries.end() && it->second.recordings > 0;
}

bool VulkanLifetimeTracker::IsComplete(uint64_t id)
{
    LockAuto lock(&m_mutex);
    auto it = m_entries.find(id);
    return it != m_entries.end() && it->second.IsComplete();
}

void VulkanLifetimeTracker::Retire(uint64_t id, void* pResource, DestroyResource destroy)
{
    bool retired = false;
    {
        LockAuto lock(&m_mutex);
        auto it = m_entries.find(id);
        VERIFY_EXPR(it != m_entries.end() && !it->second.retired);
        if (it != m_entries.end() && !it->second.retired)
        {
            retired = true;
            m_retired.push_back(id);
            it->second.pResource = pResource;
            it->second.destroy   = destroy;
            it->second.retired   = true;
        }
    }
    if (retired)
    {
        Collect();
    }
}

void VulkanLifetimeTracker::Collect()
{
    SmallVector<RetiredResource, 8> resources;
    {
        LockAuto lock(&m_mutex);
        size_t retained = 0;
        for (uint64_t id : m_retired)
        {
            auto it = m_entries.find(id);
            if (!it->second.IsComplete())
            {
                m_retired[retained++] = id;
                continue;
            }
            if (it->second.destroy != nullptr)
            {
                resources.push_back({it->second.destroy, it->second.pResource});
            }
            m_entries.erase(it);
        }
        m_retired.resize(retained);
    }
    DestroyResources(resources);
}

void VulkanLifetimeTracker::RemoveQueue(const VulkanQueue* pQueue)
{
    {
        LockAuto lock(&m_mutex);
        for (FlatHashMap<uint64_t, Entry>::value_type& item : m_entries)
        {
            Entry& entry                             = item.second;
            SmallVector<QueueSerial, 3>& submissions = entry.submissions;
            submissions.erase(std::remove_if(submissions.begin(), submissions.end(),
                                             [pQueue](const QueueSerial& value) {
                                                 return value.pQueue == pQueue;
                                             }),
                              submissions.end());
        }
    }
    Collect();
}

void VulkanLifetimeTracker::Destroy()
{
    // Called after work has stopped and owners have retired their storage, before
    // native allocators/device destruction. Device loss also ends these lifetimes.
    SmallVector<RetiredResource, 8> resources;
    {
        LockAuto lock(&m_mutex);
        for (FlatHashMap<uint64_t, Entry>::value_type& item : m_entries)
        {
            Entry& entry = item.second;
            VERIFY_EXPR(entry.retired);
            if (entry.destroy != nullptr)
            {
                resources.push_back({entry.destroy, entry.pResource});
            }
        }
        m_retired.clear();
        m_entries.clear();
    }
    DestroyResources(resources);
}

size_t VulkanLifetimeTracker::GetTrackedCount()
{
    LockAuto lock(&m_mutex);
    return m_entries.size();
}
} // namespace zen
