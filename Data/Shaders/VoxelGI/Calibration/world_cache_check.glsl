#define GI_CACHE_WRITE
#define WRC_LIGHTING_WRITE
#include "../world_cache.glsl"
#include "../world_cache_accumulate.glsl"
layout(local_size_x = 1) in;
layout(push_constant) uniform CheckOperation
{
    uint operation;
};
layout(std430, set = 1, binding = 80) buffer Results
{
    vec4 results[];
};
void main()
{
    vec3 point = vec3(.25) * CacheCellSize(0), normal = vec3(0, 0, 1);
    if (operation == 0u)
    {
        // Mature parent, then the first fine observation. The second observation
        // shares the key despite a plane offset. Its black sample counts in the mean.
        uint parent = CacheClaim(CacheCellKey(point, normal, 1));
        for (uint axis = 0u; axis < WORLD_SEED_RECORD; ++axis)
        {
            cacheResolved[parent * GI_CACHE_RECORDS + axis] = uvec4(CachePack(vec3(axis == 4u ? 2.0 : 0.0)), 128u, 128u);
        }
        cacheResolved[parent * GI_CACHE_RECORDS + WORLD_MOVING_RECORD] = uvec4(CachePack(vec3(4)), 2u, 2u);
        cacheStats.z                                                   = 1u;
        worldCells[parent].lastSample                                  = worldCache.schedule.x;
        memoryBarrierBuffer();
        bool exact, usable;
        vec3 prior = WorldLookup(point, normal, normal, exact, usable);
        results[0] = vec4(prior.x, float(exact), float(usable), 0);
        uint ordinal;
        uint slot = WorldBeginSample(point, normal, ordinal);
        WorldAddRecord(slot * GI_CACHE_RECORDS + WORLD_SEED_RECORD, vec3(2, 0, 0));
        WorldAddRecord(slot * GI_CACHE_RECORDS + WORLD_MOVING_RECORD, vec3(2));
        WorldProject(slot, vec3(1, 0, 0), vec3(4, 0, 0));
        WorldRecordResponse(slot, .0625);
        uint same    = WorldBeginSample(point + normal * CacheCellSize(0) * .2, normal, ordinal);
        results[0].w = float(slot == same && slot != kNoWorldCell);
        // A directional beam and a uniform hemisphere remain distinguishable at
        // a tilted shading normal, even though both are stored in six axis records.
        vec3 geometric = vec3(0, -1, 0), beam = normalize(vec3(0, -1, 1)), receiver = normalize(vec3(0, -1, -1));
        uint angular = WorldBeginSample(point, geometric, ordinal);
        for (uint axis = 0u; axis < 6u; ++axis)
        {
            vec3 face       = vec3(0);
            face[axis / 2u] = (axis & 1u) == 0u ? 1.0 : -1.0;
            vec2 channels   = vec2(max(dot(beam, face), 0.0), .5 * (1.0 + dot(geometric, face)));
            WorldAddRecord(angular * GI_CACHE_RECORDS + axis, vec3(channels, channels.x + channels.y));
        }
    }
    else
    {
        bool exact, usable;
        vec3 value     = WorldLookup(point, normal, normal, exact, usable);
        vec3 geometric = vec3(0, -1, 0), beam = normalize(vec3(0, -1, 1)), receiver = normalize(vec3(0, -1, -1));
        results[3] = vec4(WorldLookup(point, geometric, receiver), WorldLookup(point, geometric, beam).r);
        vec3 seedValue, immatureValue;
        bool mature   = WorldIndirect(point, normal, normal, 2u, seedValue);
        bool immature = WorldIndirect(point, normal, normal, 3u, immatureValue);
        results[4]    = vec4(seedValue.x, float(mature), float(immature), immatureValue.x);
        results[1]    = vec4(value.x, float(exact), float(usable), WorldLookup(point, -normal, -normal).x);
        if (operation == 2u)
        {
            uint parent                                                  = WorldFind(point, normal, 1, ivec3(0));
            cacheResolved[parent * GI_CACHE_RECORDS + WORLD_SEED_RECORD] = uvec4(CachePack(vec3(.5)), 8u, 8u);
            memoryBarrierBuffer();
            bool parentUsed;
            bool warm  = WorldIndirect(point, normal, normal, 3u, seedValue, parentUsed);
            results[5] = vec4(seedValue.x, float(warm), float(parentUsed), 0);
            editLog.w  = 1u;
            memoryBarrierBuffer();
            uint  fineSlot    = WorldFind(point, normal, 0, ivec3(0));
            float freshMoving = WorldMovingIndirect(point, normal, fineSlot, true).x;
            movingLog.z       = worldCache.schedule.x;
            responseLog.y     = worldCache.schedule.x;
            memoryBarrierBuffer();
            float change;
            vec3  settled = WorldLookup(point, normal, normal, false, vec2(0), exact, usable, change, fineSlot);
            results[7]    = vec4(freshMoving, WorldMovingIndirect(point, normal, fineSlot, exact).x, settled.x, change);
            vec3 origin = vec3(-2, 0, 0), ray = vec3(1, 0, 0);
            results[6] = vec4(WorldSurfaceChanged(origin, ray, 4.0, 0u, 1u), WorldSurfaceChanged(origin, ray, .5, 0u, 1u),
                              WorldSurfaceChanged(origin, -ray, 1.0, 11u, 1u), WorldSurfaceChanged(origin, ray, 4.0, 11u, 0u));
        }
        uint slot  = WorldFind(point, normal, 0, ivec3(0));
        results[2] = slot == kNoWorldCell
                       ? vec4(0)
                       : vec4(CacheUnpack(cacheResolved[slot * GI_CACHE_RECORDS]).x, cacheResolved[slot * GI_CACHE_RECORDS].w,
                              cacheResolved[slot * GI_CACHE_RECORDS].z, worldCells[slot].samples);
    }
}
