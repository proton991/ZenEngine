#include "query_common.glsl"
layout(set=1,binding=0,std140) uniform uGIReference { uvec4 referenceInfo; };
layout(set=1,binding=1,std430) readonly buffer GIReferenceHits { GIHit referenceHits[]; };
GIHit ResolveHitSurface(GIHit hit) { return hit; }
GIHit TraceClosest(GIQuery ray,uint queryIndex)
{
    GIHit result=EmptyGIHit(GI_UNKNOWN);
    if(ValidGIQuery(ray) && queryIndex<referenceInfo.x) result=referenceHits[queryIndex];
    return result;
}
uint TraceOccluded(GIQuery ray,uint queryIndex)
{
    return TraceClosest(ray,queryIndex).identity.x;
}
