#include "frame_common.glsl"
layout(local_size_x=GI_QUERY_GROUP_SIZE) in;
layout(set=3,binding=1,std430) readonly buffer StaticCount { uint occupiedCount; };
layout(set=3,binding=2,std430) readonly buffer StaticList { uint occupiedList[]; };
#define GI_CACHE_WRITE
#include "hit_cache.glsl"
void main()
{
    uint index=StaticItem();
    if(index<staticGI.volume.y*staticGI.sampling.x)
    {
        GIHit hit=EmptyGIHit(GI_UNKNOWN);
        uint receiver=index/staticGI.sampling.x;
        if(batch.reserved==0u && occupiedCount<=staticGI.volume.y && receiver<occupiedCount)
            hit=TraceClosest(StaticRay(occupiedList[receiver],batch.face,index%staticGI.sampling.x),
                             batch.face*staticGI.volume.y*staticGI.sampling.x+index);
        StoreCachedHit(index,hit);
    }
}
