#ifndef ZEN_GI_COMMON
#define ZEN_GI_COMMON
#include "../Common/scene_lighting.glsl"
layout(std140,set=3,binding=0) uniform uGISettings
{
    vec4 gridMinVoxelSize;
    vec4 volume; // resolution, inverse side, mip count, indirect intensity
    vec4 cone; // tangent of half aperture, step scale, bias in voxels, maximum grid distance
    vec4 limits; // cone count, max cone steps, analytic shadows, reserved
} gi;
vec3 WorldToVoxelUV(vec3 position) { return (position-gi.gridMinVoxelSize.xyz)*gi.volume.y; }
bool InsideVoxelVolume(vec3 uv) { return all(greaterThanEqual(uv,vec3(0))) && all(lessThan(uv,vec3(1))); }
vec3 TraceOrigin(vec3 position,vec3 normal) { return position+normal*gi.cone.z*gi.gridMinVoxelSize.w; }
// Exact base-level occupancy traversal. A voxel is opaque independently of its albedo.
float VoxelVisibility(sampler3D opacity,vec3 origin,vec3 direction,float maxDistance)
{
    vec3 p=(origin-gi.gridMinVoxelSize.xyz)/gi.gridMinVoxelSize.w;
    vec3 uv=p/gi.volume.x;
    if(!InsideVoxelVolume(uv)) return 1.0;
    ivec3 cell=ivec3(floor(p));
    ivec3 stepDirection=ivec3(sign(direction));
    vec3 delta=1.0/max(abs(direction),vec3(1e-9));
    vec3 boundary=vec3(cell)+max(vec3(stepDirection),vec3(0));
    vec3 distanceToBoundary=abs(boundary-p)*delta;
    for(int axis=0;axis<3;++axis) if(abs(direction[axis])<1e-9) distanceToBoundary[axis]=1e20;
    float endDistance=maxDistance/gi.gridMinVoxelSize.w;
    for(int i=0;i<int(3.0*gi.volume.x)+3;++i)
    {
        if(any(lessThan(cell,ivec3(0))) || any(greaterThanEqual(cell,ivec3(gi.volume.x)))) return 1.0;
        if(texelFetch(opacity,cell,0).a>0.5) return 0.0;
        int axis=distanceToBoundary.x<distanceToBoundary.y ? 0:1;
        axis=distanceToBoundary[axis]<distanceToBoundary.z ? axis:2;
        if(distanceToBoundary[axis]>=endDistance) return 1.0;
        cell[axis]+=stepDirection[axis];
        distanceToBoundary[axis]+=delta[axis];
    }
    return 0.0;
}
// Bias changes the ray's start, so finite lights need a new segment to their position.
// BRDF direction and attenuation are still evaluated at the original surface position.
float VoxelLightVisibility(sampler3D opacity, SceneLight light, vec3 origin)
{
    float visibility = 1.0;
    if (gi.limits.z > 0 && light.coneShadow.z > 0)
    {
        vec3 direction = -light.directionType.xyz;
        float distanceToLight = 1e20;
        if (int(light.directionType.w) != 0)
        {
            vec3 delta = light.positionRange.xyz - origin;
            distanceToLight = length(delta);
            direction = delta / max(distanceToLight, 1e-6);
        }
        if (distanceToLight > 1e-6)
            visibility = VoxelVisibility(opacity, origin, direction, distanceToLight);
    }
    return visibility;
}
vec3 HemisphereCone(vec3 normal,int index,int count,out float weight)
{
    weight=index==0 ? 2.0/float(count+1) : 1.0/float(count+1);
    vec3 helper=abs(normal.y)<0.99 ? vec3(0,1,0):vec3(1,0,0);
    vec3 tangent=normalize(cross(helper,normal));
    vec3 bitangent=cross(normal,tangent);
    float phi=6.28318530718*float(index-1)/float(count-1);
    return index==0 ? normal : 0.5*normal+0.8660254*(cos(phi)*tangent+sin(phi)*bitangent);
}
#endif
