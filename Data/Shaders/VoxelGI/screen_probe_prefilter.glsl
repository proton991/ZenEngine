#define SCREEN_CDF_READ
#if !defined(SCREEN_PROBE_PREFILTER_HITS) || defined(PROBE_STATIC_HITS)
#    define SCREEN_RAYS_READ
#endif
#define SCREEN_SPECULAR_WRITE
#define SCREEN_PROBE_COMPUTE
#include "screen_probe.glsl"
#include "gi_common.glsl"
#ifdef SCREEN_PROBE_PREFILTER_HITS
#    include "screen_probe_hits.glsl"
#endif
layout(local_size_x = 64) in;
// Each sample's radiance atlas cell and its inverse PDF (zero below the probe's horizon).
shared uint  sampleCells[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
shared float sampleWeights[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
shared vec4  radiance[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
#include "history_response.glsl"
shared float response[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
shared float acceptedAge;
shared float densities[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
#if defined(SCREEN_PROBE_PREFILTER_HITS) && !defined(PROBE_STATIC_HITS)
#    define PROBE_ROUGH_PREDICTION
// Each moving-light hit's change since the history frame, evaluated on the same surface, and its direction.
shared vec3 deltas[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
shared vec3 sampleDirections[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
shared vec2 laneShares[64];
// Sum one value per lane across the 64-lane workgroup; every lane receives the total.
vec2 ProbeReduceLanes(vec2 value, uint lane)
{
    barrier();
    laneShares[lane] = value;
    for (uint stride = 32u; stride != 0u; stride >>= 1u)
    {
        barrier();
        if (lane < stride)
        {
            laneShares[lane] += laneShares[lane + stride];
        }
    }
    barrier();
    return laneShares[0];
}
#endif
// A non-negative value rounded to one of its two neighbouring halves, choosing the farther one with
// probability proportional to its closeness. The stored value is exact on average: a long running
// mean in round-to-nearest halves drops every update below about 1/2048 of the value, so it stops
// converging near any value it once held.
uint StochasticHalf(float value, float random)
{
    uint  nearest  = packHalf2x16(vec2(value, 0)) & 0xffffu;
    float rounded  = unpackHalf2x16(nearest).x;
    uint  neighbor = rounded < value ? nearest + 1u : max(nearest, 1u) - 1u;
    float other    = unpackHalf2x16(neighbor).x;
    float fraction = other != rounded ? (value - rounded) / (other - rounded) : 0.0;
    return random < fraction ? neighbor : nearest;
}
uvec2 StochasticPackHalf(vec4 value, uint seed)
{
    uint first = HybridHash(seed), second = HybridHash(first);
    vec4 random = vec4(first & 0xffffu, first >> 16u, second & 0xffffu, second >> 16u) / 65536.0;
    return uvec2(StochasticHalf(value.r, random.x) | (StochasticHalf(value.g, random.y) << 16u),
                 StochasticHalf(value.b, random.z) | (StochasticHalf(value.a, random.w) << 16u));
}
#ifdef PROBE_ROUGH_FOOTPRINT
// The footprint's mean of one atlas cell over the taps that observe it above their horizon. support is the
// share of the footprint's weight those taps carry.
vec4 ProbeRelocatedRoughHistory(uint cell, vec3 axis, out float support)
{
    vec4  history = vec4(0);
    float total   = 0.0;
    for (uint tap = 0u; tap < 18u; ++tap)
    {
        if (pastWeights[tap] > 0.0 && dot(axis, PreviousProbe(pastIndices[tap]).normal.xyz) > 0.0)
        {
            uvec2 packed  = PreviousProbeSpecular(pastIndices[tap], cell);
            history      += pastWeights[tap] * vec4(unpackHalf2x16(packed.x), unpackHalf2x16(packed.y));
            total        += pastWeights[tap];
        }
    }
    support = total / max(pastWeightSum, 1e-20);
    return history / max(total, 1e-20);
}
#endif
void main()
{
    uint index = ProbeGroupIndex(), lane = gl_LocalInvocationID.x;
    if (index < screenProbe.grid.w)
    {
        ScreenProbe probe = probes[index];
        if (probe.position.w == 0.0)
        {
            if (lane == 0u && screenProbe.localResponse != 0u)
            {
                probeSpecularData[screenProbe.grid.w * ZEN_SCREEN_PROBE_ATLAS_CELLS + index] = uvec2(0);
            }
            for (uint texel = lane; texel < ZEN_SCREEN_PROBE_ATLAS_CELLS; texel += 64u)
            {
                probeSpecularData[index * ZEN_SCREEN_PROBE_ATLAS_CELLS + texel] = uvec2(0);
            }
            return;
        }
        uint sampleCount =
            min(ProbeScheduledBatches(index) * uint(ZEN_SCREEN_PROBE_RAYS), uint(ZEN_SCREEN_PROBE_SPECULAR_RAYS));
        // Accumulate the radiance of escaped paths and their visibility independently.
        float hitGain = hybrid.reflection.x != 0u ? gi.volume.w : 0.0;
        for (uint sampleIndex = lane; sampleIndex < sampleCount; sampleIndex += 64u)
        {
#ifdef SCREEN_PROBE_PREFILTER_HITS
            uint address = ProbeRecordAddress(
                ProbeRecordSlot(index, sampleIndex % ZEN_SCREEN_PROBE_RAYS, sampleIndex / ZEN_SCREEN_PROBE_RAYS));
            // The trace has already resolved these samples for diffuse lighting.
            bool sky = (probeHits[address] & (kHitValid | kHitSurface)) == kHitValid;
            vec4 ray = ProbeHitSample(address);
#    ifdef PROBE_STATIC_HITS
            vec3 incoming = uintBitsToFloat(uvec3(probeHits[address + 1u], probeHits[address + 2u], probeHits[address + 3u]));
#    else
            vec3  incoming = ProbeHitRadiance(address);
            float change   = float((probeHits[address] >> 8u) & 255u) / 255.0;
            vec3  delta    = vec3(0);
            if ((probeHits[address] & kHitMoving) != 0u)
            {
                incoming += ProbeUnpackRGBE(probeHits[address + 2u]);
                delta     = HybridUnpackSignedRGBE(probeHits[address + 3u]);
                // The predicted change moves the texel history. Only the cache's moving record is unexplained.
                float cached = float(probeHits[address] >> 24u) / 255.0;
                change = max(change, cached * min(1.0, HybridLuminance(abs(delta)) / max(HybridLuminance(incoming), 1e-6)));
            }
            deltas[sampleIndex]           = sky ? vec3(0) : delta * hitGain;
            sampleDirections[sampleIndex] = ray.xyz;
            if (sky)
            {
                uint past = ProbePastEnvironment(probe);
                if ((probeHits[address] & kHitSkyDelta) != 0u)
                {
                    change = max(change, min(1.0, HybridLuminance(abs(HybridUnpackSignedRGBE(probeHits[address + 2u])))
                                                      / max(HybridLuminance(incoming), 1e-6)));
                }
                else if (screenProbe.environment[0].x != screenProbe.environment[past].x)
                {
                    change = max(change, abs(screenProbe.environment[0].x - screenProbe.environment[past].x)
                                             / max(screenProbe.environment[0].x, screenProbe.environment[past].x));
                }
            }
            response[sampleIndex] = change;
#    endif
#else
            uint address = ProbeSpecularRayAddress(index, sampleIndex);
            vec2 rg = screenRays[address], blueDensity = screenRays[address + 1u];
            vec4 ray      = vec4(DecodeGBufferNormal(screenRays[address + 2u]), abs(blueDensity.y));
            bool sky      = blueDensity.y < 0.0;
            vec3 incoming = vec3(rg, blueDensity.x);
#endif
            float nl                   = max(dot(probe.normal.xyz, ray.xyz), 0.0);
            sampleCells[sampleIndex]   = ProbeOctBin(ray.xyz);
            sampleWeights[sampleIndex] = nl > 0.0 ? 1.0 / ray.w : 0.0;
            radiance[sampleIndex]      = vec4(incoming * (sky ? 1.0 : hitGain), float(sky));
            densities[sampleIndex]     = nl / ray.w;
        }
        barrier();
#ifdef PROBE_ROUGH_FOOTPRINT
        bool relocated = probe.source.x != 0xffffffffu && screenProbe.control.z != 0.0
                      && probe.position.xyz != PreviousProbe(probe.source.x).position.xyz;
        if (relocated)
        {
            ProbeHistoryFootprint(probe, lane);
        }
#endif
#ifdef SCREEN_PROBE_PREFILTER_HITS
        if (screenProbe.localResponse != 0u)
        {
#    ifdef PROBE_ROUGH_PREDICTION
            // A changed ray restarts its share of the hemisphere, weighted by its energy. A ray that is
            // dark now still counts at the density-weighted mean radiance, since its previous radiance is
            // unknown. Each lane holds at most one sample, so both sums reduce across the workgroup.
            bool  sampled  = lane < sampleCount;
            float luma     = sampled ? HybridLuminance(radiance[lane].rgb) : 0.0;
            float density  = sampled ? densities[lane] : 0.0;
            vec2  sums     = ProbeReduceLanes(vec2(luma * density, density), lane);
            float meanLuma = sums.x / max(sums.y, 1e-20);
            float share    = density * max(luma, meanLuma);
            sums           = ProbeReduceLanes(vec2(share * (sampled ? response[lane] : 0.0), share), lane);
#    endif
            if (lane == 0u)
            {
#    ifdef PROBE_STATIC_HITS
                vec2  evidence = unpackUnorm2x16(floatBitsToUint(screenRays[ProbeChangeAddress(index)].x));
                float change   = max(evidence.x, evidence.y);
#    else
                float change = sums.x / max(sums.y, 1e-20);
#    endif
                vec2 previous = vec2(0);
                if (probe.source.x != 0xffffffffu && screenProbe.control.z != 0.0 && hybrid.reflection.y != 0u)
                {
                    previous = uintBitsToFloat(PreviousProbeSpecular(probe.source.x, ZEN_SCREEN_PROBE_ATLAS_CELLS));
                }
#    ifdef PROBE_ROUGH_FOOTPRINT
                if (relocated && hybrid.reflection.y != 0u)
                {
                    previous = vec2(0);
                    for (uint tap = 0u; tap < 18u; ++tap)
                    {
                        if (pastWeights[tap] > 0.0)
                        {
                            previous += pastWeights[tap]
                                      * uintBitsToFloat(PreviousProbeSpecular(pastIndices[tap], ZEN_SCREEN_PROBE_ATLAS_CELLS));
                        }
                    }
                    previous   /= max(pastWeightSum, 1e-20);
                    previous.x *= pastSupport;
                }
#    endif
                // A rare bright hit must not choose its own weight in the rough radiance mean.
                acceptedAge = WorldHistoryCap(previous.y, .266, min(1.0 + previous.x, probe.normal.w));
#    ifdef PROBE_ROUGH_PREDICTION
                // Predicted moving-light changes add their own noise. Use the probe's measured balance.
                if (screenProbe.layoutInfo.y != 0u)
                {
                    acceptedAge = min(acceptedAge, max(1.0, probeLighting[ProbeMovingBase(index) + 5u].z));
                }
#    endif
                probeSpecularData[screenProbe.grid.w * ZEN_SCREEN_PROBE_ATLAS_CELLS + index] =
                    floatBitsToUint(vec2(acceptedAge, change));
            }
            barrier();
        }
#endif
        // The cosine-weighted mean radiance of this frame's samples, the probe's irradiance over pi. It stands
        // in for atlas cells that have no samples yet.
        vec4  meanRadiance = vec4(0);
        float meanWeight   = 0.0;
        for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
        {
            meanRadiance += radiance[sampleIndex] * densities[sampleIndex];
            meanWeight   += densities[sampleIndex];
        }
        meanRadiance /= max(meanWeight, 1e-20);
        // The radiance atlas: the 64 cells of the octahedral direction map that the proposal CDF also bins
        // (ProbeOctBin), one per lane. The cells above the probe's horizon keep their mean incoming radiance and
        // sky share as history, in place of the sharpest reflection level. Probe rays are drawn for the
        // hemisphere and the lights, not for any reflection lobe, so estimating a lobe from the rays gave a ray
        // near a narrow lobe's peak tens of times its expected weight, and new probes on rims reflected lamps
        // many times brighter than settled. A ray now informs only its own cell, at its inverse PDF against the
        // cell's expected weight, which the uniform proposal lanes bound.
        {
            uint  cell       = lane;
            vec3  axis       = DecodeGBufferNormal((vec2(cell & 7u, cell >> 3u) + .5) / 8.0);
            bool  upper      = dot(axis, probe.normal.xyz) > 0.0;
            float solidAngle = 1.0 / ProbeOctDensity(axis);
            vec4  sum        = vec4(0);
            float weight     = 0.0;
#ifdef PROBE_ROUGH_PREDICTION
            vec3 shift = vec3(0);
#endif
            for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
            {
                if (sampleCells[sampleIndex] == cell)
                {
                    float inverse  = sampleWeights[sampleIndex];
                    sum           += radiance[sampleIndex] * inverse;
                    weight        += inverse;
#ifdef PROBE_ROUGH_PREDICTION
                    shift += deltas[sampleIndex] * inverse;
#endif
                }
            }
            // A cell's samples have unit expected weight per frame. One expected frame of weight reproduces
            // the running mean, as for the probe's diffuse lighting.
            float expected    = float(max(sampleCount, 1u)) * solidAngle;
            vec4  current     = sum / expected;
            float totalWeight = weight / expected;
            if (upper && probe.source.x != 0xffffffffu && screenProbe.control.z != 0.0)
            {
                uvec2 packed  = PreviousProbeSpecular(probe.source.x, cell);
                vec4  history = vec4(unpackHalf2x16(packed.x), unpackHalf2x16(packed.y));
                // The source holds radiance only for cells above its own horizon.
                float supported = float(dot(axis, PreviousProbe(probe.source.x).normal.xyz) > 0.0);
#ifdef PROBE_ROUGH_FOOTPRINT
                if (relocated)
                {
                    history = ProbeRelocatedRoughHistory(cell, axis, supported);
                }
#endif
#ifdef PROBE_ROUGH_PREDICTION
                // The ratio estimate of this cell's change in mean radiance. A cell without samples this frame takes
                // the change its nearby directions saw: each sample at its inverse PDF times cos^8 of its angle to
                // the cell's axis. A moving light's reflection must not wait for a ray to land in every cell. The
                // final clamp follows the blend.
                vec3 predicted = weight > 0.0 ? shift / weight : vec3(0);
                if (weight == 0.0)
                {
                    float near = 0.0;
                    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
                    {
                        float cosine  = max(dot(axis, sampleDirections[sampleIndex]), 0.0);
                        float fourth  = (cosine * cosine) * (cosine * cosine);
                        float lobe    = sampleWeights[sampleIndex] * fourth * fourth;
                        predicted    += deltas[sampleIndex] * lobe;
                        near         += lobe;
                    }
                    predicted /= max(near, 1e-20);
                }
                history.rgb += predicted;
#endif
                float age    = min(probeLighting[index * uint(ZEN_SCREEN_PROBE_LIGHTING_VECTORS) + 8u].z,
                                   probeLighting[index * uint(ZEN_SCREEN_PROBE_LIGHTING_VECTORS) + 9u].z);
                age          = screenProbe.localResponse != 0u ? acceptedAge : age;
                float prior  = (max(age, 1.0) - 1.0) * supported;
                current     += history * prior;
                totalWeight += prior;
            }
            // Until a cell has one expected frame of weight, the missing weight takes the probe's mean radiance.
            // A cell sampled at or above its expected weight keeps its ratio, which cannot exceed its brightest
            // sample.
            float missing  = max(1.0 - totalWeight, 0.0);
            current       += meanRadiance * missing;
            totalWeight   += missing;
            vec4 value     = clamp(current / max(totalWeight, 1e-20), vec4(0), vec4(65504, 65504, 65504, 1));
            // Cells below the horizon receive few or no rays and are not read as radiance. The specular spatial
            // filter integrates the reflection levels from the cells above it.
            uint address               = index * ZEN_SCREEN_PROBE_ATLAS_CELLS + cell;
            probeSpecularData[address] = StochasticPackHalf(value, address ^ HybridHash(hybrid.sampling.x ^ 0x5f3a1u));
        }
    }
}
