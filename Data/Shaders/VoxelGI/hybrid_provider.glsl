#ifndef ZEN_HYBRID_PROVIDER
#define ZEN_HYBRID_PROVIDER
#include "gi_common.glsl"
struct HybridRayResult
{
    bool hit;
    vec3 position;
    vec3 normal;
    ivec3 cell;
};
// The starting cell is tested. Do not skip occupied starts: that leaks at wall bases.
// Slab entry also handles rays starting outside the voxel volume.
HybridRayResult HybridTraceRay(sampler3D occupancy, vec3 origin, vec3 direction, float maximumDistance)
{
    HybridRayResult result = HybridRayResult(false,origin,vec3(0),ivec3(-1));
    vec3 p = (origin-gi.gridMinVoxelSize.xyz)/gi.gridMinVoxelSize.w;
    float enter = 0.0, leave = maximumDistance/gi.gridMinVoxelSize.w;
    vec3 entryNormal=-direction; // An occupied origin has no entered face.
    bool intersects = true;
    for(int axis=0;axis<3;++axis)
    {
        if(abs(direction[axis])<1e-9) intersects = intersects && p[axis]>=0.0 && p[axis]<gi.volume.x;
        else
        {
            float a=-p[axis]/direction[axis], b=(gi.volume.x-p[axis])/direction[axis];
            float nearDistance=min(a,b);
            if(nearDistance>enter)
            {
                enter=nearDistance;
                entryNormal=vec3(0); entryNormal[axis]=-sign(direction[axis]);
            }
            leave=min(leave,max(a,b));
        }
    }
    if(intersects && leave>enter)
    {
        vec3 entry=p+direction*enter;
        ivec3 cell=ivec3(clamp(floor(entry),vec3(0),vec3(gi.volume.x-1.0)));
        ivec3 stepDirection=ivec3(sign(direction));
        vec3 delta=1.0/max(abs(direction),vec3(1e-9));
        vec3 boundary=vec3(cell)+max(vec3(stepDirection),vec3(0));
        vec3 next=abs(boundary-entry)*delta+enter;
        next=mix(next,vec3(1e30),lessThan(abs(direction),vec3(1e-9)));
        float distance=enter;
        vec3 normal=entryNormal;
        for(int i=0;i<int(3.0*gi.volume.x)+3;++i)
        {
            if(any(lessThan(cell,ivec3(0))) || any(greaterThanEqual(cell,ivec3(gi.volume.x))) || distance>=leave) break;
            if(texelFetch(occupancy,cell,0).a>0.5)
            {
                result=HybridRayResult(true,origin+direction*distance*gi.gridMinVoxelSize.w,normal,cell);
                break;
            }
            int axis=next.x<next.y ? 0:1; axis=next[axis]<next.z ? axis:2;
            distance=next[axis]; cell[axis]+=stepDirection[axis]; next[axis]+=delta[axis];
            normal=vec3(0); normal[axis]=-float(stepDirection[axis]);
        }
    }
    return result;
}
#endif
