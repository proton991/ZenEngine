#ifndef ZEN_WORLD_CACHE_LIGHTING
#define ZEN_WORLD_CACHE_LIGHTING
#include "../Common/light_parameters.glsl"
const uint kWorldEditCapacity = 32u;
struct WorldLightEdit
{
    SceneLight before;
    SceneLight after;
    uvec4      stamp;
};
#ifdef WRC_LIGHTING_WRITE
layout(std430, set = 1, binding = 78) buffer WorldCacheLighting
#else
layout(std430, set = 1, binding = 78) readonly buffer WorldCacheLighting
#endif
{
    uvec4          cacheStats;   // exact probe hits, other probe hits, resident cells, inserts rejected
    uvec4          lookupCounts; // parent usable, absent, unresolved, last global lighting edit frame
    uvec4          sourceSignature;
    uvec4          lightChange;
    uvec4          editSources;
    vec4           changedMinimum;
    vec4           changedMaximum;
    vec4           previousLights[33];
    uvec4          lightFootprint[9];
    uvec4          nextLightFootprint[9];
    uvec4          editLog; // Next write index, valid entries, last overwritten frame, local response enabled.
    uvec4          lastLightIds[16];
    SceneLight     lastLights[33];
    WorldLightEdit lightEdits[kWorldEditCapacity];
    vec4           lastEnvironment, lastOrientation;
    // Each entry retains the state immediately before an edit. Sparse cells walk back to lastSample.
    vec4  envBefore[8], orientationBefore[8];
    uvec4 envFrames[2];
    uvec4 envLog;
    uvec4 responseLog; // last settled edit, current frame, direct/feedback warmup confidence
    // Movable lights and the light ball keep their own pose log. A light that moves every frame
    // cannot evict a settled edit that sparse cells still need to reconstruct.
    uvec4          movingLog; // next write index, valid entries, last overwritten frame, last moving edit frame
    WorldLightEdit movingEdits[kWorldEditCapacity];
    uvec4          seedCounts; // age-zero probes, exact warm hits, warm parent hits
};
#include "history_response.glsl"
// Only probes that observed an edit wait for the secondary cache to refill. Its
// feedback confidence accounts for the successive direct and indirect solves.
float ProbeObservationConfidence(vec2 prior)
{
    float startup = uintBitsToFloat(responseLog.w);
    float settled = uintBitsToFloat(sourceSignature.w);
    return max(.001, min(startup, mix(1.0, settled, smoothstep(.005, .1, prior.x))) * prior.y);
}
bool WorldMovingHistoryComplete(uint elapsed)
{
    return movingLog.z == 0u || responseLog.y - movingLog.z >= elapsed;
}
bool WorldFollowsLocalEdit(uint signature)
{
    return lightChange.z != 0u && signature != 0u && any(equal(editSources, uvec4(signature)));
}
float WorldDistantFromEdit(vec3 position)
{
    vec3  delta = max(max(changedMinimum.xyz - position, position - changedMaximum.xyz), vec3(0));
    float size  = max(length(changedMaximum.xyz - changedMinimum.xyz), 1e-6);
    return lightChange.z != 0u ? smoothstep(2.0 * size, 6.0 * size, length(delta)) : 0.0;
}
#endif
