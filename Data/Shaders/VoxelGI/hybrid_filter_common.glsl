#include "hybrid_guides.glsl"
layout(set=1,binding=6) uniform sampler2D inputSky;
layout(set=1,binding=12) uniform sampler2D inputVariance;
// RGB is the visible irradiance guide. Triangle visibility has no occupied-start boundary.
layout(set=1,binding=14) uniform sampler2D skyGuide;
// The temporal pass already writes this frame's full-precision receiver positions.
layout(set=1,binding=25) uniform sampler2D filterPosition;
vec3 FilterPosition(ivec2 p) { return texelFetch(filterPosition,p,0).xyz; }
vec3 SkyGuide(ivec2 p) { return texelFetch(skyGuide,p,0).rgb; }
float SurfaceWeight(ivec2 q, uint identity, vec3 position, vec3 normal, vec3 geometric)
{
    if(!HybridInView(q)) return 0.0;
    uvec2 surface=texelFetch(receiverSurface,q,0).rg;
    if(surface.g!=identity) return 0.0;
    vec3 delta=FilterPosition(q)-position;
    if(abs(dot(delta,geometric))>hybrid.rejection.x*max(1.0,length(delta))
        || dot(geometric,DecodeGBufferNormal(unpackUnorm2x16(surface.r)))<0.9) return 0.0;
    return pow(max(dot(normal,HybridNormal(q)),0.0),32.0);
}
float SkyWeight(vec3 sampleSky, vec3 sampleGuide, vec3 center, vec3 guide, float sigma)
{
    vec3 difference=sampleSky/sampleGuide-center/guide;
    return exp(-0.5*dot(difference,difference)/(3.0*sigma*sigma));
}
