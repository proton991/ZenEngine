#include "Graphics/Shared/DynamicVoxelGI.h"
layout(set=3,binding=0,std140) uniform uStaticGI { GIStaticUniform staticGI; };
layout(push_constant) uniform StaticBatch { uint firstItem; uint itemCount; uint face; uint reserved; } batch;

// Diagnostic response tables use uint indices. Saturate unrepresentable ranges
// instead of wrapping into unrelated hits; DDA does not use the table index.
uint GIReferenceIndex(uint base,uint block,uint stride,uint offset)
{
    uint index=GI_INVALID_CELL;
    if(base!=GI_INVALID_CELL && stride!=0u && block<=(GI_INVALID_CELL-base)/stride) {
        uint first=base+block*stride;
        if(offset<GI_INVALID_CELL-first) index=first+offset;
    }
    return index;
}

uint StaticItem()
{
    uint group=gl_WorkGroupID.x+gl_NumWorkGroups.x*(gl_WorkGroupID.y+gl_NumWorkGroups.y*gl_WorkGroupID.z);
    uint local=group*GI_QUERY_GROUP_SIZE+gl_LocalInvocationIndex;
    return local<batch.itemCount ? batch.firstItem+local : GI_INVALID_CELL;
}
ivec3 StaticCell(uint id)
{
    uint n=staticGI.volume.x;
    return ivec3(id/(n*n),(id/n)%n,id%n);
}
uint StaticCellID(ivec3 p)
{
    uint n=staticGI.volume.x;
    return uint(p.z)+n*(uint(p.y)+n*uint(p.x));
}
vec3 StaticCenter(uint id)
{
    return staticGI.minimumCellSize.xyz+(vec3(StaticCell(id))+0.5)*staticGI.minimumCellSize.w;
}
// Stable stratified cosine hemisphere: p(w)=cos(theta)/pi, so cos/p=pi.
// Midpoints avoid zero-PDF samples. A fixed per-stratum azimuth offset breaks rows.
vec3 StaticDirection(uint face,uint ray)
{
    float u=(float(ray)+0.5)/float(staticGI.sampling.x);
    float phi=6.283185307179586*fract(float(ray)*0.6180339887498949+0.5);
    vec3 local=vec3(sqrt(u)*cos(phi),sqrt(u)*sin(phi),sqrt(1.0-u));
    vec3 n=vec3(0); n[face/2u]=(face%2u==0u ? 1.0 : -1.0);
    vec3 t=vec3(0); t[(face/2u+1u)%3u]=1.0;
    return normalize(local.x*t+local.y*cross(n,t)+local.z*n);
}
GIQuery StaticRay(uint cell,uint face,uint ray)
{
    vec3 n=vec3(0); n[face/2u]=(face%2u==0u ? 1.0 : -1.0);
    return GIQuery(vec4(StaticCenter(cell)+n*(0.5*staticGI.minimumCellSize.w),staticGI.lighting.w),
                   vec4(StaticDirection(face,ray),staticGI.lighting.z),uvec4(GI_STATIC,GI_STATIC,cell,0));
}
