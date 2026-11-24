#ifdef SCREEN_PROBE_PREFILTER_HITS
#    define SCREEN_CDF_READ
#else
#    define SCREEN_RAYS_READ
#endif
#define SCREEN_SPECULAR_WRITE
#define SCREEN_PROBE_COMPUTE
#include "screen_probe.glsl"
#ifdef SCREEN_PROBE_PREFILTER_HITS
#    include "screen_probe_hits.glsl"
#endif
layout(local_size_x = 64) in;
shared vec4 directions[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
shared vec4 radiance[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
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
// Integral over the reflection hemisphere of cos(theta) * alpha^2 /
// (1 + (alpha^2 - 1) * (1 + cos(theta)) / 2)^2, including azimuth.
// The diffuse ray proposal changes with the probe's direction and light history.
// Dividing by its random GGX weight sum each frame biases narrow reflected peaks.
float ProbeSpecularNormalization(float alphaSquared)
{
    float halfWidth = .5 * (1.0 - alphaSquared);
    float integral  = halfWidth > .001
                        ? (halfWidth + alphaSquared * log(alphaSquared / (1.0 - halfWidth))) / (halfWidth * halfWidth)
                        : .5 + (2.0 / 3.0) * halfWidth;
    return 2.0 * HYBRID_PI * integral;
}
void main()
{
    uint index = ProbeGroupIndex(), lane = gl_LocalInvocationID.x;
    if (index < screenProbe.grid.w)
    {
        ScreenProbe probe = probes[index];
#ifdef SCREEN_PROBE_PREFILTER_HITS
        uint sampleCount = probe.position.w != 0.0 ? ProbeScheduledBatches(index) * uint(ZEN_SCREEN_PROBE_RAYS) : 0u;
#else
        uint sampleCount = uint(ZEN_SCREEN_PROBE_SPECULAR_RAYS);
#endif
        for (uint sampleIndex = lane; sampleIndex < sampleCount; sampleIndex += 64u)
        {
#ifdef SCREEN_PROBE_PREFILTER_HITS
            uint address = ProbeRecordAddress(
                ProbeRecordSlot(index, sampleIndex % ZEN_SCREEN_PROBE_RAYS, sampleIndex / ZEN_SCREEN_PROBE_RAYS));
            // The trace has already resolved these samples for diffuse lighting.
            bool sky                = (probeHits[address] & (kHitValid | kHitSurface)) == kHitValid;
            directions[sampleIndex] = ProbeHitSample(address);
            radiance[sampleIndex]   = sky ? vec4(0, 0, 0, 1) : vec4(ProbeHitRadiance(address), 0);
#else
            uint address = ProbeSpecularRayAddress(index, sampleIndex);
            vec2 rg = screenRays[address], blueDensity = screenRays[address + 1u];
            directions[sampleIndex] = vec4(DecodeGBufferNormal(screenRays[address + 2u]), abs(blueDensity.y));
            float sky               = float(blueDensity.y < 0.0);
            radiance[sampleIndex]   = vec4(vec3(rg, blueDensity.x) * (1.0 - sky), sky);
#endif
        }
        barrier();
        for (uint texel = lane; texel < ZEN_SCREEN_PROBE_SPECULAR_TEXELS; texel += 64u)
        {
            uint  level     = texel < 64u ? 0u : 1u + (texel - 64u) / 16u;
            uint  offset    = level == 0u ? 0u : 64u + (level - 1u) * 16u;
            uint  side      = level == 0u ? 8u : 4u;
            vec3  axis      = DecodeGBufferNormal((vec2((texel - offset) % side, (texel - offset) / side) + .5) / float(side));
            float roughness = .25 * float(level + 1u);
            float alpha = roughness * roughness, alphaSquared = alpha * alpha;
            vec4  sum     = vec4(0);
            uint  samples = 0u;
            for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
            {
                vec4  ray               = directions[sampleIndex];
                float cosine            = max(dot(axis, ray.xyz), 0.0);
                float halfCosineSquared = .5 * (cosine + 1.0);
                float denominator       = 1.0 + (alphaSquared - 1.0) * halfCosineSquared;
                // GGX prefilter at V=N=axis. The proposal density is shared with the diffuse estimator.
                float weight  = ray.w > 0.0 ? cosine * alphaSquared / (denominator * denominator * ray.w) : 0.0;
                sum          += radiance[sampleIndex] * weight;
                samples      += uint(ray.w > 0.0);
            }
            vec4 current = samples != 0u ? sum / (float(samples) * ProbeSpecularNormalization(alphaSquared)) : vec4(0);
            if (probe.source.x != 0xffffffffu && screenProbe.control.z != 0.0)
            {
                uvec2 packed  = PreviousProbeSpecular(probe.source.x, texel);
                vec4  history = vec4(unpackHalf2x16(packed.x), unpackHalf2x16(packed.y));
                float age     = min(probeLighting[index * ZEN_SCREEN_PROBE_LIGHTING_VECTORS + 8u].z,
                                    probeLighting[index * ZEN_SCREEN_PROBE_LIGHTING_VECTORS + 9u].z);
                current       = mix(history, current, 1.0 / max(age, 1.0));
            }
            current                    = clamp(current, vec4(0), vec4(65504, 65504, 65504, 1));
            uint address               = index * ZEN_SCREEN_PROBE_SPECULAR_TEXELS + texel;
            probeSpecularData[address] = StochasticPackHalf(current, address ^ HybridHash(hybrid.sampling.x ^ 0x5f3a1u));
        }
    }
}
