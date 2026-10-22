#ifndef ZEN_VOXEL_GI_H
#define ZEN_VOXEL_GI_H

// Scene mobility classes used by voxelization and coverage checks.
#define GI_STATIC  1u
#define GI_DYNAMIC 2u
#define GI_ALL     3u

// Safe shader defaults, specialized per device by the volume workgroup policy.
#define ZEN_VOXEL_VOLUME_GROUP_SIZE 4
#define ZEN_VOXEL_VOLUME_GROUP_X_ID 0
#define ZEN_VOXEL_VOLUME_GROUP_Y_ID 1
#define ZEN_VOXEL_VOLUME_GROUP_Z_ID 2

// Round each bounded linear contribution to nearest (ties upward).
// UINT32_MAX / scale bounds the admitted triangle records before accumulation.
#define ZEN_VOXEL_REFLECTANCE_SCALE 4095u

// Surface orientation classes per cell: the dominant axis of the oriented geometric normal and its
// sign (voxel_surface_class.glsl). Class volumes stack the classes along z.
#define ZEN_VOXEL_SURFACE_CLASSES 6u
// The class owners and the light-visibility cache use at most this grid resolution (coarser at 256).
#define ZEN_LIGHT_CACHE_MAX_RESOLUTION 128u

#ifdef __cplusplus
#    include "Math/Math.h"
namespace zen::rc
{
struct VoxelGridUniform
{
    Vec4       minimumSize;
    glm::uvec4 selection; // x: static/dynamic/all class mask; y: nonzero to elect class owners; zw reserved.
};
} // namespace zen::rc
#endif

#endif // ZEN_VOXEL_GI_H
