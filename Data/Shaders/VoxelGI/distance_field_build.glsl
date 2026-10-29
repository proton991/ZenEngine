// Native exact-distance construction informed by FidelityFX SDK v1.1.4, commit
// c6efa6bf7f2027b3ec94f28578bb5965eabb9e55. Eight samples span seven intervals and
// R8 stores distance/brick extent. Zen uses bounded triangle batches, a full-brick
// reference halo, and exact closest points instead of AMD's sparse seeds/Eikonal.
// Material coverage is bounded by conservative patch refinement and a covered
// witness at uncertain leaves. Unresolved coverage remains explicitly unavailable.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#define DISTANCE_FIELD_BUILD
#include "distance_field_common.glsl"
#include "Graphics/Shared/DistanceFieldScene.h"
#include "Graphics/Shared/DistanceFieldScheduler.h"

layout(std140, set = 4, binding = 4) uniform uDistanceFieldBuild
{
    uvec4 triangleRange; // first triangle, count, total triangles, job capacity
    uvec4 limits;        // reference capacity, references/cell, crop flags, selected range count
    uvec4 workRange;     // first job/active cell, count, hierarchy child/parent offsets
    uvec4 hierarchy;     // x parent side; grid side selects leaf construction
}
fieldBuild;

#if defined(FIELD_RETAIN) || defined(FIELD_COUNT) || defined(FIELD_COMPACT)
layout(std140, set = 4, binding = 19) uniform uFieldDirty
{
    uvec4 count;
    vec4  minimum[ZEN_FIELD_STALE_BOXES];
    vec4  maximum[ZEN_FIELD_STALE_BOXES];
}
fieldDirty;
bool FieldDirtyCell(uvec3 cell)
{
    bool dirty = (fieldBuild.limits.z & ZEN_FIELD_BUILD_PARTIAL) == 0u;
    for (uint box = 0u; box < fieldDirty.count.x && !dirty; ++box)
    {
        dirty = all(greaterThanEqual(vec3(cell), fieldDirty.minimum[box].xyz))
             && all(lessThan(vec3(cell), fieldDirty.maximum[box].xyz));
    }
    return dirty;
}
#endif
#if defined(FIELD_CLEAR) || defined(FIELD_RETAIN) || defined(FIELD_RECYCLE)
#    ifdef FIELD_RECYCLE
#        define FIELD_USED_ACCESS readonly
#    else
#        define FIELD_USED_ACCESS writeonly
#    endif
layout(std430, set = 4, binding = 20) FIELD_USED_ACCESS buffer FieldBrickUsed
{
    uint fieldBrickUsed[];
};
#endif
#if defined(FIELD_CLEAR) || defined(FIELD_RECYCLE) || defined(FIELD_ALLOCATE)
#    ifdef FIELD_ALLOCATE
#        define FIELD_FREE_ACCESS readonly
#    else
#        define FIELD_FREE_ACCESS writeonly
#    endif
layout(std430, set = 4, binding = 21) FIELD_FREE_ACCESS buffer FieldFreeBricks
{
    uint fieldFreeBricks[];
};
#endif
#ifdef FIELD_RETAIN
layout(std430, set = 4, binding = 22) readonly buffer FieldPreviousCells
{
    uint fieldPreviousCells[];
};
#endif

// Clear defines all scratch storage before zero-dispatch inactive phases. Later
// producers populate bounded prefixes; their element consumers stay readonly.
#if (defined(FIELD_JOBS) || defined(FIELD_COVERAGE)) || defined(FIELD_CLEAR)
#    define FIELD_JOBS_ACCESS writeonly
#else
#    define FIELD_JOBS_ACCESS readonly
#endif
#if defined(FIELD_BATCH_CLEAR) || defined(FIELD_CLEAR)
#    define FIELD_COUNTS_ACCESS  writeonly
#    define FIELD_CURSORS_ACCESS writeonly
#else
#    ifdef FIELD_COUNT
#        define FIELD_COUNTS_ACCESS
#    else
#        define FIELD_COUNTS_ACCESS readonly
#    endif
#    ifdef FIELD_COMPACT
#        define FIELD_CURSORS_ACCESS
#    else
#        define FIELD_CURSORS_ACCESS readonly
#    endif
#endif
#if defined(FIELD_SCAN) || defined(FIELD_CLEAR)
#    define FIELD_OFFSETS_ACCESS writeonly
#    define FIELD_SCAN_ACCESS    writeonly
#else
#    ifdef FIELD_ALLOCATE
#        define FIELD_OFFSETS_ACCESS
#    else
#        define FIELD_OFFSETS_ACCESS readonly
#    endif
#    ifdef FIELD_SCAN_SUMS
#        define FIELD_SCAN_ACCESS
#    else
#        define FIELD_SCAN_ACCESS readonly
#    endif
#endif
#if defined(FIELD_COMPACT) || defined(FIELD_CLEAR)
#    define FIELD_REFERENCES_ACCESS writeonly
#else
#    define FIELD_REFERENCES_ACCESS readonly
#endif
#if defined(FIELD_ALLOCATE) || defined(FIELD_CLEAR)
#    define FIELD_ACTIVE_ACCESS writeonly
#else
#    define FIELD_ACTIVE_ACCESS readonly
#endif
#ifdef FIELD_CLEAR
#    define FIELD_SPLIT_ACCESS writeonly
#else
#    define FIELD_SPLIT_ACCESS
#endif
#ifdef FIELD_CLEAR
#    define FIELD_COUNTER_ACCESS writeonly
#else
#    define FIELD_COUNTER_ACCESS
#endif
layout(std430, set = 4, binding = 5) FIELD_COUNTER_ACCESS buffer FieldBuildCounters
{
    uint fieldCounters[];
};
#if (defined(FIELD_JOBS) || defined(FIELD_COVERAGE)) || defined(FIELD_COUNT) || defined(FIELD_COMPACT) || defined(FIELD_EMIT) \
    || defined(FIELD_CLEAR)
layout(std430, set = 4, binding = 6) FIELD_JOBS_ACCESS buffer FieldBuildJobs
{
    uvec4 fieldJobs[]; // triangle record, packed region xyz, quadtree node code, depth
};
#endif
#if defined(FIELD_BATCH_CLEAR) || defined(FIELD_COUNT) || defined(FIELD_SCAN) || defined(FIELD_ALLOCATE) \
    || defined(FIELD_COMPACT) || defined(FIELD_EMIT) || defined(FIELD_CLEAR)
