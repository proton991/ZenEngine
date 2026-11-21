#define GI_CACHE_WRITE
#include "../gi_radiance_cache.glsl"
layout(local_size_x = 64) in;
layout(push_constant) uniform Check
{
    uint operation;
}
check;
layout(std430, set = 1, binding = 74) buffer Results
{
    vec4 results[];
};
void main()
{
    uint thread = gl_GlobalInvocationID.x;
    if (check.operation == 0u && thread == 0u)
    {
        vec3 normal = vec3(0, 0, 1);
        results[0]  = vec4(CacheLevel(vec3(0.5, 0, 0), vec3(0)), CacheLevel(vec3(4, 0, 0), vec3(0)),
                           CacheCellKey(vec3(-0.001, 0, 0), normal, 0) != CacheCellKey(vec3(0.001, 0, 0), normal, 0),
                           CacheCellKey(vec3(0.001), normal, 0) == CacheCellKey(vec3(0.009), normal, 0));
        bool valid  = true;
        for (uint index = 0u; index < 8u; ++index)
        {
            valid = valid && CacheClaim(CacheKey(1u + 16u * index)) == index + 1u;
        }
        valid = valid && CacheClaim(CacheKey(129u)) == 0xffffffffu;
#ifdef GI_CACHE_KEY64
        cacheKeys[2] = uint64_t(0);
#else
        cacheKeys[2] = uvec2(0);
#endif
        valid      = valid && CacheClaim(CacheKey(113u)) == 8u;
        results[1] = vec4(valid, CacheFind(CacheKey(113u)) == 8u, CacheClaim(CacheKey(129u)) == 2u, 0);
        results[2] = vec4(0);
    }
    else if (check.operation == 1u)
    {
        CacheAccumulate(vec3(0.001), vec3(0, 0, 1), vec3(0.125, 0.25, 0.5));
    }
    else if (check.operation == 2u && thread == 0u)
    {
        vec3 value;
        bool found = CacheLookup(vec3(0.001), vec3(0, 0, 1), 0, value);
        results[2] = vec4(value, found);
    }
}
