#include "hybrid_guides.glsl"
#include "hybrid_sampling.glsl"
#include "hybrid_provider.glsl"
#include "environment_sampling.glsl"
layout(local_size_x=8,local_size_y=8) in;
#ifndef HYBRID_HARDWARE_PROVIDER
layout(set=1,binding=6) uniform sampler3D voxelOpacity;
#endif
layout(set=1,binding=7) uniform samplerCube coneEnvironmentMap;
layout(set=1,binding=8,rgba16f) uniform writeonly image2D rawSky;
layout(set=1,binding=9,rgba16f) uniform writeonly image2D rawSpecular;
void main()
{
    ivec2 p=ivec2(gl_GlobalInvocationID.xy);
    if(!HybridInView(p)) return;
    vec4 sky=vec4(0), specular=vec4(0,0,0,1);
    if(texelFetch(receiverDepth,p,0).r<1.0 && texelFetch(receiverSurface,p,0).g!=0u)
    {
        vec3 position=HybridPosition(p), n=HybridNormal(p), ng=HybridGeometricNormal(p);
        float error=0.0;
#ifdef HYBRID_HARDWARE_PROVIDER
        float depth=texelFetch(receiverDepth,p,0).r;
        float adjacent=uintBitsToFloat(floatBitsToUint(depth)+2u);
        vec3 shifted=ReconstructGBufferPosition(vec2(p)+0.5,textureSize(receiverDepth,0),adjacent,hybrid.inverseViewProjection)+hybrid.worldOrigin.xyz;
        error=length(shifted-position);
#endif
        vec3 origin=HybridTraceOrigin(position,ng,error);
        for(uint i=0u;i<hybrid.sampling.y;++i)
        {
            float pdf;
            vec2 u=HybridSample(uvec2(p),hybrid.sampling.x*hybrid.sampling.y+i,0u);
            bool environmentSample=HybridEnvironmentProposal(i,hybrid.sampling.y,u);
            vec3 w=HybridSkySample(n,u,environmentSample,pdf);
            bool escaped=dot(n,w)>0.0 && dot(ng,w)>0.0 && !HybridTraceRay(voxelOpacity,origin,w,1e20).hit;
            if(escaped)
            {
                float weight=max(dot(n,w),0.0)/(HYBRID_PI*max(pdf,1e-8));
                sky.rgb+=textureLod(coneEnvironmentMap,EnvironmentDirection(w),0).rgb*weight
                    *sceneUbo.environment.x*sceneUbo.environment.z*gi.lighting.y;
                sky.a+=weight;
            }
        }
        sky/=float(hybrid.sampling.y);
        if(hybrid.rejection.y>0.0)
        {
            vec3 view=sceneUbo.viewPosition.w<0.5 ? normalize(sceneUbo.lightInfo.yzw) : normalize(sceneUbo.viewPosition.xyz-position);
            float roughness=clamp(texelFetch(receiverRoughness,p,0).g,0.04,1.0);
            uint specularSamples=hybrid.sampling.y>4u ? hybrid.sampling.y : 1u;
            specular.a=0.0;
            for(uint i=0u;i<specularSamples;++i)
            {
                vec3 w=HybridSpecularDirection(n,view,roughness,uvec2(p),hybrid.sampling.x*specularSamples+i,0x1234u);
                specular.a+=dot(ng,w)>0.0 && !HybridTraceRay(voxelOpacity,origin,w,1e20).hit ? 1.0 : 0.0;
            }
            specular.a/=float(specularSamples);
        }
    }
    imageStore(rawSky,p,sky);
    imageStore(rawSpecular,p,specular);
}
