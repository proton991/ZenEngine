#ifndef ZEN_RESTIR_TARGET
#define ZEN_RESTIR_TARGET
#include "restir_reservoir.glsl"
layout(set = 1, binding = 39) uniform sampler2D restirLobeIntegral;
layout(std140, set = 1, binding = 43) uniform uReSTIRData
{
    uvec4 restirOptions;
};
// The existing reflection signal is the average Lo under the normalized GGX lobe.
// Its split-sum material factor is applied later by deferred lighting.
float GILobeIntegral(float nv, float roughness)
{
    vec2  size       = vec2(textureSize(restirLobeIntegral, 0));
    vec2  coordinate = clamp(vec2(nv, roughness) * size - 0.5, vec2(0), size - 1.0);
    ivec2 p = ivec2(coordinate), q = min(p + 1, ivec2(size) - 1);
    vec2  f = fract(coordinate);
    return mix(mix(texelFetch(restirLobeIntegral, p, 0).r, texelFetch(restirLobeIntegral, ivec2(q.x, p.y), 0).r, f.x),
               mix(texelFetch(restirLobeIntegral, ivec2(p.x, q.y), 0).r, texelFetch(restirLobeIntegral, q, 0).r, f.x), f.y);
}
float GISpecularDensity(vec3 n, vec3 view, vec3 direction, float roughness)
{
    float nv = max(dot(n, view), 1e-5), nl = dot(n, direction);
    vec3  halfVector = view + direction;
    float density    = 0.0;
    if (nl > 0.0 && dot(halfVector, halfVector) > 1e-20)
    {
        float nh    = max(dot(n, normalize(halfVector)), 0.0);
        float alpha = max(roughness * roughness, 0.0016), a2 = alpha * alpha;
        float denominator  = nh * nh * (a2 - 1.0) + 1.0;
        float distribution = a2 / (HYBRID_PI * denominator * denominator);
        density            = distribution * HybridSmithG1(nv, alpha) * HybridSmithG1(nl, alpha)
                           / (4.0 * nv * max(GILobeIntegral(nv, roughness), 1e-5));
    }
    return density;
}
vec3 GIIntegrand(GIReservoir r, vec3 position, vec3 n, vec3 ng, vec3 view, float roughness, bool specular)
{
    vec3 result = GIDiffuse(r, position, n, ng);
    if (specular)
    {
        vec3  delta     = r.position - position;
        float distance2 = dot(delta, delta);
        vec3  direction = delta * inversesqrt(max(distance2, 1e-20));
        result = distance2 > 1e-20 && dot(ng, direction) > 0.0 ? r.radiance * GISpecularDensity(n, view, direction, roughness)
                                                               : vec3(0);
    }
    return result;
}
#endif
