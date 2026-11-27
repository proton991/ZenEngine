#include "hybrid_guides.glsl"
#include "Graphics/Shared/LightingCapture.h"
#if defined(SCREEN_PROBE_CAPTURE) || defined(WORLD_CACHE_CAPTURE)
#    include "world_cache.glsl"
#endif
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 1, binding = 6) uniform sampler2D rawSky;
layout(set = 1, binding = 7) uniform sampler2D rawSpecular;
layout(set = 1, binding = 8) uniform sampler2D reconstructedSky;
layout(set = 1, binding = 9) uniform sampler2D reconstructedSpecular;
#ifndef SCREEN_PROBE_CAPTURE
layout(set = 1, binding = 10) uniform sampler2D moments;
layout(set = 1, binding = 11) uniform sampler2D historyLength;
#endif
layout(set = 1, binding = 12) uniform sampler2D rawBounce;
layout(set = 1, binding = 13) uniform sampler2D reconstructedBounce;
#ifndef SCREEN_PROBE_CAPTURE
layout(set = 1, binding = 14) uniform sampler2D rawReflection;
layout(set = 1, binding = 15) uniform sampler2D reflectionHistory;
#endif
#ifdef DENOISE_VALIDATION
layout(set = 1, binding = 16) uniform sampler2DArray temporalValidation;
layout(set = 1, binding = 17) uniform sampler2DArray repairValidation;
#endif
layout(set = 4, binding = 0, std430) writeonly buffer HybridCapture
{
    vec4 diagnostics[];
};
void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (!HybridInView(p))
    {
        return;
    }
    // Raw observations and history diagnostics remain nearest samples of the actual GI grid.
    ivec2 q        = min(p / int(max(hybrid.resolution.z, 1u)), textureSize(rawSky, 0) - 1);
    uint  index    = uint(p.x + textureSize(receiverDepth, 0).x * p.y) * ZEN_HYBRID_CAPTURE_COMPONENTS;
    bool  valid    = texelFetch(receiverDepth, p, 0).r < 1.0 && texelFetch(receiverSurface, p, 0).g != 0u;
    vec3  position = valid ? HybridPosition(p) : vec3(0);
    vec3  n = HybridNormal(p), ng = HybridGeometricNormal(p);
    diagnostics[index + 0] = texelFetch(rawSky, q, 0);
    diagnostics[index + 1] = texelFetch(reconstructedSky, min(p, textureSize(reconstructedSky, 0) - 1), 0);
    diagnostics[index + 2] = texelFetch(rawBounce, q, 0);
    diagnostics[index + 3] = texelFetch(reconstructedBounce, p, 0);
    diagnostics[index + 4] = texelFetch(rawSpecular, q, 0);
    diagnostics[index + 5] = texelFetch(reconstructedSpecular, p, 0);
#ifdef SCREEN_PROBE_CAPTURE
    diagnostics[index + 6] = vec4(0);
    diagnostics[index + 7] = vec4(texelFetch(rawSky, q, 0).a, texelFetch(rawBounce, q, 0).a, 0, 0);
#else
    diagnostics[index + 6] = texelFetch(moments, q, 0);
    diagnostics[index + 7] = texelFetch(historyLength, q, 0);
#endif
    diagnostics[index + 8]  = vec4(position, valid ? 1.0 : 0.0);
    diagnostics[index + 9]  = vec4(n, texelFetch(receiverRoughness, p, 0).g);
    diagnostics[index + 10] = vec4(ng, texelFetch(receiverDepth, p, 0).r);
    diagnostics[index + 11] = HybridMotion(p);
#ifdef SCREEN_PROBE_CAPTURE
    vec2  hit    = vec2(0);
    float length = 0.0;
#else
    vec2  hit    = hybrid.reflection.x != 0u ? texelFetch(rawReflection, q, 0).rg : vec2(0);
    float length = hybrid.reflection.x != 0u ? texelFetch(reflectionHistory, q, 0).w : 0.0;
#endif
    diagnostics[index + 12] = vec4(uintBitsToFloat(texelFetch(receiverSurface, p, 0).g), hit, length);
    for (int layer = 0; layer < 5; ++layer)
    {
#ifdef DENOISE_VALIDATION
        diagnostics[index + 13u + uint(layer)] = layer < 3 ? texelFetch(temporalValidation, ivec3(q, layer), 0)
                                                           : texelFetch(repairValidation, ivec3(q, layer - 3), 0);
#else
        diagnostics[index + 13u + uint(layer)] = vec4(0);
#endif
    }
#if defined(SCREEN_PROBE_CAPTURE) || defined(WORLD_CACHE_CAPTURE)
    diagnostics[index + 13u] = vec4(cacheStats.xyz, worldCache.schedule.x);
    diagnostics[index + 14u] = vec4(lookupCounts.xyz, cacheStats.w);
    bool found               = false;
    vec3 irradiance          = valid ? WorldLookup(position, ng, n, found) : vec3(0);
    diagnostics[index + 15u] = vec4(irradiance, float(found));
    uint slot                = valid ? WorldFind(position, ng, WorldLevel(position), ivec3(0)) : 0xffffffffu;
    if (slot != 0xffffffffu)
    {
        diagnostics[index + 16u] = vec4(position, float(cacheResolved[slot * GI_CACHE_RECORDS].z));
        diagnostics[index + 17u] = vec4(ng, CacheCellSize(WorldLevel(position)));
    }
#endif
}
