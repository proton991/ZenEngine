#define SCREEN_LIGHT_WRITE
#define SCREEN_RAYS_READ
#define SCREEN_CDF_READ
#define SCREEN_REPROJECT_LANES uint(ZEN_SCREEN_PROBE_RAYS)
#define SCREEN_PROBE_COMPUTE
#include "screen_probe.glsl"
layout(local_size_x = ZEN_SCREEN_PROBE_RAYS) in;
shared vec4  sum[ZEN_SCREEN_PROBE_RAYS][8];
shared float batchWeight[2];
shared float frameAlpha[2];
#ifdef PROBE_LOCAL_RESPONSE
shared uint nextResponse[2];
#endif
#include "world_cache_lighting.glsl"
const uint  kStatistics = 8u, kSource = 10u;
shared vec4 pastIrradiance[2];
// The trace already returns the directional first moment of every lane. Keep
// separate angular sectors: a single mean can point between two bright surfaces.
shared vec4  observedDirection[ZEN_SCREEN_PROBE_RAYS];
shared uint  observedSector[ZEN_SCREEN_PROBE_RAYS];
shared uvec2 currentLobes[3];
void         ProbeObserveDirection(uint lane, mat3 basis)
{
    vec4  sh                = .2126 * sum[lane][4] + .7152 * sum[lane][5] + .0722 * sum[lane][6];
    float weight            = HybridLuminance(sum[lane][7].rgb);
    vec3  mean              = sh.x > 0.0 ? .5773502692 * sh.yzw / sh.x : vec3(0);
    vec2  tangent           = vec2(dot(basis[0], mean), dot(basis[1], mean));
    vec3  sector            = vec3(tangent.x, -.5 * tangent.x + .8660254 * tangent.y, -.5 * tangent.x - .8660254 * tangent.y);
    observedSector[lane]    = sector.x >= max(sector.y, sector.z) ? 0u : sector.y >= sector.z ? 1u : 2u;
    observedDirection[lane] = vec4(mean * weight, weight);
}
void ProbeFitLobe(ScreenProbe probe, uint sector, bool history)
{
    vec4 observed = vec4(0);
    for (uint lane = 0u; lane < uint(ZEN_SCREEN_PROBE_RAYS); ++lane)
    {
        if (observedSector[lane] == sector)
        {
            observed += observedDirection[lane];
        }
    }
    if (history)
    {
        uint      a      = floatBitsToUint(PreviousProbeLighting(probe.source.x, kSource)[1u + sector]);
        uint      b      = floatBitsToUint(PreviousProbeLighting(probe.source.x, kSource + 1u)[1u + sector]);
        ProbeLobe prior  = ProbeReadLobe(a, b);
        vec4      moment = vec4(prior.axis * prior.concentration, 1.0) * prior.weight;
        observed         = mix(moment, observed, frameAlpha[1]);
    }
    currentLobes[sector] = ProbeWriteLobe(observed);
}
#ifdef PROBE_LOCAL_RESPONSE
float ProbeNeighborhoodChange(ScreenProbe probe, uint signal, float center)
{
    // The previous sample and its spatial neighbors are independent of the current ray batch.
    vec2        totals   = vec2(center, 1.0);
    ScreenProbe previous = PreviousProbe(probe.source.x);
    ivec2       tile     = ivec2(previous.source.yz / screenProbe.spacing);
    uint        side     = probe.source.x & 0xc0000000u;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            ivec2 q = tile + ivec2(x, y);
            if ((x != 0 || y != 0) && all(greaterThanEqual(q, ivec2(0))) && all(lessThan(q, ivec2(screenProbe.grid.xy))))
            {
                uint        source = side | (uint(q.y) * screenProbe.grid.x + uint(q.x));
                ScreenProbe other  = PreviousProbe(source);
                float       weight = ProbePlaneWeight(probe.position.xyz, probe.normal.xyz, other) / float(1 + x * x + y * y);
                if (other.source.w == previous.source.w && weight > .05)
                {
                    float change  = unpackHalf2x16(floatBitsToUint(PreviousProbeLighting(source, kSource + signal).x)).x;
                    totals       += vec2(change, 1.0) * weight;
                }
            }
        }
    }
    return max(center, totals.x / totals.y);
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
            // An empty placement has no reprojectable radiance. Clear the two diffuse signals and their guides.
            if (lane < uint(ZEN_SCREEN_PROBE_LIGHTING_VECTORS))
            {
                probeLighting[ProbeLightingAddress(index, lane)] = vec4(0);
            }
            return;
        }
        uint address = (index * uint(ZEN_SCREEN_PROBE_RAYS) + lane) * 4u;
        uint batches = ProbeScheduledBatches(index);
        mat3 basis   = HybridBasis(probe.normal.xyz);
        for (uint signal = 0u; signal < 2u; ++signal)
        {
            uvec4 packed = floatBitsToUint(vec4(screenRays[address + 2u * signal], screenRays[address + 2u * signal + 1u]));
            vec3  energy = ProbeUnpackRGBE(packed.x);
            vec3  x = HybridUnpackSignedRGBE(packed.y), y = HybridUnpackSignedRGBE(packed.z), z = ProbeUnpackRGBE(packed.w);
            for (uint channel = 0u; channel < 3u; ++channel)
            {
                vec3 directionMoment = basis * vec3(x[channel], y[channel], z[channel]);
                sum[lane][signal * 4u + channel] =
                    vec4(.2820947918 * energy[channel], .4886025119 * directionMoment) / float(ZEN_SCREEN_PROBE_RAYS);
            }
            vec3  irradiance            = z / (HYBRID_PI * float(ZEN_SCREEN_PROBE_RAYS));
            float luminance             = dot(irradiance, vec3(.2126, .7152, .0722));
            sum[lane][signal * 4u + 3u] = vec4(irradiance, luminance * luminance);
        }
        ProbeObserveDirection(lane, basis);
        bool history   = probe.source.x != 0xffffffffu && screenProbe.control.z != 0.0;
        bool relocated = history && any(notEqual(probe.position.xyz, PreviousProbe(probe.source.x).position.xyz));
        if (relocated)
        {
            ProbeHistoryFootprint(probe, lane);
            history = pastWeightSum > 1e-4;
        }
        barrier();
        for (uint stride = uint(ZEN_SCREEN_PROBE_RAYS / 2); stride != 0u; stride /= 2u)
        {
            if (lane < stride)
            {
                for (uint item = 0u; item < 8u; ++item)
                {
                    sum[lane][item] += sum[lane + stride][item];
                }
            }
            barrier();
        }
        if (lane < 2u)
        {
            vec4 statistics = history ? (relocated ? ProbePastValue(probe, kStatistics + lane)
                                                   : PreviousProbeLighting(probe.source.x, kStatistics + lane))
                                      : vec4(0);
            // A re-placed probe inherits its footprint's age, but only the share of that history that
            // ProbeRelocatedBatches retains applies here. Record that share in frames, so the spatial filter
            // widens a re-placed probe's footprint and the gather and pixel history see its actual noise.
            float retained = 1.0;
            if (history && relocated)
            {
                float spatialVariance;
                pastIrradiance[lane]  = ProbePastValue(probe, lane * 4u + 3u, spatialVariance);
                retained              = ProbeRelocatedBatches(statistics, 1.0, spatialVariance);
                statistics.z         *= retained;
            }
#ifdef PROBE_LOCAL_RESPONSE
            float age, uncapped;
            vec2  pastResponse =
                history ? unpackHalf2x16(floatBitsToUint(PreviousProbeLighting(probe.source.x, kSource + lane).x)) : vec2(0, 1);
#    ifdef PROBE_SKY_PREDICTION
            uint past     = ProbePastEnvironment(probe);
            bool skyDelta = ProbeNeedsSkyDelta(past);
            if (screenProbe.localResponse != 0u && lane == 0u && history)
            {
                float scale    = ProbeSkyScale(past);
                statistics.x  *= scale;
                statistics.yw *= scale * scale;
            }
#    endif
            {
                float change = unpackUnorm2x16(floatBitsToUint(screenRays[ProbeChangeAddress(index)].x))[lane];
#    ifdef PROBE_SKY_PREDICTION
                if (lane == 0u && skyDelta)
                {
                    // The same-ray prediction moves the history below. Its noise sets the weight there,
                    // so the predicted change is not also counted as unexplained evidence.
                    uint  base   = ProbeChangeAddress(index);
                    vec3  delta  = vec3(screenRays[base + 7u], screenRays[base + 8u].x);
                    float mean   = max(0.0, statistics.x + dot(delta, vec3(.2126, .7152, .0722)));
                    statistics.y = max(statistics.y - statistics.x * statistics.x, 0.0) + mean * mean;
                    statistics.x = mean;
                }
#    endif
                float accepted =
                    history && (lane == 0u || hybrid.bounce.y != 0u) ? min(statistics.z + 1.0, probe.normal.w) : 1.0;
                float sigma = statistics.x > .005 ? sqrt(max(statistics.w, 0.0)) / statistics.x : .266;
                // The rays that reveal a changed source also see its radiance. Choose this observation's weight
                // from earlier rays, or a rare bright hit repeatedly promotes itself after a switch-on.
                float held = pastResponse.x;
                if (held > .005)
                {
                    held = ProbeNeighborhoodChange(probe, lane, held);
                }
                // Keep observing the local indirect field while the edited secondary surfaces refill.
                if (lane == 1u && held > .005 && min(sourceSignature.y, sourceSignature.z) < 64u)
                {
                    accepted = min(accepted, 16.0);
                }
                uncapped = accepted;
                age      = WorldHistoryCap(held, clamp(sigma, .15, .75) / sqrt(float(batches)), accepted);
#    ifdef PROBE_SKY_PREDICTION
                if (lane == 0u && skyDelta && age > 1.0)
                {
                    // The rotated sky's same-ray prediction is a 16-lane estimate. Weight it against this
                    // observation by inverse variance instead of adding its noise to a long history.
                    float luminance     = HybridLuminance(sum[0][3].rgb);
                    float observed      = max((float(ZEN_SCREEN_PROBE_RAYS) * sum[0][3].w - luminance * luminance)
                                                  / float(ZEN_SCREEN_PROBE_RAYS - 1),
                                              0.0);
                    float deltaVariance = screenRays[ProbeChangeAddress(index) + 9u].x;
                    float ratio         = deltaVariance / max(max(statistics.w, observed), 1e-30);
                    age                 = min(age, 1.0 + 1.0 / (1.0 / (age - 1.0) + ratio));
                }
#    endif
                float confidence = lane == 1u ? max(.125, screenRays[ProbeChangeAddress(index)].y) : 1.0;
                // A step decays with a two-observation half-life for every source. While the cache refills
                // after an edit, its later solves report their own measured changes on the rays that see them.
                nextResponse[lane] = packHalf2x16(vec2(max(change, .70710678 * pastResponse.x), confidence));
            }
#else
            uint previousSource = history ? floatBitsToUint(PreviousProbeLighting(probe.source.x, kSource + lane).x) : 0u;
            bool changed        = previousSource != sourceSignature.x || (lane == 1u && hybrid.bounce.y == 0u);
            // The GPU source signature is independent of Monte Carlo noise, including
            // lighting edits for which the CPU did not supply a revision hint. Retain
            // a short response window while the cache propagates that source change.
            float age = history && !changed ? min(statistics.z + 1.0, probe.normal.w) : 1.0;
            if (sourceSignature.y < 64u)
            {
                float distant = WorldDistantFromEdit(probe.position.xyz);
                if (history && changed && lane == 0u && WorldFollowsLocalEdit(previousSource))
                {
                    age = min(min(statistics.z + 1.0, probe.normal.w), mix(1.0, 256.0, distant));
                }
                age = min(age, mix(16.0, 256.0, distant));
            }
            age = sourceSignature.z < 64u ? min(age, 16.0) : age;
#endif
            // Each batch carries an independent estimate. A shortened history retains
            // the same fraction of its accumulated batches as its accepted frame age.
            float previousBatches = 0.0;
            if (history && relocated)
            {
                previousBatches = pastIrradiance[lane].w * retained;
            }
            else if (history)
            {
                previousBatches = PreviousProbeLighting(probe.source.x, lane * 4u + 3u).w;
            }
            // A ray batch sees one realization of the secondary radiance cache.
            // Weight the temporal observations by the fraction of the cell means
            // that has responded to the current lighting since the last edit.
#ifdef PROBE_LOCAL_RESPONSE
            float confidence = lane == 1u ? ProbeObservationConfidence(pastResponse) : 1.0;
#else
            float confidence = lane == 1u ? uintBitsToFloat(sourceSignature.w) : 1.0;
#endif
            float observation = float(batches) * confidence * confidence;
            // A re-placed probe's retained age can be below one frame; its batches are already discounted.
            batchWeight[lane] = observation + previousBatches * ((age - 1.0) / max(statistics.z, 1e-6));
#ifdef PROBE_LOCAL_RESPONSE
            if (age < uncapped - .5)
            {
                // A measured change shortened the history. Older observations were weighted at another
                // confidence; keep exactly (age - 1) observations' weight in today's units.
                batchWeight[lane] = observation * age;
            }
#endif
            float alpha      = observation / max(batchWeight[lane], 1e-20);
            frameAlpha[lane] = alpha;
            float luminance  = dot(sum[0][lane * 4u + 3u].rgb, vec3(.2126, .7152, .0722));
            float variance   = max((float(ZEN_SCREEN_PROBE_RAYS) * sum[0][lane * 4u + 3u].w - luminance * luminance)
                                       / float(ZEN_SCREEN_PROBE_RAYS - 1),
                                   0.0);
            probeLighting[index * uint(ZEN_SCREEN_PROBE_LIGHTING_VECTORS) + kStatistics + lane] =
                vec4(mix(statistics.x, luminance, alpha), mix(statistics.y, luminance * luminance, alpha), age,
                     mix(statistics.w, variance, alpha));
        }
        barrier();
        if (lane < 3u)
        {
            ProbeFitLobe(probe, lane, history);
        }
        barrier();
        if (lane < 2u)
        {
            uvec3 words = uvec3(currentLobes[0][lane], currentLobes[1][lane], currentLobes[2][lane]);
#ifdef PROBE_LOCAL_RESPONSE
            uint source = nextResponse[lane];
#else
            uint source = sourceSignature.x;
#endif
            probeLighting[index * uint(ZEN_SCREEN_PROBE_LIGHTING_VECTORS) + kSource + lane] =
                uintBitsToFloat(uvec4(source, words));
        }
        if (lane < 8u)
        {
            uint signal = lane / 4u;
            vec4 result = sum[0][lane];
            if (history)
            {
                vec4 previous = relocated ? ((lane & 3u) == 3u ? pastIrradiance[signal] : ProbePastValue(probe, lane))
                                          : PreviousProbeLighting(probe.source.x, lane);
#ifdef PROBE_SKY_PREDICTION
                bool predicted = false;
                if (signal == 0u)
                {
                    uint past  = ProbePastEnvironment(probe);
                    previous  *= ProbeSkyScale(past);
                    predicted  = ProbeNeedsSkyDelta(past);
                    if (predicted)
                    {
                        uint base  = ProbeChangeAddress(index) + 1u + 2u * lane;
                        previous  += vec4(screenRays[base], screenRays[base + 1u]);
                    }
                }
#endif
                float alpha = frameAlpha[signal];
                result      = mix(previous, result, alpha);
#ifdef PROBE_SKY_PREDICTION
                // Clamp after blending. Clamping each noisy prediction first would keep only its positive errors.
                if (predicted && lane == 3u)
                {
                    result.rgb = max(result.rgb, vec3(0));
                }
                else if (predicted)
                {
                    result.x    = max(result.x, 0.0);
                    result.yzw *= min(1.0, 1.732051 * result.x / max(length(result.yzw), 1e-20));
                }
#endif
            }
            if ((lane & 3u) == 3u)
            {
                result.w = batchWeight[signal];
            }
            probeLighting[index * uint(ZEN_SCREEN_PROBE_LIGHTING_VECTORS) + lane] = result;
        }
    }
}
