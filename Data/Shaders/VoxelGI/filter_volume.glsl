#include "../Common/bindless_heap.glsl"
#include "Graphics/Shared/VoxelGI.h"
layout(local_size_x=ZEN_VOXEL_VOLUME_GROUP_SIZE, local_size_x_id=ZEN_VOXEL_VOLUME_GROUP_X_ID,
       local_size_y=ZEN_VOXEL_VOLUME_GROUP_SIZE, local_size_y_id=ZEN_VOXEL_VOLUME_GROUP_Y_ID,
       local_size_z=ZEN_VOXEL_VOLUME_GROUP_SIZE, local_size_z_id=ZEN_VOXEL_VOLUME_GROUP_Z_ID) in;
#ifdef FILTER_ALBEDO
layout(set=1,binding=0,rgba8) uniform readonly image3D sourceVolume;
layout(set=1,binding=1,rgba8) uniform writeonly image3D targetVolume;
#else
layout(set=1,binding=0,rgba16f) uniform readonly image3D sourceVolume;
layout(set=1,binding=1,rgba16f) uniform writeonly image3D targetVolume;
#endif
void main()
{
    ivec3 p=ivec3(gl_GlobalInvocationID);
    if(any(greaterThanEqual(p,imageSize(targetVolume)))) return;
    vec4 sum=vec4(0);
    for(int z=0;z<2;++z) for(int y=0;y<2;++y) for(int x=0;x<2;++x)
        sum+=imageLoad(sourceVolume,p*2+ivec3(x,y,z));
    imageStore(targetVolume,p,sum*0.125);
}
