#ifdef AVERAGED_REFLECTANCE
// Keep contribution evaluation consistent across fragment and compute stages.
#    define VOXEL_PRECISE_CONTRIBUTION
#endif
#include "voxel_surface.glsl"
#include "voxel_surface_class.glsl"
#include "Graphics/Shared/VoxelGI.h"
layout(set = 4, binding = 0, r32ui) uniform uimage3D voxelOwner;
#ifdef AVERAGED_REFLECTANCE
layout(set = 4, binding = 4, std430) buffer ReflectanceSums
{
    uvec4 reflectanceSums[];
};
#endif
// Per-class owners for the light-visibility cache; written only when gridSelection.y is set.
layout(set = 4, binding = 6, r32ui) uniform uimage3D voxelClassOwner;
// A triangle's class: its winding normal turned toward the authored vertex normal, as injection orients it.
int TriangleSurfaceClass(uint record, Vertex a, vec3 windingNormal)
{
    vec3 reference = mat3(nodesData[triangles[record].y].normalMatrix) * a.normal.xyz;
    return VoxelSurfaceClass(dot(windingNormal, reference) < 0.0 ? -windingNormal : windingNormal);
}
void AccumulateVoxel(uint record, ivec3 cell, int surfaceClass)
{
    VOXEL_MATERIAL_PRECISION vec3 position = gridMinSize.xyz + (vec3(cell) + 0.5) * gridMinSize.w;
    vec3                          reflectance;
    if (SurfaceVisible(record, position, reflectance))
    {
        imageAtomicMin(voxelOwner, cell, record);
        if (gridSelection.y != 0u)
        {
            int resolution = imageSize(voxelClassOwner).x;
            imageAtomicMin(voxelClassOwner,
                           VoxelClassTexel(cell * resolution / imageSize(voxelOwner).x, surfaceClass, resolution), record);
        }
#ifdef AVERAGED_REFLECTANCE
        ivec3 size    = imageSize(voxelOwner);
        uint  index   = uint(cell.x + size.x * (cell.y + size.y * cell.z));
        uvec3 encoded = uvec3(floor(reflectance * float(ZEN_VOXEL_REFLECTANCE_SCALE) + 0.5));
        atomicAdd(reflectanceSums[index].x, encoded.x);
        atomicAdd(reflectanceSums[index].y, encoded.y);
        atomicAdd(reflectanceSums[index].z, encoded.z);
        atomicAdd(reflectanceSums[index].w, 1u);
#endif
    }
}
