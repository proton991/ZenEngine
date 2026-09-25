#include "Graphics/Shared/VoxelGI.h"
layout(local_size_x=ZEN_VOXEL_VOLUME_GROUP_SIZE,local_size_y=ZEN_VOXEL_VOLUME_GROUP_SIZE,local_size_z=ZEN_VOXEL_VOLUME_GROUP_SIZE,
    local_size_x_id=ZEN_VOXEL_VOLUME_GROUP_X_ID,local_size_y_id=ZEN_VOXEL_VOLUME_GROUP_Y_ID,local_size_z_id=ZEN_VOXEL_VOLUME_GROUP_Z_ID) in;
layout(set=1,binding=0,r32ui) readonly uniform uimage3D voxelOwner;
layout(set=1,binding=1,std430) writeonly buffer OccupiedList { uint cells[]; };
#ifdef CLEAR_COMPACT_LIST
layout(set=1,binding=2,std430) writeonly buffer OccupiedCount { uint count; };
#else
layout(set=1,binding=2,std430) buffer OccupiedCount { uint count; };
layout(set=1,binding=3,std430) writeonly buffer GridToList { uint indices[]; };
#endif
void main()
{
    ivec3 cell=ivec3(gl_GlobalInvocationID);
    ivec3 size=imageSize(voxelOwner);
    if(any(greaterThanEqual(cell,size))) return;
    uint id=uint(cell.z+size.z*(cell.y+size.y*cell.x));
#ifdef CLEAR_COMPACT_LIST
    cells[id]=0xffffffffu;
    if(id==0) count=0;
#else
    uint index=0xffffffffu;
    if(imageLoad(voxelOwner,cell).r!=0xffffffffu)
    {
        // One lane per cell and N^3 capacity prove no list/count overflow.
        index=atomicAdd(count,1);
        cells[index]=id;
    }
    indices[id]=index;
#endif
}
