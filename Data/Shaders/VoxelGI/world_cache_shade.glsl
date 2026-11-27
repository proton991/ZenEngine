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
#    include "moving_light.glsl"
#endif
#include "world_cache_sampling.glsl"
layout(push_constant) uniform WorldBounce
{
    uint bounce;
};
layout(local_size_x = 8, local_size_y = 8) in;

// Importance across the six records depends on incident power. The cosine
// projection for each signed axis is applied only after selecting the light.
vec3 WorldSampleLight(vec3 position, vec3 geometric, uint seed, uint slot, out vec3 direction)
{
    vec3 radiance = vec3(0);
    direction     = vec3(0);
    float sum = 0.0, selectedWeight = 0.0;
    int   chosen = -1;
    for (int i = 0; gi.lighting.x != 0.0 && i <= int(sceneUbo.lightInfo.x); ++i)
    {
        SceneLight light = i == int(sceneUbo.lightInfo.x) ? sceneUbo.cameraLight : sceneUbo.lights[i];
        if (worldCache.lightCount.w != 0u && light.coneShadow.w != 0.0)
        {
            continue;
        }
        vec3  axis;
        float distance;
        vec3  power  = EvaluateLight(light, position, axis, distance);
        float w      = dot(geometric, axis) > 0.0 ? HybridLuminance(power) : 0.0;
        sum         += w;
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
        if (slot != kNoWorldCell
            && WorldSurfaceChanged(position, direction,
                                   light.directionType.w > .5 ? distance(light.positionRange.xyz, position) : 1e20, 0xffffffffu,
                                   WorldSampleInterval(slot)))
        {
            WorldRecordResponse(slot, 1.0);
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
vec3 WorldSkyRadiance(vec3 direction)
{
    return textureLod(coneEnvironmentMap, EnvironmentDirection(direction), 0).rgb
         * (sceneUbo.environment.x * sceneUbo.environment.z * gi.lighting.y);
}
#ifndef HYBRID_FIELD_TRAVERSAL
vec3 WorldHitReflectance(HybridRayResult hit, vec3 view, out vec3 normal, out vec3 emission, out vec4 cacheNormal)
{
    bool flipped;
    normal = RayHitVertexNormal(hit, vec3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics), flipped);
    return RayHitBounceReflectance(hit, LoadRayHitMaterial(hit.triangle.z), normal, view, emission, cacheNormal);
}
// One continuation supplies the missing reflected and escaped radiance at an uncached vertex.
vec3 WorldMissIndirect(HybridRayResult surface, vec4 cacheNormal, uint seed)
{
    float weight;
    vec3  direction = WorldSampleDirection(surface.normal, WorldDirectionSample(kNoWorldCell, 0u, seed), true, weight);
    float cosine    = max(dot(cacheNormal.xyz, direction), 0.0);
    vec3  radiance  = vec3(0);
    if (cosine > 0.0 && dot(surface.normal, direction) > 0.0)
    {
        HybridRayResult hit = HardwareTraceRay(HybridTraceOrigin(surface.position, surface.normal, 0.0), direction, 1e20, true);
        if (!hit.hit)
        {
            radiance = WorldSkyRadiance(direction);
        }
        else
        {
            vec3 normal, emission;
            vec4 nextNormal;
            vec3 reflectance = WorldHitReflectance(hit, -direction, normal, emission, nextNormal);
            bool exact, usable;
            vec3 irradiance = WorldLookup(hit.position, hit.normal, nextNormal.xyz, exact, usable) * nextNormal.w;
            if (!usable)
            {
                vec3 axis;
                vec3 power = WorldSampleLight(hit.position, hit.normal, HybridHash(seed ^ 0xe1172cd5u), kNoWorldCell, axis);
                irradiance = power * max(dot(nextNormal.xyz, axis), 0.0) * nextNormal.w;
            }
            radiance = emission + reflectance * irradiance;
        }
    }
    return radiance * (cosine * cacheNormal.w * weight);
}
// Mature directional records can use the otherwise idle parent snapshot word until resolve.
void WorldProjectCorrection(uint slot, vec3 oldAxis, vec3 oldPower, vec3 newAxis, vec3 newPower)
{
    if (slot == kNoWorldCell || cacheResolved[slot * GI_CACHE_RECORDS].z == 0u)
    {
        return;
    }
    for (uint face = 0u; face < 6u; ++face)
    {
        float sign  = (face & 1u) == 0u ? 1.0 : -1.0;
        vec3  delta = WorldBoundRecord(newPower * max(0.0, newAxis[face / 2u] * sign))
                    - WorldBoundRecord(oldPower * max(0.0, oldAxis[face / 2u] * sign));
        if (any(notEqual(delta, vec3(0))))
        {
            uint address = slot * GI_CACHE_RECORDS + face;
            uint word    = atomicAdd(cacheAccumulation[address].w, 0u);
            uint observed;
            do
            {
                observed     = word;
                uint updated = HybridPackSignedRGBE(HybridUnpackSignedRGBE(observed) + delta);
                word         = atomicCompSwap(cacheAccumulation[address].w, observed, updated);
            } while (word != observed);
        }
    }
}
#    define WORLD_PROJECT_CORRECTION
#    include "world_light_response.glsl"
void WorldMeasureSky(uint slot, vec3 direction, vec3 geometric, vec3 current, float weight)
{
    if (slot == kNoWorldCell || editLog.w == 0u || envLog.y == 0u)
    {
        return;
    }
    uint elapsed = WorldSampleInterval(slot);
    if (worldCache.schedule.x - envLog.w < elapsed)
    {
        uint past = (envLog.x + 7u) % 8u;
        for (uint i = 1u; i < envLog.y; ++i)
        {
            uint next = (past + 7u) % 8u;
            if (worldCache.schedule.x - envFrames[next / 4u][next % 4u] >= elapsed)
            {
                break;
            }
            past = next;
        }
        vec4  env      = envBefore[past];
        vec3  previous = textureLod(coneEnvironmentMap, EnvironmentDirection(direction, env.y, orientationBefore[past]), 0).rgb
                       * (env.x * gi.lighting.y);
        bool  found;
        float residual;
        vec3  cached     = WorldEvaluate(slot, geometric, geometric, found, residual);
        float projection = weight * max(dot(geometric, direction), 0.0);
        float reference  = max(HybridLuminance(cached), max(HybridLuminance(previous), HybridLuminance(current)) * projection);
        float change     = HybridLuminance(abs(previous - current)) * projection / max(reference, 1e-6);
        if (envLog.z != 0u && worldCache.schedule.x - envLog.z < elapsed)
        {
            change = 1.0;
        }
        else
        {
            WorldProjectCorrection(slot, direction, previous * weight, direction, current * weight);
        }
        WorldRecordResponse(slot, change);
    }
}
void WorldMeasureLights(uint slot, vec3 position, vec3 geometric)
{
    if (slot != kNoWorldCell && responseLog.x != 0u && editLog.w != 0u
        && worldCache.schedule.x - responseLog.x < WorldSampleInterval(slot))
    {
        bool  found;
        float residual;
        vec3  previous = WorldEvaluate(slot, geometric, geometric, found, residual);
        vec3  delta;
        float change = WorldLightResponse(WorldSampleInterval(slot), position, geometric, previous, slot, delta);
        if (change > 0.0)
        {
            WorldRecordResponse(slot, change);
        }
    }
}
#endif
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
        vec3 power = WorldSampleLight(path.origin.xyz, geometric, seed ^ 0x77254u, path.state.x, axis);
        WorldProject(path.state.x, axis, power);
#ifndef HYBRID_FIELD_TRAVERSAL
        WorldMeasureLights(path.state.x, path.origin.xyz, geometric);
#endif
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
        vec3  emission = vec3(0), reflectance = vec3(0), normal = hit.normal;
        vec3  incoming         = vec3(0);
        float feedbackResponse = 0.0;
        // After a settled source changes, current radiance cannot bound the energy that just disappeared.
        bool sourceTransition = (responseLog.x != 0u && worldCache.schedule.x - responseLog.x < giCache.limits.y)
                             || (envLog.w != 0u && worldCache.schedule.x - envLog.w < giCache.limits.y);
        uint ordinal          = 0u;
        uint nextSlot         = kNoWorldCell;
        bool extend           = hit.hit && hit.valid && path.state.w != kTerminal;
        if (hit.hit && hit.valid)
        {
            vec4 cacheNormal = vec4(normal, 1.0);
#ifdef HYBRID_FIELD_TRAVERSAL
            extend = NativeFieldMaterial(hit, reflectance, emission) && extend;
#else
            reflectance = WorldHitReflectance(hit, -path.ray.xyz, normal, emission, cacheNormal);
#endif
            bool  exact, usable;
            float residual;
            uint  sampledSlot;
            vec3  lighting = WorldLookup(hit.position, hit.normal, cacheNormal.xyz, true, WorldFeedbackSelection(seed), exact,
                                         usable, residual, sampledSlot)
                           * cacheNormal.w;
            if (extend)
            {
                nextSlot = WorldBeginSample(hit.position, hit.normal, ordinal);
            }
            // A terminal vertex can still reflect direct light when neither cache level has a usable record.
            bool localMiss = !usable && editLog.w != 0u && giCache.grid.z > 0.0;
            if (extend || localMiss)
            {
                vec3 axis;
                vec3 power = WorldSampleLight(hit.position, hit.normal, seed, nextSlot, axis);
                WorldProject(nextSlot, axis, power);
#ifndef HYBRID_FIELD_TRAVERSAL
                WorldMeasureLights(nextSlot, hit.position, hit.normal);
#endif
                if (!usable)
                {
                    lighting = power * max(dot(normal, axis), 0.0);
#ifndef HYBRID_FIELD_TRAVERSAL
                    if (localMiss)
                    {
                        lighting = power * max(dot(cacheNormal.xyz, axis), 0.0) * cacheNormal.w
                                 + WorldMissIndirect(hit, cacheNormal, HybridHash(seed ^ 0x7ad3901bu));
                    }
#endif
                }
            }
#ifndef HYBRID_FIELD_TRAVERSAL
            if (worldCache.lightCount.w != 0u && path.state.x != kNoWorldCell && giCache.grid.z != 0.0)
            {
                vec3 moving         = MovingDirect(hit.position, hit.normal, normal)
                                    + WorldMovingIndirect(hit.position, hit.normal, sampledSlot, exact);
                vec3 receiverNormal = DecodeGBufferNormal(unpackUnorm2x16(path.state.y));
                WorldAddRecord(path.state.x * GI_CACHE_RECORDS + WORLD_MOVING_RECORD,
                               reflectance * moving * path.ray.w * max(0.0, dot(receiverNormal, path.ray.xyz)));
            }
#endif
            incoming = emission + reflectance * lighting;
            if (editLog.w != 0u && giCache.grid.z != 0.0 && path.state.x != kNoWorldCell && usable && residual > .005
                && WorldSampleInterval(path.state.x) != 0u
                && worldCache.schedule.x - worldCells[sampledSlot].lastSample <= WorldSampleInterval(path.state.x))
            {
                // Only reflected illumination carried the upstream edit. An emitter's own
                // radiance and the part absorbed by this surface cannot propagate its uncertainty.
                float reflected  = HybridLuminance(reflectance * lighting);
                float incident   = max(HybridLuminance(lighting), HybridLuminance(incoming));
                feedbackResponse = sourceTransition ? residual : residual * clamp(reflected / max(incident, 1e-6), 0.0, 1.0);
            }
        }
        else if (hit.valid)
        {
            incoming = WorldSkyRadiance(path.ray.xyz);
#ifndef HYBRID_FIELD_TRAVERSAL
            WorldMeasureSky(path.state.x, path.ray.xyz, DecodeGBufferNormal(unpackUnorm2x16(floatBitsToUint(path.origin.w))),
                            incoming, path.ray.w);
#endif
        }
        if (path.state.x != kNoWorldCell)
        {
            vec3 weighted = incoming * path.ray.w;
#ifndef HYBRID_FIELD_TRAVERSAL
            bool surfaceChange =
                WorldSurfaceChanged(path.origin.xyz, path.ray.xyz, hit.hit ? distance(hit.position, path.origin.xyz) : 1e20,
                                    hit.hit ? hit.triangle.z : 0xffffffffu, WorldSampleInterval(path.state.x));
            if (surfaceChange || feedbackResponse > 0.0)
            {
                // A changed sample represents only its share of the cell integral. An atomic maximum
                // lets a single weak secondary path shorten every observation in a busy cell.
                vec3  geometric = DecodeGBufferNormal(unpackUnorm2x16(floatBitsToUint(path.origin.w)));
                bool  found;
                float residual;
                vec3  previous       = WorldEvaluate(path.state.x, geometric, geometric, found, residual);
                float contribution   = HybridLuminance(weighted) * max(dot(geometric, path.ray.xyz), 0.0);
                float share          = contribution / max(HybridLuminance(previous), 1e-5);
                float reflectedShare = sourceTransition ? share : min(share, 1.0);
                WorldRecordResponse(path.state.x, surfaceChange ? max(.25, share) : feedbackResponse * reflectedShare);
            }
#endif
            WorldProject(path.state.x, path.ray.xyz, weighted * (hit.hit ? giCache.grid.z : 1.0));
            if (WORLD_HAS_SEED && hit.hit)
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
