// Adapted from FidelityFX SDK v1.1.4, commit c6efa6bf7f2027b3ec94f28578bb5965eabb9e55:
// ffx_brixelizer_trace_ops.h and ffx_brixelizer_brick_common.h.
// Atlas coordinates, distance steps and one-sided gradients follow those sources. Zen uses a
// fixed occupancy/availability mip hierarchy instead of AMD's cascade AABB stamps, explicit
// slab entry, and unavailable results when data or the iteration budget is exhausted.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef ZEN_DISTANCE_FIELD_TRACE
#define ZEN_DISTANCE_FIELD_TRACE
#include "distance_field_common.glsl"

#ifdef DISTANCE_FIELD_LOCALS
#    include "Graphics/Shared/DistanceFieldScene.h"
// Local detail volumes around point and spot lights, copied into one pool (DistanceFieldScene): per
// volume one cell map and hierarchy at fixed offsets, and its atlas tiles at its slot in a 3D atlas.
struct DistanceFieldLocalRecord
{
    vec4  gridMinCellSize;  // xyz grid minimum, w brick extent
    vec4  authorityMinimum; // Rays inside the authority box trace this volume instead of the scene field.
    vec4  authorityMaximum;
    uvec4 slot; // x pool column, y pool row, z nonzero once published, w reserved
};
struct DistanceFieldLocalNode
{
    vec4 minimum; // w: slot + 1, or zero for an internal node
    vec4 maximum; // w: escape index
};
layout(std140, set = 4, binding = DISTANCE_FIELD_BINDING + 5) uniform uDistanceFieldLocals
{
    uvec4                    header; // x volumes, y cells per axis, z hierarchy levels, w atlas texels per slot side
    DistanceFieldLocalRecord volumes[ZEN_FIELD_LOCAL_VOLUMES];
    DistanceFieldLocalNode   nodes[2u * ZEN_FIELD_LOCAL_VOLUMES - 1u];
}
fieldLocals;
layout(std430, set = 4, binding = DISTANCE_FIELD_BINDING + 6) readonly buffer DistanceFieldLocalCells
{
    uint localFieldCells[];
};
layout(std430, set = 4, binding = DISTANCE_FIELD_BINDING + 7) readonly buffer DistanceFieldLocalHierarchy
{
    uint localFieldHierarchy[];
};
layout(set = 4, binding = DISTANCE_FIELD_BINDING + 8) uniform sampler3D localFieldAtlas;
#endif

// One traceable level: the scene field, or a local volume's slot in the pool.
struct DistanceFieldLevel
{
    vec4  gridMinCellSize;
    uvec4 dimensions;      // as uDistanceFieldData: cells per axis, brick capacity, hierarchy levels, trace budget
    uvec4 atlasDimensions; // texel extents whose tiles hold the level's bricks
    vec4  tracing;
    uvec4 pool;      // x nonzero for a pool slot, y cell offset, z hierarchy offset
    uvec4 slotTexel; // xyz texel origin of the slot's tiles in the pool atlas
};

DistanceFieldLevel DistanceFieldSceneLevel()
{
    return DistanceFieldLevel(distanceField.gridMinCellSize, distanceField.dimensions, distanceField.atlasDimensions,
                              distanceField.tracing, uvec4(0), uvec4(0));
}

uint DistanceFieldLevelHierarchyOffset(DistanceFieldLevel level, uint mip)
{
    uint side      = level.dimensions.x;
    uint remaining = side >> mip;
    // Sum of the power-of-eight mip sizes; divide first to avoid overflow at side 1024.
    return ((side * side * side - remaining * remaining * remaining) / 7u) * 8u + uint(remaining == 0u);
}

uint DistanceFieldLevelCell(DistanceFieldLevel level, uint index)
{
    uint value = 0u;
#ifdef DISTANCE_FIELD_LOCALS
    if (level.pool.x != 0u)
    {
        value = localFieldCells[level.pool.y + index];
    }
    else
#endif
    {
        value = distanceFieldCells[index];
    }
    return value;
}

