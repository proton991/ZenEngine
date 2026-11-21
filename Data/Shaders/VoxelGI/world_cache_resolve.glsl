#define GI_CACHE_WRITE
#define GI_CACHE_EXCLUSIVE_READ
#define WRC_LIGHTING_WRITE
#include "world_cache.glsl"
#include "world_cache_accumulate.glsl"
layout(local_size_x = 256) in;
void main()
{
    uint slot = gl_GlobalInvocationID.x;
    if (slot >= giCache.limits.x)
    {
        return;
    }
    if (CacheLoadKey(slot) == CacheKey(0))
    {
        return;
    }
    uint samples = min(worldCells[slot].samples, kWorldSamples);
    bool expired = samples == 0u && worldCache.schedule.x - worldCells[slot].lastSample >= giCache.limits.z;
    if (!expired && samples == 0u)
    {
        return;
    }
    if (expired)
    {
#ifdef GI_CACHE_KEY64
        cacheKeys[slot] = CacheKey(0);
#else
        cacheKeys[slot] = uvec2(0);
#endif
        atomicAdd(cacheStats.z, 0xffffffffu);
    }
    else if (cacheResolved[slot * GI_CACHE_RECORDS].z == 0u)
    {
        atomicAdd(cacheStats.z, 1u);
    }
    uint cap = giCache.limits.y;
    if (min(sourceSignature.y, sourceSignature.z) < 128u)
    {
        cap = uint(mix(min(16.0, float(cap)), float(cap), uintBitsToFloat(sourceSignature.w)));
    }
    if (samples != 0u && cacheResolved[slot * GI_CACHE_RECORDS].z != 0u
        && worldCache.schedule.x - worldCells[slot].lastSample > min(sourceSignature.y, sourceSignature.z))
    {
        cap = lightChange.z != 0u ? min(cap, 16u) : 1u;
    }
    bool stale = samples != 0u && cacheResolved[slot * GI_CACHE_RECORDS].z != 0u && !WorldHistoryUsable(slot);
    if (stale)
    {
        cap = 1u;
    }
    for (uint axis = 0u; axis < GI_CACHE_RECORDS; ++axis)
    {
        uint  address = slot * GI_CACHE_RECORDS + axis;
        uvec4 result  = uvec4(0);
        if (samples != 0u)
        {
            uvec4 old = cacheResolved[address], sums = cacheAccumulation[address];
            bool  inherit  = old.z == 0u && worldCells[slot].parentAge != 0u;
            uint  age      = min(cap, 1u + (inherit ? worldCells[slot].parentAge : old.w));
            vec3  previous = inherit ? HybridUnpackSignedRGBE(sums.w) : CacheUnpack(old);
            vec3  current  = vec3(sums.xyz) / (kWorldScale * float(samples));
            // Directional records retain the sampling ordinal across lighting edits.
            // The optional seed counts only observations under the current lighting.
            bool restart = stale && axis == WORLD_SEED_RECORD;
            uint count   = restart ? samples : old.z + samples;
            // Preserve maturity when the direction sequence wraps in a long-lived hot cell.
            if (!restart && count < old.z)
            {
                count = max(count, 128u);
            }
            result = uvec4(CachePack(mix(previous, current, 1.0 / float(age))), count, age);
        }
        cacheResolved[address]     = result;
        cacheAccumulation[address] = uvec4(0);
    }
    worldCells[slot] = WorldCellState(0u, worldCache.schedule.x, 0u);
}
