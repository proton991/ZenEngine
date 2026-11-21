#ifndef ZEN_GI_RADIANCE_CACHE
#define ZEN_GI_RADIANCE_CACHE
#include "hybrid_sampling.glsl"
#ifndef GI_CACHE_RECORDS
#    define GI_CACHE_RECORDS 1u
#endif
#ifdef GI_CACHE_KEY64
#    extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#    extension GL_EXT_shader_atomic_int64 : require
#    define CacheKey uint64_t
#else
#    define CacheKey uint
#endif
layout(std140, set = 1, binding = 70) uniform uGICacheData
{
    uvec4 limits; // capacity (power of two), frame cap, stale cap, reset response
    vec4  camera;
    vec4  previousCamera;
    vec4  grid; // base cell size, minimum distance, moving, clear
}
giCache;
#ifdef GI_CACHE_WRITE
#    define CACHE_ACCESS
#else
#    define CACHE_ACCESS readonly
#endif
layout(std430, set = 1, binding = 71) CACHE_ACCESS buffer GICacheKeys
{
#ifdef GI_CACHE_KEY64
    uint64_t cacheKeys[];
#else
    uvec2 cacheKeys[]; // Keep the same 40-byte entry budget on both capability paths.
#endif
};
layout(std430, set = 1, binding = 72) CACHE_ACCESS buffer GICacheAccumulation
{
    uvec4 cacheAccumulation[];
};
layout(std430, set = 1, binding = 73) CACHE_ACCESS buffer GICacheResolved
{
    uvec4 cacheResolved[];
};