uint DistanceFieldLevelHierarchy(DistanceFieldLevel level, uint index)
{
    uint value = 0u;
#ifdef DISTANCE_FIELD_LOCALS
    if (level.pool.x != 0u)
    {
        value = localFieldHierarchy[level.pool.z + index];
    }
    else
#endif
    {
        value = distanceFieldHierarchy[index];
    }
    return value;
}

vec3 DistanceFieldAtlasScale(DistanceFieldLevel level)
{
    vec3 dimensions = vec3(level.atlasDimensions.xyz);
#ifdef DISTANCE_FIELD_LOCALS
    if (level.pool.x != 0u)
    {
        dimensions = vec3(textureSize(localFieldAtlas, 0));
    }
#endif
    return 1.0 / dimensions;
}

vec3 DistanceFieldBrickTexel(DistanceFieldLevel level, uint brick)
{
    uvec3 tiles = level.atlasDimensions.xyz / 8u;
    uvec3 tile  = uvec3(brick % tiles.x, (brick / tiles.x) % tiles.y, brick / (tiles.x * tiles.y));
    // Eight samples span seven intervals. Clamp within this tile, never into its atlas neighbour.
    return vec3(level.slotTexel.xyz + tile * 8u) + vec3(0.5);
}

float DistanceFieldAtlasSample(DistanceFieldLevel level, vec3 coordinate)
{
    float value = 1.0;
#ifdef DISTANCE_FIELD_LOCALS
    if (level.pool.x != 0u)
    {
        value = textureLod(localFieldAtlas, coordinate, 0.0).r;
    }
    else
#endif
    {
        value = textureLod(distanceFieldAtlas, coordinate, 0.0).r;
    }
    return value;
}

float DistanceFieldLevelSample(DistanceFieldLevel level, uint brick, vec3 uvw)
{
    vec3 texel = DistanceFieldBrickTexel(level, brick) + clamp(uvw, vec3(0.0), vec3(1.0)) * 7.0;
    return DistanceFieldAtlasSample(level, texel * DistanceFieldAtlasScale(level));
}

float DistanceFieldSample(uint brick, vec3 uvw)
{
    return DistanceFieldLevelSample(DistanceFieldSceneLevel(), brick, uvw);
}

vec3 DistanceFieldBrickNormal(DistanceFieldLevel level, uint brick, vec3 uvw)
{
    const float offset        = 0.25 / 7.0;
    vec3        stepDirection = mix(vec3(1.0), vec3(-1.0), greaterThan(uvw, vec3(0.5)));
    float       center        = DistanceFieldLevelSample(level, brick, uvw);
    vec3        gradient;
    gradient.x =
        stepDirection.x * (DistanceFieldLevelSample(level, brick, uvw + vec3(stepDirection.x * offset, 0, 0)) - center);
    gradient.y =
        stepDirection.y * (DistanceFieldLevelSample(level, brick, uvw + vec3(0, stepDirection.y * offset, 0)) - center);
    gradient.z =
        stepDirection.z * (DistanceFieldLevelSample(level, brick, uvw + vec3(0, 0, stepDirection.z * offset)) - center);
    float squaredLength = dot(gradient, gradient);
    return squaredLength > 1e-20 ? gradient * inversesqrt(squaredLength) : vec3(0.0);
}

