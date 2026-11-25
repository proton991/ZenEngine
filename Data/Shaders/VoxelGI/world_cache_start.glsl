#define WORLD_PATH_START
#define GI_CACHE_WRITE
#define WRC_LIGHTING_WRITE
#include "hybrid_guides.glsl"
#include "world_cache.glsl"
#include "world_cache_path.glsl"
#include "gi_common.glsl"
#include "world_cache_sampling.glsl"
#include "world_cache_accumulate.glsl"
layout(local_size_x = 8, local_size_y = 8) in;
void WorldStart(uvec2 tile, out WorldPath path)
{
    uvec2 extent   = uvec2(textureSize(receiverDepth, 0));
    uint  size     = worldCache.schedule.y;
    uvec2 start    = tile * size;
    uvec2 tileSize = min(uvec2(size), extent - start);
    uvec2 offset   = uvec2(HybridSkySample(tile, worldCache.schedule.x, 0x378au) * vec2(tileSize));
    ivec2 pixel    = ivec2(start + offset);
    bool  visible  = texelFetch(receiverDepth, pixel, 0).r < 1.0;
    // A partially covered tile still has a useful path origin. Probe the other
    // quadrants in a rotated order; completely empty tiles leave their path dead.
    for (uint i = 1u; i < 4u && !visible; ++i)
    {
        uvec2 quadrant = uvec2(i & 1u, i >> 1u);
        pixel          = ivec2(start + (offset + quadrant * ((tileSize + 1u) / 2u)) % tileSize);
        visible        = texelFetch(receiverDepth, pixel, 0).r < 1.0;
    }
    vec3  position = HybridPosition(pixel), normal = HybridNormal(pixel), geometric = HybridGeometricNormal(pixel);
    vec4  surface  = texelFetch(receiverRoughness, pixel, 0);
    float specular = hybrid.reflection.x != 0u ? mix(.04, 1.0, clamp(surface.r, 0.0, 1.0)) : 0.0;
    uint  seed     = HybridHash(HybridMorton(tile) ^ HybridHash(worldCache.schedule.x));
    bool  glossy   = float(seed >> 8u) / 16777216.0 < specular;
    // Include the G-buffer receiver as one of the three vertices of a diffuse path.
    // This supplies isolated visible surfaces that secondary rays never revisit.
    uint ordinal = 0u, slot = kNoWorldCell;
    if (visible && !glossy)
    {
        slot = WorldBeginSample(position, geometric, ordinal);
    }
    // Direction is stratified per raster pixel, independently of which pixels
    // win atomics at the same cell. An 8x8 tile visits each pixel once per 64 frames.
    uint  pixelVisit = worldCache.schedule.x / (size * size);
    vec2  u          = HybridSkySample(uvec2(pixel), pixelVisit, 0xf177u);
    float weight     = 0.0;
    vec3  direction;
    if (glossy)
    {
        direction = HybridGGXSample(normal, normalize(giCache.camera.xyz - position), clamp(surface.g, .04, 1.0), u);
    }
    else
    {
        direction = WorldSampleDirection(geometric, u, true, weight);
    }
    bool activeRay = visible && dot(direction, geometric) > 0.0;
    path.origin    = vec4(position, uintBitsToFloat(packUnorm2x16(EncodeGBufferNormal(geometric))));
    path.ray       = vec4(direction, weight);
    path.state     = uvec4(slot, packUnorm2x16(EncodeGBufferNormal(normal)), floatBitsToUint(1.0),
                           activeRay ? (glossy ? kMaterial : kIncludesReceiver) : kDead);
    path.hit       = uvec4(0xffffffffu);
}
void main()
{
    uvec2 tile  = gl_GlobalInvocationID.xy;
    uint  size  = worldCache.schedule.y;
    uvec2 tiles = (uvec2(textureSize(receiverDepth, 0)) + size - 1u) / size;
    if (all(lessThan(tile, tiles)))
    {
        WorldPath path;
        WorldStart(tile, path);
        paths[tile.y * tiles.x + tile.x] = path;
    }
}
