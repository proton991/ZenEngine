#include "debug_view.glsl"
#include "../Common/gbuffer.glsl"
#include "../VoxelGI/gi_radiance_cache.glsl"
layout(set = 1, binding = 0) uniform sampler2D cachePosition;
layout(set = 1, binding = 2) uniform sampler2D cacheNormal;
layout(std430, set = 1, binding = 74) readonly buffer CacheTraceCounts
{
    uvec4 counts[];
};
void main()
{
    ivec2 extent = textureSize(cachePosition, 0);
    ivec2 pixel  = clamp(ivec2(inUV * vec2(extent)), ivec2(0), extent - 1);
    vec3  color  = vec3(0);
    if (debug.selection.x == 0u)
    {
        vec3     position = texelFetch(cachePosition, pixel, 0).xyz;
        vec3     normal   = DecodeGBufferNormal(texelFetch(cacheNormal, pixel, 0).rg);
        CacheKey key      = CacheCellKey(position, normal, CacheLevel(position, giCache.camera.xyz));
        uint     hash     = uint(key);
        color             = vec3(uvec3(hash, hash >> 8, hash >> 16) & 255u) / 255.0;
    }
    else if (debug.selection.x == 1u)
    {
        uint slot =
            min(uint((float(pixel.x) + float(pixel.y) * extent.x) / float(extent.x * extent.y) * float(giCache.limits.x)),
                giCache.limits.x - 1u);
        uvec4 value = cacheResolved[slot];
        color       = value.z != 0u ? vec3(float(value.w & 255u) / float(giCache.limits.y), 1, 0) : vec3(0);
    }
    else
    {
        uint  offset = debug.selection.z + 2u * (uint(pixel.y) * uint(extent.x) + uint(pixel.x));
        uvec4 rays = counts[offset], cache = counts[offset + 1u];
        float value = debug.selection.x == 2u ? float(cache.y) / max(float(cache.x), 1.0)
                                              : float(rays.x + rays.y + rays.z + rays.w) / max(debug.range.y, 1.0);
        color       = vec3(value);
    }
    outColor = vec4(clamp(color, vec3(0), vec3(1)), 1);
}
