#ifndef ZEN_WORLD_CACHE
#define ZEN_WORLD_CACHE
#include "world_surface_edits.glsl"
// Six directional records of irradiance/pi. The optional seventh holds only the indirect part along
// the cell's shading normal, which seeds disoccluded pixel histories (WorldIndirect).
#define WORLD_SEED_RECORD   6u
#define GI_CACHE_RECORDS    max(6u, worldCache.lightCount.y)
#define WORLD_HAS_SEED      (GI_CACHE_RECORDS - worldCache.lightCount.w == 7u)
#define WORLD_MOVING_RECORD (GI_CACHE_RECORDS - 1u)
#include "../Common/gbuffer.glsl"
#include "gi_radiance_cache.glsl"
struct WorldCellState
{
    uint samples;
    uint lastSample;
    uint parentAge; // settled parent observations in the low byte, moving observations in the next
    uint change;    // sum of normalized per-path evidence in kWorldScale units
    uint response;  // FP16 held evidence and change published in the last resolved mean
};
#ifdef GI_CACHE_WRITE
layout(std430, set = 1, binding = 76) buffer WorldCacheCells
#else
layout(std430, set = 1, binding = 76) readonly buffer WorldCacheCells
#endif
{
    WorldCellState worldCells[];
};
#include "world_cache_lighting.glsl"
// Bound spatial variation from inverse-square light sources. Camera distance
// alone can put an entire bright reflector in one cell when seen across a room.
int WorldLevel(vec3 position)
{
    int   level          = CacheLevel(position, giCache.camera.xyz);
    float footprint      = 8.0 * CacheCellSize(level);
    float nearestSquared = footprint * footprint;
    for (uint i = 0u; i < worldCache.lightCount.x; ++i)
    {
        vec3 delta     = worldCache.localLights[i].xyz - position;
        nearestSquared = min(nearestSquared, dot(delta, delta));
    }
    if (nearestSquared < footprint * footprint)
    {
        // Scene lighting clamps squared distance at 1e-6 inside a light.
        float baseFootprint = 8.0 * giCache.grid.x;
        int   localLevel    = int(floor(.5 * log2(max(nearestSquared, 1e-6) / (baseFootprint * baseFootprint))));
        level               = min(level, max(localLevel, -4));
    }
    return level;
}
uint WorldFind(vec3 position, vec3 geometric, int level, ivec3 offset)
{
    ivec3 cell = ivec3(floor(position / CacheCellSize(level))) + offset;
    return CacheFind(CacheSpatialKey(cell, CacheNormalBin(geometric), level));
}
// A sparse cell may miss every sample during a light edit. Only read records resolved
// since the last global change, including when a later local change has occurred.
bool WorldHistoryUsable(uint slot)
{
    return slot != 0xffffffffu && worldCache.schedule.x - worldCells[slot].lastSample <= worldCache.schedule.x - lookupCounts.w;
}
uint WorldSampleInterval(uint slot)
{
    // A first observation has no stored light or visibility state to invalidate.
    return slot != 0xffffffffu && cacheResolved[slot * GI_CACHE_RECORDS].z != 0u
             ? worldCache.schedule.x - worldCells[slot].lastSample
             : 0u;
}
vec3 WorldEvaluate(uint slot, vec3 geometric, vec3 normal, out bool found, out float change)
{
    vec3 value = vec3(0);
    found      = false;
    change     = 0.0;
    if (WorldHistoryUsable(slot))
    {
        change = unpackHalf2x16(worldCells[slot].response).y;
        vec3 total[3], moment[3];
        vec3 ambient = vec3(1e30), axial = vec3(0);
        for (uint axis = 0u; axis < 3u; ++axis)
        {
            uvec4 positive  = cacheResolved[slot * GI_CACHE_RECORDS + axis * 2u];
            uvec4 negative  = cacheResolved[slot * GI_CACHE_RECORDS + axis * 2u + 1u];
            vec3  a         = positive.z != 0u ? CacheUnpack(positive) : vec3(0);
            vec3  b         = negative.z != 0u ? CacheUnpack(negative) : vec3(0);
            found           = found || positive.z != 0u || negative.z != 0u;
            total[axis]     = a + b;
            moment[axis]    = a - b;
            axial          += moment[axis] * geometric[axis];
            // The six face measurements bound the common uniform hemisphere.
            // A collimated component contributes nothing to its three opposite faces.
            float pa = .25 * (1.0 + geometric[axis]), pb = .25 * (1.0 - geometric[axis]);
            if (pa > 0.0)
            {
                ambient = min(ambient, a / pa);
            }
            if (pb > 0.0)
            {
                ambient = min(ambient, b / pb);
            }
        }
        ambient         = min(ambient, 2.0 * max(axial, vec3(0)));
        float alignment = dot(geometric, normal);
        vec3  tangent   = normal - geometric * alignment;
        vec3  projected = vec3(0), spread = vec3(0);
        for (uint axis = 0u; axis < 3u; ++axis)
        {
            vec3 directed  = moment[axis] - .5 * ambient * geometric[axis];
            projected     += directed * normal[axis];
            // Opposed residual contributions have zero first moment. Their
            // angular spread vanishes along the geometric hemisphere normal.
            spread += max(total[axis] - .5 * ambient - abs(directed), vec3(0)) * (tangent[axis] * tangent[axis]);
        }
        value = .25 * (1.0 + alignment) * ambient + max(projected, vec3(0)) + .5 * spread;
    }
    return max(value, vec3(0));
}
// Reconstruct incoming radiance on the surface before it enters another bounce.
// A cell stores an area mean; nearest-cell feedback broadcasts the noise of
// one record to every path that lands anywhere in that footprint.
// Each path reads one bilinear tap, chosen per axis with its tent weight, so its
// expected value is the bilinear reconstruction. A neighbor costs up to four
// hash probes; this way a path pays for at most one instead of three.
// A missing neighbor's weight stays with the center cell.
vec3 WorldSurfaceIrradiance(vec3        position,
                            vec3        geometric,
                            vec3        normal,
                            int         level,
                            vec3        centerValue,
                            vec2        selection,
                            inout float change,
                            inout uint  sampledSlot)
{
    float size = CacheCellSize(level);
    vec3  grid = position / size;
    int   z    = int(CacheNormalBin(geometric) / 2u);
    int   x = (z + 1) % 3, y = (z + 2) % 3;
    vec2  phase  = vec2(fract(grid[x]), fract(grid[y])) - .5;
    ivec2 step   = ivec2(sign(phase));
    ivec2 corner = ivec2(lessThan(selection, abs(phase)));
    vec3  value  = centerValue;
    if (any(notEqual(corner, ivec2(0))))
    {
        ivec3 offset = ivec3(0);
        offset[x]    = corner.x * step.x;
        offset[y]    = corner.y * step.y;
        // Intersect this receiver's tangent plane with the neighboring column.
        vec2 shift = vec2(offset[x], offset[y]) - phase;
        offset[z]  = int(floor(fract(grid[z]) - dot(vec2(geometric[x], geometric[y]), shift) / geometric[z]));
        bool  found;
        float neighborChange;
        uint  neighbor   = WorldFind(position, geometric, level, offset);
        vec3  irradiance = WorldEvaluate(neighbor, geometric, normal, found, neighborChange);
        sampledSlot      = found ? neighbor : sampledSlot;
        change           = found ? neighborChange : change;
        value            = found ? irradiance : centerValue;
    }
    return value;
}
// Two uniforms, independent of the path's other decisions, select one feedback tap.
vec2 WorldFeedbackSelection(uint seed)
{
    return vec2(uvec2(HybridHash(seed ^ 0x7f4a7c15u), HybridHash(seed ^ 0x94d049bbu)) >> 8u) / 16777216.0;
}
vec3 WorldLookup(vec3      position,
                 vec3      geometric,
                 vec3      normal,
                 bool      feedback,
                 vec2      selection,
                 out bool  found,
                 out bool  usable,
                 out float change,
                 out uint  sampledSlot)
{
    int  level  = WorldLevel(position);
    uint slot   = WorldFind(position, geometric, level, ivec3(0));
    vec3 value  = WorldEvaluate(slot, geometric, normal, found, change);
    sampledSlot = slot;
    usable      = found;
    if (found && feedback && giCache.grid.z > 0.0)
    {
        value = WorldSurfaceIrradiance(position, geometric, normal, level, value, selection, change, sampledSlot);
    }
    if (!found)
    {
        uint parent = WorldFind(position, geometric, level + 1, ivec3(0));
        sampledSlot = parent;
        value       = WorldEvaluate(parent, geometric, normal, usable, change);
        // Preserve the exact-cell outcome for diagnostics.
    }
    if (feedback && usable)
    {
        // Feedback carries the uncertainty still present upstream. Screen and glossy receivers
        // see the fraction that has reached the published mean, including signed corrections.
        change = unpackHalf2x16(worldCells[sampledSlot].response).x;
    }
    return value;
}
vec3 WorldLookup(vec3      position,
                 vec3      geometric,
                 vec3      normal,
                 bool      feedback,
                 vec2      selection,
                 out bool  found,
                 out bool  usable,
                 out float change)
{
    uint sampledSlot;
    return WorldLookup(position, geometric, normal, feedback, selection, found, usable, change, sampledSlot);
}
vec3 WorldLookup(vec3 position, vec3 geometric, vec3 normal, bool feedback, vec2 selection, out bool found, out bool usable)
{
    float change;
    return WorldLookup(position, geometric, normal, feedback, selection, found, usable, change);
}
vec3 WorldLookup(vec3 position, vec3 geometric, vec3 normal, out bool found, out bool usable)
{
    return WorldLookup(position, geometric, normal, false, vec2(0), found, usable);
}
vec3 WorldLookup(vec3 position, vec3 geometric, vec3 normal, out bool found)
{
    bool usable;
    return WorldLookup(position, geometric, normal, found, usable);
}
vec3 WorldLookup(vec3 position, vec3 geometric, vec3 normal)
{
    bool found;
    return WorldLookup(position, geometric, normal, found);
}
bool WorldReadMoving(uint slot, out vec3 value)
{
    value = vec3(0);
    if (!WorldHistoryUsable(slot) || !WorldMovingHistoryComplete(worldCache.schedule.x - worldCells[slot].lastSample))
    {
        return false;
    }
    uvec4 record = cacheResolved[slot * GI_CACHE_RECORDS + WORLD_MOVING_RECORD];
    value        = CacheUnpack(record);
    return record.z != 0u;
}
vec3 WorldMovingIndirect(vec3 position, vec3 geometric, uint slot, bool fine)
{
    // The settled lookup already selected the surface cell, including its bilinear feedback tap.
    vec3 value;
    if (!WorldReadMoving(slot, value) && fine)
    {
        uint parent = WorldFind(position, geometric, WorldLevel(position) + 1, ivec3(0));
        WorldReadMoving(parent, value);
    }
    return value;
}
#ifdef WRC_LIGHTING_WRITE
// One in 64 first-hit probe samples across the hardware and field traversal paths.
void WorldCountProbe(uint seed, vec3 position, vec3 normal, bool exact, bool usable)
{
    if ((seed & 63u) == 0u)
    {
        if (exact)
        {
            atomicAdd(cacheStats.x, 1u);
        }
        else
        {
            atomicAdd(cacheStats.y, 1u);
            if (usable)
            {
                atomicAdd(lookupCounts.x, 1u);
            }
            else if (WorldFind(position, normal, WorldLevel(position), ivec3(0)) == 0xffffffffu)
            {
                atomicAdd(lookupCounts.y, 1u);
            }
            else
            {
                atomicAdd(lookupCounts.z, 1u);
            }
        }
    }
}
#endif
// Indirect irradiance/pi of the receiver's own cell along the sampled shading normals, without the
// cell's direct light. The seed becomes available once enough observations have resolved.
bool WorldIndirect(vec3 position, vec3 geometric, vec3 normal, uint minimumUpdates, out vec3 value, out bool parent)
{
    bool found = false;
    value      = vec3(0);
    parent     = false;
    if (WORLD_HAS_SEED)
    {
        int level = WorldLevel(position);
        for (int offset = 0; offset < 2 && !found; ++offset)
        {
            uint slot = WorldFind(position, geometric, level + offset, ivec3(0));
            if (WorldHistoryUsable(slot) && unpackHalf2x16(worldCells[slot].response).x < .05)
            {
                uvec4 record = cacheResolved[slot * GI_CACHE_RECORDS + WORLD_SEED_RECORD];
                found        = record.z >= minimumUpdates;
                parent       = found && offset != 0;
                value        = found ? CacheUnpack(record) : vec3(0);
            }
        }
    }
    return found;
}
bool WorldIndirect(vec3 position, vec3 geometric, vec3 normal, uint minimumUpdates, out vec3 value)
{
    bool parent;
    return WorldIndirect(position, geometric, normal, minimumUpdates, value, parent);
}
#endif