vec3 DistanceFieldNormal(DistanceFieldLevel level,
                         vec3               position,
                         vec3               direction,
                         uint               budget,
                         inout uint         iterations,
                         inout uint         distanceSamples)
{
    vec3 normal = vec3(0.0);
    // An unsigned field has a cusp at the surface. Measure on the approaching side before
    // falling back toward the hit point; a derivative across that cusp can become tangential.
    // Resolve every probe's own brick so crossing a cell face never clamps into the hit brick.
    for (uint probe = 0u; probe < 3u && dot(normal, normal) == 0.0; ++probe)
    {
        vec3  positionBeforeHit = position - direction * (float(2u - probe) * 0.5 / 7.0);
        ivec3 cell              = ivec3(floor(positionBeforeHit));
        if (all(greaterThanEqual(cell, ivec3(0))) && all(lessThan(cell, ivec3(level.dimensions.x)))
            && budget - iterations >= 5u)
        {
            uint value = DistanceFieldLevelCell(level, DistanceFieldIndex(uvec3(cell), level.dimensions.x));
            value      = value == DISTANCE_FIELD_UNAVAILABLE ? value : value & DISTANCE_FIELD_BRICK_MASK;
            ++iterations;
            if (value != DISTANCE_FIELD_EMPTY && value != DISTANCE_FIELD_UNAVAILABLE && value <= level.dimensions.y)
            {
                vec3 measured    = DistanceFieldBrickNormal(level, value - 1u, positionBeforeHit - vec3(cell));
                iterations      += 4u;
                distanceSamples += 4u;
                // Never flip an arbitrary gradient to manufacture an incoming-facing normal.
                if (dot(measured, direction) < -1e-4)
                {
                    normal = measured;
                }
            }
        }
    }
    return normal;
}

bool DistanceFieldIntersectBounds(vec3        origin,
                                  vec3        direction,
                                  vec3        inverseDirection,
                                  float       extent,
                                  inout float enter,
                                  inout float leave)
{
    bool intersects = true;
    for (uint axis = 0u; axis < 3u; ++axis)
    {
        if (direction[axis] == 0.0)
        {
            intersects = intersects && origin[axis] >= 0.0 && origin[axis] <= extent;
        }
        else
        {
            float a = -origin[axis] * inverseDirection[axis];
            float b = (extent - origin[axis]) * inverseDirection[axis];
            enter   = max(enter, min(a, b));
            leave   = min(leave, max(a, b));
        }
    }
    return intersects && enter < leave;
}

float DistanceFieldNodeExit(vec3 origin, vec3 direction, vec3 inverseDirection, uvec3 coordinate, uint mip)
{
    float side     = float(1u << mip);
    vec3  lower    = vec3(coordinate) * side;
    vec3  boundary = lower + mix(vec3(0), vec3(side), greaterThan(direction, vec3(0)));
    vec3  leave    = mix(vec3(1e30), (boundary - origin) * inverseDirection, notEqual(direction, vec3(0)));
    return min(leave.x, min(leave.y, leave.z));
}

bool DistanceFieldCellAtDistance(DistanceFieldLevel level,
                                 vec3               origin,
                                 vec3               direction,
                                 vec3               inverseDirection,
                                 float              distance,
                                 out ivec3          cell)
{
    vec3 position      = origin + direction * distance;
    cell               = ivec3(clamp(floor(position), vec3(0.0), vec3(level.dimensions.x - 1u)));
    bvec3 moving       = notEqual(direction, vec3(0));
    vec3  forward      = mix(vec3(-1), vec3(1), greaterThan(direction, vec3(0)));
    vec3  nearBoundary = vec3(cell) + max(-forward, vec3(0));
    vec3  farBoundary  = nearBoundary + forward;
    vec3  nearDistance = (nearBoundary - origin) * inverseDirection;
    vec3  farDistance  = (farBoundary - origin) * inverseDirection;
    // Reconstructed positions can round behind a crossed face. Resolve all three axes
    // from the same half-open crossing times without divergent per-axis branches or bias.
    vec3 correction  = mix(vec3(0), -forward, lessThan(vec3(distance), nearDistance));
    correction       = mix(correction, forward, greaterThanEqual(vec3(distance), farDistance));
    correction       = mix(vec3(0), correction, moving);
    cell            += ivec3(correction);
    nearDistance     = (nearBoundary + correction - origin) * inverseDirection;
    farDistance      = (farBoundary + correction - origin) * inverseDirection;
    // More than one cell of lost precision cannot certify coverage.
    bool valid = all(mix(bvec3(true), greaterThanEqual(cell, ivec3(0)), moving))
              && all(mix(bvec3(true), lessThan(cell, ivec3(level.dimensions.x)), moving))
              && all(mix(bvec3(true), lessThanEqual(nearDistance, vec3(distance)), moving))
              && all(mix(bvec3(true), lessThan(vec3(distance), farDistance), moving));
    return valid;
}

