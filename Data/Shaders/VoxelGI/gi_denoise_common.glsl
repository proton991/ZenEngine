#ifndef ZEN_GI_DENOISE_COMMON
#define ZEN_GI_DENOISE_COMMON
#include "hybrid_guides.glsl"
#include "../Common/scene_lighting.glsl"

struct GIDenoiseSignals
{
    vec4 sky;
    vec4 bounce;
    vec4 specular;
};

struct GIDenoiseGuide
{
    vec3  position;
    vec3  normal;
    vec3  geometric;
    uint  identity;
    float roughness;
};

#ifndef DENOISE_GUIDE_ONLY
layout(constant_id = 1) const bool DENOISE_SKY_ENABLED = true;

// The CPU selects a matching pipeline and one-texel images for disabled sky lighting.
vec4 DenoiseSkySignal(sampler2D sky, ivec2 p)
{
    return DENOISE_SKY_ENABLED ? texelFetch(sky, p, 0) : vec4(0, 0, 0, 1);
}
#endif

GIDenoiseGuide DenoiseGuide(ivec2 p)
{
    uvec2 surface  = texelFetch(receiverSurface, p, 0).rg;
    uint  identity = texelFetch(receiverDepth, p, 0).r < 1.0 ? surface.g : 0u;
    return GIDenoiseGuide(HybridPosition(p), HybridNormal(p), HybridGeometricNormal(p), identity,
                          texelFetch(receiverRoughness, p, 0).g);
}

float DenoisePlaneLimit(vec3 position)
{
    return hybrid.rejection.x * max(1.0, length(position - sceneUbo.viewPosition.xyz));
}

float DenoiseHistoryRejection(GIDenoiseGuide center, GIDenoiseGuide old, bool specular)
{
    vec3  delta     = old.position - center.position;
    float threshold = specular ? mix(0.99, 0.9, center.roughness * center.roughness) : 0.9;
    float reason =
        old.identity != center.identity                                                                               ? 3.0
        : max(abs(dot(delta, center.geometric)), abs(dot(delta, old.geometric))) > DenoisePlaneLimit(center.position) ? 4.0
        : dot(center.normal, old.normal) < threshold                                                                  ? 5.0
        : dot(center.geometric, old.geometric) < (specular ? 0.99 : 0.9)                                              ? 6.0
        : specular && abs(center.roughness - old.roughness) > HybridReflectionRoughnessTolerance(center.roughness)    ? 7.0
                                                                                                                      : 0.0;
    return reason;
}

vec4 CatmullRomWeights(float t)
{
    float t2 = t * t, t3 = t2 * t;
    return vec4(-0.5 * t + t2 - 0.5 * t3, 1.0 - 2.5 * t2 + 1.5 * t3, 0.5 * t + 2.0 * t2 - 1.5 * t3, -0.5 * t2 + 0.5 * t3);
}

vec3 DenoiseView(vec3 position)
{
    vec3 direction = sceneUbo.viewPosition.w < 0.5 ? sceneUbo.lightInfo.yzw : sceneUbo.viewPosition.xyz - position;
    return direction / max(length(direction), 1e-20);
}

// Geometric rejection is shared by the pyramid, history fix and spatial filters.
// Normal cosine (zero when rejected), followed by the plane-distance exponent.
vec2 DenoiseSurfaceTerms(GIDenoiseGuide center, GIDenoiseGuide sampleGuide)
{
    vec3  delta = sampleGuide.position - center.position;
    float plane = max(abs(dot(delta, center.geometric)), abs(dot(delta, sampleGuide.geometric)));
    float limit = DenoisePlaneLimit(center.position);
    bool  valid = center.identity != 0u && center.identity == sampleGuide.identity
               && dot(center.geometric, sampleGuide.geometric) >= 0.9 && dot(center.normal, sampleGuide.normal) >= 0.9
               && plane <= limit;
    return vec2(valid ? max(dot(center.normal, sampleGuide.normal), 0.0) : 0.0, plane / max(limit * 0.5, 1e-8));
}

float DenoiseSurfaceWeight(GIDenoiseGuide center, GIDenoiseGuide sampleGuide)
{
    vec2 terms = DenoiseSurfaceTerms(center, sampleGuide);
    return terms.x > 0.0 ? exp(-terms.y) * pow(terms.x, 16.0) : 0.0;
}

vec2 DenoiseLobeExponents(GIDenoiseGuide center, GIDenoiseGuide sampleGuide)
{
    vec3  centerLobe = reflect(-DenoiseView(center.position), center.normal);
    vec3  sampleLobe = reflect(-DenoiseView(sampleGuide.position), sampleGuide.normal);
    float width      = max(center.roughness * center.roughness, 0.002);
    return vec2(abs(center.roughness - sampleGuide.roughness) * 32.0, (1.0 - max(dot(centerLobe, sampleLobe), 0.0)) / width);
}

float DenoiseLobeWeight(GIDenoiseGuide center, GIDenoiseGuide sampleGuide)
{
    vec2 exponents = DenoiseLobeExponents(center, sampleGuide);
    return exp(-exponents.x) * exp(-exponents.y);
}

// History fix evaluates both signals together; combine their independent exponential weights.
vec2 DenoiseSpatialWeights(GIDenoiseGuide center, GIDenoiseGuide sampleGuide, vec2 kernelExponent)
{
    vec2  surface = DenoiseSurfaceTerms(center, sampleGuide);
    vec2  lobe    = DenoiseLobeExponents(center, sampleGuide);
    float square  = surface.x * surface.x;
    float fourth  = square * square;
    float eighth  = fourth * fourth;
    return (eighth * eighth) * exp(-surface.y - kernelExponent - vec2(0, lobe.x + lobe.y));
}

GIDenoiseSignals DenoisePyramidSignals(sampler2DArray signals, ivec2 p, int level)
{
    return GIDenoiseSignals(texelFetch(signals, ivec3(p, 0), level), texelFetch(signals, ivec3(p, 1), level),
                            texelFetch(signals, ivec3(p, 2), level));
}

GIDenoiseGuide DenoiseUnpackGuide(uvec4 position, uvec4 normal)
{
    return GIDenoiseGuide(uintBitsToFloat(position.xyz), DecodeGBufferNormal(uintBitsToFloat(normal.xy)),
                          DecodeGBufferNormal(unpackUnorm2x16(normal.w)), position.w, uintBitsToFloat(normal.z));
}

GIDenoiseGuide DenoisePyramidGuide(usampler2DArray guides, ivec2 p, int level)
{
    // Guide storage begins at signal level one; level zero is never consumed.
    return DenoiseUnpackGuide(texelFetch(guides, ivec3(p, 0), level - 1), texelFetch(guides, ivec3(p, 1), level - 1));
}
#endif
