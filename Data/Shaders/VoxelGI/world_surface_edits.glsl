#ifndef ZEN_WORLD_SURFACE_EDITS
#define ZEN_WORLD_SURFACE_EDITS
#include "world_cache_lighting.glsl"
struct WorldSurfaceEdit
{
    uvec4 stamp;
    vec4  minimum, maximum;
};
layout(std140, set = 1, binding = 75) uniform uWorldCacheData
{
    uvec4            schedule;   // frame + 1, tile side, path count, initialization/geometry bits + lighting epoch
    uvec4            lightCount; // local lights, records per cell (6, or 7 with the seed record), indirect intensity bits
    vec4             localLights[33];
    uvec4            surfaceLog;
    WorldSurfaceEdit surfaceEdits[32];
    uvec4            movingLightIndices[9];
}
worldCache;
// Test finite ray segments, including parallel axes and points inside the box.
bool WorldSurfaceChanged(vec3 origin, vec3 direction, float distance, uint material, uint elapsed)
{
    if (editLog.w == 0u)
    {
        return false;
    }
    if (worldCache.surfaceLog.z != 0u && worldCache.schedule.x - worldCache.surfaceLog.z < elapsed)
    {
        return true;
    }
    for (uint i = 0u; i < worldCache.surfaceLog.y; ++i)
    {
        WorldSurfaceEdit edit = worldCache.surfaceEdits[(worldCache.surfaceLog.x + 31u - i) % 32u];
        if (worldCache.schedule.x - edit.stamp.x >= elapsed)
        {
            break;
        }
        if (edit.stamp.y != 0u)
        {
            if (edit.stamp.z == material)
            {
                return true;
            }
        }
        else
        {
            vec3 d = mix(direction, vec3(1e-20), lessThan(abs(direction), vec3(1e-20)));
            vec3 a = (edit.minimum.xyz - origin) / d, b = (edit.maximum.xyz - origin) / d;
            vec3 lo = min(a, b), hi = max(a, b);
            if (max(0.0, max(lo.x, max(lo.y, lo.z))) <= min(distance, min(hi.x, min(hi.y, hi.z))))
            {
                return true;
            }
        }
    }
    return false;
}
#endif
