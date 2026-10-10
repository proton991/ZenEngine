#include "../Common/gbuffer.glsl"
layout(set=3,binding=9) uniform sampler2D hybridSky;
layout(set=3,binding=10) uniform sampler2D hybridSpecular;
layout(set=3,binding=11) uniform usampler2D hybridSurface;
layout(set=3,binding=12) uniform sampler2D hybridDepth;
layout(set=3,binding=13) uniform sampler2D hybridBounce;
bool HybridReceiverMatches(uint nodeIndex, bool facing)
{
    ivec2 p=ivec2(gl_FragCoord.xy);
    uint identity=(nodeIndex+1u)*2u+(facing ? 1u:0u);
    return texelFetch(hybridSurface,p,0).g==identity && abs(texelFetch(hybridDepth,p,0).r-gl_FragCoord.z)<1e-6;
}