layout(std430, set = 4, binding = 7) FIELD_COUNTS_ACCESS buffer FieldBuildCounts
{
    uint fieldCounts[];
};
#endif
#if defined(FIELD_SCAN) || defined(FIELD_ALLOCATE) || defined(FIELD_COMPACT) || defined(FIELD_EMIT) || defined(FIELD_CLEAR)
layout(std430, set = 4, binding = 8) FIELD_OFFSETS_ACCESS buffer FieldBuildOffsets
{
    uint fieldOffsets[];
};
#endif
#if defined(FIELD_BATCH_CLEAR) || defined(FIELD_COMPACT) || defined(FIELD_CLEAR)
layout(std430, set = 4, binding = 9) FIELD_CURSORS_ACCESS buffer FieldBuildCursors
{
    uint fieldCursors[];
};
#endif
#if defined(FIELD_COMPACT) || defined(FIELD_EMIT) || defined(FIELD_CLEAR)
layout(std430, set = 4, binding = 10) FIELD_REFERENCES_ACCESS buffer FieldBuildReferences
{
    uint fieldReferences[];
};
#endif
#if defined(FIELD_ALLOCATE) || defined(FIELD_EMIT) || defined(FIELD_CLEAR)
layout(std430, set = 4, binding = 11) FIELD_ACTIVE_ACCESS buffer FieldBuildActive
{
    // Cell index; bit 0 one when this batch allocated the brick, bits 1-7 the reference chunk,
    // bits 8+ the split slot + 1 (zero when one workgroup emits the whole cell).
    uvec2 fieldActive[];
};
// Dense cells' split slots (ZEN_FIELD_EMIT_SPLIT_WORDS each): texel minima as float bits, then the
// number of chunks that have merged into them.
layout(std430, set = 4, binding = 23) FIELD_SPLIT_ACCESS buffer FieldBuildEmitSplit
{
    uint fieldEmitSplit[];
};
#endif
#if defined(FIELD_SCAN) || defined(FIELD_SCAN_SUMS) || defined(FIELD_ALLOCATE) || defined(FIELD_CLEAR)
layout(std430, set = 4, binding = 12) FIELD_SCAN_ACCESS buffer FieldBuildScan
{
    uint fieldScan[]; // one block sum, then exclusive block offset, per 256 cells
};
#endif

#if defined(FIELD_CLEAR)
#    define FIELD_CONTROL_ACCESS writeonly
#elif defined(FIELD_BATCH_CLEAR) || defined(FIELD_SCAN) || defined(FIELD_HIERARCHY) || defined(FIELD_PREPARE_JOBS)
#    define FIELD_CONTROL_ACCESS readonly
#else
#    define FIELD_CONTROL_ACCESS
#endif
layout(std430, set = 4, binding = 13) FIELD_CONTROL_ACCESS buffer FieldBuildControl
{
    uint fieldControl[];
};
#if defined(FIELD_CLEAR) || defined(FIELD_PREPARE_SCHEDULE)
layout(std430, set = 4, binding = 14) writeonly buffer FieldBuildIndirect
{
    uint fieldIndirect[];
};
#endif
#if defined(FIELD_PREPARE_SCHEDULE) || defined(FIELD_ADVANCE_SCHEDULE)
layout(std140, set = 4, binding = 15) uniform uFieldSchedule
{
    uvec4 settings;
}
fieldSchedule;
#endif

#if defined(FIELD_CLEAR) || defined(FIELD_BATCH_CLEAR)
#    define FIELD_COVERAGE_ACCESS writeonly
#elif defined(FIELD_PREPARE_SCHEDULE)
#    define FIELD_COVERAGE_ACCESS readonly
#else
#    define FIELD_COVERAGE_ACCESS
#endif
#if defined(FIELD_CLEAR) || defined(FIELD_JOBS)
#    define FIELD_COVERAGE_QUEUE_ACCESS writeonly
#else
#    define FIELD_COVERAGE_QUEUE_ACCESS readonly
#endif
#if defined(FIELD_CLEAR) || defined(FIELD_JOBS) || defined(FIELD_COVERAGE) || defined(FIELD_PREPARE_SCHEDULE)
layout(std430, set = 4, binding = 16) FIELD_COVERAGE_ACCESS buffer FieldBuildCoverage
{
    uint fieldCoverage[];
};
#endif
#if defined(FIELD_CLEAR) || defined(FIELD_JOBS) || defined(FIELD_COVERAGE) || defined(FIELD_PREPARE_SCHEDULE)
layout(std430, set = 4, binding = 17) FIELD_COVERAGE_QUEUE_ACCESS buffer FieldBuildCoverageQueue
{
    uint fieldCoverageQueue[];
};
#endif

#if defined(FIELD_JOBS) || defined(FIELD_COVERAGE)
layout(std430, set = 4, binding = 18) readonly buffer FieldBuildRanges
{
    uvec4 fieldRanges[]; // source first triangle, count, logical first triangle, reserved
};
#endif

#if (defined(FIELD_JOBS) || defined(FIELD_COVERAGE)) || defined(FIELD_COUNT) || defined(FIELD_COMPACT) || defined(FIELD_EMIT)
#    include "distance_field_geometry.glsl"
#    include "voxel_coverage.glsl"
#endif

#if defined(FIELD_COUNT) || defined(FIELD_COMPACT) || (defined(FIELD_JOBS) || defined(FIELD_COVERAGE))
layout(local_size_x = 64) in;
#elif defined(FIELD_EMIT)
layout(local_size_x = 8, local_size_y = 8, local_size_z = 8) in;
#else
layout(local_size_x = 256) in;
#endif

uint FieldCellCount()
{
    uint side = distanceField.dimensions.x;
    return side * side * side;
}

uint FieldCoverageJobLimit()
{
    // A 4^3-cell region contributes at most64 references and at mostone/cell.
    return min(min(fieldBuild.limits.x / 64u, fieldBuild.limits.y), fieldBuild.triangleRange.w);
}

uint FieldLinearGroup()
{
    return gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
}

uint FieldLinearInvocation()
{
    return FieldLinearGroup() * gl_WorkGroupSize.x + gl_LocalInvocationIndex;
}

