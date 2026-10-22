#include "AssetKernels.h"
#include <bit>
#include <cstring>

namespace zen::asset::kernels
{
namespace
{
constexpr uint64_t Prime1 = 0x9E3779B185EBCA87ull;
constexpr uint64_t Prime2 = 0xC2B2AE3D27D4EB4Full;
constexpr uint64_t Prime3 = 0x165667B19E3779F9ull;
constexpr uint64_t Prime4 = 0x85EBCA77C2B2AE63ull;
constexpr uint64_t Prime5 = 0x27D4EB2F165667C5ull;

uint64_t Read64(const uint8_t* bytes)
{
    uint64_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    if constexpr (std::endian::native == std::endian::big)
    {
        value = ((value & 0x00000000000000FFull) << 56) | ((value & 0x000000000000FF00ull) << 40)
              | ((value & 0x0000000000FF0000ull) << 24) | ((value & 0x00000000FF000000ull) << 8)
              | ((value & 0x000000FF00000000ull) >> 8) | ((value & 0x0000FF0000000000ull) >> 24)
              | ((value & 0x00FF000000000000ull) >> 40) | ((value & 0xFF00000000000000ull) >> 56);
    }
    return value;
}

uint32_t Read32(const uint8_t* bytes)
{
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    if constexpr (std::endian::native == std::endian::big)
    {
        value = ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) | ((value & 0x00FF0000u) >> 8)
              | ((value & 0xFF000000u) >> 24);
    }
    return value;
}

uint64_t Round(uint64_t accumulator, uint64_t input)
{
    accumulator += input * Prime2;
    accumulator  = std::rotl(accumulator, 31);
    return accumulator * Prime1;
}

uint64_t Merge(uint64_t hash, uint64_t lane)
{
    hash ^= Round(0, lane);
    return hash * Prime1 + Prime4;
}

void ProcessStripe(uint64_t* lanes, const uint8_t* bytes)
{
    lanes[0] = Round(lanes[0], Read64(bytes));
    lanes[1] = Round(lanes[1], Read64(bytes + 8));
    lanes[2] = Round(lanes[2], Read64(bytes + 16));
    lanes[3] = Round(lanes[3], Read64(bytes + 24));
}
} // namespace

void ResetHash(HashState& state, uint64_t seed)
{
    state.lanes[0]    = seed + Prime1 + Prime2;
    state.lanes[1]    = seed + Prime2;
    state.lanes[2]    = seed;
    state.lanes[3]    = seed - Prime1;
    state.length      = 0;
    state.seed        = seed;
    state.pendingSize = 0;
}

void UpdateHash(HashState& state, const void* data, size_t size)
{
    const uint8_t* bytes  = static_cast<const uint8_t*>(data);
    state.length         += size;
    if (state.pendingSize + size < 32)
    {
        if (size > 0)
        {
            std::memcpy(state.pending + state.pendingSize, bytes, size);
        }
        state.pendingSize += static_cast<uint32_t>(size);
    }
    else
    {
        if (state.pendingSize > 0)
        {
            const size_t fill = 32 - state.pendingSize;
            std::memcpy(state.pending + state.pendingSize, bytes, fill);
            ProcessStripe(state.lanes, state.pending);
            bytes             += fill;
            size              -= fill;
            state.pendingSize  = 0;
        }
        uint64_t lanes[4] = {state.lanes[0], state.lanes[1], state.lanes[2], state.lanes[3]};
        while (size >= 32)
        {
            ProcessStripe(lanes, bytes);
            bytes += 32;
            size  -= 32;
        }
        state.lanes[0] = lanes[0];
        state.lanes[1] = lanes[1];
        state.lanes[2] = lanes[2];
        state.lanes[3] = lanes[3];
        if (size > 0)
        {
            std::memcpy(state.pending, bytes, size);
        }
        state.pendingSize = static_cast<uint32_t>(size);
    }
}

uint64_t FinishHash(const HashState& state)
{
    uint64_t hash = 0;
    if (state.length >= 32)
    {
        hash = std::rotl(state.lanes[0], 1) + std::rotl(state.lanes[1], 7) + std::rotl(state.lanes[2], 12)
             + std::rotl(state.lanes[3], 18);
        hash = Merge(hash, state.lanes[0]);
        hash = Merge(hash, state.lanes[1]);
        hash = Merge(hash, state.lanes[2]);
        hash = Merge(hash, state.lanes[3]);
    }
    else
    {
        hash = state.seed + Prime5;
    }
    hash                 += state.length;
    const uint8_t* bytes  = state.pending;
    const uint8_t* end    = state.pending + state.pendingSize;
    while (bytes + 8 <= end)
    {
        hash  ^= Round(0, Read64(bytes));
        hash   = std::rotl(hash, 27) * Prime1 + Prime4;
        bytes += 8;
    }
    if (bytes + 4 <= end)
    {
        hash  ^= static_cast<uint64_t>(Read32(bytes)) * Prime1;
        hash   = std::rotl(hash, 23) * Prime2 + Prime3;
        bytes += 4;
    }
    while (bytes < end)
    {
        hash ^= static_cast<uint64_t>(*bytes) * Prime5;
        hash  = std::rotl(hash, 11) * Prime1;
        ++bytes;
    }
    hash ^= hash >> 33;
    hash *= Prime2;
    hash ^= hash >> 29;
    hash *= Prime3;
    hash ^= hash >> 32;
    return hash;
}

bool AllFinite(const float* values, size_t count)
{
    uint32_t invalid = 0;
    for (size_t index = 0; index < count; ++index)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, values + index, sizeof(bits));
        invalid |= static_cast<uint32_t>((bits & 0x7F800000u) == 0x7F800000u);
    }
    return invalid == 0;
}

bool AllFinite(const double* values, size_t count)
{
    uint64_t invalid = 0;
    for (size_t index = 0; index < count; ++index)
    {
        uint64_t bits = 0;
        std::memcpy(&bits, values + index, sizeof(bits));
        invalid |= static_cast<uint64_t>((bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull);
    }
    return invalid == 0;
}

bool IndicesInRange(const uint32_t* indices, size_t indexCount, uint32_t first, uint32_t count)
{
    uint32_t invalid = 0;
    for (size_t index = 0; index < indexCount; ++index)
    {
        invalid |= static_cast<uint32_t>(indices[index] - first >= count);
    }
    return invalid == 0;
}
} // namespace zen::asset::kernels
