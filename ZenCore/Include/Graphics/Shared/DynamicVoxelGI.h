#ifndef ZEN_DYNAMIC_VOXEL_GI_H
#define ZEN_DYNAMIC_VOXEL_GI_H
#include "Graphics/Shared/GIVisibility.h"

#define GI_FACE_COUNT         6u
#define GI_FACE_RAYS          128u
#define GI_STATIC_OVERFLOW    1u
#define GI_STATIC_UNKNOWN     2u
#define GI_STATIC_UNSUPPORTED 4u
#define GI_CACHE_PENDING      8u
#define GI_CACHE_BATCH        4096u
#define GI_FRAME_CELL_BYTES   488u
#define GI_FRAME_FIXED_BYTES  48u
#define GI_COMPACT_HIT_BYTES  8u

#ifdef __cplusplus
namespace zen::rc
{
#endif
struct GIStaticUniform
{
    vec4 minimumCellSize;
    uvec4 volume;  // Side, compact static capacity, dynamic neighbor radius, enabled lights.
    vec4 lighting; // Indirect gain, shadows enabled, finite ray length, ray epsilon.
    uvec4 sampling; // Rays per face (32/64/128), reserved.
};
struct GIWorkUniform
{
    uvec4 limits; // Device dispatch XYZ limits, allocated indirect command count.
    uvec4
        state; // Cached receiver end, batch start, camera selection enabled, dynamic inputs present.
    uvec4 cache; // Compact DDA records enabled, reserved.
};
struct GIFilterUniform
{
    vec4 timing; // Seconds since initialization, cooldown/maximum history gap, alpha, reference Hz.
    uvec4 control; // Temporal mode (off/fixed/elapsed), spatial enabled, reset, reserved.
};
struct GILightingUniform
{
    uvec4 enabled; // Analytic lighting, environment, emissive senders, updated light-mask bits.
};
#ifdef __cplusplus
static_assert(sizeof(GILightingUniform) == 16);
static_assert(sizeof(GIFilterUniform) == 32);
static_assert(sizeof(GIWorkUniform) == 48);
static_assert(sizeof(GIStaticUniform) == 64);
} // namespace zen::rc
#endif
#endif