#ifndef FIELD_CLEAR
uint FieldTriangleFirst()
{
    return fieldControl[ZEN_FIELD_CONTROL_TRIANGLE_FIRST];
}
uint FieldTriangleCount()
{
    return fieldControl[ZEN_FIELD_CONTROL_TRIANGLE_COUNT];
}
uint FieldWorkFirst()
{
    return fieldControl[ZEN_FIELD_CONTROL_WORK_FIRST];
}
uint FieldWorkCount()
{
    return fieldControl[ZEN_FIELD_CONTROL_WORK_COUNT];
}
uint FieldHierarchySide()
{
    return distanceField.dimensions.x >> fieldControl[ZEN_FIELD_CONTROL_HIERARCHY_LEVEL];
}
uint FieldChildOffset()
{
    return fieldControl[ZEN_FIELD_CONTROL_CHILD_OFFSET];
}
uint FieldParentOffset()
{
    return fieldControl[ZEN_FIELD_CONTROL_PARENT_OFFSET];
}
#endif

#if defined(FIELD_PREPARE_SCHEDULE) || defined(FIELD_ADVANCE_SCHEDULE)
#    include "distance_field_schedule.glsl"
#endif

#if (defined(FIELD_JOBS) || defined(FIELD_COVERAGE)) || defined(FIELD_COUNT) || defined(FIELD_COMPACT) \
    || defined(FIELD_SCAN_SUMS) || defined(FIELD_ALLOCATE) || defined(FIELD_EMIT)
void FieldFail(uint reason)
{
    uint retryable = fieldControl[ZEN_FIELD_CONTROL_PHASE] <= ZEN_FIELD_PHASE_SCAN_SUMS
                          && fieldControl[ZEN_FIELD_CONTROL_COVERAGE_STREAMED] == 0u
                       ? reason & ZEN_FIELD_RETRYABLE_FAILURES
                       : 0u;
    if (retryable != 0u)
    {
        atomicOr(fieldControl[ZEN_FIELD_CONTROL_BATCH_FAILURE], retryable);
    }
    if ((reason & ~retryable) != 0u)
    {
        atomicOr(fieldCounters[ZEN_FIELD_COUNTER_FAILURE], reason & ~retryable);
    }
}
#endif

#if (defined(FIELD_JOBS) || defined(FIELD_COVERAGE)) || defined(FIELD_EMIT)
void FieldDiagnostic(uint site, uint triangle, uint cell, uint sampleIndex)
{
    if (atomicCompSwap(fieldCounters[ZEN_FIELD_COUNTER_FAILURE_SITE], 0u, site) == 0u)
    {
        fieldCounters[ZEN_FIELD_COUNTER_FAILURE_TRIANGLE] = triangle;
        fieldCounters[ZEN_FIELD_COUNTER_FAILURE_CELL]     = cell;
        fieldCounters[ZEN_FIELD_COUNTER_FAILURE_SAMPLE]   = sampleIndex;
    }
}

void FieldGeometryFail(uint site, uint triangle, uint cell, uint sampleIndex)
{
    FieldDiagnostic(site, triangle, cell, sampleIndex);
    FieldFail(ZEN_FIELD_FAILURE_GEOMETRY);
}
#endif

uvec3 FieldCellCoordinate(uint index)
{
    uint side = distanceField.dimensions.x;
    return uvec3(index % side, (index / side) % side, index / (side * side));
}

#if (defined(FIELD_JOBS) || defined(FIELD_COVERAGE)) || defined(FIELD_COUNT) || defined(FIELD_COMPACT) || defined(FIELD_EMIT)
void FieldTriangle(uint triangle, out vec3 a, out vec3 b, out vec3 c)
{
    vec3 alpha;
    FieldTriangleVertices(triangle, alpha, a, b, c);
    a = (a - distanceField.gridMinCellSize.xyz) / distanceField.gridMinCellSize.w;
    b = (b - distanceField.gridMinCellSize.xyz) / distanceField.gridMinCellSize.w;
    c = (c - distanceField.gridMinCellSize.xyz) / distanceField.gridMinCellSize.w;
}
#    include "distance_field_patch.glsl"
#endif

#if defined(FIELD_JOBS) || defined(FIELD_COVERAGE)
uint FieldSourceTriangle(uint logicalTriangle)
{
    uint triangle = logicalTriangle;
    if (fieldBuild.limits.w != 0u)
    {
        uint lower = 0u;
        uint upper = min(fieldBuild.limits.w, uint(fieldRanges.length()));
        triangle   = 0xffffffffu;
        // Prefix ranges are sorted by logical first, with no gaps or overlaps.
        // Malformed input cannot become an empty selected interval.
        while (lower < upper)
        {
            uint  middle = lower + (upper - lower) / 2u;
            uvec4 range  = fieldRanges[middle];
            if (logicalTriangle < range.z)
            {
                upper = middle;
            }
            else if (logicalTriangle - range.z >= range.y)
            {
                lower = middle + 1u;
            }
            else
            {
                uint offset = logicalTriangle - range.z;
                triangle    = range.x <= 0xffffffffu - offset ? range.x + offset : 0xffffffffu;
                lower       = upper;
            }
        }
    }
    if (triangle >= triangles.length())
    {
        FieldGeometryFail(ZEN_FIELD_GEOMETRY_TRIANGLE_RANGE, triangle, logicalTriangle, 0xffffffffu);
    }
    return triangle;
}

bool FieldPatchAffectsVolume(vec3 a, vec3 b, vec3 c)
{
    // Cropping changes spatial authority, not material coverage. Include the full
    // distance-saturation halo so every stored sample sees nearby geometry.
    vec3 minimum = min(a, min(b, c));
    vec3 maximum = max(a, max(b, c));
    return (fieldBuild.limits.z & ZEN_FIELD_BUILD_CROP_VOLUME) == 0u
        || (all(lessThanEqual(minimum, vec3(float(distanceField.dimensions.x) + 1.0001)))
            && all(greaterThanEqual(maximum, vec3(-1.0001))));
}

#    include "distance_field_alpha.glsl"

