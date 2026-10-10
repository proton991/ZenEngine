#ifndef ZEN_HYBRID_SKY_GUIDE
#define ZEN_HYBRID_SKY_GUIDE
#include "environment_distribution.glsl"
// Unoccluded environment irradiance for a shading normal, from the order-2 projection of the
// sampled environment. The spatial filter averages sky as a ratio to it and remodulates by
// the center's value, so normal-map shading detail is kept while neighbors with different
// normals still average their visibility. Only ratios between pixels matter: the result is
// normalized by the brightest axis, floored at 1e-3 of it for every pixel alike, and 1 for a
// black environment, which disables the ratio.
layout(std430,set=1,binding=22) readonly buffer EnvironmentHarmonics { vec4 environmentHarmonics[]; };
vec3 HybridSkyGuide(vec3 normal)
{
    vec3 L[ZEN_ENVIRONMENT_HARMONIC_COEFFICIENTS];
    for(uint k=0u;k<ZEN_ENVIRONMENT_HARMONIC_COEFFICIENTS;++k)
        L[k]=environmentHarmonics[ZEN_ENVIRONMENT_HARMONIC_GROUPS*ZEN_ENVIRONMENT_HARMONIC_COEFFICIENTS+k].rgb;
    float brightest=0.0;
    for(int axis=0;axis<6;++axis)
    {
        vec3 direction=vec3(0); direction[axis/2]=(axis&1)==0 ? 1.0 : -1.0;
        vec3 g=EnvironmentHarmonicIrradiance(direction,L);
        brightest=max(brightest,max(max(g.r,g.g),g.b));
    }
    if(brightest<=1e-20) return vec3(1);
    return max(EnvironmentHarmonicIrradiance(EnvironmentDirection(normal),L)/brightest,vec3(1e-3));
}
#endif
