#include "probe_pixel.glsl"
#ifdef PROBE_FUSED_RECONSTRUCTION
#    include "probe_integrate.glsl"
#    include "probe_specular_resolve.glsl"

// Preserve the rgba16f store/load boundary of the separate reconstruction passes. Raw sky is
// rgba32f there (probe_integrate.comp), so only bounce and specular are rounded.
vec4 ProbeRoundToHalf(vec4 value)
{
    return vec4(unpackHalf2x16(packHalf2x16(value.xy)), unpackHalf2x16(packHalf2x16(value.zw)));
}
#endif
layout(local_size_x = 8, local_size_y = 8) in;
#ifndef PROBE_FUSED_RECONSTRUCTION
layout(set = 7, binding = 30) uniform sampler2D probeSourceSky;
layout(set = 7, binding = 4) uniform sampler2D probeRawSky;
layout(set = 7, binding = 5) uniform sampler2D probeRawBounce;
layout(set = 7, binding = 8) uniform sampler2D probeRawSpecular;
#endif
layout(set = 7, binding = 9) uniform sampler2D previousProbeSky;
layout(set = 7, binding = 10) uniform sampler2D previousProbeBounce;
layout(set = 7, binding = 11) uniform sampler2D previousProbeSpecular;
layout(set = 7, binding = 12) uniform sampler2D previousProbePosition;
layout(set = 7, binding = 13) uniform usampler2D previousProbeSurface;
layout(set = 7, binding = 14) uniform sampler2D previousProbeNormalRoughness;
// Float32: the residual history is the stored sky minus the source history, and a half-float
// sum loses most of the residual's precision on receivers lit by a much brighter source.
layout(set = 7, binding = 15, rgba32f) uniform writeonly image2D nextProbeSky;
layout(set = 7, binding = 16, rgba16f) uniform writeonly image2D nextProbeBounce;
layout(set = 7, binding = 17, rgba16f) uniform writeonly image2D nextProbeSpecular;
layout(set = 7, binding = 18, rgba32f) uniform writeonly image2D nextProbePosition;
layout(set = 7, binding = 19, rg32ui) uniform writeonly uimage2D nextProbeSurface;
layout(set = 7, binding = 20, rgba16f) uniform writeonly image2D nextProbeNormalRoughness;
layout(set = 7, binding = 31) uniform sampler2D previousProbeSourceSky;
// Float32 avoids half-float EMA stagnation when a 1/64 update is smaller than one stored ULP.
layout(set = 7, binding = 32, rgba32f) uniform writeonly image2D nextProbeSourceSky;

