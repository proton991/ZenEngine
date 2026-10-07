#ifndef ZEN_ENVIRONMENT_VISIBILITY_GLSL
#define ZEN_ENVIRONMENT_VISIBILITY_GLSL
#include "gi_common.glsl"

// Bindings 1..7 are also used by the forward material/environment inputs.
layout(std140,set=3,binding=8) uniform uGIVisibilityBounds
{
    vec4 minimum;
    vec4 maximum;
} visibilityBounds;

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
    // Only tighten the exit side of each axis. A ray starting in empty grid
    // padding can still enter the occupied bounds, so keep its entry side open.
    // The original DDA increments and z/y/x tie order remain unchanged.
    ivec3 exitMin=mix(ivec3(0),ivec3(visibilityBounds.minimum.xyz),lessThanEqual(direction,vec3(0)));
    ivec3 exitMax=mix(ivec3(gi.volume.x),ivec3(visibilityBounds.maximum.xyz),greaterThanEqual(direction,vec3(0)));
    for(int i=0;i<int(3.0*gi.volume.x)+3;++i)
    {
        if(any(lessThan(cell,exitMin)) || any(greaterThanEqual(cell,exitMax))) return 1.0;
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
float VoxelEnvironmentConeVisibility(sampler3D opacity, vec3 origin, vec3 direction)
{
    // A blocker on the axis occludes only part of a wide environment cone.
    // Keep exact occupancy per sample so closed geometry still blocks the sky.
    float visibility = 0.0;
    for (int i = 0; i < ENVIRONMENT_CONE_SAMPLES; ++i)
        visibility += VoxelEnvironmentVisibility(opacity, origin, EnvironmentConeDirection(direction, i), 1e20);
    return visibility / float(ENVIRONMENT_CONE_SAMPLES);
}
#endif