bool DistanceFieldLevelLayoutValid(DistanceFieldLevel level)
{
    uint  side   = level.dimensions.x;
    uint  levels = level.dimensions.z;
    uvec3 atlas  = level.atlasDimensions.xyz;
    bool  valid  = side > 0u && side <= 1024u && (side & (side - 1u)) == 0u && levels == uint(findMSB(side) + 1)
                && level.gridMinCellSize.w > 0.0 && level.tracing.x > 0.0 && level.tracing.y >= 1.0
                && all(greaterThanEqual(atlas, uvec3(8u))) && all(equal(atlas % 8u, uvec3(0u)));
    if (valid)
    {
        uvec3 tiles      = atlas / 8u;
        uint  cells      = side * side * side;
        uint  levelWords = DistanceFieldLevelHierarchyOffset(level, levels);
        valid            = level.dimensions.y <= tiles.x * tiles.y * tiles.z;
#ifdef DISTANCE_FIELD_LOCALS
        if (level.pool.x != 0u)
        {
            valid = valid && level.pool.y + cells <= localFieldCells.length()
                 && level.pool.z + levelWords <= localFieldHierarchy.length();
        }
        else
#endif
        {
            valid = valid && cells <= distanceFieldCells.length() && levelWords <= distanceFieldHierarchy.length();
        }
    }
    return valid;
}

bool DistanceFieldLayoutValid()
{
    return DistanceFieldLevelLayoutValid(DistanceFieldSceneLevel());
}

