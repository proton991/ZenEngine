#ifndef ZEN_BRICK_TRACE
#define ZEN_BRICK_TRACE
// Brick-scene traversal (BrickScene.cpp); include after HybridRayResult and gi_common.glsl.
// A cell DDA skips empty space by each empty cell's Chebyshev distance; a fine DDA tests the
// 8x8x8 occupancy bits of brick cells. Distances are in fine voxels until the hit is reported.
#include "brick_common.glsl"

vec3 BrickInverseDirection(vec3 direction)
{
    return 1.0 / mix(direction, vec3(1e-30), lessThan(abs(direction), vec3(1e-30)));
}

// Time at which the ray leaves the cell along each axis, in fine voxels.
vec3 BrickCellExit(ivec3 cell, vec3 p, vec3 inverse, ivec3 stepDirection, float scale)
{
    vec3 boundary = (vec3(cell) + vec3(max(stepDirection, ivec3(0)))) * scale;
    return mix((boundary - p) * inverse, vec3(1e30), equal(stepDirection, ivec3(0)));
}

int BrickMinimumAxis(vec3 value)
{
    int axis = value.x < value.y ? 0 : 1;
    return value[axis] < value.z ? axis : 2;
}

// Fine DDA through one brick from distance t, leaving empty 4x4x4 blocks in one step. Returns true
// on a hit; otherwise t and normal describe where the ray leaves the brick. While skipping, occupied
// voxels of the run the ray started in are passed until the first empty one.
bool BrickMarch(uint        value,
                ivec3       cell,
                vec3        p,
                vec3        direction,
                vec3        inverse,
                ivec3       stepDirection,
                float       leave,
                inout float t,
                inout vec3  normal,
                inout bool  skipping)
{
    int   size   = int(ZEN_BRICK_SIZE);
    ivec3 origin = cell * size;
    ivec3 voxel  = clamp(ivec3(floor(p + direction * t)), origin, origin + size - 1);
    uint  base   = (value - 1u) * ZEN_BRICK_WORDS;
    uint  mask   = brickMasks[value - 1u];
    int   block  = int(ZEN_BRICK_BLOCK);
    bool  hit    = false;
    bool  inside = true;
    vec3  next   = BrickCellExit(voxel, p, inverse, stepDirection, 1.0);
    for (int i = 0; i < 3 * size && inside && !hit && t < leave; ++i)
    {
        ivec3 local = voxel - origin;
        ivec3 index = local / block;
        if ((mask & (1u << uint(index.z * 4 + index.y * 2 + index.x))) == 0u)
        {
            // An empty block: leave it at once, landing in the next block's first voxel.
            skipping        = false;
            vec3  blockExit = BrickCellExit(cell * 2 + index, p, inverse, stepDirection, float(block));
            int   axis      = BrickMinimumAxis(blockExit);
            ivec3 lower     = origin + index * block;
            ivec3 upper     = lower + block - 1;
            t               = max(t, blockExit[axis]);
            voxel           = clamp(ivec3(floor(p + direction * t)), lower, upper);
            voxel[axis]     = stepDirection[axis] > 0 ? upper[axis] + 1 : lower[axis] - 1;
            next            = BrickCellExit(voxel, p, inverse, stepDirection, 1.0);
            normal          = vec3(0);
            normal[axis]    = -float(stepDirection[axis]);
            inside          = voxel[axis] >= origin[axis] && voxel[axis] < origin[axis] + size;
        }
        else
        {
            uint bit      = BrickBitIndex(local);
            bool occupied = (brickBits[base + bit / 32u] & (1u << (bit % 32u))) != 0u;
            hit           = occupied && !(skipping && t < ZEN_BRICK_START_RUN_VOXELS);
            skipping      = skipping && occupied;
            if (!hit)
            {
                int axis      = BrickMinimumAxis(next);
                t             = next[axis];
                voxel[axis]  += stepDirection[axis];
                next[axis]   += abs(inverse[axis]);
                normal        = vec3(0);
                normal[axis]  = -float(stepDirection[axis]);
                inside        = voxel[axis] >= origin[axis] && voxel[axis] < origin[axis] + size;
            }
        }
    }
    return hit;
}

