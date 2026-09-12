#pragma once
#include <cstdint>

namespace zen::rc
{
// Device-lifetime counters. CPU timers run only with RDG preparation timing enabled.
// Per-execution RDG reports take differences; resize invalidations can happen outside graphs.
struct PipelineCacheMetrics
{
    uint64_t requests{0};
    uint64_t hits{0};
    uint64_t misses{0};
    uint64_t creations{0};
    uint64_t failures{0};
    uint64_t evictions{0};
    uint64_t invalidations{0};
    uint64_t invalidatedEntries{0};
    uint64_t timedRequests{0};
    double keyCPUUs{0};
    double lookupCPUUs{0};
    double creationCPUUs{0};

    PipelineCacheMetrics Since(const PipelineCacheMetrics& previous) const
    {
        return {requests - previous.requests,
                hits - previous.hits,
                misses - previous.misses,
                creations - previous.creations,
                failures - previous.failures,
                evictions - previous.evictions,
                invalidations - previous.invalidations,
                invalidatedEntries - previous.invalidatedEntries,
                timedRequests - previous.timedRequests,
                keyCPUUs - previous.keyCPUUs,
                lookupCPUUs - previous.lookupCPUUs,
                creationCPUUs - previous.creationCPUUs};
    }

    void Accumulate(const PipelineCacheMetrics& other)
    {
        requests += other.requests;
        hits += other.hits;
        misses += other.misses;
        creations += other.creations;
        failures += other.failures;
        evictions += other.evictions;
        invalidations += other.invalidations;
        invalidatedEntries += other.invalidatedEntries;
        timedRequests += other.timedRequests;
        keyCPUUs += other.keyCPUUs;
        lookupCPUUs += other.lookupCPUUs;
        creationCPUUs += other.creationCPUUs;
    }
};
} // namespace zen::rc