int CacheLevel(vec3 position, vec3 camera)
{
    return clamp(int(floor(log2(max(length(position - camera), giCache.grid.y) / giCache.grid.y))), 0, 20);
}
float CacheCellSize(int level)
{
    return giCache.grid.x * exp2(float(level));
}
uint CacheNormalBin(vec3 normal)
{
    vec3 magnitude = abs(normal);
    uint axis      = magnitude.x >= magnitude.y && magnitude.x >= magnitude.z ? 0u : magnitude.y >= magnitude.z ? 1u : 2u;
    return axis * 2u + uint(normal[axis] < 0.0);
}
CacheKey CacheSpatialKey(ivec3 cell, uint surface, int level)
{
    uint low = HybridHash(uint(cell.x) ^ HybridHash(uint(cell.y)) ^ HybridHash(uint(cell.z) + 0x92d68ca2u));
    low      = HybridHash(low ^ (uint(level) << 3u) ^ surface) | 1u;
#ifdef GI_CACHE_KEY64
    uint high = HybridHash(uint(cell.z) ^ HybridHash(uint(cell.x) + 0x18bc9a53u) ^ HybridHash(uint(cell.y) + 0xa0761d65u)
                           ^ (uint(level) * 8u + surface));
    return (uint64_t(high) << 32) | uint64_t(low);
#else
    return low;
#endif
}
CacheKey CacheCellKey(vec3 position, vec3 normal, int level)
{
    return CacheSpatialKey(ivec3(floor(position / CacheCellSize(level))), CacheNormalBin(normal), level);
}
CacheKey CacheLoadKey(uint slot)
{
#ifdef GI_CACHE_KEY64
#    if defined(GI_CACHE_WRITE) && !defined(GI_CACHE_EXCLUSIVE_READ)
    return atomicCompSwap(cacheKeys[slot], uint64_t(0), uint64_t(0));
#    else
    return cacheKeys[slot];
#    endif
#else
#    if defined(GI_CACHE_WRITE) && !defined(GI_CACHE_EXCLUSIVE_READ)
    return atomicCompSwap(cacheKeys[slot].x, 0u, 0u);
#    else
    return cacheKeys[slot].x;
#    endif
#endif
}
uint CacheProbeSlot(CacheKey key, uint probe)
{
#ifdef ZEN_WORLD_CACHE
    // The low bit of every valid key is set. Use the remaining bits for
    // the home slot and independently displaced pairs for collisions.
    // Independent pairs disperse keys with a shared home slot.
#    ifdef GI_CACHE_KEY64
    uint stride = uint(key >> 32u) | 1u;
#    else
    uint stride = HybridHash(uint(key) ^ 0xa0761d65u) | 1u;
#    endif
    uint address = (uint(key) >> 1u) + (probe & ~1u) * stride + (probe & 1u);
#else
    uint address = uint(key) + probe;
#endif
    return address & (giCache.limits.x - 1u);
}
#ifdef ZEN_WORLD_CACHE
const uint kCacheProbes = 4u;
#else
const uint kCacheProbes = 8u;
#endif
uint CacheFind(CacheKey key)
{
    uint found = 0xffffffffu;
    // Do not stop at an empty slot: eviction can leave a hole before a live colliding key.
    for (uint probe = 0u; probe < kCacheProbes && found == 0xffffffffu; ++probe)
    {
        uint slot = CacheProbeSlot(key, probe);
        if (CacheLoadKey(slot) == key)
        {
            found = slot;
        }
    }
    return found;
}
vec3 CacheUnpack(uvec4 data)
{
#ifdef ZEN_WORLD_CACHE
    vec2 chroma = unpackHalf2x16(data.x);
    vec3 color  = vec3(chroma, max(0.0, 1.0 - chroma.x - chroma.y));
    return color * (uintBitsToFloat(data.y) / max(1.0, chroma.x + chroma.y));
#else
    return vec3(unpackHalf2x16(data.x), unpackHalf2x16(data.y).x);
#endif
}
uvec2 CachePack(vec3 value)
{
#ifdef ZEN_WORLD_CACHE
    // Preserve intensity in FP32 so long feedback histories keep small updates.
    // Two normalized color coordinates need only FP16; reconstructing the third
    // preserves the RGB sum even when their quantization crosses the simplex edge.
    float magnitude = value.r + value.g + value.b;
    vec2  chroma    = magnitude > 0.0 ? value.rg / magnitude : vec2(0);
    return uvec2(packHalf2x16(chroma), floatBitsToUint(magnitude));
#else
    return uvec2(packHalf2x16(value.rg), packHalf2x16(vec2(value.b, 0)));
#endif
}
bool CacheLookup(vec3 position, vec3 normal, int level, out vec3 value)
{
    uint  slot = CacheFind(CacheCellKey(position, normal, level));
    uvec4 data = slot != 0xffffffffu ? cacheResolved[slot] : uvec4(0);
    value      = CacheUnpack(data);
    return data.z != 0u;
}
#ifdef GI_CACHE_WRITE
uint CacheClaim(CacheKey key)
{
    // An eviction hole may precede this key. Reuse its existing resolved history
    // before claiming a hole; otherwise one key can acquire two independent means.
    uint found = CacheFind(key);
    for (uint probe = 0u; probe < kCacheProbes && found == 0xffffffffu; ++probe)
    {
        uint slot = CacheProbeSlot(key, probe);
#    ifdef GI_CACHE_KEY64
        CacheKey old = atomicCompSwap(cacheKeys[slot], uint64_t(0), key);
#    else
        CacheKey old = atomicCompSwap(cacheKeys[slot].x, 0u, key);
#    endif
        if (old == CacheKey(0) || old == key)
        {
            found = slot;
        }
    }
    return found;
}
uvec4 CacheEncodeSample(vec3 sampleValue)
{
    bool invalid = any(greaterThan(sampleValue, vec3(255.0)));
    return uvec4(invalid ? uvec3(0) : uvec3(round(sampleValue * 65536.0)), invalid ? 0x80000000u : 1u);
}
void CacheAccumulateSlot(uint slot, vec3 sampleValue)
{
    // Bounded fixed-point additions cannot overflow even if all update paths share a cell.
    // Values outside the encoding range invalidate this cell for the frame.
    uvec4 encoded = CacheEncodeSample(sampleValue);
    if ((encoded.w & 0x80000000u) != 0u)
    {
        atomicOr(cacheAccumulation[slot].w, 0x80000000u);
    }
    else
    {
        uint count   = atomicAdd(cacheAccumulation[slot].w, 0u);
        bool claimed = false;
        while ((count & 0x3fffffffu) < 255u && (count & 0x80000000u) == 0u && !claimed)
        {
            uint previous = atomicCompSwap(cacheAccumulation[slot].w, count, count + 1u);
            claimed       = previous == count;
            count         = previous;
        }
        if (claimed)
        {
            atomicAdd(cacheAccumulation[slot].x, encoded.x);
            atomicAdd(cacheAccumulation[slot].y, encoded.y);
            atomicAdd(cacheAccumulation[slot].z, encoded.z);
        }
    }
}
void CacheAccumulate(vec3 position, vec3 normal, vec3 radiance)
{
    int  level = CacheLevel(position, giCache.camera.xyz);
    uint slot  = CacheClaim(CacheCellKey(position, normal, level));
    if (slot != 0xffffffffu && !any(isnan(radiance)) && !any(isinf(radiance)))
    {
        vec3 sampleValue = max(radiance, vec3(0));
        // The read-only resolved buffer still contains the previous frame here. At a level
        // transition seed an empty cell from its adjacent level, without mixing direct/emission.
        int previousLevel = CacheLevel(position, giCache.previousCamera.xyz);
        if (giCache.grid.z != 0.0 && cacheResolved[slot].z == 0u && abs(previousLevel - level) == 1)
        {
            vec3 adjacent;
            if (CacheLookup(position, normal, previousLevel, adjacent))
            {
                sampleValue = mix(adjacent, sampleValue, 0.5);
            }
        }
        CacheAccumulateSlot(slot, sampleValue);
    }
}
#endif
#undef CACHE_ACCESS
#endif
