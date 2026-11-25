// Screen probes integrate the residual environment. Each source tile (a compact sun) is lit per
// pixel instead, so its visibility is resolved at pixel resolution and never enters the probes'
// sparse Monte Carlo estimates. Like the deferred receiver visibility, a coarse pass traces every
// eighth pixel and the full-resolution pass traces only where coplanar coarse samples disagree.
// Each source is treated as directional (screen_probe_sources_setup.comp): its ray follows the
// tile's median direction, which lies in the source, and carries the tile's energy, so the result
// is noise free.
#define HYBRID_HARDWARE_PROVIDER 1
#include "hybrid_provider.glsl"
#include "hybrid_guides.glsl"
#include "hybrid_sampling.glsl"
#include "Graphics/Shared/EnvironmentSampling.h"
#include "Graphics/Shared/ScreenProbeGI.h"
#include "screen_probe_source_lights.glsl"
#ifdef SCREEN_PROBE_SOURCES_COARSE
layout(set = 1, binding = 130, r32ui) uniform writeonly uimage2D coarseSourceVisibility;
#else
layout(set = 1, binding = 129, r32ui) uniform writeonly uimage2D probeSourceVisibility;
layout(set = 1, binding = 130) uniform usampler2D coarseSourceVisibility;
#endif
layout(local_size_x = 8, local_size_y = 8) in;
// Bits of the sources above the receiver's geometric horizon.
uint SourceCandidates(vec3 geometric)
{
    uint candidates = 0u;
    for (uint source = 0u; source < ZEN_ENVIRONMENT_MAX_SOURCES; ++source)
    {
        vec4 direction = sourceLights[2u * source];
        if (direction.w > 0.0 && dot(geometric, direction.xyz) > 0.0)
        {
            candidates |= 1u << source;
        }
    }
    return candidates;
}
// Bits of the candidate sources the receiver sees.
uint TraceSources(vec3 position, vec3 geometric, uint candidates)
{
    uint visible = 0u;
    vec3 origin  = HybridTraceOrigin(position, geometric, 0.0);
    while (candidates != 0u)
    {
        int source  = findLSB(candidates);
        candidates &= candidates - 1u;
        if (!HardwareOccluded(origin, sourceLights[2 * source].xyz, 1e20))
        {
            visible |= 1u << uint(source);
        }
    }
    return visible;
}
#ifndef SCREEN_PROBE_SOURCES_COARSE
// Reuse the visibility of the surrounding coarse samples that lie on this surface where they agree.
uint RefinedSourceVisibility(ivec2 pixel, vec3 position, vec3 geometric, uint candidates)
{
    const int stride     = int(ZEN_SCREEN_PROBE_SOURCE_STRIDE);
    uint      allVisible = 0xffffffffu, anyVisible = 0u;
    bool      support  = false;
    uint      identity = texelFetch(receiverSurface, pixel, 0).g;
    ivec2     base     = ivec2(floor((vec2(pixel) - float(stride / 2)) / float(stride)));
    bool      exact    = all(equal(pixel % stride, ivec2(stride / 2)));
    for (uint tap = 0u; tap < (exact ? 1u : 4u); ++tap)
    {
        ivec2 coarse      = clamp(base + ivec2(tap & 1u, tap >> 1u), ivec2(0), textureSize(coarseSourceVisibility, 0) - 1);
        ivec2 samplePixel = min(coarse * stride + stride / 2, textureSize(receiverDepth, 0) - 1);
        vec3  other       = HybridGeometricNormal(samplePixel);
        vec3  delta       = HybridPosition(samplePixel) - position;
        float limit       = max(1e-6, length(delta) * .02);
        bool  coplanar    = texelFetch(receiverSurface, samplePixel, 0).g == identity
                         && texelFetch(receiverDepth, samplePixel, 0).r < 1.0 && dot(geometric, other) > .995
                         && max(abs(dot(delta, geometric)), abs(dot(delta, other))) <= limit;
        if (coplanar)
        {
            uint bits   = texelFetch(coarseSourceVisibility, coarse, 0).r;
            allVisible &= bits;
            anyVisible |= bits;
            support     = true;
        }
    }
    // Trace the sources whose coplanar samples disagree, or every candidate without one.
    uint pending = support ? candidates & (allVisible ^ anyVisible) : candidates;
    uint reused  = support ? allVisible & candidates & ~pending : 0u;
    return reused | TraceSources(position, geometric, pending);
}
#endif
