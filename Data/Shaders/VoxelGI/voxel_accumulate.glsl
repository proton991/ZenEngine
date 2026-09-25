#ifdef AVERAGED_REFLECTANCE
// Keep contribution evaluation consistent across fragment and compute stages.
#define VOXEL_PRECISE_CONTRIBUTION
#endif
#include "voxel_surface.glsl"
#include "Graphics/Shared/VoxelGI.h"
layout(set=4,binding=0,r32ui) uniform uimage3D voxelOwner;
#ifdef AVERAGED_REFLECTANCE
layout(set=4,binding=4,std430) buffer ReflectanceSums { uvec4 reflectanceSums[]; };
#endif
void AccumulateVoxel(uint record, ivec3 cell)
{
    VOXEL_MATERIAL_PRECISION vec3 position=gridMinSize.xyz+(vec3(cell)+0.5)*gridMinSize.w;
    vec3 reflectance;
    if(SurfaceVisible(record,position,reflectance))
    {
        imageAtomicMin(voxelOwner,cell,record);
#ifdef AVERAGED_REFLECTANCE
        ivec3 size=imageSize(voxelOwner);
        uint index=uint(cell.x+size.x*(cell.y+size.y*cell.z));
        uvec3 encoded=uvec3(floor(reflectance*float(ZEN_VOXEL_REFLECTANCE_SCALE)+0.5));
        atomicAdd(reflectanceSums[index].x,encoded.x);
        atomicAdd(reflectanceSums[index].y,encoded.y);
        atomicAdd(reflectanceSums[index].z,encoded.z);
        atomicAdd(reflectanceSums[index].w,1u);
#endif
    }
}
