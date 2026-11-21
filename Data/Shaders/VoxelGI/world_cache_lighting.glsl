#ifndef ZEN_WORLD_CACHE_LIGHTING
#define ZEN_WORLD_CACHE_LIGHTING
#ifdef WRC_LIGHTING_WRITE
layout(std430, set = 1, binding = 78) buffer WorldCacheLighting
#else
layout(std430, set = 1, binding = 78) readonly buffer WorldCacheLighting
#endif
{
    uvec4 cacheStats;   // exact probe hits, other probe hits, resident cells, inserts rejected
    uvec4 lookupCounts; // parent usable, absent, unresolved, last global lighting edit frame
    uvec4 sourceSignature;
    uvec4 lightChange;
    uvec4 editSources;
    vec4  changedMinimum;
    vec4  changedMaximum;
    vec4  previousLights[33];
    uvec4 lightFootprint[9];
    uvec4 nextLightFootprint[9];
};
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