uint FieldPatchRegionLayout(vec3 a, vec3 b, vec3 c, out ivec3 lower, out uvec3 extent)
{
    vec3 minimum = min(a, min(b, c));
    vec3 maximum = max(a, max(b, c));
    int  side    = int(distanceField.dimensions.x);
    lower        = clamp(ivec3(floor(minimum - vec3(1.0001))), ivec3(0), ivec3(side - 1)) / int(ZEN_FIELD_REGION_SIDE);
    ivec3 upper  = clamp(ivec3(floor(maximum + vec3(1.0001))), ivec3(0), ivec3(side - 1)) / int(ZEN_FIELD_REGION_SIDE);
    extent       = uvec3(upper - lower + 1);
    return extent.x * extent.y * extent.z;
}

void FieldStorePatchRegions(uint  triangle,
                            uint  code,
                            uint  depth,
                            ivec3 lower,
                            uvec3 extent,
                            uint  outputFirst,
                            uint  regionFirst,
                            uint  count)
{
    for (uint index = 0u; index < count; ++index)
    {
        uint  regionIndex = regionFirst + index;
        uvec3 region =
            (uvec3(lower)
             + uvec3(regionIndex % extent.x, (regionIndex / extent.x) % extent.y, regionIndex / (extent.x * extent.y)))
            * ZEN_FIELD_REGION_SIDE;
        uint packed                    = region.x | (region.y << 6u) | (region.z << 12u);
        fieldJobs[outputFirst + index] = uvec4(triangle, packed, code, depth);
    }
}

void FieldAppendPatchJobs(uint triangle, uint code, uint depth, vec3 a, vec3 b, vec3 c)
{
    uint current = atomicAdd(fieldCounters[ZEN_FIELD_COUNTER_JOBS], 0u);
    if (current < fieldBuild.triangleRange.w)
    {
        ivec3 lower;
        uvec3 extent;
        uint  count = FieldPatchRegionLayout(a, b, c, lower, extent);
        uint  first = atomicAdd(fieldCounters[ZEN_FIELD_COUNTER_JOBS], count);
        if (first > fieldBuild.triangleRange.w || count > fieldBuild.triangleRange.w - min(first, fieldBuild.triangleRange.w))
        {
            FieldFail(ZEN_FIELD_FAILURE_JOBS);
        }
        else
        {
            FieldStorePatchRegions(triangle, code, depth, lower, extent, first, 0u, count);
        }
    }
    else
    {
        FieldFail(ZEN_FIELD_FAILURE_JOBS);
    }
}

uint FieldAppendCoveragePatchJobs(uint triangle, uint code, uint depth, vec3 a, vec3 b, vec3 c, uint regionFirst)
{
    ivec3 lower;
    uvec3 extent;
    uint  total      = FieldPatchRegionLayout(a, b, c, lower, extent);
    uint  limit      = FieldCoverageJobLimit();
    uint  nextRegion = regionFirst;
    if (atomicAdd(fieldCounters[ZEN_FIELD_COUNTER_JOBS], 0u) < limit)
    {
        // Written reservation intersections form a complete prefix. The ordered
        // Advance dispatch clamps the over-reserved counter to the window end.
        uint first = atomicAdd(fieldCounters[ZEN_FIELD_COUNTER_JOBS], total - regionFirst);
        uint count = min(total - regionFirst, limit - min(first, limit));
        FieldStorePatchRegions(triangle, code, depth, lower, extent, first, regionFirst, count);
        nextRegion += count;
    }
    return nextRegion < total ? nextRegion + 1u : 0u;
}

#    ifdef FIELD_JOBS
void FieldCreateJobs(uint logicalTriangle)
{
    uint triangle = FieldSourceTriangle(logicalTriangle);
    if (triangle < triangles.length() && triangles[triangle].w != 0u)
    {
        vec3 vertexAlpha, a, b, c;
        FieldTriangleVertices(triangle, vertexAlpha, a, b, c);
        bool finite = !any(isnan(a)) && !any(isinf(a)) && !any(isnan(b)) && !any(isinf(b)) && !any(isnan(c)) && !any(isinf(c));
        if (!finite)
        {
            FieldGeometryFail(ZEN_FIELD_GEOMETRY_NONFINITE_INPUT, triangle, 0xffffffffu, 0xffffffffu);
        }
        else
        {
            a             = (a - distanceField.gridMinCellSize.xyz) / distanceField.gridMinCellSize.w;
            b             = (b - distanceField.gridMinCellSize.xyz) / distanceField.gridMinCellSize.w;
            c             = (c - distanceField.gridMinCellSize.xyz) / distanceField.gridMinCellSize.w;
            vec3  minimum = min(a, min(b, c));
            vec3  maximum = max(a, max(b, c));
            float side    = float(distanceField.dimensions.x);
            bool  covered = all(greaterThanEqual(minimum, vec3(-1e-4))) && all(lessThanEqual(maximum, vec3(side + 1e-4)));
            if (!covered && (fieldBuild.limits.z & ZEN_FIELD_BUILD_CROP_VOLUME) == 0u)
            {
                FieldGeometryFail(ZEN_FIELD_GEOMETRY_OUTSIDE_DOMAIN, triangle, 0xffffffffu, 0xffffffffu);
            }
            else if (FieldPatchAffectsVolume(a, b, c) && any(notEqual(FieldDistanceCross(b - a, c - a), vec3(0))))
            {
                FieldCreateCoverageJobs(triangle, logicalTriangle - FieldTriangleFirst(), a, b, c, vertexAlpha);
            }
        }
    }
}
#    endif

#endif

