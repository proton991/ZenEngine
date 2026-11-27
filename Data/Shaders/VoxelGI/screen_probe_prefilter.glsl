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
shared vec4 directions[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
shared vec4 radiance[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
#include "history_response.glsl"
shared float response[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
shared float acceptedAge;
shared float densities[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
#if defined(SCREEN_PROBE_PREFILTER_HITS) && !defined(PROBE_STATIC_HITS)
#    define PROBE_ROUGH_PREDICTION
// Each moving-light hit's change since the history frame, evaluated on the same surface.
shared vec3 deltas[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
// Nonzero when any sample of this probe carries a predicted change.
shared uint movingDeltas;
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
layout(set = 1, binding = 128) uniform sampler2D secondaryBRDFMap;
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
vec4 ProbeRelocatedRoughHistory(uint texel)
{
    vec4 history = vec4(0);
    for (uint tap = 0u; tap < 18u; ++tap)
    {
        if (pastWeights[tap] > 0.0)
        {
            uvec2 packed  = PreviousProbeSpecular(pastIndices[tap], texel);
            history      += pastWeights[tap] * vec4(unpackHalf2x16(packed.x), unpackHalf2x16(packed.y));
        }
    }
    return history / max(pastWeightSum, 1e-20);
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
                probeSpecularData[screenProbe.grid.w * ZEN_SCREEN_PROBE_SPECULAR_TEXELS + index] = uvec2(0);
            }
            for (uint texel = lane; texel < ZEN_SCREEN_PROBE_SPECULAR_TEXELS; texel += 64u)
            {
                probeSpecularData[index * ZEN_SCREEN_PROBE_SPECULAR_TEXELS + texel] = uvec2(0);
            }
            return;
        }
        uint sampleCount =
            min(ProbeScheduledBatches(index) * uint(ZEN_SCREEN_PROBE_RAYS), uint(ZEN_SCREEN_PROBE_SPECULAR_RAYS));
        // Accumulate the radiance of escaped paths and their visibility independently.
        float hitGain = hybrid.reflection.x != 0u ? gi.volume.w : 0.0;
#ifdef PROBE_ROUGH_PREDICTION
        if (lane == 0u)
        {
            movingDeltas = 0u;
        }
        barrier();
#endif
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
            deltas[sampleIndex] = sky ? vec3(0) : delta * hitGain;
            if (any(notEqual(deltas[sampleIndex], vec3(0))))
            {
                atomicOr(movingDeltas, 1u);
            }
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
            float nl                = max(dot(probe.normal.xyz, ray.xyz), 0.0);
            directions[sampleIndex] = vec4(ray.xyz, nl);
            radiance[sampleIndex]   = vec4(incoming * (sky ? 1.0 : hitGain), float(sky));
            densities[sampleIndex]  = nl / ray.w;
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
                    previous = uintBitsToFloat(PreviousProbeSpecular(probe.source.x, ZEN_SCREEN_PROBE_SPECULAR_TEXELS));
                }
#    ifdef PROBE_ROUGH_FOOTPRINT
                if (relocated && hybrid.reflection.y != 0u)
                {
                    previous = vec2(0);
                    for (uint tap = 0u; tap < 18u; ++tap)
                    {
                        if (pastWeights[tap] > 0.0)
                        {
                            previous +=
                                pastWeights[tap]
                                * uintBitsToFloat(PreviousProbeSpecular(pastIndices[tap], ZEN_SCREEN_PROBE_SPECULAR_TEXELS));
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
                probeSpecularData[screenProbe.grid.w * ZEN_SCREEN_PROBE_SPECULAR_TEXELS + index] =
                    floatBitsToUint(vec2(acceptedAge, change));
            }
            barrier();
        }
#endif
        // The cosine-weighted mean radiance of this frame's samples, the probe's irradiance over pi. It stands
        // in for the directions a sparsely sampled lobe did not receive (below).
        vec4  meanRadiance = vec4(0);
        float meanWeight   = 0.0;
        for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
        {
            meanRadiance += radiance[sampleIndex] * densities[sampleIndex];
            meanWeight   += densities[sampleIndex];
        }
        meanRadiance /= max(meanWeight, 1e-20);
        for (uint texel = lane; texel < ZEN_SCREEN_PROBE_SPECULAR_TEXELS; texel += 64u)
        {
            uint  level     = texel < 64u ? 0u : 1u + (texel - 64u) / 16u;
            uint  offset    = level == 0u ? 0u : 64u + (level - 1u) * 16u;
            uint  side      = level == 0u ? 8u : 4u;
            vec3  axis      = DecodeGBufferNormal((vec2((texel - offset) % side, (texel - offset) / side) + .5) / float(side));
            float roughness = .25 * float(level + 1u);
            float alpha = roughness * roughness, alphaSquared = alpha * alpha;
            // An atlas direction represents reflect(-V,N). Extend lower-hemisphere texels
            // along the horizon so bilinear filtering is defined for grazing receivers.
            vec3  n       = probe.normal.xyz;
            float nv      = dot(n, axis);
            vec3  tangent = axis - n * nv;
            nv            = clamp(nv, 0.01, 1.0);
            vec3 v        = dot(tangent, tangent) > 1e-8 ? nv * n - normalize(tangent) * sqrt(1.0 - nv * nv) : n;
            nv            = max(dot(n, v), .01);
            float k       = alpha * .5;
            // The compositor applies colored F0. Use the material's scalar Fresnel shape here
            // and divide by the matching split-sum response to avoid applying it twice.
            float f0   = mix(.04, 1.0, clamp(texelFetch(receiverRoughness, ivec2(probe.source.yz), 0).r, 0.0, 1.0));
            vec2  brdf = textureLod(secondaryBRDFMap, vec2(nv, roughness), 0).rg;
            float norm = 4.0 * HYBRID_PI * (nv * (1.0 - k) + k) * max(f0 * brdf.x + brdf.y, 1e-5);
            vec4  sum  = vec4(0);
            float lobe = 0.0;
#ifdef PROBE_ROUGH_PREDICTION
            vec3 shift = vec3(0);
#endif
            for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
            {
                vec4  ray          = directions[sampleIndex];
                float nl           = ray.w;
                float vh2          = max(.5 * (1.0 + dot(v, ray.xyz)), 1e-7);
                float nh2          = min((nv + nl) * (nv + nl) * .25 / vh2, 1.0);
                float denominator  = 1.0 + (alphaSquared - 1.0) * nh2;
                float grazing      = max(1.0 - sqrt(vh2), 0.0);
                float squared      = grazing * grazing;
                float fresnel      = f0 + (1.0 - f0) * squared * squared * grazing;
                float weight       = densities[sampleIndex] * fresnel / ((nl * (1.0 - k) + k) * denominator * denominator);
                sum               += radiance[sampleIndex] * weight;
                lobe              += weight;
#ifdef PROBE_ROUGH_PREDICTION
                if (movingDeltas != 0u)
                {
                    shift += deltas[sampleIndex] * weight;
                }
#endif
            }
            // Scaled, each sample's lobe weight has unit expectation. A ray near the peak of a
            // narrow lobe can still carry tens of times that, which made a young probe's reflection
            // tens of times its brightest sample. Normalize by the samples' actual total weight (the
            // ratio estimator of stochastic screen-space reflections) and count history at its
            // expected weight. One expected frame of weight reproduces the running mean.
            float scale       = alphaSquared / norm;
            vec4  current     = sum * scale;
            float totalWeight = lobe * scale;
            if (probe.source.x != 0xffffffffu && screenProbe.control.z != 0.0)
            {
                uvec2 packed  = PreviousProbeSpecular(probe.source.x, texel);
                vec4  history = vec4(unpackHalf2x16(packed.x), unpackHalf2x16(packed.y));
#ifdef PROBE_ROUGH_FOOTPRINT
                if (relocated)
                {
                    history = ProbeRelocatedRoughHistory(texel);
                }
#endif
#ifdef PROBE_ROUGH_PREDICTION
                // The ratio estimate of this lobe's change in mean radiance. The final clamp follows the blend.
                history.rgb += lobe > 0.0 ? shift / lobe : vec3(0);
#endif
                float age    = min(probeLighting[index * uint(ZEN_SCREEN_PROBE_LIGHTING_VECTORS) + 8u].z,
                                   probeLighting[index * uint(ZEN_SCREEN_PROBE_LIGHTING_VECTORS) + 9u].z);
                age          = screenProbe.localResponse != 0u ? acceptedAge : age;
                float prior  = (max(age, 1.0) - 1.0) * float(max(sampleCount, 1u));
                current     += history * prior;
                totalWeight += prior;
            }
            // Probe rays are drawn for the hemisphere and the lights, not for each lobe. A grazing or narrow
            // lobe can hold only a few guided samples, and their ratio estimate is their own mean, not the
            // lobe's: a new probe on a rim reflected a lamp many times brighter than its settled value.
            // Filling the missing weight with zero instead loses the rare peak hits that carry a narrow lobe's
            // mean, and new probes read dark. Until the estimate has one expected frame of weight, the missing
            // weight takes the probe's mean radiance. A lobe sampled at or above its expected weight keeps its
            // ratio, which cannot exceed the brightest sample, and a probe with history converges as before.
            float missing               = max(float(max(sampleCount, 1u)) - totalWeight, 0.0);
            current                    += meanRadiance * missing;
            totalWeight                += missing;
            current                     = clamp(current / max(totalWeight, 1e-20), vec4(0), vec4(65504, 65504, 65504, 1));
            uint address                = index * ZEN_SCREEN_PROBE_SPECULAR_TEXELS + texel;
            probeSpecularData[address]  = StochasticPackHalf(current, address ^ HybridHash(hybrid.sampling.x ^ 0x5f3a1u));
        }
    }
}
