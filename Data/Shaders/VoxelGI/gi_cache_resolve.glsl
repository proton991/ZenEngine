#define GI_CACHE_WRITE
#include "gi_radiance_cache.glsl"
void ResolveCacheRecord(uint slot, bool clear)
{
    if (slot < giCache.limits.x * GI_CACHE_RECORDS)
    {
        uvec4 old     = clear ? uvec4(0) : cacheResolved[slot];
        uvec4 added   = clear ? uvec4(0) : cacheAccumulation[slot];
        bool  invalid = (added.w & 0x80000000u) != 0u;
        if (giCache.limits.w != 0u || invalid)
        {
            old = uvec4(0);
        }
        if (invalid)
        {
            added = uvec4(0);
        }
        uint  age = old.w & 255u, stale = (old.w >> 8u) & 255u, falling = (old.w >> 16u) & 255u;
        uvec4 next = old;
        if (!clear && added.w != 0u)
        {
            vec3 current = vec3(added.xyz) / (65536.0 * float(added.w));
            vec3 history = CacheUnpack(old);
            falling = old.z != 0u && dot(current, vec3(0.2126, 0.7152, 0.0722)) < dot(history, vec3(0.2126, 0.7152, 0.0722))
                        ? min(falling + 1u, 32u)
                        : 0u;
            if (falling == 32u)
            {
                age = 0u;
            }
            uint historyLimit = clamp(giCache.limits.y, 1u, 255u);
            age               = min(age + 1u, historyLimit);
            vec3 value        = mix(history, current, 1.0 / float(age));
            next              = uvec4(CachePack(value), min(old.z + added.w, 65535u), age | (falling << 16u));
        }
        else
        {
            stale  = min(stale + 1u, 255u);
            next.w = age | (stale << 8u);
        }
        if (clear || stale >= giCache.limits.z && added.w == 0u)
        {
#ifdef GI_CACHE_KEY64
            if (slot % GI_CACHE_RECORDS == 0u)
            {
                cacheKeys[slot / GI_CACHE_RECORDS] = uint64_t(0);
            }
#else
            if (slot % GI_CACHE_RECORDS == 0u)
            {
                cacheKeys[slot / GI_CACHE_RECORDS] = uvec2(0);
            }
#endif
            next = uvec4(0);
        }
        cacheResolved[slot]     = next;
        cacheAccumulation[slot] = uvec4(0);
    }
}

layout(local_size_x = 256) in;
void main()
{
    ResolveCacheRecord(gl_GlobalInvocationID.x, giCache.grid.w != 0.0);
}
