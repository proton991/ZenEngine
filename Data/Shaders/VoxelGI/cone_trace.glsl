#include "environment_visibility.glsl"
layout(set=1,binding=9) uniform sampler3D voxelRadiance;
layout(set=1,binding=10) uniform sampler3D voxelOpacity;
layout(set=1,binding=11) uniform samplerCube coneEnvironmentMap;
#ifdef LIGHTING_CAPTURE
vec3 captureConeBounced, captureConeEscaped;
vec3 captureDiffuseBounced, captureDiffuseEscaped;
#endif
vec3 TraceDiffuseCone(vec3 origin,vec3 direction)
{
    float distance=gi.gridMinVoxelSize.w;
    float transmittance=1.0;
    vec3 incoming=vec3(0);
    float maxDistance=gi.cone.w/gi.volume.y;
    for(int step=0;step<int(gi.limits.y);++step)
    {
        vec3 uv=WorldToVoxelUV(origin+direction*distance);
        if(!InsideVoxelVolume(uv) || distance>maxDistance || transmittance<0.01) break;
        float diameter=max(gi.gridMinVoxelSize.w,2.0*gi.cone.x*distance);
        float lod=clamp(log2(diameter/gi.gridMinVoxelSize.w),0.0,gi.volume.z-1.0);
        vec4 sampleValue=textureLod(voxelRadiance,uv,lod);
        float stepLength=max(gi.gridMinVoxelSize.w*0.5,diameter*0.5*gi.cone.y);
        float alpha=1.0-pow(max(1.0-sampleValue.a,0.0),stepLength/diameter);
        float correction=sampleValue.a>1e-5 ? alpha/sampleValue.a : 0.0;
        incoming+=transmittance*sampleValue.rgb*correction;
        transmittance*=1.0-alpha;
        distance+=stepLength;
    }
    incoming*=gi.volume.w;
#ifdef LIGHTING_CAPTURE
    captureConeBounced=incoming;
    captureConeEscaped=vec3(0);
#endif
    if(sceneUbo.environment.z>0 && gi.lighting.y>0)
    {
        // The visibility rays trace occupancy to the volume boundary and estimate the
        // unblocked part of the cone. Transmittance estimates the same blockers from
        // filtered mips, so it must not attenuate the sky again or gate it.
        float visibility=VoxelEnvironmentConeVisibility(voxelOpacity,origin,direction);
        vec3 escaped=visibility*ConeEnvironmentRadiance(coneEnvironmentMap,direction)*sceneUbo.environment.x;
        incoming+=escaped;
#ifdef LIGHTING_CAPTURE
        captureConeEscaped=escaped;
#endif
    }
    return incoming;
}
vec3 DiffuseVoxelLighting(vec3 position,vec3 normal)
{
    vec3 result=vec3(0);
#ifdef LIGHTING_CAPTURE
    captureDiffuseBounced=vec3(0);
    captureDiffuseEscaped=vec3(0);
#endif
    vec3 origin=TraceOrigin(position,normal);
    int count=int(gi.limits.x);
    for(int i=0;i<count;++i)
    {
        float weight;
        vec3 direction=HemisphereCone(normal,i,count,weight);
        result+=TraceDiffuseCone(origin,direction)*weight;
#ifdef LIGHTING_CAPTURE
        captureDiffuseBounced+=captureConeBounced*weight;
        captureDiffuseEscaped+=captureConeEscaped*weight;
#endif
    }
    return result;
}
