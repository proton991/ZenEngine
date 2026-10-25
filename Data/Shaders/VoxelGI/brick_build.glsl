// Brick scene construction (BrickScene.cpp). One entry point per BRICK_* define:
// clear, mark, finalize, mark large, allocate, rasterize, rasterize large, prune, distance x/y/z.
#define BRICK_BUILD
#include "brick_common.glsl"
#if defined(BRICK_RASTERIZE) || defined(BRICK_RASTERIZE_LARGE)
#    include "scene_ray_geometry.glsl"
#endif

struct BrickTriangle
{
    vec4  a;
    vec4  b;
    vec4  c;
    uvec4 record; // index offset, node, material, enabled class (SoftwareSceneRayQuery)
};
layout(std430, set = 4, binding = 3) readonly buffer BrickTriangles
{
    BrickTriangle brickTriangles[];
};
#ifdef BRICK_CLEAR
#    define BRICK_COUNTER_ACCESS writeonly
#else
#    define BRICK_COUNTER_ACCESS
#endif
layout(std430, set = 4, binding = 4) BRICK_COUNTER_ACCESS buffer BrickCounters
{
    uint largeGroupsX;
    uint largeGroupsY;
    uint largeGroupsZ;
    uint largeCount;
    uint allocatedBricks;
    uint brickOverflow;
    uint counterReserved0;
    uint counterReserved1;
};
layout(std430, set = 4, binding = 5) BRICK_STORAGE_ACCESS buffer BrickLargeTriangles
{
    uint brickLargeTriangles[];
};
#if defined(BRICK_DISTANCE_X) || defined(BRICK_DISTANCE_Y) || defined(BRICK_DISTANCE_Z)
#    define BRICK_DISTANCE
#    if defined(BRICK_DISTANCE_X)
const int BRICK_DISTANCE_AXIS = 0;
#    elif defined(BRICK_DISTANCE_Y)
const int BRICK_DISTANCE_AXIS = 1;
#    else
const int BRICK_DISTANCE_AXIS = 2;
#    endif
#endif
// The x pass reads occupancy and the z pass writes the cells, so each declares only what it uses.
#if defined(BRICK_DISTANCE_Y) || defined(BRICK_DISTANCE_Z)
layout(std430, set = 4, binding = 6) readonly buffer BrickDistanceInput
{
    uint distanceInput[];
};
#endif
#if defined(BRICK_DISTANCE_X) || defined(BRICK_DISTANCE_Y)
layout(std430, set = 4, binding = 7) writeonly buffer BrickDistanceOutput
{
    uint distanceOutput[];
};
#endif

#if defined(BRICK_CLEAR) || defined(BRICK_ALLOCATE) || defined(BRICK_PRUNE) || defined(BRICK_DISTANCE)
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;
#else
layout(local_size_x = 64) in;
#endif

// Separating-axis triangle/box overlap (Akenine-Moller, "Fast 3D Triangle-Box Overlap Testing").
bool BrickPlaneBox(vec3 normal, vec3 vertex, vec3 halfSize)
{
    bvec3 positive = greaterThan(normal, vec3(0));
    vec3  lower    = mix(halfSize, -halfSize, positive) - vertex;
    vec3  upper    = mix(-halfSize, halfSize, positive) - vertex;
    return dot(normal, lower) <= 0.0 && dot(normal, upper) >= 0.0;
}

bool BrickTriangleBox(vec3 center, vec3 halfSize, vec3 a, vec3 b, vec3 c)
{
    vec3 v0 = a - center, v1 = b - center, v2 = c - center;
    vec3 edges[3] = vec3[3](v1 - v0, v2 - v1, v0 - v2);
    bool overlap = all(lessThanEqual(min(v0, min(v1, v2)), halfSize)) && all(greaterThanEqual(max(v0, max(v1, v2)), -halfSize));
    for (int i = 0; i < 3 && overlap; ++i)
    {
        vec3 e = edges[i];
        for (int k = 0; k < 3 && overlap; ++k)
        {
            vec3  axis = k == 0 ? vec3(0, -e.z, e.y) : k == 1 ? vec3(e.z, 0, -e.x) : vec3(-e.y, e.x, 0);
            float p0 = dot(axis, v0), p1 = dot(axis, v1), p2 = dot(axis, v2);
            float radius = dot(halfSize, abs(axis));
            overlap      = !(min(p0, min(p1, p2)) > radius || max(p0, max(p1, p2)) < -radius);
        }
    }
    return overlap && BrickPlaneBox(cross(edges[0], edges[1]), v0, halfSize);
}

