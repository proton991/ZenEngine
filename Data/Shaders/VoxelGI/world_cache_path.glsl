// One path per screen tile. Only the trace and shade stages of that path access this record.
struct WorldPath
{
    vec4  origin; // Unoffset previous vertex and packed geometric normal
    vec4  ray;    // Direction and inverse pi*PDF of the previous sample
    uvec4 state;  // Previous slot, shading normal, cumulative albedo luminance, path status
    uvec4 hit;    // Recorded instance, geometry, primitive and barycentrics
};
#ifdef WORLD_PATH_START
layout(std430, set = 1, binding = 113) writeonly buffer WorldCachePaths
#else
layout(std430, set = 1, binding = 113) buffer WorldCachePaths
#endif
{
    WorldPath paths[];
};
const uint kDead = 0u, kMaterial = 1u, kTerminal = 2u, kIncludesReceiver = 3u;