// Closest hit within maximumDistance (world units) from origin. Rays may start outside the grid.
// With skipStart, a ray starting in occupied voxels passes that run (ZEN_BRICK_START_RUN_VOXELS) and
// hits from the first empty voxel on, so a receiver near another surface is not blocked in every
// direction while the far side of that geometry still occludes. Without it, as for a ray continued
// in mid-air, an occupied start hits. A start inside a solid cell hits immediately.
HybridRayResult BrickTraceRaySkip(vec3 origin, vec3 direction, float maximumDistance, bool skipStart)
{
    HybridRayResult result = HybridRayResult(false, origin, vec3(0), ivec3(-1));
#ifdef HYBRID_COUNT_RAYS
    ++issuedComputeRays[issuedRayFeature];
#endif
    float scale   = float(ZEN_BRICK_SIZE);
    float extent  = float(BrickResolution()) * scale;
    vec3  p       = (origin - brick.gridMinCellSize.xyz) * brick.voxel.y;
    vec3  inverse = BrickInverseDirection(direction);
    float enter = 0.0, leave = maximumDistance * brick.voxel.y;
    vec3  normal     = vec3(0);
    bool  intersects = true;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (abs(direction[axis]) < 1e-30)
        {
            intersects = intersects && p[axis] >= 0.0 && p[axis] < extent;
        }
        else
        {
            float a = -p[axis] * inverse[axis], b = (extent - p[axis]) * inverse[axis];
            if (min(a, b) > enter)
            {
                enter        = min(a, b);
                normal       = vec3(0);
                normal[axis] = -sign(direction[axis]);
            }
            leave = min(leave, max(a, b));
        }
    }
    if (intersects && leave > enter)
    {
        ivec3 stepDirection = ivec3(sign(direction)) * ivec3(greaterThanEqual(abs(direction), vec3(1e-30)));
        float t             = enter;
        ivec3 cell          = clamp(ivec3(floor((p + direction * t) / scale)), ivec3(0), ivec3(BrickResolution() - 1));
        bool  hit           = false;
        bool  inside        = true;
        bool  skipping      = skipStart && enter == 0.0;
        // Every iteration advances at least one cell; skips and bricks only shorten the walk.
        for (int i = 0; i < 4 * BrickResolution() && inside && !hit && t < leave; ++i)
        {
            uint value = brickCells[BrickCellIndex(cell)];
            vec3 exit  = BrickCellExit(cell, p, inverse, stepDirection, scale);
            if (value == ZEN_BRICK_SOLID)
            {
                hit = true;
            }
            else if (BrickCellHasBrick(value)
                     && BrickMarch(value, cell, p, direction, inverse, stepDirection, leave, t, normal, skipping))
            {
                hit = true;
            }
            else if ((value & ZEN_BRICK_EMPTY_BIT) != 0u && (value & 0xffu) > 1u)
            {
                // Every cell within Chebyshev radius distance-1 is empty: leave that cube at once.
                skipping     = false;
                int   radius = int(value & 0xffu) - 1;
                ivec3 lower = cell - radius, upper = cell + radius;
                vec3  boxExit = mix(((vec3(mix(lower, upper + 1, greaterThan(stepDirection, ivec3(0))))) * scale - p) * inverse,
                                    vec3(1e30), equal(stepDirection, ivec3(0)));
                int   axis    = BrickMinimumAxis(boxExit);
                t             = max(t, boxExit[axis]);
                ivec3 landed  = ivec3(floor((p + direction * t) / scale));
                cell          = clamp(landed, lower, upper);
                cell[axis]    = stepDirection[axis] > 0 ? upper[axis] + 1 : lower[axis] - 1;
                normal        = vec3(0);
                normal[axis]  = -float(stepDirection[axis]);
                inside        = BrickCellInside(cell);
            }
            else
            {
                // An empty cell ends the starting run; a brick the ray left updated it.
                skipping      = skipping && BrickCellHasBrick(value);
                int axis      = BrickMinimumAxis(exit);
                t             = exit[axis];
                cell[axis]   += stepDirection[axis];
                normal        = vec3(0);
                normal[axis]  = -float(stepDirection[axis]);
                inside        = BrickCellInside(cell);
            }
        }
        if (hit && t < leave)
        {
            result.hit      = true;
            result.position = origin + direction * (t * brick.voxel.x);
            result.normal   = normal;
            // The radiance cache cell holding the hit voxel: half a fine voxel past the entered face.
            vec3 interior = result.position + direction * (0.5 * brick.voxel.x);
            result.cell   = ivec3(floor((interior - gi.gridMinVoxelSize.xyz) / gi.gridMinVoxelSize.w));
        }
    }
    return result;
}

HybridRayResult BrickTraceRay(vec3 origin, vec3 direction, float maximumDistance)
{
    return BrickTraceRaySkip(origin, direction, maximumDistance, true);
}

// Start offset that leaves every fine voxel the receiver's own surface can touch: a plane through
// a voxel lies within sqrt(3) voxels of all its points.
vec3 BrickTraceOrigin(vec3 position, vec3 normal)
{
    return position + normal * brick.voxel.z;
}
#endif
