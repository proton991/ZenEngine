#ifndef ZEN_DISTANCE_FIELD_COMMON
#define ZEN_DISTANCE_FIELD_COMMON

// One visibility level covers all committed scene occluders, independently of the radiance cache.
// A published level certifies that the space outside its bounds is empty. Unknown cells inside
// those bounds must remain UNAVAILABLE until their construction and hierarchy updates complete.
const uint DISTANCE_FIELD_EMPTY       = 0u;
const uint DISTANCE_FIELD_UNAVAILABLE = 0xffffffffu;
// The brick remains readable for gradients, but its samples cannot meet the trace hit band.
const uint DISTANCE_FIELD_NO_SURFACE        = 0x80000000u;
const uint DISTANCE_FIELD_BRICK_MASK        = 0x7fffffffu;
const uint DISTANCE_FIELD_OCCUPIED_BIT      = 1u;
const uint DISTANCE_FIELD_UNAVAILABLE_BIT   = 2u;
const uint DISTANCE_FIELD_COVERED_MISS      = 0u;
const uint DISTANCE_FIELD_HIT               = 1u;
const uint DISTANCE_FIELD_TRACE_UNAVAILABLE = 2u;
const uint DISTANCE_FIELD_FAILURE_NONE      = 0u;
const uint DISTANCE_FIELD_FAILURE_QUERY     = 1u;
const uint DISTANCE_FIELD_FAILURE_WORK      = 2u;
const uint DISTANCE_FIELD_FAILURE_MISSING   = 3u;
const uint DISTANCE_FIELD_FAILURE_PROGRESS  = 4u;
const uint DISTANCE_FIELD_FAILURE_STORAGE   = 5u;
const uint DISTANCE_FIELD_FAILURE_BRICK     = 6u;
// First set-4 binding of the field. Construction uses 0; GI traces move it past the scene shadow maps.
#ifndef DISTANCE_FIELD_BINDING
#    define DISTANCE_FIELD_BINDING 0
#endif

layout(std140, set = 4, binding = DISTANCE_FIELD_BINDING) uniform uDistanceFieldData
{
    vec4  gridMinCellSize; // xyz visibility minimum, w brick world extent
    uvec4 dimensions;      // x power-of-two cells/axis, y atlas capacity, z hierarchy levels including leaves, w trace budget
    uvec4 atlasDimensions; // xyz texel extents, multiples of eight; w reserved
    vec4  tracing;         // x hit tolerance in brick units, y per-brick sample limit; zw reserved
}
distanceField;
#ifdef DISTANCE_FIELD_BUILD
#    ifdef FIELD_CLEAR
#        define DISTANCE_FIELD_STORAGE_ACCESS writeonly
#    else
#        define DISTANCE_FIELD_STORAGE_ACCESS
#    endif
#else
#    define DISTANCE_FIELD_STORAGE_ACCESS readonly
#endif
layout(std430, set = 4, binding = DISTANCE_FIELD_BINDING + 1) DISTANCE_FIELD_STORAGE_ACCESS buffer DistanceFieldCells
{
    uint distanceFieldCells[]; // 0 empty, UINT_MAX unavailable, otherwise brick index + 1 and optional NO_SURFACE bit
};
layout(std430, set = 4, binding = DISTANCE_FIELD_BINDING + 2) DISTANCE_FIELD_STORAGE_ACCESS buffer DistanceFieldHierarchy
{
    // Fine-to-coarse mip pyramid, leaves first. Each parent ORs its eight children's flags.
    uint distanceFieldHierarchy[];
};
#undef DISTANCE_FIELD_STORAGE_ACCESS
#ifdef DISTANCE_FIELD_BUILD
layout(r8, set = 4, binding = DISTANCE_FIELD_BINDING + 3) uniform image3D distanceFieldBuildAtlas;
#else
layout(set = 4, binding = DISTANCE_FIELD_BINDING + 3) uniform sampler3D distanceFieldAtlas;
#endif

struct DistanceFieldTraceResult
{
    uint  status;
    float distance;
    vec3  position;
    vec3  normal; // A zero gradient has no valid normal; never substitute a cell face.
    uint  brick;
    uint  iterations;      // Hierarchy/map loads and all distance texture samples, including normal probes.
    uint  distanceSamples; // Primary samples and four samples per evaluated normal probe.
    uint  failureReason;   // Diagnostic only; zero for a completed hit or covered miss.
};

uint DistanceFieldIndex(uvec3 coordinate, uint side)
{
    return coordinate.x + side * (coordinate.y + side * coordinate.z);
}

uint DistanceFieldHierarchyOffset(uint level)
{
    uint offset = 0u;
    uint side   = distanceField.dimensions.x;
    for (uint index = 0u; index < level; ++index)
    {
        offset  += side * side * side;
        side   >>= 1u;
    }
    return offset;
}
#endif
