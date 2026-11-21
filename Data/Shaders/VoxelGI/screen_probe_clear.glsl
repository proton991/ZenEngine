#define SCREEN_PROBE_WRITE
#define SCREEN_LIGHT_WRITE
#define SCREEN_PROBE_CLEAR
#define SCREEN_CDF_WRITE
#define SCREEN_PROBE_COMPUTE
#include "screen_probe.glsl"
layout(local_size_x = 64) in;
void main()
{
    uint index = ProbeGroupIndex();
    uint lane  = gl_LocalInvocationID.x;
    if (index < screenProbe.grid.w)
    {
        if (lane == 0u)
        {
            ScreenProbe empty               = ScreenProbe(vec4(0), vec4(0), uvec4(0xffffffffu, 0, 0, 0));
            probes[index]                   = empty;
            previousProbes[0].values[index] = empty;
            previousProbes[1].values[index] = empty;
            previousProbes[2].values[index] = empty;
        }
        for (uint item = lane; item < ZEN_SCREEN_PROBE_PROPOSAL_WORDS; item += gl_WorkGroupSize.x)
        {
            uint address                        = index * ZEN_SCREEN_PROBE_PROPOSAL_WORDS + item;
            uint word                           = item < 32u ? packUnorm2x16((vec2(1.0, 2.0) + float(2u * item)) / 64.0) : 0u;
            probeCDF[address]                   = word;
            previousProbeCDF[0].values[address] = word;
            previousProbeCDF[1].values[address] = word;
            previousProbeCDF[2].values[address] = word;
        }
        for (uint item = lane; item < ZEN_SCREEN_PROBE_LIGHTING_VECTORS; item += gl_WorkGroupSize.x)
        {
            probeLighting[index * ZEN_SCREEN_PROBE_LIGHTING_VECTORS + item]                   = vec4(0);
            previousProbeLighting[0].values[index * ZEN_SCREEN_PROBE_LIGHTING_VECTORS + item] = vec4(0);
            previousProbeLighting[1].values[index * ZEN_SCREEN_PROBE_LIGHTING_VECTORS + item] = vec4(0);
            previousProbeLighting[2].values[index * ZEN_SCREEN_PROBE_LIGHTING_VECTORS + item] = vec4(0);
        }
#ifndef SCREEN_PROBE_DIFFUSE_ONLY
        for (uint item = lane; item < ZEN_SCREEN_PROBE_SPECULAR_TEXELS; item += gl_WorkGroupSize.x)
        {
            probeSpecularData[index * ZEN_SCREEN_PROBE_SPECULAR_TEXELS + item]                   = uvec2(0);
            previousProbeSpecularData[0].values[index * ZEN_SCREEN_PROBE_SPECULAR_TEXELS + item] = uvec2(0);
            previousProbeSpecularData[1].values[index * ZEN_SCREEN_PROBE_SPECULAR_TEXELS + item] = uvec2(0);
            previousProbeSpecularData[2].values[index * ZEN_SCREEN_PROBE_SPECULAR_TEXELS + item] = uvec2(0);
        }
#endif
    }
}