#if defined(FIELD_COUNT) || defined(FIELD_COMPACT)
void FieldVisitJob(uint jobIndex)
{
    uvec4 job    = fieldJobs[jobIndex];
    uint  lane   = gl_LocalInvocationIndex;
    uvec3 region = uvec3(job.y & 63u, (job.y >> 6u) & 63u, (job.y >> 12u) & 63u);
    uvec3 cell   = region + uvec3(lane % 4u, (lane / 4u) % 4u, lane / 16u);
    if (all(lessThan(cell, uvec3(distanceField.dimensions.x))) && FieldDirtyCell(cell))
    {
        vec3 a, b, c;
        FieldPatchTriangle(job, a, b, c);
        vec3 center = vec3(cell) + vec3(0.5);
        // Reuse the engine's exact SAT test after scaling the expanded box to
        // the unit box expected by VoxelTriangleBox. Include a rounding guard.
        // Unknown patches invalidate the cells they can intersect. They emit no distance
        // samples, so unlike retained surfaces they do not need the saturation halo.
        // Traversal stops at every cell/node exit before entering an unavailable cell.
        float width    = (job.w & ZEN_FIELD_PATCH_UNAVAILABLE) != 0u ? 1.0002 : 3.0002;
        bool  overlaps = VoxelTriangleBox((a - center) / width + vec3(0.5), (b - center) / width + vec3(0.5),
                                          (c - center) / width + vec3(0.5), ivec3(0));
        if (overlaps)
        {
            uint index = DistanceFieldIndex(cell, distanceField.dimensions.x);
            if ((job.w & ZEN_FIELD_PATCH_UNAVAILABLE) != 0u)
            {
#    ifdef FIELD_COUNT
                if (atomicExchange(distanceFieldCells[index], DISTANCE_FIELD_UNAVAILABLE) != DISTANCE_FIELD_UNAVAILABLE)
                {
                    atomicAdd(fieldCounters[ZEN_FIELD_COUNTER_UNAVAILABLE], 1u);
                }
#    endif
            }
            else
            {
#    ifdef FIELD_COUNT
                uint previous = atomicAdd(fieldCounts[index], 1u);
                atomicMax(fieldCounters[ZEN_FIELD_COUNTER_CELL_PEAK], previous + 1u);
                if (previous >= fieldBuild.limits.y)
                {
                    FieldFail(ZEN_FIELD_FAILURE_CELL_REFS);
                }
#    else
                uint relative = atomicAdd(fieldCursors[index], 1u);
                uint offset   = fieldOffsets[index];
                if (relative < fieldCounts[index] && offset < fieldBuild.limits.x && relative < fieldBuild.limits.x - offset)
                {
                    fieldReferences[offset + relative] = jobIndex;
                }
                else
                {
                    FieldFail(ZEN_FIELD_FAILURE_REFERENCES);
                }
#    endif
            }
        }
    }
}
#endif

#if defined(FIELD_SCAN) || defined(FIELD_SCAN_SUMS)
#    ifdef FIELD_SCAN
shared uint fieldPrefix[ZEN_FIELD_SCAN_GROUP];
const uint  FIELD_SCAN_SIZE = ZEN_FIELD_SCAN_GROUP;
#    else
shared uint fieldPrefix[ZEN_FIELD_MAX_SCAN_BLOCKS];
const uint  FIELD_SCAN_SIZE = ZEN_FIELD_MAX_SCAN_BLOCKS;
#    endif

void FieldExclusiveScan()
{
    uint lane = gl_LocalInvocationIndex;
    for (uint stride = 1u; stride < FIELD_SCAN_SIZE; stride <<= 1u)
    {
        for (uint pair = lane; pair < FIELD_SCAN_SIZE / (2u * stride); pair += ZEN_FIELD_SCAN_GROUP)
        {
            uint right          = (pair + 1u) * (2u * stride) - 1u;
            fieldPrefix[right] += fieldPrefix[right - stride];
        }
        barrier();
    }
    if (lane == 0u)
    {
#    ifdef FIELD_SCAN
        fieldScan[FieldLinearGroup()] = fieldPrefix[FIELD_SCAN_SIZE - 1u];
#    else
        fieldCounters[ZEN_FIELD_COUNTER_REFERENCES] = fieldPrefix[FIELD_SCAN_SIZE - 1u];
        if (fieldPrefix[FIELD_SCAN_SIZE - 1u] > fieldBuild.limits.x)
        {
            FieldFail(ZEN_FIELD_FAILURE_REFERENCES);
        }
#    endif
        fieldPrefix[FIELD_SCAN_SIZE - 1u] = 0u;
    }
    barrier();
    for (uint stride = FIELD_SCAN_SIZE >> 1u; stride > 0u; stride >>= 1u)
    {
        for (uint pair = lane; pair < FIELD_SCAN_SIZE / (2u * stride); pair += ZEN_FIELD_SCAN_GROUP)
        {
            uint right                   = (pair + 1u) * (2u * stride) - 1u;
            uint left                    = fieldPrefix[right - stride];
            fieldPrefix[right - stride]  = fieldPrefix[right];
            fieldPrefix[right]          += left;
        }
        barrier();
    }
}
#endif

#ifdef FIELD_EMIT
shared uint fieldHitSamples;
shared uint fieldEmitLast;
shared vec3 fieldTriangleA[ZEN_FIELD_REFERENCE_TILE];
shared vec3 fieldTriangleB[ZEN_FIELD_REFERENCE_TILE];
shared vec3 fieldTriangleC[ZEN_FIELD_REFERENCE_TILE];

