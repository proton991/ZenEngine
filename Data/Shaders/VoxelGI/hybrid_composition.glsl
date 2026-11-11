#include "../Common/gbuffer.glsl"
#include "hybrid_common.glsl"
layout(set = 3, binding = 9) uniform sampler2D hybridSky;
layout(set = 3, binding = 10) uniform sampler2D hybridSpecular;
layout(set = 3, binding = 11) uniform usampler2D hybridSurface;
layout(set = 3, binding = 12) uniform sampler2D hybridDepth;
layout(set = 3, binding = 13) uniform sampler2D hybridBounce;
vec3 HybridSky(ivec2 pixel)
{
    // A disabled sky uses a one-texel zero placeholder.
    return texelFetch(hybridSky, min(pixel, textureSize(hybridSky, 0) - 1), 0).rgb;
}
bool HybridReceiverMatches(uint nodeIndex, bool facing)
{
    ivec2 p        = ivec2(gl_FragCoord.xy);
    uint  identity = (nodeIndex + 1u) * 2u + (facing ? 1u : 0u);
    return texelFetch(hybridSurface, p, 0).g == identity && abs(texelFetch(hybridDepth, p, 0).r - gl_FragCoord.z) < 1e-6;
}
#include "hybrid_receiver_reference.glsl"