// Trace [minimumDistance, maximumDistance), in world units, with a normalized direction.
// A zero override selects dimensions.w. The budget bounds hierarchy/map loads and texture samples.
// Covered miss certifies the full segment: leaving a published full-scene field is safe, while
// entering an unavailable leaf, exhausting either cap, or malformed storage is unavailable.
DistanceFieldTraceResult DistanceFieldTraceLevel(DistanceFieldLevel level,
                                                 vec3               origin,
                                                 vec3               direction,
                                                 float              minimumDistance,
                                                 float              maximumDistance,
                                                 uint               iterationBudget)
{
    DistanceFieldTraceResult result =
        DistanceFieldTraceResult(DISTANCE_FIELD_TRACE_UNAVAILABLE, 0.0, origin, vec3(0), DISTANCE_FIELD_UNAVAILABLE, 0u, 0u,
                                 DISTANCE_FIELD_FAILURE_QUERY);
    bool valid = DistanceFieldLevelLayoutValid(level) && minimumDistance >= 0.0 && maximumDistance >= minimumDistance
              && abs(dot(direction, direction) - 1.0) < 1e-3;
    if (valid)
    {
        uint  budget           = iterationBudget == 0u ? level.dimensions.w : iterationBudget;
        float extent           = float(level.dimensions.x);
        float worldScale       = level.gridMinCellSize.w;
        vec3  localOrigin      = (origin - level.gridMinCellSize.xyz) / worldScale;
        vec3  inverseDirection = 1.0 / mix(direction, vec3(1), equal(direction, vec3(0)));
        float cursor           = minimumDistance / worldScale;
        float leave            = maximumDistance / worldScale;
        bool  intersects       = DistanceFieldIntersectBounds(localOrigin, direction, inverseDirection, extent, cursor, leave);
        result.status          = DISTANCE_FIELD_COVERED_MISS;
        result.failureReason   = DISTANCE_FIELD_FAILURE_NONE;
        bool finished          = !intersects;
        vec3 atlasScale        = DistanceFieldAtlasScale(level);
        while (!finished && cursor < leave)
        {
            ivec3 cell;
            bool  cellValid    = DistanceFieldCellAtDistance(level, localOrigin, direction, inverseDirection, cursor, cell);
            uint  mip          = 0u;
            uint  value        = DISTANCE_FIELD_UNAVAILABLE;
            bool  mapAvailable = cellValid && result.iterations < budget;
            if (mapAvailable)
            {
                value = DistanceFieldLevelCell(level, DistanceFieldIndex(uvec3(cell), level.dimensions.x));
                ++result.iterations;
                if (value != DISTANCE_FIELD_UNAVAILABLE && (value & DISTANCE_FIELD_NO_SURFACE) != 0u
                    && (value & DISTANCE_FIELD_BRICK_MASK) > 0u && (value & DISTANCE_FIELD_BRICK_MASK) <= level.dimensions.y)
                {
                    value = DISTANCE_FIELD_EMPTY;
                }
            }
            // Occupied cells need only their map entry. Ascend only to certify a larger empty
            // span, instead of reloading every ancestor of each occupied cell along the ray.
            bool ascend = mapAvailable && value == DISTANCE_FIELD_EMPTY;
            while (ascend && mip + 1u < level.dimensions.z && result.iterations < budget)
            {
                uint parent = mip + 1u;
                uint flags  = DistanceFieldLevelHierarchy(
                    level, DistanceFieldLevelHierarchyOffset(level, parent)
                               + DistanceFieldIndex(uvec3(cell) >> parent, level.dimensions.x >> parent));
                ++result.iterations;
                ascend = flags == 0u;
                if (ascend)
                {
                    mip = parent;
                }
            }
            uvec3 coordinate = uvec3(cell) >> mip;
            if (!cellValid)
            {
                result.status        = DISTANCE_FIELD_TRACE_UNAVAILABLE;
                result.failureReason = DISTANCE_FIELD_FAILURE_PROGRESS;
                finished             = true;
            }
            else if (!mapAvailable)
            {
                result.status        = DISTANCE_FIELD_TRACE_UNAVAILABLE;
                result.failureReason = DISTANCE_FIELD_FAILURE_WORK;
                finished             = true;
            }
            else
            {
                float nodeExit = min(leave, DistanceFieldNodeExit(localOrigin, direction, inverseDirection, coordinate, mip));
                if (value == DISTANCE_FIELD_UNAVAILABLE || nodeExit <= cursor)
                {
                    result.status = DISTANCE_FIELD_TRACE_UNAVAILABLE;
                    result.failureReason =
                        value == DISTANCE_FIELD_UNAVAILABLE ? DISTANCE_FIELD_FAILURE_MISSING : DISTANCE_FIELD_FAILURE_PROGRESS;
                    finished = true;
                }
                else if (value != DISTANCE_FIELD_EMPTY)
                {
                    if (value == DISTANCE_FIELD_EMPTY || value == DISTANCE_FIELD_UNAVAILABLE || value > level.dimensions.y)
                    {
                        result.status        = DISTANCE_FIELD_TRACE_UNAVAILABLE;
                        result.failureReason = !mapAvailable                       ? DISTANCE_FIELD_FAILURE_WORK
                                             : value == DISTANCE_FIELD_UNAVAILABLE ? DISTANCE_FIELD_FAILURE_MISSING
                                                                                   : DISTANCE_FIELD_FAILURE_STORAGE;
                        finished             = true;
                    }
                    else
                    {
                        uint localSamples = 0u;
                        // Address the brick once. The march only advances within this tile.
                        vec3 brickTexel = DistanceFieldBrickTexel(level, value - 1u);
                        while (cursor < nodeExit && !finished && result.iterations < budget
                               && localSamples < uint(level.tracing.y))
                        {
                            vec3  uvw          = localOrigin + direction * cursor - vec3(cell);
                            vec3  coordinate   = (brickTexel + clamp(uvw, vec3(0), vec3(1)) * 7.0) * atlasScale;
                            float stepDistance = DistanceFieldAtlasSample(level, coordinate) - level.tracing.x;
                            ++result.iterations;
                            ++result.distanceSamples;
                            ++localSamples;
                            if (stepDistance < level.tracing.x)
                            {
                                result.status   = DISTANCE_FIELD_HIT;
                                result.distance = cursor * worldScale;
                                result.position = origin + direction * result.distance;
                                if (level.tracing.z == 0.0)
                                {
                                    result.normal = DistanceFieldNormal(level, localOrigin + direction * cursor, direction,
                                                                        budget, result.iterations, result.distanceSamples);
                                }
                                result.brick = value - 1u;
                                finished     = true;
                            }
                            else
                            {
                                cursor = min(cursor + stepDistance, nodeExit);
                            }
                        }
                        if (!finished && cursor < nodeExit)
                        {
                            result.status = DISTANCE_FIELD_TRACE_UNAVAILABLE;
                            result.failureReason =
                                result.iterations >= budget ? DISTANCE_FIELD_FAILURE_WORK : DISTANCE_FIELD_FAILURE_BRICK;
                            finished = true;
                        }
                    }
                }
                if (!finished)
                {
                    // The next cell owns this exact crossing time. Cell selection resolves
                    // the forward side from boundary times instead of perturbing the ray.
                    cursor = nodeExit;
                }
            }
        }
    }
    return result;
}