void FieldEmit(uint activeIndex)
{
    uvec2 activeEntry = fieldActive[activeIndex];
    uint  cell        = activeEntry.x;
    uint  split       = activeEntry.y >> 8u;
    uint  brick       = (distanceFieldCells[cell] & DISTANCE_FIELD_BRICK_MASK) - 1u;
    uint  lane        = gl_LocalInvocationIndex;
    if (lane == 0u)
    {
        fieldHitSamples = 0u;
    }
    barrier();
    uint  count    = min(fieldCounts[cell], ZEN_FIELD_MAX_CELL_REFS);
    uint  begin    = split != 0u ? ((activeEntry.y >> 1u) & 127u) * ZEN_FIELD_EMIT_CHUNK_REFERENCES : 0u;
    uint  end      = split != 0u ? min(count, begin + ZEN_FIELD_EMIT_CHUNK_REFERENCES) : count;
    vec3  position = vec3(FieldCellCoordinate(cell)) + vec3(gl_LocalInvocationID) / 7.0;
    float nearest  = 1.0;
    for (uint first = begin; first < end; first += ZEN_FIELD_REFERENCE_TILE)
    {
        uint chunk = min(end - first, ZEN_FIELD_REFERENCE_TILE);
        if (lane < chunk)
        {
            uint job = fieldReferences[fieldOffsets[cell] + first + lane];
            FieldPatchTriangle(fieldJobs[job], fieldTriangleA[lane], fieldTriangleB[lane], fieldTriangleC[lane]);
        }
        barrier();
        for (uint index = 0u; index < chunk; ++index)
        {
            float sampleDistance =
                FieldTriangleDistance(position, fieldTriangleA[index], fieldTriangleB[index], fieldTriangleC[index]);
            if (isnan(sampleDistance) || isinf(sampleDistance))
            {
                uint job = fieldReferences[fieldOffsets[cell] + first + index];
                FieldGeometryFail(ZEN_FIELD_GEOMETRY_CLOSEST_POINT, fieldJobs[job].x, cell, lane);
            }
            else
            {
                nearest = min(nearest, sampleDistance);
            }
        }
        // Every sample must finish this tile before any lane replaces it.
        barrier();
    }
    if (isnan(nearest) || isinf(nearest))
    {
        FieldGeometryFail(ZEN_FIELD_GEOMETRY_NONFINITE_DISTANCE, 0xffffffffu, cell, lane);
        nearest = 1.0;
    }
    bool finish = true;
    if (split != 0u)
    {
        // Non-negative floats order as their bits, so the merged minimum is the one a single
        // workgroup finds, in any chunk order; the brick stores the clamped value either way.
        // The last chunk to merge finishes the cell.
        uint base = (split - 1u) * ZEN_FIELD_EMIT_SPLIT_WORDS;
        atomicMin(fieldEmitSplit[base + lane], nearest > 0.0 ? floatBitsToUint(nearest) : 0u);
        memoryBarrierBuffer();
        barrier();
        if (lane == 0u)
        {
            uint chunks   = (count + ZEN_FIELD_EMIT_CHUNK_REFERENCES - 1u) / ZEN_FIELD_EMIT_CHUNK_REFERENCES;
            fieldEmitLast = atomicAdd(fieldEmitSplit[base + 512u], 1u) + 1u == chunks ? 1u : 0u;
        }
        barrier();
        finish = fieldEmitLast != 0u;
        if (finish)
        {
            nearest = uintBitsToFloat(atomicMin(fieldEmitSplit[base + lane], 0xffffffffu));
        }
    }
    // Uniform across the workgroup, so its barrier is valid.
    if (finish)
    {
        uvec3 tiles = distanceField.atlasDimensions.xyz / 8u;
        uvec3 tile  = uvec3(brick % tiles.x, (brick / tiles.x) % tiles.y, brick / (tiles.x * tiles.y));
        ivec3 texel = ivec3(tile * 8u + gl_LocalInvocationID);
        if ((activeEntry.y & 1u) == 0u)
        {
            nearest = min(nearest, imageLoad(distanceFieldBuildAtlas, texel).r);
        }
        imageStore(distanceFieldBuildAtlas, texel, vec4(clamp(nearest, 0.0, 1.0)));
        // Trilinear values cannot fall below their samples. Keep a quantization margin so a
        // rounding-down UNORM sample cannot turn a skipped brick into a possible hit.
        if (nearest <= 2.0 * distanceField.tracing.x + 1.0 / 255.0)
        {
            atomicOr(fieldHitSamples, 1u);
        }
        barrier();
        if (lane == 0u && fieldHitSamples != 0u)
        {
            distanceFieldCells[cell] &= DISTANCE_FIELD_BRICK_MASK;
        }
    }
}
#endif

