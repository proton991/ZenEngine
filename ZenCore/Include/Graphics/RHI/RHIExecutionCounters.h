#pragma once

#include "RHIOptions.h"
#include <atomic>
#include <cstdint>

namespace zen
{
// Cumulative backend counters. Consumers subtract snapshots for a frame/run interval.
struct RHIExecutionCounters
{
    uint64_t draws{0};
    uint64_t dispatches{0};
    uint64_t submissions{0};
    uint64_t descriptorHits{0};
    uint64_t descriptorMisses{0};
    uint64_t descriptorInserts{0};
    uint64_t descriptorRetirements{0};
    uint64_t bindlessCaptures{0};
};

struct RHIExecutionCounterStorage
{
    std::atomic<uint64_t> draws{0};
    std::atomic<uint64_t> dispatches{0};
    std::atomic<uint64_t> submissions{0};
    std::atomic<uint64_t> descriptorHits{0};
    std::atomic<uint64_t> descriptorMisses{0};
    std::atomic<uint64_t> descriptorInserts{0};
    std::atomic<uint64_t> descriptorRetirements{0};
    std::atomic<uint64_t> bindlessCaptures{0};

    void Increment(std::atomic<uint64_t>& counter)
    {
        if (RHIOptions::GetInstance().ExecutionCountersEnabled())
        {
            counter.fetch_add(1, std::memory_order_relaxed);
        }
    }

    RHIExecutionCounters Read() const
    {
        return {draws.load(std::memory_order_relaxed),
                dispatches.load(std::memory_order_relaxed),
                submissions.load(std::memory_order_relaxed),
                descriptorHits.load(std::memory_order_relaxed),
                descriptorMisses.load(std::memory_order_relaxed),
                descriptorInserts.load(std::memory_order_relaxed),
                descriptorRetirements.load(std::memory_order_relaxed),
                bindlessCaptures.load(std::memory_order_relaxed)};
    }
};
} // namespace zen