DistanceFieldTraceResult DistanceFieldTrace(vec3  origin,
                                            vec3  direction,
                                            float minimumDistance,
                                            float maximumDistance,
                                            uint  iterationBudget)
{
    return DistanceFieldTraceLevel(DistanceFieldSceneLevel(), origin, direction, minimumDistance, maximumDistance,
                                   iterationBudget);
}

#ifdef DISTANCE_FIELD_LOCALS
// Local volume as a traceable level, with the scene field's tolerance (in brick units) and budget.
DistanceFieldLevel DistanceFieldLocalLevel(uint volume)
{
    uint               side     = fieldLocals.header.y;
    uint               slotSide = fieldLocals.header.w;
    uint               tiles    = slotSide / 8u;
    uint               cells    = side * side * side;
    uvec4              slot     = fieldLocals.volumes[volume].slot;
    DistanceFieldLevel level =
        DistanceFieldLevel(fieldLocals.volumes[volume].gridMinCellSize,
                           uvec4(side, tiles * tiles * tiles, fieldLocals.header.z, distanceField.dimensions.w),
                           uvec4(slotSide, slotSide, slotSide, 0u), distanceField.tracing, uvec4(1u, volume * cells, 0u, 0u),
                           uvec4(slot.x * slotSide, slot.y * slotSide, 0u, 0u));
    level.pool.z = volume * DistanceFieldLevelHierarchyOffset(level, level.dimensions.z);
    return level;
}

// Entry and exit of a ray against a world box over [minimum, maximum); false when they do not overlap.
bool DistanceFieldBoxSpan(vec3      origin,
                          vec3      direction,
                          vec3      inverseDirection,
                          vec3      boxMinimum,
                          vec3      boxMaximum,
                          float     minimum,
                          float     maximum,
                          out float enter,
                          out float leave)
{
    bvec3 moving        = notEqual(direction, vec3(0));
    vec3  a             = (boxMinimum - origin) * inverseDirection;
    vec3  b             = (boxMaximum - origin) * inverseDirection;
    vec3  nearDistance  = mix(vec3(-1e30), min(a, b), moving);
    vec3  farDistance   = mix(vec3(1e30), max(a, b), moving);
    enter               = max(minimum, max(nearDistance.x, max(nearDistance.y, nearDistance.z)));
    leave               = min(maximum, min(farDistance.x, min(farDistance.y, farDistance.z)));
    bool parallelInside = all(mix(greaterThanEqual(origin, boxMinimum), bvec3(true), moving))
                       && all(mix(lessThan(origin, boxMaximum), bvec3(true), moving));
    return parallelInside && enter < leave;
}

