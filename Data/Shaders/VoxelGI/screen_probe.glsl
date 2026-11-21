#ifndef ZEN_SCREEN_PROBE
#define ZEN_SCREEN_PROBE
#ifndef SCREEN_PROBE_CLEAR
#    include "hybrid_guides.glsl"
#    include "hybrid_sampling.glsl"
#endif
#include "Graphics/Shared/ScreenProbeGI.h"
layout(std140, set = 1, binding = 80) uniform uScreenProbeData
{
    mat4  projectionView;
    mat4  previousProjectionViews[ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1];
    uvec4 grid;    // tile extent, base count, total count
    vec4  control; // scene scale, valid history frames, temporal enabled, spatial enabled
    uint  spacing;
    uint  adaptiveTiles;
}
screenProbe;
struct ScreenProbe
{
    vec4  position;
    vec4  normal; // accepted history length in w
    uvec4 source; // previous probe (high two bits: frame offset), pixel x/y, surface identity
};
#if defined(SCREEN_PROBE_READ_WRITE)
#    define PROBE_ACCESS
#elif defined(SCREEN_PROBE_WRITE)
#    define PROBE_ACCESS writeonly
#else
#    define PROBE_ACCESS readonly
#endif
layout(std430, set = 1, binding = 81) PROBE_ACCESS buffer ScreenProbes
{
    ScreenProbe probes[];
};
#ifdef SCREEN_PROBE_CLEAR
#    define PREVIOUS_ACCESS writeonly
#else
#    define PREVIOUS_ACCESS readonly
#endif
layout(std430, set = 1, binding = 82) PREVIOUS_ACCESS buffer PreviousScreenProbes
{
    ScreenProbe values[];
}
previousProbes[ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1];
// Three RGB L1 coefficient vectors and exact irradiance/pi at the anchor normal, per signal.
#ifdef SCREEN_LIGHT_READ_WRITE
#    define LIGHT_ACCESS
#elif defined(SCREEN_LIGHT_WRITE)
#    define LIGHT_ACCESS writeonly
#else
#    define LIGHT_ACCESS readonly
#endif
layout(std430, set = 1, binding = 83) LIGHT_ACCESS buffer ScreenProbeLighting
{
    vec4 probeLighting[];
};
layout(std430, set = 1, binding = 84) PREVIOUS_ACCESS buffer PreviousScreenProbeLighting
{
    vec4 values[];
}
previousProbeLighting[ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1];
#if defined(SCREEN_SPECULAR_READ) || defined(SCREEN_SPECULAR_WRITE) \
    || (defined(SCREEN_PROBE_CLEAR) && !defined(SCREEN_PROBE_DIFFUSE_ONLY))
