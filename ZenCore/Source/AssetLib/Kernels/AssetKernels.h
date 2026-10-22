#pragma once
#include <cstddef>
#include <cstdint>

// Byte-throughput loops used on whole asset payloads. They are compiled with
// optimizations in every configuration (see this directory's CMakeLists.txt),
// so they take only raw pointers and no checked containers.
namespace zen::asset::kernels
{
// Streaming XXH64. The state is plain data so callers can embed it anywhere.
struct HashState
{
    uint64_t lanes[4];
    uint64_t length;
    uint64_t seed;
    uint8_t  pending[32];
    uint32_t pendingSize;
};

void     ResetHash(HashState& state, uint64_t seed);
void     UpdateHash(HashState& state, const void* data, size_t size);
uint64_t FinishHash(const HashState& state);

bool AllFinite(const float* values, size_t count);
bool AllFinite(const double* values, size_t count);
// True when every index lies in [first, first + count).
bool IndicesInRange(const uint32_t* indices, size_t indexCount, uint32_t first, uint32_t count);
} // namespace zen::asset::kernels
