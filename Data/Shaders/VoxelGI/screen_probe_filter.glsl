#define SCREEN_PROBE_COMPUTE
#include "screen_probe.glsl"
layout(std430, set = 1, binding = 87) writeonly buffer FilteredScreenProbeLighting
{
    vec4 filteredProbeLighting[];
};
#ifdef PROBE_FILTER_VERTICAL
layout(std430, set = 1, binding = 115) readonly buffer HorizontalScreenProbeLighting
{
    vec4 horizontalLighting[];
};
#endif
vec4 ProbeFilterInput(uint index, uint item)
{
#ifdef PROBE_FILTER_VERTICAL
    return horizontalLighting[index * ZEN_SCREEN_PROBE_FILTER_COEFFICIENTS + item];
#else
    return probeLighting[index * ZEN_SCREEN_PROBE_LIGHTING_VECTORS + item];
#endif
}
layout(local_size_x = 64) in;
void main()
{
    uint index = ProbeInvocationIndex(64u);
    if (index < screenProbe.grid.w)
    {
        ScreenProbe probe = probes[index];
        vec4        result[8];
        vec4        statistics[2];
#ifdef PROBE_FILTER_VERTICAL
        vec2 gatherNoise;
#endif
        vec2 total;
        vec2 groupLuma[3], groupWeight[3];
        vec2 firstOffset = vec2(0), secondOffset = vec2(0);
        vec3 offsetIrradiance[2] = {vec3(0), vec3(0)};
        // Brightest nearest probe on any surface, evaluated at this probe's normal (negative: none).
        vec2 nearestPeak = vec2(-1.0);
        for (int group = 0; group < 3; ++group)
        {
            groupLuma[group] = groupWeight[group] = vec2(0);
        }
        for (uint signal = 0u; signal < 2u; ++signal)
        {
            statistics[signal] = probeLighting[index * ZEN_SCREEN_PROBE_LIGHTING_VECTORS + 8u + signal];
#ifdef PROBE_FILTER_VERTICAL
            // Gather needs the temporal moments before the spatial filter replaces their mean.
            vec4 raw            = statistics[signal];
            gatherNoise[signal] = max(raw.w, raw.y - raw.x * raw.x) / max(raw.z, 1.0);
#endif
            total[signal]          = max(probeLighting[index * ZEN_SCREEN_PROBE_LIGHTING_VECTORS + signal * 4u + 3u].w, 1.0);
            float centerLuma       = HybridLuminance(ProbeFilterInput(index, signal * 4u + 3u).rgb);
            statistics[signal].x   = centerLuma;
            groupWeight[0][signal] = total[signal];
            groupLuma[0][signal]   = centerLuma * total[signal];
        }
#ifdef PROBE_FILTER_VERTICAL
        filteredProbeLighting[screenProbe.grid.w * ZEN_SCREEN_PROBE_FILTER_COEFFICIENTS + index] =
            vec4(gatherNoise, min(statistics[0].z, statistics[1].z), 0);
#endif
        for (uint item = 0u; item < 8u; ++item)
        {
            result[item] = ProbeFilterInput(index, item) * total[item / 4u];
        }
        if (screenProbe.control.w != 0.0 && probe.position.w != 0.0)
        {
            ivec2 tile      = ivec2(probe.source.yz / screenProbe.spacing);
            vec2  sampleAge = vec2(ProbeSampleAge(statistics[0].z, total[0]), ProbeSampleAge(statistics[1].z, total[1]));
            // Young probes need a wider footprint. Fade outer rings continuously as
            // samples accumulate instead of changing the image at an age threshold.
            float radiusPixels = clamp(512.0 * inversesqrt(max(min(sampleAge.x, sampleAge.y), 1.0)), 8.0, 192.0);
            float cold         = 1.0 - smoothstep(1.5, 8.0, min(sampleAge.x, sampleAge.y));
            float radius       = radiusPixels / float(screenProbe.spacing);
            int   reach        = min(int(ceil(radius)), 12);
            float spread       = .5 * (radius + 1.0) * (radius + 1.0);
            // A sparse uniform stride can skip every tile next to a new receiver. Put the
            // first three taps one tile apart while cold, distributing the spare width outside.
            float extra      = max(radius - float(reach), 0.0);
            float stride     = 1.0 + (1.0 - cold) * extra / float(reach);
            float outer      = cold * extra / max(float(reach - 3), 1.0);
            float skyFalloff = mix(.5, .10, cold);
            for (int tap = -reach; tap <= reach; ++tap)
            {
                float distance = abs(float(tap));
                int   offset   = int(sign(float(tap)) * round(distance * stride + max(distance - 3.0, 0.0) * outer));
#ifdef PROBE_FILTER_VERTICAL
                ivec2 neighbor = tile + ivec2(0, offset);
#else
                ivec2 neighbor = tile + ivec2(offset, 0);
#endif
                if (all(greaterThanEqual(neighbor, ivec2(0))) && all(lessThan(neighbor, ivec2(screenProbe.grid.xy))))
                {
                    uint base = uint(neighbor.x) + uint(neighbor.y) * screenProbe.grid.x;
                    for (uint kind = 0u; kind < 2u; ++kind)
                    {
                        uint        other    = kind == 0u ? base : screenProbe.grid.z + base / screenProbe.adaptiveTiles;
                        ScreenProbe adjacent = probes[other];
                        if (kind == 1u && any(notEqual(ivec2(adjacent.source.yz / screenProbe.spacing), neighbor)))
                        {
                            continue;
                        }
                        if (min(statistics[0].z, statistics[1].z) < 192.0 && abs(offset) <= 2 && other != index
                            && adjacent.position.w != 0.0)
                        {
                            vec3 delta = ProbeNormalOffset(adjacent.normal.xyz, probe.normal.xyz);
                            for (uint signal = 0u; signal < 2u; ++signal)
                            {
                                vec3 level = ProbeFilterInput(other, signal * 4u + 3u).rgb;
                                for (uint channel = 0u; channel < 3u; ++channel)
                                {
                                    level[channel] += dot(ProbeFilterInput(other, signal * 4u + channel).yzw, delta);
                                }
                                nearestPeak[signal] = max(nearestPeak[signal], HybridLuminance(max(level, vec3(0))));
                            }
                        }
                        float spatial = exp(-float(offset * offset) / spread)
                                      * clamp(radius + 1.0 - float(abs(offset)), 0.0, 1.0)
                                      * ProbePlaneWeight(probe.position.xyz, probe.normal.xyz, adjacent)
                                      * float(other != index && probe.source.w == adjacent.source.w);
                        if (spatial > 0.0)
                        {
                            for (uint signal = 0u; signal < 2u; ++signal)
                            {

                                vec4 value[4];
                                for (uint item = 0u; item < 4u; ++item)
                                {
                                    value[item] = ProbeFilterInput(other, signal * 4u + item);
                                }
                                vec3 delta = ProbeNormalOffset(adjacent.normal.xyz, probe.normal.xyz);
                                for (uint channel = 0u; channel < 3u; ++channel)
                                {
                                    value[3][channel] += dot(value[channel].yzw, delta);
                                }
                                vec4  a = statistics[signal];
                                vec4  b = probeLighting[other * ZEN_SCREEN_PROBE_LIGHTING_VECTORS + 8u + signal];
                                float variance =
                                    max(a.w, a.y - a.x * a.x) / max(a.z, 1.0) + max(b.w, b.y - b.x * b.x) / max(b.z, 1.0);
                                float difference = dot(value[3].rgb, vec3(.2126, .7152, .0722)) - a.x;
                                // A fresh probe has few rays; a stable neighbor can have
                                // thousands. Preserve resolved lighting edges while using that
                                // mature history to repair uncertain newly exposed receivers.
                                // Count samples, not frames: a young probe's sun and sky estimates
                                // resolve within a few frames and must not spread across sun shadows.
                                float batches = probeLighting[other * ZEN_SCREEN_PROBE_LIGHTING_VECTORS + signal * 4u + 3u].w;
                                float confidence =
                                    clamp((min(sampleAge[signal], ProbeSampleAge(b.z, batches)) - 32.0) / 96.0, 0.0, 1.0);
                                if (signal == 0u)
                                {
                                    confidence = max(confidence, cold);
                                }
                                float edge   = mix(1.0, exp(-difference * difference / max(8.0 * variance, 1e-12)), confidence);
                                float weight = spatial * max(batches, 1.0) * edge;
                                // Environment-importance rays resolve local sky visibility even at
                                // disocclusion. Keep their repair footprint close to the receiver.
                                if (signal == 0u)
                                {
                                    weight *= exp(-skyFalloff * float(offset * offset));
                                }
                                firstOffset[signal]      += float(offset) * weight;
                                secondOffset[signal]     += float(offset * offset) * weight;
                                offsetIrradiance[signal] += float(offset) * weight * value[3].rgb;
                                for (uint item = 0u; item < 4u; ++item)
                                {
                                    result[signal * 4u + item] += value[item] * weight;
                                }
                                uint group                  = uint(tap + 24) % 3u;
                                groupWeight[group][signal] += weight;
                                groupLuma[group][signal]   += (difference + a.x) * weight;
                                total[signal]              += weight;
                            }
                        }
                    }
                }
            }
        }
        // Test independent, interleaved estimates of the same surface patch.
        // Reduce an isolated high estimate only on young, well-supported probes.
        vec2 scale = vec2(1);
        for (uint signal = 0u; signal < 2u; ++signal)
        {
            // Unequally aged neighbors move the weighted mean away from the
            // receiver. Fit the local irradiance slope along this filter axis.
            float meanOffset = firstOffset[signal] / total[signal];
            float variance   = max(secondOffset[signal] / total[signal] - meanOffset * meanOffset, 0.0);
            uint  item       = 4u * signal + 3u;
            vec3  mean       = result[item].rgb / total[signal];
            vec3  covariance = offsetIrradiance[signal] / total[signal] - meanOffset * mean;
            vec3  correction = -meanOffset * covariance / (variance + 1.0 + meanOffset * meanOffset);
            result[item].rgb = total[signal] * max(mean + clamp(correction, -.5 * mean, .5 * mean), vec3(0));
            vec3  w          = vec3(groupWeight[0][signal], groupWeight[1][signal], groupWeight[2][signal]);
            vec3  luma       = vec3(groupLuma[0][signal], groupLuma[1][signal], groupLuma[2][signal]);
            float young      = 1.0 - smoothstep(32.0, 192.0, statistics[signal].z);
            scale[signal] =
                ProbeSpatialOutlierScale(luma, w, total[signal], HybridLuminance(result[item].rgb) / total[signal], young);
            // The interleaved test needs coplanar support, which a probe on a narrow face or thin
            // detail lacks: its one-frame estimate would otherwise reach the screen unfiltered.
            // Bound it by its nearest probes on any surface instead, over the same young window: the
            // history keeps the raw estimate, so a shorter bound would only delay the outlier.
            if (young > 0.0 && nearestPeak[signal] >= 0.0 && min(min(w.x, w.y), w.z) <= .05 * total[signal])
            {
                float level   = HybridLuminance(result[item].rgb) / total[signal];
                float limit   = kIsolatedProbeSlack * nearestPeak[signal];
                scale[signal] = min(scale[signal], mix(1.0, clamp(limit / max(level, 1e-20), 0.0, 1.0), young));
            }
        }
        for (uint signal = 0u; signal < 2u; ++signal)
        {
            uint  base          = signal * 4u;
            float normalization = scale[signal] / total[signal];
            vec4  anchor        = result[base + 3u] * normalization;
            for (uint channel = 0u; channel < 3u; ++channel)
            {
                vec4 filtered = result[base + channel] * normalization;
#ifdef PROBE_FILTER_VERTICAL
                filtered = ProbeNormalResponse(filtered, probe.normal.xyz);
                // The anchor projection is constant for all receivers of this probe.
                anchor[channel] -= max(dot(filtered.xyz, probe.normal.xyz), 0.0);
#endif
                filteredProbeLighting[index * ZEN_SCREEN_PROBE_FILTER_COEFFICIENTS + base + channel] = filtered;
            }
            filteredProbeLighting[index * ZEN_SCREEN_PROBE_FILTER_COEFFICIENTS + base + 3u] = anchor;
        }
    }
}