#    if defined(SCREEN_SPECULAR_WRITE) || defined(SCREEN_PROBE_CLEAR)
#        define SPECULAR_ACCESS writeonly
#    else
#        define SPECULAR_ACCESS readonly
#    endif
// Four GGX roughness levels (8x8, then 4x4), packed RGBA16F.
layout(std430, set = 1, binding = 91) SPECULAR_ACCESS buffer ScreenProbeSpecular
{
    uvec2 probeSpecularData[];
};
#endif
#if defined(SCREEN_SPECULAR_WRITE) || (defined(SCREEN_PROBE_CLEAR) && !defined(SCREEN_PROBE_DIFFUSE_ONLY))
layout(std430, set = 1, binding = 92) PREVIOUS_ACCESS buffer PreviousScreenProbeSpecular
{
    uvec2 values[];
}
previousProbeSpecularData[ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1];
#endif
#ifdef SCREEN_CDF_WRITE
#    define CDF_ACCESS writeonly
#else
#    define CDF_ACCESS readonly
#endif
#if defined(SCREEN_CDF_WRITE) || defined(SCREEN_CDF_READ)
layout(std430, set = 1, binding = 85) CDF_ACCESS buffer ScreenProbeCDF
{
    uint probeCDF[];
};
#endif
#if defined(SCREEN_CDF_WRITE)
layout(std430, set = 1, binding = 95) PREVIOUS_ACCESS buffer PreviousScreenProbeCDF
{
    uint values[];
}
previousProbeCDF[ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1];
#endif
#ifdef SCREEN_PROBE_COMPUTE
uint ProbeGroupIndex()
{
    return gl_WorkGroupID.x + gl_WorkGroupID.y * gl_NumWorkGroups.x;
}
uint ProbeInvocationIndex(uint groupSize)
{
    return ProbeGroupIndex() * groupSize + gl_LocalInvocationID.x;
}
#endif
#ifndef SCREEN_PROBE_CLEAR
// Constant descriptor indices keep this path available without nonuniform storage-buffer indexing.
ScreenProbe PreviousProbe(uint source)
{
    uint index = source & 0x3fffffffu;
    return source >> 30u == 0u ? previousProbes[0].values[index]
         : source >> 30u == 1u ? previousProbes[1].values[index]
                               : previousProbes[2].values[index];
}
vec4 PreviousProbeLighting(uint source, uint item)
{
    uint index = (source & 0x3fffffffu) * ZEN_SCREEN_PROBE_LIGHTING_VECTORS + item;
    return source >> 30u == 0u ? previousProbeLighting[0].values[index]
         : source >> 30u == 1u ? previousProbeLighting[1].values[index]
                               : previousProbeLighting[2].values[index];
}
#    ifdef SCREEN_SPECULAR_WRITE
uvec2 PreviousProbeSpecular(uint source, uint texel)
{
    uint index = (source & 0x3fffffffu) * ZEN_SCREEN_PROBE_SPECULAR_TEXELS + texel;
    return source >> 30u == 0u ? previousProbeSpecularData[0].values[index]
         : source >> 30u == 1u ? previousProbeSpecularData[1].values[index]
                               : previousProbeSpecularData[2].values[index];
}
#    endif
#    ifdef SCREEN_CDF_WRITE
uint PreviousProbeCDF(uint source, uint entry)
{
    uint index = (source & 0x3fffffffu) * ZEN_SCREEN_PROBE_PROPOSAL_WORDS + entry;
    return source >> 30u == 0u ? previousProbeCDF[0].values[index]
         : source >> 30u == 1u ? previousProbeCDF[1].values[index]
                               : previousProbeCDF[2].values[index];
}
#    endif
uint ProbeMatureBatches()
{
    return hybrid.rejection.y == 0.0 ? ZEN_SCREEN_PROBE_DIFFUSE_BATCHES : ZEN_SCREEN_PROBE_MATURE_BATCHES;
}
// Accumulated ray batches in frames of a mature probe's sampling rate. Young probes trace
// extra batches, so their frame age understates how much they have resolved.
float ProbeSampleAge(float age, float batches)
{
    float mature = float(ProbeMatureBatches());
    return max(age, batches / mature);
}
// Limit a young estimate only when three independently weighted groups of taps support the comparison.
float ProbeSpatialOutlierScale(vec3 groupLuma, vec3 groupWeight, float total, float mean, float young)
{
    float scale = 1.0;
    if (young > 0.0 && min(min(groupWeight.x, groupWeight.y), groupWeight.z) > .05 * total)
    {
        vec3  luma = groupLuma / groupWeight;
        float lo = min(min(luma.x, luma.y), luma.z), hi = max(max(luma.x, luma.y), luma.z);
        float median = luma.x + luma.y + luma.z - lo - hi;
        float limit  = median + 2.0 * min(median - lo, hi - median);
        scale        = mix(1.0, clamp(limit / max(mean, 1e-20), 0.0, 1.0), young);
    }
    return scale;
}
#    ifdef SCREEN_RAYS_WRITE
#        define RAYS_ACCESS writeonly
#    else
#        define RAYS_ACCESS readonly
#    endif
#    if defined(SCREEN_RAYS_WRITE) || defined(SCREEN_RAYS_READ)
layout(std430, set = 1, binding = 86) RAYS_ACCESS buffer ScreenProbeRays
{
    vec2 screenRays[];
};
// Each diffuse lane stores eight packed RGB moments. Each optional GGX ray stores
// six floats (radiance, signed density and an octahedral direction).
uint ProbeSpecularRayAddress(uint index, uint sampleIndex)
{
    return 4u * screenProbe.grid.w * uint(ZEN_SCREEN_PROBE_RAYS)
         + 3u * (index * uint(ZEN_SCREEN_PROBE_SPECULAR_RAYS) + sampleIndex);
}
uint ProbePackRGBE(vec3 value)
{
    float largest  = max(value.x, max(value.y, value.z));
    float exponent = clamp(ceil(log2(max(largest, 1e-30))), -126.0, 127.0);
    return packUnorm4x8(vec4(value * exp2(-exponent), (exponent + 127.0) / 255.0));
}
vec3 ProbeUnpackRGBE(uint word)
{
    vec4 packed = unpackUnorm4x8(word);
    return packed.rgb * exp2(round(packed.a * 255.0) - 127.0);
}
#    endif
vec4 ProbeSH(vec3 direction)
{
    return vec4(.2820947918, .4886025119 * direction);
}
// All traced directions lie in the geometric hemisphere. Differentiating the
// clamped cosine at its anchor gives the full tangential first moment divided
// by pi. The normal component integrates constant hemispherical radiance
// exactly at a tilted normal, with a factor of (1 + dot(n, anchor)) / 2.
// Fit uniform hemispherical radiance plus a directional component to the
// measured zeroth and first moments. Both components integrate exactly for
// any receiver normal. Keep the measured irradiance at the anchor itself.
vec4 ProbeNormalResponse(vec4 sh, vec3 anchor)
{
    float energy  = max(sh.x / (.2820947918 * HYBRID_PI), 0.0);
    vec3  moment  = sh.yzw / (.4886025119 * HYBRID_PI);
    float axial   = max(dot(moment, anchor), 0.0);
    float c       = max(energy * energy - dot(moment, moment), 0.0);
    float b       = 2.0 * energy - axial;
    float ambient = 2.0 * c / max(b + sqrt(max(b * b - 3.0 * c, 0.0)), 1e-20);
    ambient       = clamp(ambient, 0.0, min(energy, 2.0 * axial));
    return vec4(moment - .5 * ambient * anchor, .25 * ambient);
}
float ProbeNormalCorrection(vec4 response, vec3 anchor, vec3 normal)
{
    return max(dot(response.xyz, normal), 0.0) - max(dot(response.xyz, anchor), 0.0) + response.w * (dot(anchor, normal) - 1.0);
}
vec3 ProbeNormalOffset(vec3 anchor, vec3 normal)
{
    vec3 delta = normal - anchor;
    return (delta - .5 * anchor * dot(delta, anchor)) / (HYBRID_PI * .4886025119);
}
vec2 ProbeProject(vec3 position, mat4 matrix)
{
    vec4 clip = matrix * vec4(position, 1);
    return clip.w > 0.0 ? (clip.xy / clip.w * .5 + .5) * vec2(hybrid.resolution.xy) : vec2(-1e8);
}
float ProbePlaneWeight(vec3 position, vec3 normal, ScreenProbe probe)
{
    vec3  delta     = position - probe.position.xyz;
    float tolerance = max(screenProbe.control.x * .00002, length(delta) * .04);
    float plane     = max(abs(dot(delta, normal)), abs(dot(delta, probe.normal.xyz)));
    return probe.position.w * pow(max(dot(normal, probe.normal.xyz), 0.0), 16.0)
         * exp2(-plane * plane / (tolerance * tolerance));
}
uint ProbeBaseTile(uint index)
{
    return index < screenProbe.grid.z ? index : (index - screenProbe.grid.z) * screenProbe.adaptiveTiles;
}
float ProbeOctDensity(vec3 direction)
{
    float l1 = dot(abs(direction), vec3(1));
    return 16.0 / (l1 * l1 * l1);
}
uint ProbeOctBin(vec3 direction)
{
    uvec2 bin = min(uvec2(EncodeGBufferNormal(direction) * 8.0), uvec2(7));
    return bin.x + bin.y * 8u;
}
#    ifdef SCREEN_CDF_READ
uint ProbeScheduledBatches(uint index)
{
    return probeCDF[index * ZEN_SCREEN_PROBE_PROPOSAL_WORDS + 33u] >> 24u;
}
#    endif
#    ifdef SCREEN_REPROJECT_LANES
// A relocated probe samples the previous field at its new world position.
// Keep raw histories at their original anchors. Repeated feedback of spatially
// filtered lighting would move energy along the camera path on each relocation.
shared uint  pastIndices[18];
shared float pastWeights[18];
shared vec2  pastOffsets[18];
shared float pastSupport;
shared float pastWeightSum;
void         ProbeHistoryFootprint(ScreenProbe probe, uint lane)
{
    uint  frame = probe.source.x >> 30u;
    vec2  pixel = ProbeProject(probe.position.xyz, screenProbe.previousProjectionViews[frame]);
    ivec2 tile  = ivec2(floor(pixel / float(screenProbe.spacing)));
    for (uint tap = lane; tap < 18u; tap += SCREEN_REPROJECT_LANES)
    {
        uint  neighbor = tap / 2u;
        ivec2 cell     = tile + ivec2(int(neighbor % 3u) - 1, int(neighbor / 3u) - 1);
        float weight   = 0.0;
        vec2  offset   = vec2(0);
        uint  index    = 0u;
        if (all(greaterThanEqual(cell, ivec2(0))) && all(lessThan(cell, ivec2(screenProbe.grid.xy))))
        {
            uint base = uint(cell.x) + screenProbe.grid.x * uint(cell.y);
            index     = ((tap & 1u) == 0u ? base : screenProbe.grid.z + base / screenProbe.adaptiveTiles) | (frame << 30u);
            ScreenProbe old = PreviousProbe(index);
            if (old.source.w == probe.source.w && old.position.w != 0.0
                && ((tap & 1u) == 0u || all(equal(ivec2(old.source.yz / screenProbe.spacing), cell))))
            {
                offset    = (ProbeProject(old.position.xyz, screenProbe.previousProjectionViews[frame]) - pixel)
                          / (1.25 * float(screenProbe.spacing));
                vec2 tent = max(vec2(1) - abs(offset), vec2(0));
                weight    = tent.x * tent.y * ProbePlaneWeight(probe.position.xyz, probe.normal.xyz, old);
            }
        }
        pastIndices[tap] = index;
        pastWeights[tap] = weight;
        pastOffsets[tap] = weight * offset;
    }
    barrier();
    if (lane == 0u)
    {
        float total = 0.0;
        vec2  first = vec2(0);
        for (uint tap = 0u; tap < 18u; ++tap)
        {
            total += pastWeights[tap];
            first += pastOffsets[tap];
        }
        vec2 offset   = abs(first) / max(total, 1e-20);
        pastWeightSum = total;
        // Near a disocclusion the available anchors may all lie to one side.
        // Only the overlapping part of their footprint supplies prior samples.
        pastSupport = min(total, 1.0) * max(1.0 - max(offset.x, offset.y), 0.0);
    }
    barrier();
}
vec4 ProbePastValue(ScreenProbe probe, uint item, out float radianceVariance)
{
    vec4  value  = vec4(0);
    float second = 0.0;
    for (uint tap = 0u; tap < 18u; ++tap)
    {
        float weight = pastWeights[tap];
        if (weight > 0.0)
        {
            uint index       = pastIndices[tap];
            vec4 sampleValue = PreviousProbeLighting(index, item);
            if ((item & 3u) == 3u)
            {
                vec3 anchor = PreviousProbe(index).normal.xyz;
                for (uint channel = 0u; channel < 3u; ++channel)
                {
                    vec4 sh               = PreviousProbeLighting(index, (item & 4u) + channel);
                    sampleValue[channel] += ProbeNormalCorrection(ProbeNormalResponse(sh, anchor), anchor, probe.normal.xyz);
                }
            }
            if ((item & 3u) == 3u)
            {
                float luma  = HybridLuminance(sampleValue.rgb);
                second     += weight * luma * luma;
            }
            value += sampleValue * weight;
        }
    }
    value            /= max(pastWeightSum, 1e-20);
    float mean        = HybridLuminance(value.rgb);
    radianceVariance  = max(second / max(pastWeightSum, 1e-20) - mean * mean, 0.0);
    return value;
}
vec4 ProbePastValue(ScreenProbe probe, uint item)
{
    float unused;
    return ProbePastValue(probe, item, unused);
}
float ProbeRelocatedBatches(vec4 statistics, float measuredBatches, float spatialVariance)
{
    float frameVariance = max(statistics.w, statistics.y - statistics.x * statistics.x);
    float priorVariance = frameVariance + max(statistics.z, 1.0) * spatialVariance;
    float fraction      = priorVariance > 0.0 ? frameVariance / priorVariance : 1.0;
    return measuredBatches * pastSupport * fraction;
}
#    endif
// Each direction distribution stores its normalized first angular moment and radiant weight.
struct ProbeLobe
{
    vec3  axis;
    float concentration;
    float weight;
};
ProbeLobe ProbeReadLobe(uint axis, uint shape)
{
    vec2 data = unpackHalf2x16(shape);
    return ProbeLobe(DecodeGBufferNormal(unpackUnorm2x16(axis)), data.x, shape != 0u ? exp2(data.y) : 0.0);
}
uvec2 ProbeWriteLobe(vec4 moment)
{
    float magnitude = length(moment.xyz);
    vec3  axis      = magnitude > 0.0 ? moment.xyz / magnitude : vec3(0, 0, 1);
    uint  shape     = moment.w > 0.0 ? packHalf2x16(vec2(clamp(magnitude / moment.w, .0001, 1.0), log2(moment.w))) : 0u;
    return uvec2(packUnorm2x16(EncodeGBufferNormal(axis)), shape);
}
#endif
#endif
