#define SCREEN_CDF_READ
#ifndef SCREEN_PROBE_PREFILTER_HITS
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
shared vec4  directions[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
shared vec4  radiance[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
shared float densities[ZEN_SCREEN_PROBE_SPECULAR_RAYS];
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
void main()
{
    uint index = ProbeGroupIndex(), lane = gl_LocalInvocationID.x;
    if (index < screenProbe.grid.w)
    {
        ScreenProbe probe = probes[index];
        if (probe.position.w == 0.0)
        {
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
        for (uint sampleIndex = lane; sampleIndex < sampleCount; sampleIndex += 64u)
        {
#ifdef SCREEN_PROBE_PREFILTER_HITS
            uint address = ProbeRecordAddress(
                ProbeRecordSlot(index, sampleIndex % ZEN_SCREEN_PROBE_RAYS, sampleIndex / ZEN_SCREEN_PROBE_RAYS));
            // The trace has already resolved these samples for diffuse lighting.
            bool sky      = (probeHits[address] & (kHitValid | kHitSurface)) == kHitValid;
            vec4 ray      = ProbeHitSample(address);
            vec3 incoming = ProbeHitRadiance(address);
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
                uvec2 packed   = PreviousProbeSpecular(probe.source.x, texel);
                vec4  history  = vec4(unpackHalf2x16(packed.x), unpackHalf2x16(packed.y));
                float age      = min(probeLighting[index * ZEN_SCREEN_PROBE_LIGHTING_VECTORS + 8u].z,
                                     probeLighting[index * ZEN_SCREEN_PROBE_LIGHTING_VECTORS + 9u].z);
                float prior    = (max(age, 1.0) - 1.0) * float(max(sampleCount, 1u));
                current       += history * prior;
                totalWeight   += prior;
            }
            current                    = clamp(current / max(totalWeight, 1e-20), vec4(0), vec4(65504, 65504, 65504, 1));
            uint address               = index * ZEN_SCREEN_PROBE_SPECULAR_TEXELS + texel;
            probeSpecularData[address] = StochasticPackHalf(current, address ^ HybridHash(hybrid.sampling.x ^ 0x5f3a1u));
        }
    }
}
