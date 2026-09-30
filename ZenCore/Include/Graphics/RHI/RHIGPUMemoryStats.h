#pragma once

#include <cstdint>

namespace zen
{
// Approximate live allocator commitments, including pools and pending retirement.
// Excludes driver-private, swapchain and other applications' allocations.
struct RHIGPUMemoryStats
{
    bool available{false};
    uint64_t committedBytes{0};
    uint64_t peakCommittedBytes{0};
    uint64_t deviceLocalBytes{0};
    uint64_t peakDeviceLocalBytes{0};
};
} // namespace zen
