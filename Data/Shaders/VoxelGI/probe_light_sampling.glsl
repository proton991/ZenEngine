#ifndef ZEN_PROBE_LIGHT_SAMPLING
#define ZEN_PROBE_LIGHT_SAMPLING
#include "hybrid_sampling.glsl"
#include "environment_sampling.glsl"
// Normalized Henyey-Greenstein lobe over the sphere.
const float kProbeLightAnisotropy = .94;
vec4        ProbeReflectedGuide(int light, vec3 delta, float distance)
{
    uint  word      = lightFootprint[light / 4][light % 4];
    float footprint = uintBitsToFloat(word & 0xffff0000u);
    vec3  axis      = delta / max(distance, 1e-20);
    float g         = kProbeLightAnisotropy;
    if (footprint < 1e20)
    {
        // The bright patch on a diffuse plane is centered on the projection of
        // the lamp onto the plane. The closest illuminated point measures both
        // its offset from the lamp and its radial width.
        vec3  reflected     = delta + footprint * DecodeGBufferNormal(unpackUnorm4x8(word).xy);
        float patchDistance = length(reflected);
        if (patchDistance > 1e-8)
        {
            axis = reflected / patchDistance;
        }
        float width = max(footprint, .001) / max(patchDistance, .001);
        g           = clamp(2.0 / (2.0 + width * width + width * sqrt(4.0 + width * width)), .01, .999);
    }
    return vec4(axis, g);
}
float ProbeLightPDF(vec3 axis, vec3 direction, float g)
{
    vec3  chord          = axis - direction;
    float oneMinusCosine = .5 * dot(chord, chord);
    return .5
         * (HybridHenyeyGreensteinPDF(oneMinusCosine, kProbeLightAnisotropy) + HybridHenyeyGreensteinPDF(oneMinusCosine, g));
}
vec3 ProbeLightSample(vec3 axis, vec2 u, float g)
{
    g   = u.y < .5 ? g : kProbeLightAnisotropy;
    u.y = fract(2.0 * u.y);
    return HybridHenyeyGreensteinSample(axis, u, g);
}
#endif