void FieldMain()
{
#ifdef FIELD_CLEAR
    uint index         = FieldLinearInvocation();
    uint cells         = FieldCellCount();
    uint hierarchySize = DistanceFieldHierarchyOffset(distanceField.dimensions.z);
    if (index < fieldBrickUsed.length())
    {
        fieldBrickUsed[index] = 0u;
    }
    if (index < fieldFreeBricks.length())
    {
        fieldFreeBricks[index] = index;
    }
    if (index < cells)
    {
        distanceFieldCells[index] = DISTANCE_FIELD_EMPTY;
        fieldCounts[index]        = 0u;
        fieldOffsets[index]       = 0u;
        fieldCursors[index]       = 0u;
    }
    if (index < uint(fieldActive.length()))
    {
        fieldActive[index] = uvec2(0);
    }
    if (index < uint(fieldEmitSplit.length()))
    {
        fieldEmitSplit[index] = 0u;
    }
    if (index < fieldBuild.limits.x)
    {
        fieldReferences[index] = 0u;
    }
    if (index < fieldBuild.triangleRange.w)
    {
        fieldJobs[index] = uvec4(0);
    }
    if (index < (cells + ZEN_FIELD_SCAN_GROUP - 1u) / ZEN_FIELD_SCAN_GROUP)
    {
        fieldScan[index] = 0u;
    }
    if (index == 0u)
    {
        for (uint word = 0u; word < ZEN_FIELD_CONTROL_WORDS; ++word)
        {
            fieldControl[word] = 0u;
        }
        fieldControl[ZEN_FIELD_CONTROL_PHASE] =
            fieldBuild.triangleRange.z > 0u ? ZEN_FIELD_PHASE_CLEAR_BATCH : ZEN_FIELD_PHASE_HIERARCHY;
        // The quota learns what the workspace capacities fit, from the whole scene down; the
        // owner's per-frame batch cap (triangleRange.y) bounds each batch separately.
        fieldControl[ZEN_FIELD_CONTROL_TRIANGLE_QUOTA] = max(fieldBuild.triangleRange.z, 1u);
        fieldControl[ZEN_FIELD_CONTROL_TRIANGLE_COUNT] = min(fieldBuild.triangleRange.y, fieldBuild.triangleRange.z);
        fieldControl[ZEN_FIELD_CONTROL_INITIALIZED]    = 1u;
        for (uint word = 0u; word < ZEN_FIELD_INDIRECT_WORDS; ++word)
        {
            fieldIndirect[word] = 0u;
        }
    }
    // Coverage slots are sized by the scene's coverage triangles (DistanceFieldScene::Start).
    if (index < uint(fieldCoverage.length()))
    {
        fieldCoverage[index] = 0u;
    }
    if (index < uint(fieldCoverageQueue.length()))
    {
        fieldCoverageQueue[index] = 0u;
    }
    if (index < hierarchySize)
    {
        // Every cell is completed-empty above, so a scene without triangles is complete here; otherwise
        // the hierarchy stays unavailable until the hierarchy phase reduces the finished map.
        distanceFieldHierarchy[index] = fieldBuild.triangleRange.z > 0u ? DISTANCE_FIELD_UNAVAILABLE_BIT : 0u;
    }
    if (index < ZEN_FIELD_COUNTER_WORDS)
    {
        fieldCounters[index] = 0u;
    }
    if (index == ZEN_FIELD_COUNTER_FAILURE)
    {
        uint  side           = distanceField.dimensions.x;
        uvec3 atlas          = distanceField.atlasDimensions.xyz;
        uint  capacity       = (atlas.x / 8u) * (atlas.y / 8u) * (atlas.z / 8u);
        bool valid           = side > 0u && side <= ZEN_FIELD_MAX_SIDE && (side & (side - 1u)) == 0u
                            && all(greaterThanEqual(atlas, uvec3(8))) && all(equal(atlas % 8u, uvec3(0)))
                            && distanceField.dimensions.y <= capacity && distanceField.gridMinCellSize.w > 0.0 && fieldBuild.limits.y > 0u
                            && fieldBuild.limits.y <= ZEN_FIELD_MAX_CELL_REFS && fieldBuild.triangleRange.y > 0u
                            && fieldBuild.triangleRange.y <= ZEN_FIELD_MAX_TRIANGLE_BATCH;
        fieldCounters[index] = valid ? 0u : ZEN_FIELD_FAILURE_SETTINGS;
    }
#elif defined(FIELD_PREPARE_SCHEDULE)
    if (FieldLinearInvocation() == 0u)
    {
        FieldAdvanceSchedule(fieldSchedule.settings.w);
        FieldPrepareSchedule();
    }
#elif defined(FIELD_ADVANCE_SCHEDULE)
    if (FieldLinearInvocation() == 0u)
    {
        FieldAdvanceSchedule(fieldSchedule.settings.x);
    }
#elif defined(FIELD_BATCH_CLEAR)
    uint index = FieldLinearInvocation();
    if (index < FieldCellCount())
    {
        fieldCounts[index]  = 0u;
        fieldCursors[index] = 0u;
    }
    if (index >= 2u && index < ZEN_FIELD_COUNTER_RETAINED
        && (index < ZEN_FIELD_COUNTER_FAILURE_SITE || index > ZEN_FIELD_COUNTER_FAILURE_CELL)
        && index != ZEN_FIELD_COUNTER_FAILURE_SAMPLE
        && (index != ZEN_FIELD_COUNTER_COVERAGE_COUNT || fieldControl[ZEN_FIELD_CONTROL_COVERAGE_RESUME] == 0u))
    {
        fieldCounters[index] = 0u;
    }
#elif defined(FIELD_JOBS)
    uint index = FieldLinearInvocation();
    if (index < FieldTriangleCount())
    {
        uint triangle = FieldTriangleFirst() + index;
        if (triangle < fieldBuild.triangleRange.z)
        {
            FieldCreateJobs(triangle);
        }
        else
        {
            FieldGeometryFail(ZEN_FIELD_GEOMETRY_TRIANGLE_RANGE, triangle, 0xffffffffu, 0xffffffffu);
        }
    }
#elif defined(FIELD_COVERAGE)
    uint index = FieldLinearInvocation();
    if (index < FieldWorkCount())
    {
        FieldContinueCoverage(FieldWorkFirst() + index);
    }
#elif defined(FIELD_PREPARE_JOBS)
    if (FieldLinearInvocation() == 0u)
    {
        uint count = fieldCounters[ZEN_FIELD_COUNTER_FAILURE] == 0u
                       ? min(fieldCounters[ZEN_FIELD_COUNTER_JOBS], fieldBuild.triangleRange.w)
                       : 0u;
        // On failure, no later direct-dispatch slice may read unwritten jobs.
        fieldCounters[ZEN_FIELD_COUNTER_JOBS]              = count;
        fieldCounters[ZEN_FIELD_COUNTER_JOB_DISPATCH]      = min(count, 65535u);
        fieldCounters[ZEN_FIELD_COUNTER_JOB_DISPATCH + 1u] = max(1u, (count + 65534u) / 65535u);
        fieldCounters[ZEN_FIELD_COUNTER_JOB_DISPATCH + 2u] = 1u;
    }
#elif defined(FIELD_COUNT) || defined(FIELD_COMPACT)
    uint group = FieldLinearGroup();
    uint job   = FieldWorkFirst() + group;
    if (group < FieldWorkCount() && job < min(fieldCounters[ZEN_FIELD_COUNTER_JOBS], fieldBuild.triangleRange.w))
    {
        FieldVisitJob(job);
    }
#elif defined(FIELD_SCAN)
    uint index        = FieldLinearInvocation();
    uint lane         = gl_LocalInvocationIndex;
    fieldPrefix[lane] = index < FieldCellCount() ? fieldCounts[index] : 0u;
    barrier();
    FieldExclusiveScan();
    if (index < FieldCellCount())
    {
        fieldOffsets[index] = fieldPrefix[lane];
    }
#elif defined(FIELD_SCAN_SUMS)
    uint count = (FieldCellCount() + ZEN_FIELD_SCAN_GROUP - 1u) / ZEN_FIELD_SCAN_GROUP;
    uint lane  = gl_LocalInvocationIndex;
    for (uint index = lane; index < ZEN_FIELD_MAX_SCAN_BLOCKS; index += ZEN_FIELD_SCAN_GROUP)
    {
        fieldPrefix[index] = index < count ? fieldScan[index] : 0u;
    }
    barrier();
    FieldExclusiveScan();
    for (uint index = lane; index < count; index += ZEN_FIELD_SCAN_GROUP)
    {
        fieldScan[index] = fieldPrefix[index];
    }
#elif defined(FIELD_ALLOCATE)
    uint index = FieldLinearInvocation();
    if (index < FieldCellCount())
    {
        uint count          = fieldCounts[index];
        uint offset         = fieldOffsets[index] + fieldScan[index / ZEN_FIELD_SCAN_GROUP];
        fieldOffsets[index] = offset;
        if (count > 0u)
        {
            bool valid = count <= fieldBuild.limits.y && count <= ZEN_FIELD_MAX_CELL_REFS && offset < fieldBuild.limits.x
                      && count <= fieldBuild.limits.x - min(offset, fieldBuild.limits.x);
            uint value = distanceFieldCells[index];
            uint fresh = 0u;
            if (valid && value == DISTANCE_FIELD_EMPTY)
            {
                uint brick   = atomicAdd(fieldCounters[ZEN_FIELD_COUNTER_ALLOCATED], 1u);
                bool partial = (fieldBuild.limits.z & ZEN_FIELD_BUILD_PARTIAL) != 0u;
                valid        = brick < (partial ? fieldCounters[ZEN_FIELD_COUNTER_FREE] : distanceField.dimensions.y);
                if (valid)
                {
                    brick = fieldFreeBricks[brick];
                    value = (brick + 1u) | DISTANCE_FIELD_NO_SURFACE;
                    fresh = 1u;
                }
                else
                {
                    FieldFail(ZEN_FIELD_FAILURE_ATLAS);
                }
            }
            valid = valid && value != DISTANCE_FIELD_UNAVAILABLE;
            if (valid)
            {
                // A dense cell takes one emit workgroup per reference chunk. The workspace holds a
                // split slot and active entries for every chunk the reference capacity allows.
                uint chunks = (count + ZEN_FIELD_EMIT_CHUNK_REFERENCES - 1u) / ZEN_FIELD_EMIT_CHUNK_REFERENCES;
                uint split  = 0u;
                if (chunks > 1u)
                {
                    uint slot = atomicAdd(fieldCounters[ZEN_FIELD_COUNTER_SPLIT_CELLS], 1u);
                    if (slot < uint(fieldEmitSplit.length()) / ZEN_FIELD_EMIT_SPLIT_WORDS)
                    {
                        uint base = slot * ZEN_FIELD_EMIT_SPLIT_WORDS;
                        for (uint texel = 0u; texel < 512u; ++texel)
                        {
                            fieldEmitSplit[base + texel] = floatBitsToUint(1.0);
                        }
                        fieldEmitSplit[base + 512u] = 0u;
                        split                       = slot + 1u;
                    }
                    else
                    {
                        chunks = 1u;
                    }
                }
                uint activeSlot = atomicAdd(fieldCounters[ZEN_FIELD_COUNTER_ACTIVE], chunks);
                if (activeSlot + chunks > uint(fieldActive.length()))
                {
                    // A missing chunk would leave its cell unfinished.
                    FieldFail(ZEN_FIELD_FAILURE_REFERENCES);
                }
                for (uint chunk = 0u; chunk < chunks && activeSlot + chunk < uint(fieldActive.length()); ++chunk)
                {
                    fieldActive[activeSlot + chunk] = uvec2(index, fresh | (chunk << 1u) | (split << 8u));
                }
                distanceFieldCells[index] = value;
            }
            else
            {
                distanceFieldCells[index] = DISTANCE_FIELD_UNAVAILABLE;
            }
        }
    }
#elif defined(FIELD_RETAIN)
    uint index = FieldLinearInvocation();
    if (index < FieldCellCount() && !FieldDirtyCell(FieldCellCoordinate(index)))
    {
        uint value                = fieldPreviousCells[index];
        distanceFieldCells[index] = value;
        value                     = value == DISTANCE_FIELD_UNAVAILABLE ? value : value & DISTANCE_FIELD_BRICK_MASK;
        if (value == DISTANCE_FIELD_UNAVAILABLE)
        {
            atomicAdd(fieldCounters[ZEN_FIELD_COUNTER_UNAVAILABLE], 1u);
        }
        if (value != DISTANCE_FIELD_EMPTY && value != DISTANCE_FIELD_UNAVAILABLE && value <= distanceField.dimensions.y)
        {
            fieldBrickUsed[value - 1u] = 1u;
            atomicAdd(fieldCounters[ZEN_FIELD_COUNTER_RETAINED], 1u);
        }
    }
#elif defined(FIELD_RECYCLE)
    uint index = FieldLinearInvocation();
    if (index < distanceField.dimensions.y && fieldBrickUsed[index] == 0u)
    {
        uint freeIndex             = atomicAdd(fieldCounters[ZEN_FIELD_COUNTER_FREE], 1u);
        fieldFreeBricks[freeIndex] = index;
    }
#elif defined(FIELD_EMIT)
    uint group       = FieldLinearGroup();
    uint activeIndex = FieldWorkFirst() + group;
    if (group < FieldWorkCount() && activeIndex < fieldCounters[ZEN_FIELD_COUNTER_ACTIVE]
        && activeIndex < uint(fieldActive.length()))
    {
        FieldEmit(activeIndex);
    }
#elif defined(FIELD_HIERARCHY)
    uint index = FieldLinearInvocation();
    uint side  = FieldHierarchySide();
    if (index < side * side * side)
    {
        uint value = 0u;
        if (side == distanceField.dimensions.x)
        {
            uint cell = distanceFieldCells[index];
            value =
                cell == DISTANCE_FIELD_UNAVAILABLE
                    ? DISTANCE_FIELD_UNAVAILABLE_BIT
                    : (cell == DISTANCE_FIELD_EMPTY || (cell & DISTANCE_FIELD_NO_SURFACE) != 0u ? 0u
                                                                                                : DISTANCE_FIELD_OCCUPIED_BIT);
        }
        else
        {
            uvec3 coordinate = uvec3(index % side, (index / side) % side, index / (side * side));
            for (uint child = 0u; child < 8u; ++child)
            {
                uvec3 delta   = uvec3(child & 1u, (child >> 1u) & 1u, (child >> 2u) & 1u);
                uint  source  = FieldChildOffset() + DistanceFieldIndex(coordinate * 2u + delta, side * 2u);
                value        |= distanceFieldHierarchy[source];
            }
        }
        distanceFieldHierarchy[FieldParentOffset() + index] = value;
    }
#endif
}

void main()
{
#if defined(FIELD_BATCH_CLEAR) || defined(FIELD_JOBS) || defined(FIELD_COVERAGE) || defined(FIELD_COUNT)    \
    || defined(FIELD_SCAN) || defined(FIELD_SCAN_SUMS) || defined(FIELD_ALLOCATE) || defined(FIELD_COMPACT) \
    || defined(FIELD_EMIT) || defined(FIELD_HIERARCHY)
    // Scheduled work: an idle slice launches one group instead of an empty grid (distance_field_schedule.glsl).
    if (fieldControl[ZEN_FIELD_CONTROL_WORK_IDLE] == 0u)
    {
        FieldMain();
    }
#else
    FieldMain();
#endif
}
