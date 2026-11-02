#include "probe_pixel.glsl"
layout(set = 7, binding = 6) uniform sampler2D rawProbeSpecular;
layout(set = 7, binding = 7) uniform sampler2D rawProbeSpecularGuide;

vec4 ProbeResolveSpecular(ivec2 pixel, ProbeReceiver receiver)
{
    vec4 result = vec4(0, 0, 0, 1);
    if (receiver.valid && hybrid.rejection.y > 0.0)
    {
        vec3  position   = receiver.position;
        vec3  normal     = receiver.normal;
        vec3  geometric  = receiver.geometric;
        uint  identity   = receiver.surface.g;
        float roughness  = receiver.roughness;
        vec3  view       = receiver.view;
        float planeLimit = max(probe.parameters.x, 1e-6);
        float depthLimit = max(planeLimit, 0.02 * max(length(sceneUbo.viewPosition.xyz - position), 1.0));
        vec4  sum        = vec4(0);
        float total      = 0.0;
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                ivec2 block = pixel / 2 + ivec2(x, y);
                if (all(greaterThanEqual(block, ivec2(0))) && all(lessThan(block, textureSize(rawProbeSpecularGuide, 0))))
                {
                    vec4  guide  = texelFetch(rawProbeSpecularGuide, block, 0);
                    ivec2 source = ivec2(guide.xy);
                    if (ProbeValidReceiver(source) && texelFetch(receiverSurface, source, 0).g == identity)
                    {
                        vec3  sourcePosition  = HybridPosition(source);
                        vec3  sourceGeometric = HybridGeometricNormal(source);
                        vec3  delta           = sourcePosition - position;
                        float sourceRoughness = clamp(texelFetch(receiverRoughness, source, 0).g, 0.04, 1.0);
                        float normalDot       = dot(normal, HybridNormal(source));
                        float plane           = max(abs(dot(delta, geometric)), abs(dot(delta, sourceGeometric)));
                        float depth           = abs(dot(delta, view));
                        float roughnessDelta  = abs(roughness - sourceRoughness);
                        float roughnessLimit  = max(0.02, 0.15 * max(roughness, sourceRoughness));
                        vec3  sourceView =
                            sceneUbo.viewPosition.w < 0.5 ? view : normalize(sceneUbo.viewPosition.xyz - sourcePosition);
                        if (plane <= planeLimit && depth <= depthLimit && dot(geometric, sourceGeometric) >= 0.99
                            && normalDot >= mix(0.999, 0.9, roughness * roughness) && roughnessDelta <= roughnessLimit
                            && dot(view, sourceView) >= mix(0.9995, 0.9, roughness * roughness))
                        {
                            vec2  offset  = vec2(source - pixel);
                            float weight  = exp(-dot(offset, offset) / 8.0 - plane / planeLimit - depth / depthLimit
                                                - roughnessDelta / roughnessLimit)
                                          * pow(max(normalDot, 0.0), 16.0);
                            sum          += texelFetch(rawProbeSpecular, block, 0) * weight;
                            total        += weight;
                        }
                    }
                }
            }
        }
        // Alpha -1 marks a receiver no compatible block traced this frame (a thin feature whose
        // leaders landed on another surface); probe_pixel_temporal.comp keeps its history.
        result = total > 0.0 ? sum / total : vec4(0, 0, 0, -1);
    }
    return result.a < 0.0 ? vec4(0, 0, 0, -1) : vec4(max(result.rgb, vec3(0)), clamp(result.a, 0.0, 1.0));
}
