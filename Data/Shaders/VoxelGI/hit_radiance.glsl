#ifndef ZEN_HIT_RADIANCE
#define ZEN_HIT_RADIANCE
#ifdef HIT_LIGHT_CACHE
#    include "light_visibility_cache.glsl"
#    include "voxel_surface_class.glsl"
// Light-visibility caches of hitRadiance and hitRadiancePrevious (light_visibility_cache.glsl).
layout(std430, set = 1, binding = 40) readonly buffer HitLightCache
{
    uvec4      lightCount;
    SceneLight cacheLights[MAX_SCENE_LIGHTS];
    uint       classMasks[];
}
hitLightCache;
layout(std430, set = 1, binding = 41) readonly buffer HitLightCachePrevious
{
    uvec4      lightCount;
    SceneLight cacheLights[MAX_SCENE_LIGHTS];
    uint       classMasks[];
}
hitLightCachePrevious;
// Averaged reflectance, or the owner albedo that the normal's alpha weights.
layout(set = 1, binding = 42) uniform sampler3D hitReflectance;
// Whether a point or spot light reaches a hit; the trace defines it with its bounce provider.
bool HitLightVisible(HybridRayResult hit, SceneLight light);
#endif
// Base-level cache lookup shared by both query providers. The cache is isotropic,
// but its owner normal must face the incoming ray; the back of a thin wall cannot
// borrow radiance from its illuminated front. No radiance mips are sampled.
bool HitRadianceCell(sampler3D radiance, sampler3D normals, ivec3 cell, vec3 direction, out vec3 value)
{
    value      = vec3(0);
    bool valid = all(greaterThanEqual(cell, ivec3(0))) && all(lessThan(cell, textureSize(radiance, 0)));
    if (valid)
    {
        vec4 sampleValue = texelFetch(radiance, cell, 0);
        vec3 normal      = texelFetch(normals, cell, 0).rgb * 2.0 - 1.0;
        valid            = sampleValue.a > 0.0 && dot(normal, -direction) > 0.0;
        if (valid)
        {
            value = sampleValue.rgb / sampleValue.a;
        }
    }
    return valid;
}
#ifdef HIT_LIGHT_CACHE
uint HitClassMask(ivec3 cell, int surfaceClass, int resolution, bool previous)
{
    uint mask = 0u;
    if (all(greaterThanEqual(cell, ivec3(0))) && all(lessThan(cell, ivec3(resolution))))
    {
        uint index = LightCacheIndex(cell, surfaceClass, resolution);
        mask       = previous ? hitLightCachePrevious.classMasks[index] : hitLightCache.classMasks[index];
    }
    return mask;
}
// Diffuse reflectance of the voxel holding a hit, or of its neighbor on the ray's side.
vec3 HitReflectance(sampler3D normals, HybridRayResult hit, int axis, bool averaged)
{
    vec3  reflectance = vec3(0);
    bool  occupied    = false;
    ivec3 size        = textureSize(hitReflectance, 0);
    ivec3 cell        = hit.cell;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        vec4 value =
            all(greaterThanEqual(cell, ivec3(0))) && all(lessThan(cell, size)) ? texelFetch(hitReflectance, cell, 0) : vec4(0);
        if (value.a > 0.5 && !occupied)
        {
            reflectance = averaged ? value.rgb : value.rgb * texelFetch(normals, cell, 0).a;
            occupied    = true;
        }
        cell[axis] += hit.normal[axis] > 0.0 ? 1 : -1;
    }
    return reflectance;
}
// Brick hits closer than this many class cells to a cached point or spot light are re-traced
// against triangles (BrickRetraceTriangles). On New Sponza, all lights' bounce with bricks and probes
// against the CUDA reference: 0.5 cells -26% (floor -43%), 0.75 -19% (-25%), 1 -11% (-5%), 1.5
// -11% (-4%); without re-tracing -26% (-41%). The radius barely changes the cost.
#    define ZEN_BRICK_RETRACE_CELLS 1.0
// Whether a ray hit lies within ZEN_BRICK_RETRACE_CELLS class cells of a cached point or spot light.
// Lamp light reaches a room through lantern openings and frames centimetres wide, which fine voxels
// close: on New Sponza (uniform albedo, 128-cell bricks) brick traversal lost 80-85% of the floor's
// lamp bounce, and sun bounce was within 8%.
bool HitNearLocalLight(vec3 position)
{
    bool near = false;
    if (gi.limits.w > 0.5)
    {
        uvec4 header = hitLightCache.lightCount;
        float size   = gi.gridMinVoxelSize.w * gi.volume.x / float(max(int(header.z), 1));
        uint  count  = min(header.x, uint(ZEN_LIGHT_CACHE_LIGHTS));
        for (uint i = 0u; i < count && !near; ++i)
        {
            SceneLight light = hitLightCache.cacheLights[i];
            near =
                int(light.directionType.w) != 0 && distance(light.positionRange.xyz, position) < ZEN_BRICK_RETRACE_CELLS * size;
        }
    }
    return near;
}
#    ifdef HYBRID_BRICK_TRAVERSAL
// Class cells a re-trace follows triangles before bricks take over again. A ray through a
// lantern's openings would otherwise cross the room against every triangle.
#        define ZEN_BRICK_RETRACE_SEGMENT_CELLS 2.0
// Re-traces a ray queued by a brick trace (probe_trace.glsl, hybrid_trace.glsl) from one fine voxel
// before its hit next to a lamp: bricks are conservative, so no triangle lies closer. Opaque
// triangles over ZEN_BRICK_RETRACE_SEGMENT_CELLS class cells, then bricks from the segment's end,
// in mid-air, so without the starting-run skip. Alpha masks are ignored: testing them more than
// doubled the re-trace's cost in probe traces (software_ray_query.glsl).
HybridRayResult BrickRetraceTriangles(vec3 start, vec3 direction)
{
    float           size    = gi.gridMinVoxelSize.w * gi.volume.x / float(max(int(hitLightCache.lightCount.z), 1));
    float           segment = ZEN_BRICK_RETRACE_SEGMENT_CELLS * size;
    HybridRayResult result  = SoftwareTraceOpaqueRay(start, direction, segment, true);
    if (!result.hit)
    {
        result = BrickTraceRaySkip(start + direction * segment, direction, 1e20, false);
    }
    return result;
}
#    endif
// Cached scene light leaving a hit. Visibility comes from the cell's surfaces of the hit's class,
// or from a ray for near lights; attenuation and cosine are the hit's.
vec3 HitCachedLight(sampler3D normals, HybridRayResult hit, bool previous, out bool found)
{
    uvec4 header       = previous ? hitLightCachePrevious.lightCount : hitLightCache.lightCount;
    int   resolution   = max(int(header.z), 1);
    float size         = gi.gridMinVoxelSize.w * gi.volume.x / float(resolution);
    int   surfaceClass = VoxelSurfaceClass(hit.normal);
    int   axis         = surfaceClass % 3;
    ivec3 cell         = ivec3(floor((hit.position - gi.gridMinVoxelSize.xyz) / size));
    uint  mask         = HitClassMask(cell, surfaceClass, resolution, previous);
    if ((mask & ZEN_LIGHT_CACHE_PRESENT) == 0u)
    {
        // A hit on a cell face can belong to the neighbor on the ray's side.
        cell[axis] += hit.normal[axis] > 0.0 ? 1 : -1;
        mask        = HitClassMask(cell, surfaceClass, resolution, previous);
    }
    found       = (mask & ZEN_LIGHT_CACHE_PRESENT) != 0u;
    vec3 center = gi.gridMinVoxelSize.xyz + (vec3(cell) + 0.5) * size;
    vec3 sum    = vec3(0);
    uint lights = mask & ((1u << min(header.x, uint(ZEN_LIGHT_CACHE_LIGHTS))) - 1u);
    while (lights != 0u)
    {
        int i             = findLSB(lights);
        lights           &= lights - 1u;
        SceneLight light  = previous ? hitLightCachePrevious.cacheLights[i] : hitLightCache.cacheLights[i];
        vec3       direction;
        float      distanceToLight;
        vec3       incoming = EvaluateLight(light, hit.position, direction, distanceToLight);
        float      cosine   = max(dot(hit.normal, direction), 0.0);
        if (cosine > 0.0 && any(greaterThan(incoming, vec3(0)))
            && (!LightCacheNear(light, center, size) || HitLightVisible(hit, light)))
        {
            sum += incoming * cosine;
        }
    }
    if (any(greaterThan(sum, vec3(0))))
    {
        sum *= HitReflectance(normals, hit, axis, header.y != 0u) / 3.14159265359;
    }
    return sum;
}
#endif
// With the light-visibility cache (gi.limits.w), the isotropic radiance holds emission, sky and
// uncached lights, and the cached scene lights are added per hit (light_visibility_cache.glsl).
vec3 HitRadiance(sampler3D radiance, sampler3D normals, HybridRayResult hit, vec3 direction, bool previous, out bool rejected)
{
    vec3 value    = vec3(0);
    bool hasFace  = hit.hit && dot(hit.normal, hit.normal) > 0.0;
    bool accepted = hasFace && HitRadianceCell(radiance, normals, hit.cell, direction, value);
    if (hasFace && !accepted)
    {
        // The entered face chooses the adjacent cell on the ray's side. This also
        // resolves triangle hits lying exactly on a voxel boundary.
        vec3 face = abs(hit.normal);
        int  axis = face.x > face.y ? 0 : 1;
        if (face.z > face[axis])
        {
            axis = 2;
        }
        ivec3 adjacent  = hit.cell;
        adjacent[axis] += direction[axis] > 0.0 ? -1 : 1;
        accepted        = HitRadianceCell(radiance, normals, adjacent, direction, value);
    }
    bool found  = false;
    vec3 cached = vec3(0);
#ifdef HIT_LIGHT_CACHE
    if (hasFace && gi.limits.w > 0.5)
    {
        cached = HitCachedLight(normals, hit, previous, found);
    }
#endif
    rejected = !accepted && !found;
    return ((accepted ? value : vec3(0)) + cached) * gi.volume.w;
}
#endif
