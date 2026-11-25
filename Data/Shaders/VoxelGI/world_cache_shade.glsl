#define GI_CACHE_WRITE
#define WRC_LIGHTING_WRITE
#include "hybrid_guides.glsl"
#include "world_cache.glsl"
#include "world_cache_accumulate.glsl"
#include "world_cache_path.glsl"
layout(set = 1, binding = 7) uniform samplerCube coneEnvironmentMap;
#ifdef HYBRID_FIELD_TRAVERSAL
#    include "native_field.glsl"
#else
#    define HYBRID_RECORDED_HIT_SHADING
#    include "hybrid_provider.glsl"
#    include "ray_hit_lighting.glsl"
#    include "ray_hit_bounce.glsl"
#endif
#include "world_cache_sampling.glsl"
layout(push_constant) uniform WorldBounce
{
    uint bounce;
};
layout(local_size_x = 8, local_size_y = 8) in;

// Importance across the six records depends on incident power. The cosine
// projection for each signed axis is applied only after selecting the light.
vec3 WorldSampleLight(vec3 position, vec3 geometric, uint seed, out vec3 direction)
{
    vec3 radiance = vec3(0);
    direction     = vec3(0);
    float sum = 0.0, selectedWeight = 0.0;
    int   chosen = -1;
    for (int i = 0; gi.lighting.x != 0.0 && i <= int(sceneUbo.lightInfo.x); ++i)
    {
        SceneLight light = i == int(sceneUbo.lightInfo.x) ? sceneUbo.cameraLight : sceneUbo.lights[i];
        vec3       axis;
        float      distance;
        vec3       power  = EvaluateLight(light, position, axis, distance);
        float      w      = dot(geometric, axis) > 0.0 ? HybridLuminance(power) : 0.0;
        sum              += w;
        if (w > 0.0 && float(HybridHash(seed ^ uint(i)) >> 8u) / 16777216.0 * sum < w)
        {
            chosen         = i;
            selectedWeight = w;
            radiance       = power;
            direction      = axis;
        }
    }
    if (chosen >= 0)
    {
        SceneLight light      = chosen == int(sceneUbo.lightInfo.x) ? sceneUbo.cameraLight : sceneUbo.lights[chosen];
        float      visibility = 1.0;
        if (gi.limits.z > .5 && (chosen == int(sceneUbo.lightInfo.x) || light.coneShadow.z > 0.0))
        {
#ifdef HYBRID_FIELD_TRAVERSAL
            visibility = NativeFieldVisibility(position, geometric, light);
#else
            visibility = HardwareLightVisibility(position, geometric, light);
#endif
        }
        radiance *= visibility * sum / (HYBRID_PI * selectedWeight);
        if (visibility > 0.0 && light.directionType.w > .5)
        {
            float distance = length(light.positionRange.xyz - position);
            uint  word     = (floatBitsToUint(distance) & 0xffff0000u)
                           | (packUnorm4x8(vec4(EncodeGBufferNormal(-direction), 0, 0)) & 0xffffu);
            atomicMin(nextLightFootprint[chosen / 4][chosen % 4], word);
        }
    }
    return radiance;
}
void main()
{
    uvec2 tile  = gl_GlobalInvocationID.xy;
    uint  size  = worldCache.schedule.y;
    uvec2 tiles = (uvec2(textureSize(receiverDepth, 0)) + size - 1u) / size;
    if (any(greaterThanEqual(tile, tiles)))
    {
        return;
    }
    uint      index = tile.x + tile.y * tiles.x;
    WorldPath path  = paths[index];
    uint      seed  = HybridHash(index ^ HybridHash(worldCache.schedule.x) ^ HybridHash(bounce));
    // A sky proposal below the receiver contributes zero transport but its direct-light
    // observation still belongs to the cell. Both share the count taken in WorldStart.
    if (bounce == 0u && (path.state.w == kIncludesReceiver || path.state.x != kNoWorldCell))
    {
        vec3 geometric = DecodeGBufferNormal(unpackUnorm2x16(floatBitsToUint(path.origin.w)));
        vec3 axis;
        vec3 power = WorldSampleLight(path.origin.xyz, geometric, seed ^ 0x77254u, axis);
        WorldProject(path.state.x, axis, power);
    }
    if (path.state.w != kDead)
    {
#ifdef HYBRID_FIELD_TRAVERSAL
        vec3            originNormal = DecodeGBufferNormal(unpackUnorm2x16(floatBitsToUint(path.origin.w)));
        HybridRayResult hit          = NativeTraceRay(path.origin.xyz, originNormal, path.ray.xyz, 1e20, true);
#else
        HybridRayResult hit = MakeHybridRayResult(path.hit.x != 0xffffffffu, vec3(0), vec3(0), ivec3(-1), true);
        if (hit.hit)
        {
            hit.triangle     = RayTriangle(path.hit.x, path.hit.y, path.hit.z);
            hit.barycentrics = unpackUnorm2x16(path.hit.w);
            RaySurfaceGeometry(hit.triangle, RayVertexIndices(hit.triangle), hit.barycentrics, hit.position, hit.normal);
            if (dot(hit.normal, path.ray.xyz) > 0.0)
            {
                hit.normal = -hit.normal;
            }
        }
#endif
        vec3 emission = vec3(0), reflectance = vec3(0), normal = hit.normal;
        vec3 incoming = vec3(0);
        uint ordinal  = 0u;
        uint nextSlot = kNoWorldCell;
        bool extend   = hit.hit && hit.valid && path.state.w != kTerminal;
        if (hit.hit && hit.valid)
        {
            vec4 cacheNormal = vec4(normal, 1.0);
#ifdef HYBRID_FIELD_TRAVERSAL
            extend = NativeFieldMaterial(hit, reflectance, emission) && extend;
#else
            Material material = LoadRayHitMaterial(hit.triangle.z);
            bool     flipped;
            normal = RayHitVertexNormal(hit, vec3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics), flipped);
            reflectance = RayHitBounceReflectance(hit, material, normal, -path.ray.xyz, emission, cacheNormal);
#endif
            bool exact, usable;
            vec3 lighting =
                WorldLookup(hit.position, hit.normal, cacheNormal.xyz, true, WorldFeedbackSelection(seed), exact, usable)
                * cacheNormal.w;
            if (extend)
            {
                nextSlot = WorldBeginSample(hit.position, hit.normal, ordinal);
                vec3 axis;
                vec3 power = WorldSampleLight(hit.position, hit.normal, seed, axis);
                WorldProject(nextSlot, axis, power);
                if (!usable)
                {
                    lighting = power * max(dot(normal, axis), 0.0);
                }
            }
            incoming = emission + reflectance * lighting;
        }
        else if (hit.valid)
        {
            incoming = textureLod(coneEnvironmentMap, EnvironmentDirection(path.ray.xyz), 0).rgb
                     * (sceneUbo.environment.x * sceneUbo.environment.z * gi.lighting.y);
        }
        if (path.state.x != kNoWorldCell)
        {
            vec3 weighted = incoming * path.ray.w;
            WorldProject(path.state.x, path.ray.xyz, weighted * (hit.hit ? giCache.grid.z : 1.0));
            if (GI_CACHE_RECORDS == 7u && hit.hit)
            {
                vec3 previousNormal = DecodeGBufferNormal(unpackUnorm2x16(path.state.y));
                WorldAddRecord(path.state.x * GI_CACHE_RECORDS + WORLD_SEED_RECORD,
                               weighted * max(dot(previousNormal, path.ray.xyz), 0.0));
            }
        }
        bool includesReceiver = path.state.w == kIncludesReceiver;
        path.state.w          = kDead;
        if (extend)
        {
            float weight;
            vec2  u          = WorldDirectionSample(nextSlot, ordinal, seed);
            vec3  direction  = WorldSampleDirection(hit.normal, u, false, weight);
            path.origin      = vec4(hit.position, uintBitsToFloat(packUnorm2x16(EncodeGBufferNormal(hit.normal))));
            path.ray         = vec4(direction, weight);
            float throughput = uintBitsToFloat(path.state.z) * clamp(HybridLuminance(reflectance), 0.0, 1.0);
            float p          = sqrt(throughput);
            bool  material = bounce < (includesReceiver ? 1u : 2u) && float(HybridHash(seed ^ 0x781cu) >> 8u) / 16777216.0 < p;
            if (dot(direction, hit.normal) > 0.0)
            {
                path.state =
                    uvec4(nextSlot, packUnorm2x16(EncodeGBufferNormal(normal)), floatBitsToUint(throughput / max(p, 1e-8)),
                          material ? (includesReceiver ? kIncludesReceiver : kMaterial) : kTerminal);
            }
        }
    }
    paths[index] = path;
}