uint BrickTriangleCount()
{
    return brick.dimensions.z;
}

// A triangle in cell units, its overlapped cell range, and whether it touches the grid at all.
bool BrickTriangleCells(uint triangle, out vec3 a, out vec3 b, out vec3 c, out ivec3 lower, out ivec3 upper)
{
    BrickTriangle source = brickTriangles[triangle];
    float         scale  = 1.0 / brick.gridMinCellSize.w;
    a                    = (source.a.xyz - brick.gridMinCellSize.xyz) * scale;
    b                    = (source.b.xyz - brick.gridMinCellSize.xyz) * scale;
    c                    = (source.c.xyz - brick.gridMinCellSize.xyz) * scale;
    // A small margin covers CPU/GPU transform rounding on cell boundaries.
    vec3 minimum = min(a, min(b, c)) - 1e-3;
    vec3 maximum = max(a, max(b, c)) + 1e-3;
    lower        = ivec3(floor(minimum));
    upper        = ivec3(floor(maximum));
    bool inside  = all(greaterThanEqual(upper, ivec3(0))) && all(lessThan(lower, ivec3(BrickResolution())));
    lower        = clamp(lower, ivec3(0), ivec3(BrickResolution() - 1));
    upper        = clamp(upper, ivec3(0), ivec3(BrickResolution() - 1));
    return inside;
}

uint BrickRangeCount(ivec3 lower, ivec3 upper)
{
    uvec3 extent = uvec3(upper - lower + 1);
    return extent.x * extent.y * extent.z;
}

ivec3 BrickRangeCell(ivec3 lower, ivec3 upper, uint index)
{
    uvec3 extent = uvec3(upper - lower + 1);
    return lower + ivec3(index % extent.x, (index / extent.x) % extent.y, index / (extent.x * extent.y));
}

bool BrickCellOverlaps(ivec3 cell, vec3 a, vec3 b, vec3 c)
{
    return BrickTriangleBox(vec3(cell) + 0.5, vec3(0.5 + 1e-3), a, b, c);
}

uint BrickLinearGroup()
{
    return gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
}

#if defined(BRICK_RASTERIZE) || defined(BRICK_RASTERIZE_LARGE)
// Barycentric weights of the triangle point closest to p (Ericson, "Real-Time Collision Detection" 5.1.5).
vec3 BrickClosestBarycentric(vec3 p, vec3 a, vec3 b, vec3 c)
{
    vec3  ab = b - a, ac = c - a, ap = p - a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    vec3  bp = p - b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    vec3  cp = p - c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    float va = d3 * d6 - d5 * d4, vb = d5 * d2 - d1 * d6, vc = d1 * d4 - d3 * d2;
    vec3  result;
    if (d1 <= 0.0 && d2 <= 0.0)
    {
        result = vec3(1, 0, 0);
    }
    else if (d3 >= 0.0 && d4 <= d3)
    {
        result = vec3(0, 1, 0);
    }
    else if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
    {
        float v = d1 / max(d1 - d3, 1e-30);
        result  = vec3(1.0 - v, v, 0);
    }
    else if (d6 >= 0.0 && d5 <= d6)
    {
        result = vec3(0, 0, 1);
    }
    else if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
    {
        float w = d2 / max(d2 - d6, 1e-30);
        result  = vec3(1.0 - w, 0, w);
    }
    else if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0)
    {
        float w = (d4 - d3) / max((d4 - d3) + (d5 - d6), 1e-30);
        result  = vec3(0, 1.0 - w, w);
    }
    else
    {
        float denominator = 1.0 / max(va + vb + vc, 1e-30);
        float v = vb * denominator, w = vc * denominator;
        result = vec3(1.0 - v - w, v, w);
    }
    return result;
}

