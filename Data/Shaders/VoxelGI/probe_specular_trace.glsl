#include "probe_ray.glsl"
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 7, binding = 6, rgba16f) uniform writeonly image2D rawProbeSpecular;
layout(set = 7, binding = 7, rgba32f) uniform writeonly image2D rawProbeSpecularGuide;
// The traced receiver of each block for probe_specular_filter.comp: identity (zero without one),
// shading and geometric normals as stored in the receiver guides; world position and roughness.
layout(set = 7, binding = 29, rgba32ui) uniform writeonly uimage2D rawProbeSpecularSurface;
layout(set = 7, binding = 30, rgba32f) uniform writeonly image2D rawProbeSpecularPosition;

void main()
{
    ivec2 block = ivec2(gl_GlobalInvocationID.xy);
    if (all(lessThan(block, imageSize(rawProbeSpecular))))
    {
        ivec2 selected = ivec2(-1);
        uint  phase    = (hybrid.sampling.x + HybridHash(HybridMorton(uvec2(block)))) & 3u;
        for (uint candidate = 0u; candidate < 4u; ++candidate)
        {
            uint  offset = (phase + candidate) & 3u;
            ivec2 pixel  = block * 2 + ivec2(offset & 1u, offset >> 1u);
            if (selected.x < 0 && ProbeValidReceiver(pixel))
            {
                selected = pixel;
            }
        }
        vec4  specular = vec4(0, 0, 0, 1);
        vec4  guide    = vec4(vec2(selected), 0, 0);
        uvec4 surface  = uvec4(0);
        vec4  place    = vec4(0);
        if (selected.x >= 0)
        {
            uvec2 receiver = texelFetch(receiverSurface, selected, 0).rg;
            surface        = uvec4(receiver.g, packUnorm2x16(texelFetch(receiverNormal, selected, 0).rg), receiver.r, 0u);
            place          = vec4(HybridPosition(selected), clamp(texelFetch(receiverRoughness, selected, 0).g, 0.04, 1.0));
        }
        if (selected.x >= 0 && hybrid.rejection.y > 0.0)
        {
            vec3  position  = HybridPosition(selected);
            vec3  normal    = HybridNormal(selected);
            vec3  geometric = HybridGeometricNormal(selected);
            vec3  view      = sceneUbo.viewPosition.w < 0.5 ? normalize(sceneUbo.lightInfo.yzw)
                                                            : normalize(sceneUbo.viewPosition.xyz - position);
            float roughness = clamp(texelFetch(receiverRoughness, selected, 0).g, 0.04, 1.0);
            vec3  direction = HybridSpecularDirection(normal, view, roughness, uvec2(selected), hybrid.sampling.x, 0x1234u);
            specular.a      = 0.0;
            if (dot(geometric, direction) > 0.0)
            {
                bool sharedReflection = ProbeFeatureHardware(SPECULAR_PROVIDER) == ProbeFeatureHardware(REFLECTION_PROVIDER);
                HybridRayResult hit   = ProbeTraceFeature(SPECULAR_PROVIDER, position, geometric, direction,
                                                          hybrid.reflection.x != 0u && sharedReflection);
                bool            valid = hit.valid;
                specular.a            = hit.hit ? 0.0 : 1.0;
                if (hybrid.reflection.x != 0u && !sharedReflection)
                {
                    hit   = ProbeTraceFeature(REFLECTION_PROVIDER, position, geometric, direction, true);
                    valid = valid && hit.valid;
                }
                if (hit.hit && hybrid.reflection.x != 0u)
                {
                    bool rejected;
                    // RGB is the unconditional hit integral (1-S)*H; misses contribute zero.
                    specular.rgb = HitRadiance(hitRadiance, hitNormal, hit, direction, false, rejected);
                    guide.z      = length(hit.position - position);
                    guide.w      = rejected ? 1.0 : 0.0;
                }
                if (!valid)
                {
                    // An unavailable trace is no observation: publish the block as untraced, so
                    // resolve and filtering skip it and the pixel histories keep their values.
                    specular = vec4(0, 0, 0, 1);
                    guide    = vec4(-1, -1, 0, 0);
                    surface  = uvec4(0);
                    place    = vec4(0);
                }
            }
        }
        imageStore(rawProbeSpecular, block, specular);
        imageStore(rawProbeSpecularGuide, block, guide);
        imageStore(rawProbeSpecularSurface, block, surface);
        imageStore(rawProbeSpecularPosition, block, place);
    }
}
