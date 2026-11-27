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
    uint  cap              = giCache.limits.y;
    float previousResponse = unpackHalf2x16(worldCells[slot].response).x;
    float change           = min(1.0, float(worldCells[slot].change) / (kWorldScale * float(max(samples, 1u))));
    if (editLog.w != 0u)
    {
        // Use the previous observation's evidence so brightness of a stochastic feedback hit
        // cannot increase its own weight. A new cache also needs time for multiple bounces to fill.
        float startup = uintBitsToFloat(responseLog.w);
        cap           = uint(WorldHistoryCap(previousResponse, .375, mix(min(16.0, float(cap)), float(cap), startup)));
    }
    else if (min(sourceSignature.y, sourceSignature.z) < 128u)
    {
        cap = uint(mix(min(16.0, float(cap)), float(cap), uintBitsToFloat(sourceSignature.w)));
    }
    if (editLog.w == 0u && samples != 0u && cacheResolved[slot * GI_CACHE_RECORDS].z != 0u
        && worldCache.schedule.x - worldCells[slot].lastSample > min(sourceSignature.y, sourceSignature.z))
    {
        cap = lightChange.z != 0u ? min(cap, 16u) : 1u;
    }
    bool stale = samples != 0u && cacheResolved[slot * GI_CACHE_RECORDS].z != 0u && !WorldHistoryUsable(slot);
    if (stale)
    {
        cap = 1u;
    }
    bool corrected      = false;
    uint directionalAge = 1u;
    for (uint axis = 0u; axis < GI_CACHE_RECORDS; ++axis)
    {
        uint  address = slot * GI_CACHE_RECORDS + axis;
        uvec4 result  = uvec4(0);
        if (samples != 0u)
        {
            uvec4 old = cacheResolved[address], sums = cacheAccumulation[address];
            uint  parentAge = worldCells[slot].parentAge & 255u;
            uint  recordCap = cap;
            if (worldCache.lightCount.w != 0u && axis == WORLD_MOVING_RECORD)
            {
                parentAge = worldCells[slot].parentAge >> 8u;
                // The record has no prediction while a mover moves. Once every mover is still,
                // its history grows by one frame per frame up to the settled limit.
                uint still = movingLog.w != 0u ? responseLog.y - movingLog.w : cap;
                recordCap  = old.z == 0u || WorldMovingHistoryComplete(worldCache.schedule.x - worldCells[slot].lastSample)
                               ? min(cap, 16u + min(still, cap))
                               : 1u;
            }
            bool inherit  = old.z == 0u && parentAge != 0u;
            uint age      = min(recordCap, 1u + (inherit ? parentAge : old.w));
            vec3 previous = inherit ? HybridUnpackSignedRGBE(sums.w) : CacheUnpack(old);
            // An 8-bit mantissa loses small increments in heavily populated cells. In those cells
            // use the measured response window and the full-precision current estimate.
            if (editLog.w != 0u && !stale && old.z != 0u && axis < 6u && sums.w != 0u && samples <= 64u)
            {
                corrected = true;
                // Clamp after blending. Clamping the noisy same-ray correction first keeps only its positive errors.
                previous = previous + HybridUnpackSignedRGBE(sums.w) / float(samples);
            }
            if (axis == 0u)
            {
                directionalAge = age;
            }
            vec3 current = vec3(sums.xyz) / (kWorldScale * float(samples));
            // Directional records retain the sampling ordinal across lighting edits.
            // The optional seed counts only observations under the current lighting.
            bool restart = stale && WORLD_HAS_SEED && axis == WORLD_SEED_RECORD;
            uint count   = restart ? samples : old.z + samples;
            // Preserve maturity when the direction sequence wraps in a long-lived hot cell.
            if (!restart && count < old.z)
            {
                count = max(count, 128u);
            }
            result = uvec4(CachePack(max(mix(previous, current, 1.0 / float(age)), vec3(0))), count, age);
        }
        cacheResolved[address]     = result;
        cacheAccumulation[address] = uvec4(0);
    }
    // A returning path can observe a direct update and a later feedback solve.
    // Give the residual a two-observation half-life across those solves.
    float residual = expired ? 0.0 : max(change, .70710678 * previousResponse);
    if (residual <= .005)
    {
        residual = 0.0;
    }
    // Ordinary samples enter a mature mean with weight 1/N. A signed correction
    // translates its retained part as well. Secondary paths observe this published
    // change, while the cell itself keeps the full evidence for its next update.
    float published  = max(corrected ? change : 0.0, residual / float(directionalAge));
    worldCells[slot] = WorldCellState(0u, worldCache.schedule.x, 0u, 0u, packHalf2x16(vec2(residual, published)));
}