// Sets the bits of the fine voxels a triangle overlaps inside one cell. Masked materials keep
// a voxel only where the triangle point nearest its centre passes the alpha test.
void BrickRasterizeCell(uint triangle, ivec3 cell, vec3 a, vec3 b, vec3 c)
{
    uint value = brickCells[BrickCellIndex(cell)];
    if (BrickCellHasBrick(value))
    {
        uint  base = (value - 1u) * ZEN_BRICK_WORDS;
        float size = float(ZEN_BRICK_SIZE);
        vec3  fa = a * size, fb = b * size, fc = c * size;
        ivec3 origin = cell * int(ZEN_BRICK_SIZE);
        ivec3 lower  = clamp(ivec3(floor(min(fa, min(fb, fc)) - 1e-3)), origin, origin + int(ZEN_BRICK_SIZE) - 1);
        ivec3 upper  = clamp(ivec3(floor(max(fa, max(fb, fc)) + 1e-3)), origin, origin + int(ZEN_BRICK_SIZE) - 1);
        uvec4 record = brickTriangles[triangle].record;
        for (int z = lower.z; z <= upper.z; ++z)
        {
            for (int y = lower.y; y <= upper.y; ++y)
            {
                for (int x = lower.x; x <= upper.x; ++x)
                {
                    vec3 center = vec3(x, y, z) + 0.5;
                    if (BrickTriangleBox(center, vec3(0.5 + 1e-3), fa, fb, fc))
                    {
                        vec3 weights = BrickClosestBarycentric(center, fa, fb, fc);
                        if (RayCandidateAccepted(record, weights.yz))
                        {
                            uint bit = BrickBitIndex(ivec3(x, y, z) - origin);
                            atomicOr(brickBits[base + bit / 32u], 1u << (bit % 32u));
                        }
                    }
                }
            }
        }
    }
}
#endif

#if defined(BRICK_MARK) || defined(BRICK_MARK_LARGE) || defined(BRICK_RASTERIZE) || defined(BRICK_RASTERIZE_LARGE)
void BrickProcessCell(uint triangle, ivec3 cell, vec3 a, vec3 b, vec3 c)
{
    if (BrickCellOverlaps(cell, a, b, c))
    {
#    if defined(BRICK_MARK) || defined(BRICK_MARK_LARGE)
        // Several triangles may mark the same cell in this dispatch.
        atomicExchange(brickCells[BrickCellIndex(cell)], 1u);
#    else
        BrickRasterizeCell(triangle, cell, a, b, c);
#    endif
    }
}
#endif

