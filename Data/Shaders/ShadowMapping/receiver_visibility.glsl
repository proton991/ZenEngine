#include "../Common/gbuffer.glsl"
#include "../VoxelGI/gi_common.glsl"
#include "../VoxelGI/ray_shadow.glsl"
#include "../VoxelGI/gi_ray_totals.glsl"
#include "light_occluders.glsl"
#include "Graphics/Shared/ReceiverVisibility.h"
layout(set = 1, binding = 0) uniform sampler2D receiverPosition;
layout(set = 1, binding = 1) uniform sampler2D receiverNormal;
layout(set = 1, binding = 2) uniform usampler2D receiverSurface;
layout(set = 1, binding = 3) uniform sampler2D receiverDepth;
layout(set = 1, binding = 4) uniform sampler2D receiverAlbedo;
#ifdef RECEIVER_VISIBILITY_REFINE
layout(set = 1, binding = 6) uniform usampler2D coarseVisibility;
#endif
// Forward layers read the scene-light eligibility and visibility masks in xy.
// With fewer than 32 scene lights, the spare high visibility bit stores the camera light.
#ifdef RECEIVER_VISIBILITY_WIDE
layout(set = 1, binding = 5, rgba32ui) uniform writeonly uimage2D receiverVisibility;
#else
layout(set = 1, binding = 5, rg32ui) uniform writeonly uimage2D receiverVisibility;
#endif
void main()
{
    ivec2 outputPixel = ivec2(gl_LaunchIDEXT.xy);
    ivec2 p           = outputPixel;
#ifdef RECEIVER_VISIBILITY_COARSE
    p = min(p * ZEN_RECEIVER_VISIBILITY_STRIDE + ZEN_RECEIVER_VISIBILITY_STRIDE / 2, textureSize(receiverDepth, 0) - 1);
#endif
    uvec4 visibility = uvec4(0);
    if (texelFetch(receiverDepth, p, 0).r < 1.0 && texelFetch(receiverAlbedo, p, 0).a >= 0.1)
    {
        vec3 position      = texelFetch(receiverPosition, p, 0).xyz;
        vec3 normal        = DecodeGBufferNormal(texelFetch(receiverNormal, p, 0).rg);
        vec3 surfaceNormal = DecodeGBufferNormal(unpackUnorm2x16(texelFetch(receiverSurface, p, 0).r));
#ifdef RECEIVER_VISIBILITY_REFINE
        uvec4 allVisible = uvec4(0xffffffffu), anyVisible = uvec4(0);
        bool  support = true;
        ivec2 base =
            ivec2(floor((vec2(p) - float(ZEN_RECEIVER_VISIBILITY_STRIDE / 2)) / float(ZEN_RECEIVER_VISIBILITY_STRIDE)));
        bool exact = all(equal(p % ZEN_RECEIVER_VISIBILITY_STRIDE, ivec2(ZEN_RECEIVER_VISIBILITY_STRIDE / 2)));
        for (uint tap = 0u; tap < (exact ? 1u : 4u); ++tap)
        {
            ivec2 q = clamp(base + ivec2(tap & 1u, tap >> 1u), ivec2(0), textureSize(coarseVisibility, 0) - 1);
            ivec2 samplePixel =
                min(q * ZEN_RECEIVER_VISIBILITY_STRIDE + ZEN_RECEIVER_VISIBILITY_STRIDE / 2, textureSize(receiverDepth, 0) - 1);
            uvec2 surface      = texelFetch(receiverSurface, samplePixel, 0).rg;
            vec3  otherNormal  = DecodeGBufferNormal(unpackUnorm2x16(surface.r));
            vec3  delta        = texelFetch(receiverPosition, samplePixel, 0).xyz - position;
            float limit        = max(1e-6, length(delta) * .02);
            support            = support && surface.g == texelFetch(receiverSurface, p, 0).g
                              && texelFetch(receiverDepth, samplePixel, 0).r < 1.0 && dot(surfaceNormal, otherNormal) > .995
                              && max(abs(dot(delta, surfaceNormal)), abs(dot(delta, otherNormal))) <= limit;
            uvec4 bits         = texelFetch(coarseVisibility, q, 0);
            allVisible        &= bits;
            anyVisible        |= bits;
        }
        // Small fixtures close to a point light can cast sub-grid shadows even when all
        // four samples agree. Keep exact visibility on these bright nearby receivers.
        uint closeCandidates =
            anyVisible.y & (sceneUbo.lightInfo.x >= 32.0 ? 0xffffffffu : (1u << uint(sceneUbo.lightInfo.x)) - 1u);
        while (support && !exact && closeCandidates != 0u)
        {
            int lightIndex    = findLSB(closeCandidates);
            closeCandidates  &= closeCandidates - 1u;
            SceneLight light  = sceneUbo.lights[lightIndex];
            support =
                light.directionType.w == 0.0 || length(light.positionRange.xyz - position) >= .025 / max(gi.volume.y, 1e-20);
        }
        bool copy = support && (exact || all(equal(allVisible, anyVisible)));
        if (copy)
        {
            visibility = allVisible;
        }
        else
#endif
        {
            uint pending       = sceneUbo.lightInfo.x >= 32.0 ? 0xffffffffu : (1u << uint(sceneUbo.lightInfo.x)) - 1u;
            bool cameraPending = true;
#ifdef RECEIVER_VISIBILITY_REFINE
            if (support)
            {
                visibility  = allVisible;
                pending    &= (allVisible.x ^ anyVisible.x) | (allVisible.y ^ anyVisible.y);
#    ifdef RECEIVER_VISIBILITY_WIDE
                cameraPending = allVisible.z != anyVisible.z;
#    else
                cameraPending = ((allVisible.y ^ anyVisible.y) & 0x80000000u) != 0u;
#    endif
            }
#endif
            while (pending != 0u || cameraPending)
            {
                int index = pending != 0u ? findLSB(pending) : MAX_SCENE_LIGHTS;
                if (index == MAX_SCENE_LIGHTS)
                {
                    cameraPending = false;
                }
                else
                {
                    pending &= pending - 1u;
                }
                SceneLight light = index == MAX_SCENE_LIGHTS ? sceneUbo.cameraLight : sceneUbo.lights[index];
                vec3       direction;
                float      distanceToLight;
                vec3       incoming = EvaluateLight(light, position, direction, distanceToLight);
                // Coarse masks describe geometric visibility independently of the normal map.
                // Each shaded pixel still evaluates its own BRDF and light attenuation.
#ifdef RECEIVER_VISIBILITY_COARSE
                if (any(greaterThan(incoming, vec3(0))))
#else
                if (dot(normal, direction) > 0.0 && any(greaterThan(incoming, vec3(0))))
#endif
                {
                    bool visible = true;
                    if (gi.limits.z > 0.5 && (index == MAX_SCENE_LIGHTS || light.coneShadow.z > 0.0))
                    {
#ifdef RECEIVER_VISIBILITY_REFINE
                        uint bit    = 1u << uint(min(index, 31));
                        bool agreed = support && (allVisible.x & bit) != 0u && ((allVisible.y ^ anyVisible.y) & bit) == 0u;
                        bool reused = (allVisible.y & bit) != 0u;
                        if (index == MAX_SCENE_LIGHTS)
                        {
                            // Camera-light eligibility has no separate bit; retain its exact query.
                            agreed = false;
                        }
                        if (agreed)
                        {
                            visible = reused;
                        }
                        else
#endif
                        {
                            visible = !CachedLightOccluded(index, position, surfaceNormal, light)
                                   && HardwareLightVisibility(position, surfaceNormal, light) > 0.0;
                        }
                    }
                    if (index == MAX_SCENE_LIGHTS)
                    {
#ifdef RECEIVER_VISIBILITY_WIDE
                        visibility.z = uint(visible);
#else
                        visibility.y |= uint(visible) << 31u;
#endif
                    }
                    else
                    {
                        visibility.xy |= uvec2(1u, uint(visible)) << uint(index);
                    }
                }
            }
        }
    }
    imageStore(receiverVisibility, outputPixel, visibility);
#ifdef GI_COUNT_RAY_SLOT
    RecordGIRays();
#endif
}