// Traces the scene field, and each published local volume inside its authority box, along
// [minimumDistance, maximumDistance). The first non-miss segment decides the result.
// Brick size of the level that traces a position: a published local volume whose authority box
// holds it, otherwise the scene field.
float DistanceFieldBrickSizeAt(vec3 position)
{
    float size  = distanceField.gridMinCellSize.w;
    uint  mask  = 0u;
    uint  count = uint(fieldLocals.nodes[0].maximum.w);
    for (uint index = 0u; index < count;)
    {
        DistanceFieldLocalNode node = fieldLocals.nodes[index];
        bool inside = all(greaterThanEqual(position, node.minimum.xyz)) && all(lessThan(position, node.maximum.xyz));
        if (inside && node.minimum.w > 0.0)
        {
            mask |= 1u << (uint(node.minimum.w) - 1u);
        }
        index = inside ? index + 1u : uint(node.maximum.w);
    }
    if (mask != 0u && fieldLocals.volumes[findLSB(mask)].slot.z == 1u)
    {
        size = fieldLocals.volumes[findLSB(mask)].gridMinCellSize.w;
    }
    return size;
}

uint DistanceFieldLocalCandidates(vec3  origin,
                                  vec3  direction,
                                  vec3  inverseDirection,
                                  float minimumDistance,
                                  float maximumDistance)
{
    uint mask  = 0u;
    uint count = uint(fieldLocals.nodes[0].maximum.w);
    for (uint index = 0u; index < count;)
    {
        DistanceFieldLocalNode node = fieldLocals.nodes[index];
        float                  enter, leave;
        bool intersects = DistanceFieldBoxSpan(origin, direction, inverseDirection, node.minimum.xyz, node.maximum.xyz,
                                               minimumDistance, maximumDistance, enter, leave);
        if (intersects && node.minimum.w > 0.0)
        {
            mask |= 1u << (uint(node.minimum.w) - 1u);
        }
        index = intersects ? index + 1u : uint(node.maximum.w);
    }
    return mask;
}