void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (HybridInView(pixel))
    {
        vec4          sky                    = vec4(0);
        vec4          sourceSky              = vec4(0);
        vec4          bounce                 = vec4(0);
        vec4          specular               = vec4(0, 0, 0, 1);
        vec4          positionHistory        = vec4(0);
        vec4          normalRoughnessHistory = vec4(0);
        uvec2         surface                = uvec2(0);
        ProbeReceiver receiver               = ProbeLoadReceiver(pixel);
        if (receiver.valid)
        {
            vec3 position   = receiver.position;
            vec3 normal     = receiver.normal;
            vec3 geometric  = receiver.geometric;
            surface         = receiver.surface;
            float roughness = receiver.roughness;
            vec3  view      = receiver.view;
            // Reconstruct before fetching history so its accumulators are not live across the gather.
#ifdef PROBE_FUSED_RECONSTRUCTION
            ProbeIntegratePixel(pixel, receiver, sky, bounce);
            bounce   = ProbeRoundToHalf(bounce);
            specular = ProbeRoundToHalf(ProbeResolveSpecular(pixel, receiver));
#else
            sky      = texelFetch(probeRawSky, pixel, 0);
            bounce   = texelFetch(probeRawBounce, pixel, 0);
            specular = texelFetch(probeRawSpecular, pixel, 0);
#endif
            // The public sky texture includes sources. Accumulate its probe residual separately
            // so two source rays can use a longer history without lengthening bounce/contact reuse.
            vec4 rawSource       = texelFetch(probeSourceSky, pixel, 0);
            bool sourceSample    = rawSource.a >= 0.0;
            sourceSky            = sourceSample ? vec4(rawSource.rgb, 1) : vec4(0);
            sky.rgb             -= rawSource.rgb;
            vec4  motion         = texelFetch(receiverMotion, pixel, 0);
            vec2  previousPixel  = vec2(pixel) + motion.xy * 0.5 * vec2(textureSize(receiverDepth, 0));
            ivec2 base           = ivec2(floor(previousPixel));
            vec2  fraction       = fract(previousPixel);
            vec4  oldSky = vec4(0), oldBounce = vec4(0), oldSpecular = vec4(0);
            vec4  oldSourceSky  = vec4(0);
            float diffuseWeight = 0.0, specularWeight = 0.0, sourceWeight = 0.0;
            float oldSkyLength = 0.0, oldSpecularLength = 0.0;
            bool  historyValid = probe.grid.w != 0u && hybrid.sampling.w != 0u && hybrid.rejection.z > 0.0 && motion.z > 0.0;
            if (historyValid)
            {
                for (int y = 0; y < 2; ++y)
                {
                    for (int x = 0; x < 2; ++x)
                    {
                        ivec2 previous = base + ivec2(x, y);
                        if (all(greaterThanEqual(previous, ivec2(0)))
                            && all(lessThan(previous, textureSize(previousProbePosition, 0))))
                        {
                            uvec2 oldSurface         = texelFetch(previousProbeSurface, previous, 0).rg;
                            vec4  oldPosition        = texelFetch(previousProbePosition, previous, 0);
                            vec4  oldNormalRoughness = texelFetch(previousProbeNormalRoughness, previous, 0);
                            vec3  oldGeometric       = DecodeGBufferNormal(unpackUnorm2x16(oldSurface.r));
                            vec3  oldNormal          = DecodeGBufferNormal(oldNormalRoughness.xy);
                            vec3  delta              = oldPosition.xyz - position;
                            float plane              = max(abs(dot(delta, geometric)), abs(dot(delta, oldGeometric)));
                            if (oldSurface.g == surface.g && oldPosition.w > 0.0 && plane <= max(probe.parameters.x, 1e-6)
                                && dot(geometric, oldGeometric) >= 0.9 && dot(normal, oldNormal) >= 0.9)
                            {
                                float weight =
                                    (x == 0 ? 1.0 - fraction.x : fraction.x) * (y == 0 ? 1.0 - fraction.y : fraction.y);
                                vec4 previousSource = texelFetch(previousProbeSourceSky, previous, 0);
                                oldSky += (texelFetch(previousProbeSky, previous, 0) - vec4(previousSource.rgb, 0)) * weight;
                                // Neighbours without source history (length zero) store no source
                                // radiance; averaging them in would darken the reused source.
                                float sourceShare  = previousSource.a > 0.0 ? weight : 0.0;
                                oldSourceSky      += previousSource * sourceShare;
                                sourceWeight      += sourceShare;
                                oldBounce         += texelFetch(previousProbeBounce, previous, 0) * weight;
                                oldSkyLength      += oldPosition.w * weight;
                                diffuseWeight     += weight;
                                vec3 previousView  = sceneUbo.viewPosition.w < 0.5
                                                       ? normalize(hybrid.previousViewDirection.xyz)
                                                       : normalize(hybrid.previousViewPosition.xyz - oldPosition.xyz);
                                if (oldNormalRoughness.w > 0.0 && dot(geometric, oldGeometric) >= 0.99
                                    && dot(normal, oldNormal) >= mix(0.999, 0.9, roughness * roughness)
                                    && abs(oldNormalRoughness.z - roughness) <= 0.02
                                    && dot(view, previousView) >= mix(0.9995, 0.9, roughness * roughness))
                                {
                                    oldSpecular       += texelFetch(previousProbeSpecular, previous, 0) * weight;
                                    oldSpecularLength += oldNormalRoughness.w * weight;
                                    specularWeight    += weight;
                                }
                            }
                        }
                    }
                }
            }

            bool diffuseSample = sky.a >= 0.0;
            bounce             = hybrid.bounce.x != 0u && diffuseSample ? vec4(bounce.rgb, 1) : vec4(0);
            specular           = hybrid.rejection.y > 0.0 ? specular : vec4(0, 0, 0, 1);
            // probe_specular_resolve.comp marks receivers without a resolved sample by alpha -1.
            bool  specularSample = specular.a >= 0.0;
            float skyLength      = diffuseSample ? 1.0 : 0.0;
            float specularLength = hybrid.rejection.y > 0.0 && specularSample ? 1.0 : 0.0;
            // Probe-space histories already stabilize diffuse lighting. This short second
            // accumulation smooths what still changes per frame (contact samples, probes that move
            // with the camera) without retaining long screen-space trails.
            float diffuseLimit = max(1.0, min(probe.parameters.y, probe.parameters.z > 0.5 ? 2.0 : 4.0));
            if (diffuseWeight >= 0.01)
            {
                oldSky    /= diffuseWeight;
                oldBounce /= diffuseWeight;
                if (sourceWeight >= 0.01)
                {
                    oldSourceSky       /= sourceWeight;
                    float sourceLimit   = max(1.0, min(probe.parameters.y, probe.parameters.z > 0.5 ? 2.0 : 64.0));
                    float sourceLength  = min(oldSourceSky.a + (sourceSample ? 1.0 : 0.0), sourceLimit);
                    sourceSky = vec4(sourceSample ? mix(oldSourceSky.rgb, sourceSky.rgb, 1.0 / sourceLength) : oldSourceSky.rgb,
                                     sourceLength);
                }
                skyLength = min(oldSkyLength / diffuseWeight + (diffuseSample ? 1.0 : 0.0), diffuseLimit);
                sky       = diffuseSample ? mix(oldSky, sky, 1.0 / skyLength) : oldSky;
                if (!diffuseSample && hybrid.bounce.x != 0u && hybrid.bounce.y != 0u)
                {
                    bounce = oldBounce;
                }
                else if (diffuseSample && hybrid.bounce.x != 0u && hybrid.bounce.y != 0u)
                {
                    float bounceLimit = hybrid.bounce.z != 0u ? min(diffuseLimit, 2.0) : diffuseLimit;
                    bounce.a          = min(oldBounce.a + 1.0, bounceLimit);
                    bounce.rgb        = mix(oldBounce.rgb, bounce.rgb, 1.0 / bounce.a);
                }
            }
            // Visibility and reflected radiance share one history length. Reset both
            // when the reflection cache resets so fresh RGB never inherits an old count.
            bool specularHistoryValid = hybrid.reflection.x == 0u || hybrid.reflection.y != 0u;
            if (specularWeight >= 0.01 && hybrid.rejection.y > 0.0 && specularHistoryValid)
            {
                oldSpecular /= specularWeight;
                float limit  = max(1.0, min(probe.parameters.y, float(hybrid.sampling.z)));
                limit        = min(limit, mix(4.0, limit, smoothstep(0.04, 0.4, roughness)));
                if (probe.parameters.z > 0.5 || hybrid.reflection.z != 0u)
                {
                    limit = min(limit, 8.0);
                }
                if (specularSample)
                {
                    specularLength = min(oldSpecularLength / specularWeight + 1.0, limit);
                    specular.a     = mix(oldSpecular.a, specular.a, 1.0 / specularLength);
                    if (hybrid.reflection.x != 0u && hybrid.reflection.y != 0u)
                    {
                        specular.rgb = mix(oldSpecular.rgb, specular.rgb, 1.0 / specularLength);
                    }
                }
                else
                {
                    // No new sample: hold the history without lengthening it.
                    specularLength = min(oldSpecularLength / specularWeight, limit);
                    specular       = oldSpecular;
                }
            }
            if (specular.a < 0.0)
            {
                // Neither a sample nor history: the probes' diffuse sky visibility stands in for the
                // lobe's, since the unoccluded environment made such receivers shine. Zero length keeps
                // the next frame from reusing it as a sample.
                specular = vec4(0, 0, 0, sky.a);
            }
            if (hybrid.reflection.x == 0u)
            {
                specular.rgb = vec3(0);
            }
            sky                    = vec4(max(sky.rgb + sourceSky.rgb, vec3(0)), clamp(sky.a, 0.0, 1.0));
            bounce.rgb             = max(bounce.rgb, vec3(0));
            specular               = vec4(max(specular.rgb, vec3(0)), clamp(specular.a, 0.0, 1.0));
            positionHistory        = vec4(position, skyLength);
            normalRoughnessHistory = vec4(EncodeGBufferNormal(normal), roughness, specularLength);
        }
        imageStore(nextProbeSky, pixel, sky);
        imageStore(nextProbeSourceSky, pixel, sourceSky);
        imageStore(nextProbeBounce, pixel, bounce);
        imageStore(nextProbeSpecular, pixel, specular);
        imageStore(nextProbePosition, pixel, positionHistory);
        imageStore(nextProbeSurface, pixel, uvec4(surface, 0, 0));
        imageStore(nextProbeNormalRoughness, pixel, normalRoughnessHistory);
    }
}
