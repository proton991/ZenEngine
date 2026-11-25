#include "screen_probe_source_lights.glsl"
layout(set = 1, binding = 129) uniform usampler2D probeSourceVisibility;
#ifndef SCREEN_PROBE_DIFFUSE_ONLY
layout(set = 1, binding = 128) uniform sampler2D secondaryBRDFMap;
#endif

// The diffuse cosine and the GGX lobe weight each visible source's RGB independently.
void GatherProbeSources(ivec2 pixel, inout vec3 sky, out vec3 specular)
{
    uint visible = texelFetch(probeSourceVisibility, pixel, 0).r;
    specular     = vec3(0);
    if (visible != 0u)
    {
        vec3 normal = HybridNormal(pixel);
#ifndef SCREEN_PROBE_DIFFUSE_ONLY
        vec3  view      = normalize(sceneUbo.viewPosition.w < .5 ? sceneUbo.lightInfo.yzw
                                                                 : sceneUbo.viewPosition.xyz - HybridPosition(pixel));
        vec2  surface   = texelFetch(receiverRoughness, pixel, 0).rg;
        float roughness = clamp(surface.y, 0.04, 1.0);
        float alpha = roughness * roughness, alphaSquared = alpha * alpha, k = .5 * alpha;
        float nv       = clamp(dot(normal, view), .01, 1.0);
        float f0       = mix(.04, 1.0, clamp(surface.x, 0.0, 1.0));
        vec2  brdf     = textureLod(secondaryBRDFMap, vec2(nv, roughness), 0.0).rg;
        float response = max(f0 * brdf.x + brdf.y, 1e-5);
#endif
        while (visible != 0u)
        {
            int source       = findLSB(visible);
            visible         &= visible - 1u;
            vec3  direction  = sourceLights[2 * source].xyz;
            float nl         = dot(normal, direction);
            if (nl > 0.0)
            {
                vec3 energy  = sourceLights[2 * source + 1].rgb;
                sky         += energy * (nl / HYBRID_PI);
#ifndef SCREEN_PROBE_DIFFUSE_ONLY
                vec3  halfway = normalize(view + direction);
                float nh      = max(dot(normal, halfway), 0.0);
                float grazing = 1.0 - max(dot(view, halfway), 0.0), squared = grazing * grazing;
                float fresnel = f0 + (1.0 - f0) * squared * squared * grazing;
                float d       = 1.0 + (alphaSquared - 1.0) * nh * nh;
                float lobe =
                    fresnel * alphaSquared / (HYBRID_PI * d * d) * nl / (4.0 * (nl * (1.0 - k) + k) * (nv * (1.0 - k) + k));
                specular += energy * (lobe / response);
#endif
            }
        }
    }
}