DistanceFieldTraceResult DistanceFieldTraceComposite(vec3  origin,
                                                     vec3  direction,
                                                     float minimumDistance,
                                                     float maximumDistance,
                                                     uint  budget,
                                                     bool  normals)
{
    DistanceFieldTraceResult result = DistanceFieldTraceResult(DISTANCE_FIELD_COVERED_MISS, 0.0, origin, vec3(0),
                                                               DISTANCE_FIELD_UNAVAILABLE, 0u, 0u, DISTANCE_FIELD_FAILURE_NONE);
    float                    cursor = minimumDistance;
    bool                     finished         = false;
    uint                     limit            = budget == 0u ? distanceField.dimensions.w : budget;
    vec3                     inverseDirection = 1.0 / mix(direction, vec3(1), equal(direction, vec3(0)));
    uint candidates = DistanceFieldLocalCandidates(origin, direction, inverseDirection, minimumDistance, maximumDistance);
    for (uint segment = 0u; segment < 2u * ZEN_FIELD_LOCAL_VOLUMES + 1u && !finished && cursor < maximumDistance; ++segment)
    {
        int   inside    = -1;
        float until     = maximumDistance;
        uint  remaining = candidates;
        while (remaining != 0u)
        {
            uint volume  = uint(findLSB(remaining));
            remaining   &= remaining - 1u;
            float enter, leave;
            if (fieldLocals.volumes[volume].slot.z != 0u
                && DistanceFieldBoxSpan(origin, direction, inverseDirection, fieldLocals.volumes[volume].authorityMinimum.xyz,
                                        fieldLocals.volumes[volume].authorityMaximum.xyz, cursor, maximumDistance, enter,
                                        leave))
            {
                if (enter <= cursor && inside < 0)
                {
                    inside = int(volume);
                    until  = leave;
                }
                else if (enter > cursor && inside < 0)
                {
                    until = min(until, enter);
                }
            }
        }
        DistanceFieldLevel level             = inside >= 0 ? DistanceFieldLocalLevel(uint(inside)) : DistanceFieldSceneLevel();
        level.tracing.z                      = normals ? 0.0 : 1.0;
        bool                     unavailable = inside >= 0 && fieldLocals.volumes[inside].slot.z == 2u;
        DistanceFieldTraceResult trace       = result;
        if (unavailable || result.iterations >= limit)
        {
            trace.status          = DISTANCE_FIELD_TRACE_UNAVAILABLE;
            trace.failureReason   = unavailable ? DISTANCE_FIELD_FAILURE_MISSING : DISTANCE_FIELD_FAILURE_WORK;
            trace.iterations      = 0u;
            trace.distanceSamples = 0u;
        }
        else
        {
            trace = DistanceFieldTraceLevel(level, origin, direction, cursor, until, limit - result.iterations);
        }
        trace.iterations      += result.iterations;
        trace.distanceSamples += result.distanceSamples;
        result                 = trace;
        finished               = trace.status != DISTANCE_FIELD_COVERED_MISS;
        cursor                 = until;
    }
    if (!finished && cursor < maximumDistance)
    {
        result.status        = DISTANCE_FIELD_TRACE_UNAVAILABLE;
        result.failureReason = DISTANCE_FIELD_FAILURE_WORK;
    }
    return result;
}

// Regions of the published field that committed edits made stale (DistanceFieldStaleUniform).
layout(std140, set = 4, binding = DISTANCE_FIELD_BINDING + 4) uniform uDistanceFieldStale
{
    uvec4 count;
    vec4  minimum[ZEN_FIELD_STALE_BOXES];
    vec4  maximum[ZEN_FIELD_STALE_BOXES];
}
fieldStale;
// Traces the scene field and its local volumes, up to the first stale region along the ray: beyond it
// the published data no longer describes committed geometry, so only an earlier hit is a result.
DistanceFieldTraceResult DistanceFieldTraceCurrent(vec3  origin,
                                                   vec3  direction,
                                                   float maximumDistance,
                                                   uint  budget,
                                                   bool  normals)
{
    float stale            = maximumDistance;
    vec3  inverseDirection = 1.0 / mix(direction, vec3(1), equal(direction, vec3(0)));
    for (uint box = 0u; box < min(fieldStale.count.x, ZEN_FIELD_STALE_BOXES); ++box)
    {
        float enter, leave;
        if (DistanceFieldBoxSpan(origin, direction, inverseDirection, fieldStale.minimum[box].xyz, fieldStale.maximum[box].xyz,
                                 0.0, maximumDistance, enter, leave))
        {
            stale = min(stale, enter);
        }
    }
    DistanceFieldTraceResult trace = DistanceFieldTraceComposite(origin, direction, 0.0, stale, budget, normals);
    if (trace.status == DISTANCE_FIELD_COVERED_MISS && stale < maximumDistance)
    {
        trace.status        = DISTANCE_FIELD_TRACE_UNAVAILABLE;
        trace.failureReason = DISTANCE_FIELD_FAILURE_MISSING;
    }
    return trace;
}
DistanceFieldTraceResult DistanceFieldTraceCurrent(vec3 origin, vec3 direction, float maximumDistance, uint budget)
{
    return DistanceFieldTraceCurrent(origin, direction, maximumDistance, budget, true);
}
#endif
#endif
