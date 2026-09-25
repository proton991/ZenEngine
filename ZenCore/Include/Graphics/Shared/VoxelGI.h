#ifndef ZEN_VOXEL_GI_H
#define ZEN_VOXEL_GI_H

// Safe shader defaults, specialized per device by the volume workgroup policy.
#define ZEN_VOXEL_VOLUME_GROUP_SIZE 4
#define ZEN_VOXEL_VOLUME_GROUP_X_ID 0
#define ZEN_VOXEL_VOLUME_GROUP_Y_ID 1
#define ZEN_VOXEL_VOLUME_GROUP_Z_ID 2

// Round each bounded linear contribution to nearest (ties upward).
// UINT32_MAX / scale bounds the admitted triangle records before accumulation.
#define ZEN_VOXEL_REFLECTANCE_SCALE 4095u

#ifdef __cplusplus
#    include "Math/Math.h"
namespace zen::rc
{
struct VoxelGridUniform
{
    Vec4 minimumSize;
    glm::uvec4 selection; // Static/dynamic/all class mask; other words reserved.
};
} // namespace zen::rc
#endif

#endif // ZEN_VOXEL_GI_H
