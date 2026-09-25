#ifndef ZEN_GI_VISIBILITY_H
#define ZEN_GI_VISIBILITY_H

#define GI_UNKNOWN          0u
#define GI_MISS             1u
#define GI_HIT              2u
#define GI_STATIC           1u
#define GI_DYNAMIC          2u
#define GI_ALL              3u
#define GI_CELL_PRECISION   1u
#define GI_SURFACE_VALID    2u
#define GI_QUERY_GROUP_SIZE 64u
#define GI_INVALID_CELL     0xffffffffu

#ifdef __cplusplus
#    include "Math/Math.h"
namespace zen::rc
{
using vec4  = glm::vec4;
using uvec4 = glm::uvec4;
#endif

struct GIQuery
{
    vec4 originMin;    // World origin, inclusive tMin >= 0.
    vec4 directionMax; // Unit world direction, exclusive finite tMax.
    uvec4 source;      // Class mask, source class (zero disables hint), Z-fast cell ID, reserved.
};

struct GIHit
{
    vec4 positionDistance;
    vec4 normal;
    vec4 baseColorMetallic;
    vec4 emission;
    vec4 diffuseReflectance;
    uvec4 identity; // Status, class, Z-fast cell ID, precision/surface validity flags.
};

struct GIQueryResult
{
    GIHit closest;
    uvec4 occlusion; // Status; optional diagnostic closest/occlusion cell visits, reserved.
};

struct GIGridUniform
{
    vec4 minimumCellSize;
    uvec4
        dimensions; // Cubic side, complete-class mask, diagnostic step limit, query visit counters.
    uvec4 averaged;   // Static/dynamic effective reflectance present, reserved, reserved.
};

#ifdef __cplusplus
static_assert(sizeof(GIQuery) == 48);
static_assert(sizeof(GIHit) == 96);
static_assert(sizeof(GIQueryResult) == 112);
static_assert(sizeof(GIGridUniform) == 48);
} // namespace zen::rc
#endif
#endif
