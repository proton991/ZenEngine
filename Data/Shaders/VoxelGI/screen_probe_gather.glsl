#include "screen_probe.glsl"
#include "gi_common.glsl"
#ifndef SCREEN_PROBE_DIFFUSE_ONLY
#    include "screen_probe_specular.glsl"
layout(set = 1, binding = 7) uniform samplerCube coneEnvironmentMap;
#    define GLOSSY_FILTER_READ
#    include "glossy_reflection.glsl"
#endif
layout(std430, set = 1, binding = 87) readonly buffer FilteredScreenProbeLighting
{
    vec4 filteredProbeLighting[];
};
layout(set = 1, binding = 88, rgba16f) writeonly uniform image2D probeSky;
layout(set = 1, binding = 89, rgba16f) writeonly uniform image2D probeBounce;
layout(set = 1, binding = 90, rgba16f) writeonly uniform image2D probeSpecular;
layout(local_size_x = ZEN_SCREEN_PROBE_SPACING, local_size_y = ZEN_SCREEN_PROBE_SPACING) in;
// The cubic B-spline has support over two grid spacings and a continuous
// derivative at every cell boundary. All eligible probes use the same kernel.
vec2 ProbeCubicWeight(vec2 distance)
{
    vec2 outer = max(2.0 - distance, vec2(0));
    vec2 inner = max(1.0 - distance, vec2(0));
    return (outer * outer * outer - 4.0 * inner * inner * inner) / 6.0;
}
shared ScreenProbe gatherProbes[98];
shared vec2        gatherPositions[98];
shared uint        gatherIndices[98];
shared uint        gatherMask[4];
void               ProbeLoadGatherFootprints()
{
    uint lane = gl_LocalInvocationIndex;
    if (lane < 4u)
    {
        gatherMask[lane] = 0u;
    }
    barrier();
    ivec2 owner      = ivec2(gl_WorkGroupID.xy);
    vec2  tileCenter = (vec2(owner) + .5) * float(screenProbe.spacing);
    for (uint tap = lane; tap < 98u; tap += gl_WorkGroupSize.x * gl_WorkGroupSize.y)
    {
        uint  cell       = tap / 2u;
        ivec2 coordinate = owner + ivec2(int(cell % 7u) - 3, int(cell / 7u) - 3);
        if (all(greaterThanEqual(coordinate, ivec2(0))) && all(lessThan(coordinate, ivec2(screenProbe.grid.xy))))
        {
            uint        base  = uint(coordinate.x) + uint(coordinate.y) * screenProbe.grid.x;
            uint        index = (tap & 1u) == 0u ? base : screenProbe.grid.z + base / screenProbe.adaptiveTiles;
            ScreenProbe p     = probes[index];
            if (p.position.w != 0.0 && ((tap & 1u) == 0u || all(equal(ivec2(p.source.yz / screenProbe.spacing), coordinate))))
            {
                vec2 pixel = ProbeProject(p.position.xyz, screenProbe.projectionView);
                if (all(lessThan(abs(pixel - tileCenter), vec2(2.5 * float(screenProbe.spacing)))))
                {
                    gatherProbes[tap]    = p;
                    gatherPositions[tap] = pixel;
                    gatherIndices[tap]   = index;
                    atomicOr(gatherMask[tap / 32u], 1u << (tap % 32u));
                }
            }
        }
    }
    barrier();
}
void main()
{
    ProbeLoadGatherFootprints();
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (HybridInView(pixel))
    {
        vec3   sky = vec3(0), bounce = vec3(0);
        vec4   reflected = vec4(0);
        float  total = 0.0, age = 0.0;
        vec2   first = vec2(0), lower = vec2(1e20), upper = vec2(-1e20);
        vec3   second     = vec3(0);
        vec2   lumaSecond = vec2(0), measuredNoise = vec2(0);
        mat2x3 skyMoment = mat2x3(0), bounceMoment = mat2x3(0);
#ifndef SCREEN_PROBE_DIFFUSE_ONLY
        // Below the blend band the glossy reflection replaces the probe estimate entirely.
        // Resolve it first, so those pixels skip the probe GGX reads of every tap.
        GIDenoiseGuide guide        = DenoiseGuide(pixel);
        vec4           glossyValue  = vec4(0);
        float          glossyWeight = 0.0;
        if (GlossyReceiver(guide))
        {
            vec2  coordinate = GlossyCoordinate(vec2(pixel));
            ivec2 base       = ivec2(floor(coordinate));
            vec2  fraction   = fract(coordinate);
            for (uint tap = 0u; tap < 4u; ++tap)
            {
                ivec2          q         = clamp(base + ivec2(tap & 1u, tap >> 1u), ivec2(0), ivec2(glossy.extent.xy) - 1);
                uint           index     = GlossyIndex(q);
                GlossyHistory  history   = glossyCurrent[index];
                GIDenoiseGuide other     = DenoiseUnpackGuide(history.position, history.normal);
                vec2           bilinear  = mix(vec2(1) - fraction, fraction, vec2(tap & 1u, tap >> 1u));
                float          w         = bilinear.x * bilinear.y;
                w                       *= DenoiseSurfaceWeight(guide, other) * DenoiseLobeWeight(guide, other);
                glossyValue             += glossyFiltered[index] * w;
                glossyWeight            += w;
            }
        }
        float probeBlend       = smoothstep(glossy.control.z - .05, glossy.control.z, guide.roughness);
        bool  useProbeSpecular = probeBlend > 0.0;
        float specularWeight = 0.0, strongestWeight = 0.0;
        uint  strongest = 0u;
#endif
        if (texelFetch(receiverDepth, pixel, 0).r < 1.0)
        {
            vec3 position = HybridPosition(pixel), normal = HybridNormal(pixel), geometric = HybridGeometricNormal(pixel);
            uint identity = texelFetch(receiverSurface, pixel, 0).g;
#ifndef SCREEN_PROBE_DIFFUSE_ONLY
            vec3 viewDirection =
                normalize(sceneUbo.viewPosition.w < .5 ? sceneUbo.lightInfo.yzw : sceneUbo.viewPosition.xyz - position);
            vec3  reflectionDirection = reflect(-viewDirection, normal);
            float roughness           = texelFetch(receiverRoughness, pixel, 0).g;
#endif
            // A reprojected adaptive anchor may lie outside its owning tile.
            // Three surrounding rings contain all anchors whose cubic reaches this pixel.
            for (uint word = 0u; word < 4u; ++word)
            {
                uint mask = gatherMask[word];
                while (mask != 0u)
                {
                    uint tap           = word * 32u + uint(findLSB(mask));
                    mask              &= mask - 1u;
                    ScreenProbe probe  = gatherProbes[tap];
                    if (probe.source.w != identity)
                    {
                        continue;
                    }
                    uint  chosen       = gatherIndices[tap];
                    vec2  displacement = gatherPositions[tap] - (vec2(pixel) + .5);
                    vec2  axisWeight   = ProbeCubicWeight(abs(displacement) / float(screenProbe.spacing));
                    float spatial      = axisWeight.x * axisWeight.y;
                    float weight       = spatial * ProbePlaneWeight(position, geometric, probe);
                    if (weight <= 0.0)
                    {
                        continue;
                    }
                    float normalOffset = dot(probe.normal.xyz, normal) - 1.0;
                    vec3  irradiance[2];
                    for (uint signal = 0u; signal < 2u; ++signal)
                    {
                        uint address       = chosen * ZEN_SCREEN_PROBE_FILTER_COEFFICIENTS + signal * 4u;
                        irradiance[signal] = filteredProbeLighting[address + 3u].rgb;
                        for (uint channel = 0u; channel < 3u; ++channel)
                        {
                            vec4 response                = filteredProbeLighting[address + channel];
                            irradiance[signal][channel] += max(dot(response.xyz, normal), 0.0) + response.w * normalOffset;
                        }
                    }
                    vec2 delta      = displacement / float(screenProbe.spacing);
                    first          += weight * delta;
                    second         += weight * vec3(delta.x * delta.x, delta.x * delta.y, delta.y * delta.y);
                    lower           = min(lower, delta);
                    upper           = max(upper, delta);
                    vec3 skyTap     = weight * max(irradiance[0], vec3(0));
                    vec3 bounceTap  = weight * max(irradiance[1], vec3(0));
                    sky            += skyTap;
                    bounce         += bounceTap;
                    skyMoment      += mat2x3(delta.x * skyTap, delta.y * skyTap);
                    bounceMoment   += mat2x3(delta.x * bounceTap, delta.y * bounceTap);
                    total          += weight;
                    vec2 luma =
                        vec2(HybridLuminance(max(irradiance[0], vec3(0))), HybridLuminance(max(irradiance[1], vec3(0))));
                    vec3 statistics =
                        filteredProbeLighting[screenProbe.grid.w * ZEN_SCREEN_PROBE_FILTER_COEFFICIENTS + chosen].xyz;
                    lumaSecond    += weight * luma * luma;
                    measuredNoise += weight * statistics.xy;
                    age           += weight * statistics.z;
#ifndef SCREEN_PROBE_DIFFUSE_ONLY
                    // The cubic puts about 92% of a pixel's weight on the probes within one
                    // spacing. Rough reflections vary smoothly enough to take them from those
                    // alone, saving the GGX reads of the outer ring.
                    if (useProbeSpecular && all(lessThan(abs(delta), vec2(1.0))))
                    {
                        reflected      += ProbeSpecular(chosen, reflectionDirection, roughness) * weight;
                        specularWeight += weight;
                    }
                    if (weight > strongestWeight)
                    {
                        strongestWeight = weight;
                        strongest       = chosen;
                    }
#endif
                }
            }
#ifndef SCREEN_PROBE_DIFFUSE_ONLY
            if (useProbeSpecular && specularWeight <= 0.0 && strongestWeight > 0.0)
            {
                reflected      = ProbeSpecular(strongest, reflectionDirection, roughness);
                specularWeight = 1.0;
            }
            // The material adds the unoccluded environment times the measured escape weight.
            // Keep the signed difference from that approximation, so the base lobe reconstructs
            // the traced sky while other material lobes can still use directional environment light.
            if (specularWeight > 0.0 && gi.lighting.y != 0.0)
            {
                float lod          = roughness * float(textureQueryLevels(coneEnvironmentMap) - 1);
                vec3  environment  = textureLod(coneEnvironmentMap, EnvironmentDirection(reflectionDirection), lod).rgb;
                reflected.rgb     -= reflected.a * environment * (sceneUbo.environment.x * sceneUbo.environment.z);
            }
#endif
        }
        sky    /= max(total, 1e-20);
        bounce /= max(total, 1e-20);
        if (total > 0.0)
        {
            // The normalized tent reproduces a constant. At an irregular set of
            // anchors its centroid can miss the receiver, even on one flat surface.
            // Fit the spatial slope and evaluate it at the receiver's own position.
            vec2 center     = first / total;
            vec3 covariance = second / total - vec3(center.x * center.x, center.x * center.y, center.y * center.y);
            // Area of a one-pixel box supplies its position variance along each axis.
            float pixelVariance = 1.0 / (12.0 * float(screenProbe.spacing * screenProbe.spacing));
            covariance.xz       = max(covariance.xz, vec2(0));
            float spread        = length(vec2(covariance.x - covariance.z, 2.0 * covariance.y));
            vec2  eigen         = max(.5 * (covariance.x + covariance.z + vec2(spread, -spread)), vec2(0));
            // Remove null-space components along a line or at a lone anchor.
            // A slope in that direction cannot be observed from these samples.
            vec2 inverse = eigen / (eigen * eigen + pixelVariance * pixelVariance);
            mat2 inverseCovariance =
                mat2(inverse.y)
                + ((inverse.x - inverse.y) / max(spread, 1e-12)) * (mat2(covariance.xy, covariance.yz) - mat2(eigen.y));
            vec2 slope = inverseCovariance * center;
            // The corrected tap weight is w * (1 - dot(slope, delta - center)).
            // Keep every supported weight nonnegative, including one-sided footprints.
            vec2 furthest  = mix(lower, upper, greaterThanEqual(slope, vec2(0)));
            slope         /= max(1.0, dot(slope, furthest - center));
            // A moving footprint also sees noise and shadow boundaries. Estimate
            // a spatial gradient only to the extent that the local linear fit
            // explains measured radiance variation above the probes' uncertainty.
            vec2 mean            = vec2(HybridLuminance(sky), HybridLuminance(bounce));
            vec2 spatialVariance = max(lumaSecond / total - mean * mean, vec2(0));
            vec2 skyCov          = vec2(HybridLuminance(skyMoment[0]), HybridLuminance(skyMoment[1])) / total - mean.x * center;
            vec2 bounceCov = vec2(HybridLuminance(bounceMoment[0]), HybridLuminance(bounceMoment[1])) / total - mean.y * center;
            vec2 explained = vec2(dot(skyCov, inverseCovariance * skyCov), dot(bounceCov, inverseCovariance * bounceCov));
            vec2 confidence = clamp((explained - measuredNoise / total) / max(spatialVariance, vec2(1e-20)), vec2(0), vec2(1));
            float shift     = dot(center, slope);
            sky             = max(sky + confidence.x * (sky * shift - (skyMoment * slope) / total), vec3(0));
            bounce          = max(bounce + confidence.y * (bounce * shift - (bounceMoment * slope) / total), vec3(0));
        }

        imageStore(probeSky, pixel, vec4(sky, age / max(total, 1e-20)));
        imageStore(probeBounce, pixel, vec4(bounce * gi.volume.w, age / max(total, 1e-20)));
#ifdef SCREEN_PROBE_DIFFUSE_ONLY
        imageStore(probeSpecular, pixel, vec4(0, 0, 0, 1));
#else
        bool hasProbe  = specularWeight > 0.0;
        bool hasGlossy = glossyWeight > .001;
        if (hasProbe)
        {
            reflected /= specularWeight;
        }
        if (hasGlossy)
        {
            vec4 glossySample  = glossyValue / glossyWeight;
            glossySample.rgb  *= hybrid.reflection.x != 0u ? gi.volume.w : 0.0;
            reflected          = hasProbe ? mix(glossySample, reflected, probeBlend) : glossySample;
        }
        // A thin receiver can fall between both sampling grids. Mark it for a
        // reflection ray at this pixel. Only a traced miss can establish open sky.
        if (!hasProbe && !hasGlossy)
        {
            reflected.a = guide.identity != 0u && hybrid.rejection.y != 0.0 ? -1.0 : 1.0;
        }
        imageStore(probeSpecular, pixel, reflected);
#endif
    }
}
