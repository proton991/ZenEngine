#pragma once

#include "Graphics/RHI/RHICommandList.h"
#include "Templates/HeapVector.h"

namespace zen
{
class FVulkanCommandListContext;
class VulkanWorkload;

class VulkanPlatformCommandList final : public RHIPlatformCommandList
{
    friend class VulkanRHI;

public:
    void Reset()
    {
        m_workloads.clear();
        m_contextWorkloadRanges.clear();
    }

private:
    struct ContextWorkloadRange
    {
        FVulkanCommandListContext* pContext{nullptr};
        uint32_t firstWorkloadIndex{0};
        uint32_t workloadCount{0};
    };

    HeapVector<VulkanWorkload*> m_workloads;
    HeapVector<ContextWorkloadRange> m_contextWorkloadRanges;
};
} // namespace zen
