// Direct lighting and the returning segment share a single observation count.
// Two clamped contributions * 4096 units * at most 8192 accepted vertices fit in uint.
const uint  kWorldSamples = 8192u;
const float kWorldScale   = 4096.0;
const uint  kNoWorldCell  = 0xffffffffu;
uint        WorldBeginSample(vec3 position, vec3 normal, out uint ordinal)
{
    ordinal    = 0u;
    int  level = WorldLevel(position);
    uint slot  = CacheClaim(CacheCellKey(position, normal, level));
    if (slot == kNoWorldCell)
    {
        atomicAdd(cacheStats.w, 1u);
    }
    else
    {
        uint index = atomicAdd(worldCells[slot].samples, 1u);
        ordinal    = cacheResolved[slot * GI_CACHE_RECORDS].z + index;
        if (index >= kWorldSamples)
        {
            slot = kNoWorldCell;
        }
        else if (index == 0u && cacheResolved[slot * GI_CACHE_RECORDS].z == 0u)
        {
            // Snapshot yesterday's parent in fill. Resolve must never read a
            // neighboring record another resolve invocation is updating.
            uint parent = WorldFind(position, normal, level + 1, ivec3(0));
            if (WorldHistoryUsable(parent)
                && worldCache.schedule.x - worldCells[parent].lastSample <= min(sourceSignature.y, sourceSignature.z) + 1u)
            {
                uint age       = min(cacheResolved[parent * GI_CACHE_RECORDS].w, 15u);
                uint movingAge = 0u;
                if (worldCache.lightCount.w != 0u
                    && WorldMovingHistoryComplete(worldCache.schedule.x - worldCells[parent].lastSample))
                {
                    movingAge = min(cacheResolved[parent * GI_CACHE_RECORDS + WORLD_MOVING_RECORD].w, 15u);
                }
                worldCells[slot].parentAge = age | (movingAge << 8u);
                for (uint axis = 0u; axis < GI_CACHE_RECORDS && age != 0u; ++axis)
                {
                    cacheAccumulation[slot * GI_CACHE_RECORDS + axis].w =
                        HybridPackSignedRGBE(CacheUnpack(cacheResolved[parent * GI_CACHE_RECORDS + axis]));
                }
            }
        }
    }
    return slot;
}
// Stratify the directions leaving a cell even when its screen coverage or number
// of samples changes. Each disjoint block gets an independent digital shift.
vec2 WorldDirectionSample(uint slot, uint ordinal, uint seed)
{
    return slot != kNoWorldCell ? HybridSkySample(uvec2(slot, slot >> 16u), ordinal & 63u, HybridHash(ordinal >> 6u))
                                : vec2(uvec2(HybridHash(seed), HybridHash(seed ^ 0x2d57u)) >> 8u) / 16777216.0;
}
vec3 WorldBoundRecord(vec3 value)
{
    return max(value, vec3(0)) * (40.0 / max(40.0, max(value.r, max(value.g, value.b))));
}
void WorldAddRecord(uint address, vec3 value)
{
    if (any(isnan(value)) || any(isinf(value)))
    {
        return;
    }
    vec3  bounded = WorldBoundRecord(value);
    uvec3 encoded = uvec3(round(bounded * kWorldScale));
#ifdef WORLD_SUBGROUP
    // Combine the leader's cohort. Isolated lanes keep their independent atomic adds.
    uint leader = subgroupBroadcastFirst(address);
    if (address == leader)
    {
        encoded = subgroupAdd(encoded);
        if (!subgroupElect())
        {
            return;
        }
    }
#endif
    atomicAdd(cacheAccumulation[address].x, encoded.x);
    atomicAdd(cacheAccumulation[address].y, encoded.y);
    atomicAdd(cacheAccumulation[address].z, encoded.z);
}
// At most three bounded contributions per accepted vertex fit in the 32-bit sum.
void WorldRecordResponse(uint slot, float fraction)
{
    if (fraction > 0.0)
    {
        atomicAdd(worldCells[slot].change, uint(round(min(fraction, 16.0) * kWorldScale)));
    }
}
void WorldProject(uint slot, vec3 direction, vec3 value)
{
    if (slot != kNoWorldCell && any(greaterThan(value, vec3(0))))
    {
        for (uint axis = 0u; axis < 3u; ++axis)
        {
            if (direction[axis] != 0.0)
            {
                WorldAddRecord(slot * GI_CACHE_RECORDS + 2u * axis + uint(direction[axis] < 0.0), value * abs(direction[axis]));
            }
        }
    }
}
