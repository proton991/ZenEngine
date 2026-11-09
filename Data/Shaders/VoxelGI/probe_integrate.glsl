#include "probe_pixel.glsl"
layout(set = 7, binding = 30) uniform sampler2D probeSourceSky;
layout(std430, set = 7, binding = 2) readonly buffer ProbeInput
{
    ProbeRecord probeInput[];
};
layout(std430, set = 7, binding = 23) readonly buffer ProbeSubtiles
{
    uvec2 probeSubtiles[];
};

struct ProbeIntegration
{
    vec4  sky, bounce, fallbackSky, fallbackBounce;
    float weight, fallbackWeight;
};

void ProbeGatherDiffuse(ProbeRecord            sampleValue,
                        ivec2                  pixel,
                        ProbeReceiver          receiver,
                        float                  footprint,
                        inout ProbeIntegration sum)
{
    float screen    = ProbeScreenWeight(sampleValue.screen.xy, vec2(pixel), 2.0);
    float w         = ProbeSurfaceWeight(receiver.position, receiver.geometric, footprint, sampleValue) * screen;
    vec4  skyAt     = ProbeSkyIrradiance(sampleValue, receiver.normal);
    vec4  bounceAt  = ProbeBounceIrradiance(sampleValue, receiver.normal);
    sum.sky        += skyAt * w;
    sum.bounce     += bounceAt * w;
    sum.weight     += w;
    // Last resort for a receiver no compatible probe covers (even after adaptive placement ran out
    // of rounds or capacity): the best-facing nearby probe, ignoring plane distance. A dark hole is
    // more visible than this mismatch.
    float facing        = sampleValue.position.w > 0.5 ? max(dot(receiver.geometric, sampleValue.normal.xyz), 0.0) : 0.0;
    float fallback      = facing * facing * facing * facing * screen;
    sum.fallbackSky    += skyAt * fallback;
    sum.fallbackBounce += bounceAt * fallback;
    sum.fallbackWeight += fallback;
}

void ProbeIntegratePixel(ivec2 pixel, ProbeReceiver receiver, out vec4 rawSky, out vec4 rawBounce)
{
    ProbeIntegration sum;
    float            footprint;
    sum.sky            = vec4(0);
    sum.bounce         = vec4(0);
    sum.fallbackSky    = vec4(0);
    sum.fallbackBounce = vec4(0);
    sum.weight         = 0.0;
    sum.fallbackWeight = 0.0;
    footprint          = 0.0;
    if (receiver.valid)
    {
        footprint     = ProbePixelFootprint(pixel);
        ivec2 tile    = pixel / int(probe.grid.z);
        ivec2 subtile = pixel / int(ZEN_PROBE_SUBTILE);
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                ivec2 sampleCoordinate = tile + ivec2(x, y);
                if (ProbeGridInside(sampleCoordinate))
                {
                    ProbeGatherDiffuse(probeInput[ProbeIndex(sampleCoordinate)], pixel, receiver, footprint, sum);
                }
                // Adaptive probes cover receivers no uniform probe represents.
                ivec2 sampleSubtile = subtile + ivec2(x, y);
                uvec2 slots         = ProbeSubtileInside(sampleSubtile) ? probeSubtiles[ProbeSubtileIndex(sampleSubtile)]
                                                                        : uvec2(ZEN_PROBE_INVALID);
                for (uint entry = 0u; entry < ZEN_PROBE_ADAPTIVE_PER_SUBTILE; ++entry)
                {
                    if (slots[entry] < ProbeSlotCount())
                    {
                        ProbeGatherDiffuse(probeInput[slots[entry]], pixel, receiver, footprint, sum);
                    }
                }
            }
        }
    }
    // Only a receiver without any facing probe nearby stays dark.
    if (sum.weight < 1e-4 && sum.fallbackWeight > 0.0)
    {
        sum.sky    = sum.fallbackSky;
        sum.bounce = sum.fallbackBounce;
        sum.weight = sum.fallbackWeight;
    }
    sum.sky    /= max(sum.weight, 1e-8);
    sum.bounce /= max(sum.weight, 1e-8);
    if (receiver.valid)
    {
        // Contact occlusion closer than a probe tile, applied to probe-reconstructed diffuse light.
        // Bright sources have receiver visibility already and are added afterwards.
        float contact   = ProbeContactVisibility(pixel, receiver.position, receiver.geometric, footprint);
        sum.sky        *= contact;
        sum.bounce.rgb *= contact;
    }
    // Alpha -1 is missing data, distinct from a valid fully occluded sample. A missing source
    // sample only skips the source history (probe_pixel_temporal.glsl); the probe residual
    // remains a valid observation.
    vec4 source   = texelFetch(probeSourceSky, pixel, 0);
    sum.sky.rgb  += source.rgb;
    bool missing  = receiver.valid && sum.weight == 0.0;
    rawSky        = vec4(max(sum.sky.rgb, vec3(0)), missing ? -1.0 : clamp(sum.sky.a, 0.0, 1.0));
    rawBounce     = vec4(max(sum.bounce.rgb, vec3(0)), missing ? -1.0 : clamp(sum.bounce.a, 0.0, 1.0));
}
