#ifndef ZEN_ENVIRONMENT_VISIBILITY_GLSL
#define ZEN_ENVIRONMENT_VISIBILITY_GLSL
#include "gi_common.glsl"

// Fragment-stage visibility keeps exact base-level occupancy and scalar tie rules.
// Keep the compute injection/sky shaders on their existing scalar path: changing
// those shaders can also change compiler rounding in the irradiance calculations.
float VoxelEnvironmentVisibility(sampler3D opacity,vec3 origin,vec3 direction,float maxDistance)
{
    vec3 p=(origin-gi.gridMinVoxelSize.xyz)/gi.gridMinVoxelSize.w;
    vec3 uv=p/gi.volume.x;
    if(!InsideVoxelVolume(uv)) return 1.0;
    ivec3 cell=ivec3(floor(p));
    ivec3 stepDirection=ivec3(sign(direction));
    vec3 delta=1.0/max(abs(direction),vec3(1e-9));
    vec3 boundary=vec3(cell)+max(vec3(stepDirection),vec3(0));
    vec3 distanceToBoundary=abs(boundary-p)*delta;
    distanceToBoundary=mix(distanceToBoundary,vec3(1e20),lessThan(abs(direction),vec3(1e-9)));
    float endDistance=maxDistance/gi.gridMinVoxelSize.w;
    for(int i=0;i<int(3.0*gi.volume.x)+3;++i)
    {
        if(any(lessThan(cell,ivec3(0))) || any(greaterThanEqual(cell,ivec3(gi.volume.x)))) return 1.0;
        if(texelFetch(opacity,cell,0).a>0.5) return 0.0;
        // Select exactly one axis, retaining the reference's z/y/x priority at ties.
        // Vector selects avoid dynamically indexing and updating per-lane vector registers.
        bool stepX=distanceToBoundary.x<distanceToBoundary.y && distanceToBoundary.x<distanceToBoundary.z;
        bool stepY=distanceToBoundary.y<=distanceToBoundary.x && distanceToBoundary.y<distanceToBoundary.z;
        bvec3 advance=bvec3(stepX,stepY,!stepX && !stepY);
        if(min(min(distanceToBoundary.x,distanceToBoundary.y),distanceToBoundary.z)>=endDistance) return 1.0;
        cell+=mix(ivec3(0),stepDirection,advance);
        distanceToBoundary=mix(distanceToBoundary,distanceToBoundary+delta,advance);
    }
    return 0.0;
}
#endif