#if defined(BRICK_CLEAR)
void main()
{
    ivec3 cell = ivec3(gl_GlobalInvocationID);
    if (BrickCellInside(cell))
    {
        brickCells[BrickCellIndex(cell)] = 0u;
    }
    // Every word of the bit, mask and large-triangle storage is cleared, a few per invocation, so
    // the render graph knows their contents; the build reads only the parts it writes later.
    uvec3 invocations = gl_NumWorkGroups * gl_WorkGroupSize;
    uint  invocation =
        (gl_GlobalInvocationID.z * invocations.y + gl_GlobalInvocationID.y) * invocations.x + gl_GlobalInvocationID.x;
    uint stride = invocations.x * invocations.y * invocations.z;
    for (uint word = invocation; word < uint(brickBits.length()); word += stride)
    {
        brickBits[word] = 0u;
    }
    for (uint word = invocation; word < uint(brickMasks.length()); word += stride)
    {
        brickMasks[word] = 0u;
    }
    for (uint word = invocation; word < uint(brickLargeTriangles.length()); word += stride)
    {
        brickLargeTriangles[word] = 0u;
    }
    if (all(equal(gl_GlobalInvocationID, uvec3(0))))
    {
        largeGroupsX     = 0u;
        largeGroupsY     = 1u;
        largeGroupsZ     = 1u;
        largeCount       = 0u;
        allocatedBricks  = 0u;
        brickOverflow    = 0u;
        counterReserved0 = 0u;
        counterReserved1 = 0u;
    }
}
#elif defined(BRICK_MARK) || defined(BRICK_RASTERIZE)
void main()
{
    uint  triangle = BrickLinearGroup() * 64u + gl_LocalInvocationID.x;
    vec3  a, b, c;
    ivec3 lower, upper;
    if (triangle < BrickTriangleCount() && BrickTriangleCells(triangle, a, b, c, lower, upper))
    {
        uint count = BrickRangeCount(lower, upper);
        if (count > ZEN_BRICK_LARGE_CELLS)
        {
#    ifdef BRICK_MARK
            // The rasterize pass skips large triangles too; the large passes cover both stages.
            brickLargeTriangles[atomicAdd(largeCount, 1u)] = triangle;
#    endif
        }
        else
        {
            for (uint i = 0u; i < count; ++i)
            {
                BrickProcessCell(triangle, BrickRangeCell(lower, upper, i), a, b, c);
            }
        }
    }
}
#elif defined(BRICK_FINALIZE)
void main()
{
    if (gl_GlobalInvocationID.x == 0u)
    {
        largeGroupsX = min(largeCount, ZEN_BRICK_LARGE_GROUPS_X);
        largeGroupsY = (largeCount + ZEN_BRICK_LARGE_GROUPS_X - 1u) / ZEN_BRICK_LARGE_GROUPS_X;
        largeGroupsZ = 1u;
    }
}
#elif defined(BRICK_MARK_LARGE) || defined(BRICK_RASTERIZE_LARGE)
void main()
{
    uint entry = gl_WorkGroupID.y * ZEN_BRICK_LARGE_GROUPS_X + gl_WorkGroupID.x;
    if (entry < largeCount)
    {
        uint  triangle = brickLargeTriangles[entry];
        vec3  a, b, c;
        ivec3 lower, upper;
        BrickTriangleCells(triangle, a, b, c, lower, upper);
        uint count = BrickRangeCount(lower, upper);
        for (uint i = gl_LocalInvocationID.x; i < count; i += 64u)
        {
            BrickProcessCell(triangle, BrickRangeCell(lower, upper, i), a, b, c);
        }
    }
}
#elif defined(BRICK_ALLOCATE)
void main()
{
    ivec3 cell = ivec3(gl_GlobalInvocationID);
    if (BrickCellInside(cell))
    {
        uint index = BrickCellIndex(cell);
        if (brickCells[index] != 0u)
        {
            uint allocated = atomicAdd(allocatedBricks, 1u);
            if (allocated < brick.dimensions.y)
            {
                brickCells[index] = allocated + 1u;
                for (uint word = 0u; word < ZEN_BRICK_WORDS; ++word)
                {
                    brickBits[allocated * ZEN_BRICK_WORDS + word] = 0u;
                }
            }
            else
            {
                // Conservative: an unbacked occupied cell blocks every ray through it.
                brickCells[index] = ZEN_BRICK_SOLID;
                atomicExchange(brickOverflow, 1u);
            }
        }
    }
}
#elif defined(BRICK_PRUNE)
void main()
{
    // Cells marked only by alpha-rejected voxels own empty bricks; let rays skip them. Kept bricks
    // record which of their 4x4x4 blocks hold voxels.
    ivec3 cell = ivec3(gl_GlobalInvocationID);
    if (BrickCellInside(cell))
    {
        uint index = BrickCellIndex(cell);
        uint value = brickCells[index];
        if (BrickCellHasBrick(value))
        {
            uint bits = 0u, mask = 0u;
            for (uint word = 0u; word < ZEN_BRICK_WORDS; ++word)
            {
                uint content  = brickBits[(value - 1u) * ZEN_BRICK_WORDS + word];
                bits         |= content;
                // Word w holds rows y = 4 * (w & 1) .. +3 of slice z = w / 2, one byte per row with
                // x in bit order: low nibbles are block x 0, high nibbles block x 1.
                uint block  = ((word >> 3u) << 2u) | ((word & 1u) << 1u);
                mask       |= (content & 0x0f0f0f0fu) != 0u ? 1u << block : 0u;
                mask       |= (content & 0xf0f0f0f0u) != 0u ? 1u << (block | 1u) : 0u;
            }
            brickCells[index] = bits != 0u ? value : 0u;
            if (bits != 0u)
            {
                brickMasks[value - 1u] = mask;
            }
        }
    }
}
#elif defined(BRICK_DISTANCE)
// Separable Chebyshev distance to the nearest occupied cell, one axis per pass:
// d1 along x from occupancy, then min over offsets o of max(|o|, d_previous) along y and z.
uint DistanceSource(ivec3 cell)
{
    uint result = ZEN_BRICK_MAX_DISTANCE + 1u;
    if (BrickCellInside(cell))
    {
        uint index = BrickCellIndex(cell);
#    if defined(BRICK_DISTANCE_X)
        result = brickCells[index] != 0u ? 0u : ZEN_BRICK_MAX_DISTANCE + 1u;
#    else
        result = distanceInput[index];
#    endif
    }
    return result;
}
void main()
{
    ivec3 cell = ivec3(gl_GlobalInvocationID);
    if (BrickCellInside(cell))
    {
        uint best = DistanceSource(cell);
        for (int offset = 1; offset <= int(ZEN_BRICK_MAX_DISTANCE) && uint(offset) < best; ++offset)
        {
            ivec3 delta                = ivec3(0);
            delta[BRICK_DISTANCE_AXIS] = offset;
            uint candidate             = max(uint(offset), min(DistanceSource(cell + delta), DistanceSource(cell - delta)));
            best                       = min(best, candidate);
        }
        uint index = BrickCellIndex(cell);
#    if defined(BRICK_DISTANCE_Z)
        if (brickCells[index] == 0u)
        {
            brickCells[index] = ZEN_BRICK_EMPTY_BIT | min(best, ZEN_BRICK_MAX_DISTANCE);
        }
#    else
        distanceOutput[index] = best;
#    endif
    }
}
#endif
