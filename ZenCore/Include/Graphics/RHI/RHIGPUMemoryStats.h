#pragma once

#include <cstdint>
#include <array>

namespace zen
{
// Commitment counters cover this allocator, including pools and pending retirement.
// Optional heap usage/budgets are driver estimates and can include external usage.
struct RHIGPUMemoryStats
{
    struct Heap
    {
        uint64_t sizeBytes{0};
        uint64_t usageBytes{0};
        uint64_t budgetBytes{0};
        bool     deviceLocal{false};
    };

    bool                 available{false};
    uint64_t             committedBytes{0};
    uint64_t             peakCommittedBytes{0};
    uint64_t             deviceLocalBytes{0};
    uint64_t             peakDeviceLocalBytes{0};
    bool                 budgetAvailable{false};
    uint32_t             heapCount{0};
    std::array<Heap, 16> heaps{};

    bool IsUnderPressure() const
    {
        bool pressure = false;

        for (uint32_t i = 0; budgetAvailable && i < heapCount && i < heaps.size(); ++i)
        {
            const Heap& heap = heaps[i];

            pressure |=
                heap.deviceLocal && heap.budgetBytes != 0 && heap.usageBytes >= heap.budgetBytes - heap.budgetBytes / 10;
        }

        return pressure;
    }
};
} // namespace zen
