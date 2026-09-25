#include "../Common/bindless_heap.glsl"
#include "Graphics/Shared/VoxelGI.h"
layout(local_size_x=ZEN_VOXEL_VOLUME_GROUP_SIZE, local_size_x_id=ZEN_VOXEL_VOLUME_GROUP_X_ID,
       local_size_y=ZEN_VOXEL_VOLUME_GROUP_SIZE, local_size_y_id=ZEN_VOXEL_VOLUME_GROUP_Y_ID,
       local_size_z=ZEN_VOXEL_VOLUME_GROUP_SIZE, local_size_z_id=ZEN_VOXEL_VOLUME_GROUP_Z_ID) in;
layout(set=1,binding=0,r32ui) uniform writeonly uimage3D voxelOwner;
#ifdef AVERAGED_REFLECTANCE
layout(set=1,binding=1,std430) writeonly buffer ReflectanceSums { uvec4 reflectanceSums[]; };
#endif
void main()
{
    ivec3 p=ivec3(gl_GlobalInvocationID);
    ivec3 size=imageSize(voxelOwner);
    if(all(lessThan(p,size)))
    {
        imageStore(voxelOwner,p,uvec4(0xffffffffu));
#ifdef AVERAGED_REFLECTANCE
        reflectanceSums[p.x+size.x*(p.y+size.y*p.z)]=uvec4(0);
#endif
    }
}
