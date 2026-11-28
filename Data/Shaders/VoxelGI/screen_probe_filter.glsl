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
// Per probe: the light the young-probe limit removed from each output coefficient, then the total
// weight with which its coplanar neighbours receive it (screen_probe_spread.comp).
layout(std430, set = 1, binding = 99) writeonly buffer ScreenProbeClipped
{
    vec4 clippedProbeLighting[];
};
#endif
#ifdef PROBE_MOVING_FILTER
#    define PROBE_SIGNALS 3u
#    define SignalValues  vec3
#else
#    define PROBE_SIGNALS 2u
#    define SignalValues  vec2
#endif

// Bit i selects a tile offset of magnitude i. At the minimum supported spacing
// the 192-pixel footprint covers 24 tiles on each side.
uint ProbeFilterSupport(float radius, float age)
{
    int reach = min(int(ceil(radius)), 12);
    if (radius <= 12.0)
    {
        return (1u << (reach + 1)) - 1u;
    }
    float cold   = 1.0 - smoothstep(1.5, 8.0, age);
    float extra  = radius - float(reach);
    float stride = 1.0 + (1.0 - cold) * extra / float(reach);
    float outer  = cold * extra / max(float(reach - 3), 1.0);
    uint  mask   = 1u;
    for (int tap = 1; tap <= reach; ++tap)
    {
        float t  = float(tap);
        mask    |= 1u << uint(round(t * stride + max(t - 3.0, 0.0) * outer));
    }
    return mask;
}
uint SignalStatistics(uint signal)
{
    return signal < 2u ? 8u + signal : kProbeMovingStatistics;
}
uint RawCoefficient(uint item)
{
    return item < 8u ? item : item + 4u;
}

