#define SCREEN_LIGHT_READ_WRITE
#define SCREEN_PROBE_COMPUTE
#define SCREEN_REPROJECT_LANES uint(ZEN_SCREEN_PROBE_RAYS)
#define WRC_LIGHTING_WRITE
#include "screen_probe.glsl"
#include "world_cache.glsl"
layout(local_size_x = ZEN_SCREEN_PROBE_RAYS) in;
void main()
{
    uint index = ProbeGroupIndex();
    uint lane  = gl_LocalInvocationID.x;
    if (index < screenProbe.grid.w)
    {
        ScreenProbe probe   = probes[index];
        bool        fresh   = probe.source.x == 0xffffffffu;
        float       missing = fresh ? 1.0 : 0.0;
        if (probe.position.w != 0.0 && !fresh && probe.position.xyz != PreviousProbe(probe.source.x).position.xyz)
        {
            ProbeHistoryFootprint(probe, lane);
            missing = 1.0 - pastSupport;
        }
        if (lane == 0u && probe.position.w != 0.0 && missing > 0.0)
        {
            vec3 geometric = HybridGeometricNormal(ivec2(probe.source.yz));
            vec3 prior;
            bool parent;
            bool warm = WorldIndirect(probe.position.xyz, geometric, probe.normal.xyz, 4u, prior, parent);
            // These diagnostics count newly born probes, independently of relocation support.
            if (fresh)
            {
                atomicAdd(seedCounts.x, 1u);
                if (warm && parent)
                {
                    atomicAdd(seedCounts.z, 1u);
                }
                else if (warm)
                {
                    atomicAdd(seedCounts.y, 1u);
                }
            }
            if (warm)
            {
                uint  base    = index * uint(ZEN_SCREEN_PROBE_LIGHTING_VECTORS);
                vec4  current = probeLighting[base + 7u];
                float batches = missing * min(1.0, current.w / 3.0);
                float alpha   = batches / max(current.w + batches, 1e-20);
                // Uniform incident radiance over the anchor hemisphere integrates to pi L.
                vec4 sh = vec4(.2820947918 * 2.0, .4886025119 * probe.normal.xyz) * HYBRID_PI;
                for (uint channel = 0u; channel < 3u; ++channel)
                {
                    uint item           = base + 4u + channel;
                    probeLighting[item] = mix(probeLighting[item], sh * prior[channel], alpha);
                }
                probeLighting[base + 7u] = vec4(mix(current.rgb, prior, alpha), current.w + batches);
                vec4  moments            = probeLighting[base + 9u];
                float mean               = HybridLuminance(prior);
                moments.xy               = mix(moments.xy, vec2(mean, mean * mean), alpha);
                probeLighting[base + 9u] = moments;
            }
        }
    }
}