vec4 ProbeFilterInput(uint index, uint item)
{
#ifdef PROBE_FILTER_VERTICAL
    return horizontalLighting[index * (4u * PROBE_SIGNALS) + item];
#else
    return probeLighting[ProbeLightingAddress(index, RawCoefficient(item))];
#endif
}
// The output coefficients for one scale per signal: normalized, and in the vertical pass with the normal
// response separated from the anchor and the moving signal merged into the bounce.
void ProbeFilterFinish(vec4         sums[4u * PROBE_SIGNALS],
                       SignalValues scale,
                       SignalValues total,
                       vec3         normal,
                       out vec4     finished[4u * PROBE_SIGNALS])
{
    for (uint signal = 0u; signal < PROBE_SIGNALS; ++signal)
    {
        uint  base          = signal * 4u;
        float normalization = scale[signal] / total[signal];
        vec4  anchor        = sums[base + 3u] * normalization;
        for (uint channel = 0u; channel < 3u; ++channel)
        {
            vec4 filtered = sums[base + channel] * normalization;
#ifdef PROBE_FILTER_VERTICAL
            filtered         = ProbeNormalResponse(filtered, normal);
            anchor[channel] -= max(dot(filtered.xyz, normal), 0.0);
#endif
            finished[base + channel] = filtered;
        }
        finished[base + 3u] = anchor;
    }
#if defined(PROBE_FILTER_VERTICAL) && defined(PROBE_MOVING_FILTER)
    for (uint item = 0u; item < 4u; ++item)
    {
        finished[4u + item].rgb += finished[8u + item].rgb;
        if (item != 3u)
        {
            finished[4u + item].w += finished[8u + item].w;
        }
    }
#endif
}
layout(local_size_x = 64) in;
void main()
{
    uint index = ProbeInvocationIndex(64u);
    if (index < screenProbe.grid.w)
    {
        ScreenProbe probe = probes[index];
        vec4        result[4u * PROBE_SIGNALS];
        vec4        statistics[PROBE_SIGNALS];
#ifdef PROBE_FILTER_VERTICAL
        SignalValues gatherNoise;
#endif
        SignalValues total;
        SignalValues groupLuma[3], groupWeight[3];
        SignalValues firstOffset = SignalValues(0), secondOffset = SignalValues(0);
        vec3         offsetIrradiance[PROBE_SIGNALS];
        // Brightest nearest probe on any surface, evaluated at this probe's normal (negative: none).
        SignalValues nearestPeak = SignalValues(-1.0);
        for (int group = 0; group < 3; ++group)
        {
            groupLuma[group] = groupWeight[group] = SignalValues(0);
        }
        for (uint signal = 0u; signal < PROBE_SIGNALS; ++signal)
        {
            offsetIrradiance[signal] = vec3(0);
            statistics[signal]       = probeLighting[ProbeLightingAddress(index, SignalStatistics(signal))];
#ifdef PROBE_FILTER_VERTICAL
            // Gather needs the temporal moments before the spatial filter replaces their mean.
            vec4 raw            = statistics[signal];
            gatherNoise[signal] = max(raw.w, raw.y - raw.x * raw.x) / max(raw.z, 1.0);
#endif
            total[signal]          = max(probeLighting[ProbeLightingAddress(index, RawCoefficient(signal * 4u + 3u))].w, 1.0);
            float centerLuma       = HybridLuminance(ProbeFilterInput(index, signal * 4u + 3u).rgb);
            statistics[signal].x   = centerLuma;
            groupWeight[0][signal] = total[signal];
            groupLuma[0][signal]   = centerLuma * total[signal];
        }
#ifdef PROBE_FILTER_VERTICAL
        float age = min(statistics[0].z, statistics[1].z), change = 0.0;
        if (screenProbe.localResponse != 0u)
        {
            float sky    = unpackHalf2x16(floatBitsToUint(probeLighting[ProbeLightingAddress(index, kProbeSource)].x)).x;
            float bounce = unpackHalf2x16(floatBitsToUint(probeLighting[ProbeLightingAddress(index, kProbeSource + 1u)].x)).x;
            change       = max(sky, bounce);
        }
#    ifdef PROBE_MOVING_FILTER
        gatherNoise.y += gatherNoise.z;
        if (gatherNoise.z > 0.0)
        {
            age = min(age, statistics[2].z);
        }
        change = max(change, probeLighting[ProbeLightingAddress(index, kProbeMovingResponse)].x);
#    endif
        filteredProbeLighting[ProbeGatherStatisticsAddress(index)] = vec4(gatherNoise.xy, age, change);
#endif
        for (uint item = 0u; item < 4u * PROBE_SIGNALS; ++item)
        {
            result[item] = ProbeFilterInput(index, item) * total[item / 4u];
        }
        if (screenProbe.control.w != 0.0 && probe.position.w != 0.0)
        {
            ivec2        tile = ivec2(probe.source.yz / screenProbe.spacing);
            SignalValues sampleAge;
            for (uint signal = 0u; signal < PROBE_SIGNALS; ++signal)
            {
                sampleAge[signal] = ProbeSampleAge(statistics[signal].z, total[signal]);
            }
            // Young probes need a wider footprint. Fade outer rings continuously as
            // samples accumulate instead of changing the image at an age threshold.
            SignalValues radii = clamp(512.0 * inversesqrt(max(sampleAge, SignalValues(1))), SignalValues(8), SignalValues(192))
                               / float(screenProbe.spacing);
            float        radius = max(radii.x, radii.y);
#ifdef PROBE_MOVING_FILTER
            // A moving signal that stayed dark through a mature history needs no wider footprint. A young
            // probe whose current rays missed a small lit patch still borrows from its lit neighbors.
            vec4 movingHistory = probeLighting[ProbeLightingAddress(index, SignalStatistics(2u))];
            if (movingHistory.x <= 0.0 && movingHistory.z >= 16.0)
            {
                radii.z = min(radii.z, 2.0);
            }
            radius = max(radius, radii.z);
#endif
            if (screenProbe.localResponse == 0u)
            {
                radii = SignalValues(radius);
            }
            float        cold   = 1.0 - smoothstep(1.5, 8.0, min(sampleAge.x, sampleAge.y));
            int          reach  = min(int(ceil(radius)), 12);
            SignalValues spread = .5 * (radii + 1.0) * (radii + 1.0);
            // A sparse uniform stride can skip every tile next to a new receiver. Put the
            // first three taps one tile apart while cold, distributing the spare width outside.
            float extra      = max(radius - float(reach), 0.0);
            float stride     = 1.0 + (1.0 - cold) * extra / float(reach);
            float outer      = cold * extra / max(float(reach - 3), 1.0);
            float coldSky    = screenProbe.localResponse == 0u ? cold : 1.0 - smoothstep(1.5, 8.0, sampleAge.x);
            float skyFalloff = mix(.5, .10, coldSky);
            uint  support[PROBE_SIGNALS];
            bool  local    = screenProbe.localResponse != 0u;
            uint  combined = 0u;
            for (uint signal = 0u; signal < PROBE_SIGNALS; ++signal)
            {
                support[signal]  = local ? ProbeFilterSupport(radii[signal], sampleAge[signal]) : 0xffffffffu;
                combined        |= support[signal];
            }
            int lastTap = local ? int(ceil(radius)) : reach;
            for (int tap = -lastTap; tap <= lastTap; ++tap)
            {
                float distance = abs(float(tap));
                int  offset = local ? tap : int(sign(float(tap)) * round(distance * stride + max(distance - 3.0, 0.0) * outer));
                uint bit    = 1u << uint(abs(offset));
                if ((combined & bit) == 0u)
                {
                    continue;
                }
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
                        if ((local || min(statistics[0].z, statistics[1].z) < 192.0) && abs(offset) <= 2 && other != index
                            && adjacent.position.w != 0.0)
                        {
                            vec3 delta = ProbeNormalOffset(adjacent.normal.xyz, probe.normal.xyz);
                            for (uint signal = 0u; signal < PROBE_SIGNALS; ++signal)
                            {
                                if (local && (statistics[signal].z >= 192.0 || (support[signal] & bit) == 0u))
                                {
                                    continue;
                                }
                                vec3 level = ProbeFilterInput(other, signal * 4u + 3u).rgb;
                                for (uint channel = 0u; channel < 3u; ++channel)
                                {
                                    level[channel] += dot(ProbeFilterInput(other, signal * 4u + channel).yzw, delta);
                                }
                                nearestPeak[signal] = max(nearestPeak[signal], HybridLuminance(max(level, vec3(0))));
                            }
                        }
                        float spatial = ProbePlaneWeight(probe.position.xyz, probe.normal.xyz, adjacent)
                                      * float(other != index && probe.source.w == adjacent.source.w);
                        if (spatial > 0.0)
                        {
                            for (uint signal = 0u; signal < PROBE_SIGNALS; ++signal)
                            {

                                if ((support[signal] & bit) == 0u)
                                {
                                    continue;
                                }
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
                                vec4  b = probeLighting[ProbeLightingAddress(other, SignalStatistics(signal))];
                                float variance =
                                    max(a.w, a.y - a.x * a.x) / max(a.z, 1.0) + max(b.w, b.y - b.x * b.x) / max(b.z, 1.0);
                                float difference = dot(value[3].rgb, vec3(.2126, .7152, .0722)) - a.x;
                                // A fresh probe has few rays; a stable neighbor can have
                                // thousands. Preserve resolved lighting edges while using that
                                // mature history to repair uncertain newly exposed receivers.
                                // Count samples, not frames: a young probe's sun and sky estimates
                                // resolve within a few frames and must not spread across sun shadows.
                                // Full edge stopping from 32 sample frames: a later ramp kept newly
                                // exposed probes blurred across lighting edges for over two seconds.
                                float batches = probeLighting[ProbeLightingAddress(other, RawCoefficient(signal * 4u + 3u))].w;
                                float confidence =
                                    clamp((min(sampleAge[signal], ProbeSampleAge(b.z, batches)) - 4.0) / 28.0, 0.0, 1.0);
                                if (signal == 0u)
                                {
                                    confidence = max(confidence, coldSky);
                                }
                                float edge   = mix(1.0, exp(-difference * difference / max(8.0 * variance, 1e-12)), confidence);
                                float kernel = exp(-float(offset * offset) / spread[signal])
                                             * clamp(radii[signal] + 1.0 - float(abs(offset)), 0.0, 1.0);
                                float weight = spatial * kernel * max(batches, 1.0) * edge;
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
                                int  ordinal = local ? sign(offset) * bitCount(support[signal] & (bit - 1u)) : tap;
                                uint group   = uint(ordinal + 24) % 3u;
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
        SignalValues scale = SignalValues(1);
        for (uint signal = 0u; signal < PROBE_SIGNALS; ++signal)
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
            vec3 w           = vec3(groupWeight[0][signal], groupWeight[1][signal], groupWeight[2][signal]);
            vec3 luma        = vec3(groupLuma[0][signal], groupLuma[1][signal], groupLuma[2][signal]);
#ifdef PROBE_FILTER_VERTICAL
            float young = 1.0 - smoothstep(32.0, 192.0, statistics[signal].z);
#else
            // Only the vertical pass limits young estimates, so that one pass can hand the removed
            // light to the neighbours. Limiting both passes also lost twice the light.
            float young = 0.0;
#endif
            scale[signal] =
                ProbeSpatialOutlierScale(luma, w, total[signal], HybridLuminance(result[item].rgb) / total[signal], young);
            // The interleaved test needs coplanar support, which a probe on a narrow face or thin
            // detail lacks: its one-frame estimate would otherwise reach the screen unfiltered.
            // Use adjacent probes as a ceiling while the isolated receiver is new.
            float isolated = ProbeIsolatedWindow(probe, young);
            if (isolated > 0.0 && nearestPeak[signal] >= 0.0 && min(min(w.x, w.y), w.z) <= .05 * total[signal])
            {
                float level   = HybridLuminance(result[item].rgb) / total[signal];
                float limit   = kIsolatedProbeSlack * nearestPeak[signal];
                scale[signal] = min(scale[signal], mix(1.0, clamp(limit / max(level, 1e-20), 0.0, 1.0), isolated));
            }
        }
        vec4 kept[4u * PROBE_SIGNALS];
        ProbeFilterFinish(result, scale, total, probe.normal.xyz, kept);
#ifdef PROBE_FILTER_VERTICAL
        const uint stride = ZEN_SCREEN_PROBE_FILTER_COEFFICIENTS;
        // Record the light the limit removed, so that the coplanar base probes around this one receive it
        // instead of it being lost. A probe without such neighbours keeps the old, lossy limit.
        float outgoing = 0.0;
        if (any(lessThan(scale, SignalValues(1))) && index < screenProbe.grid.z && probe.position.w != 0.0)
        {
            for (int y = -kProbeSpreadReach; y <= kProbeSpreadReach; ++y)
            {
                for (int x = -kProbeSpreadReach; x <= kProbeSpreadReach; ++x)
                {
                    uint other = x != 0 || y != 0 ? ProbeSpreadNeighbor(index, ivec2(x, y)) : 0xffffffffu;
                    if (other != 0xffffffffu)
                    {
                        outgoing += ProbeSpreadWeight(probes[other], probe, ivec2(x, y));
                    }
                }
            }
        }
        outgoing         = outgoing >= kProbeSpreadMinimumWeight ? outgoing : 0.0;
        uint clippedBase = index * (stride + 1u);
        vec4 full[4u * PROBE_SIGNALS];
        if (outgoing > 0.0)
        {
            ProbeFilterFinish(result, SignalValues(1), total, probe.normal.xyz, full);
        }
        for (uint item = 0u; item < stride; ++item)
        {
            clippedProbeLighting[clippedBase + item] = outgoing > 0.0 ? full[item] - kept[item] : vec4(0);
        }
        clippedProbeLighting[clippedBase + stride] = vec4(outgoing, 0.0, 0.0, 0.0);
#else
        const uint stride = 4u * PROBE_SIGNALS;
#endif
        for (uint item = 0u; item < stride; ++item)
        {
            filteredProbeLighting[index * stride + item] = kept[item];
        }
    }
}
