#ifndef ZEN_RAY_HIT_LIGHTING
#define ZEN_RAY_HIT_LIGHTING
// Hit shading runs in closest-hit shaders, in ray-query passes, and in passes
// that shade hits recorded by an earlier trace (HYBRID_RECORDED_HIT_SHADING).
#if defined(HYBRID_HARDWARE_PROVIDER) \
    && (!defined(HYBRID_RAY_PIPELINE) || defined(HYBRID_CLOSEST_HIT) || defined(HYBRID_RECORDED_HIT_SHADING))
#    include "environment_sampling.glsl"
#    include "ray_hit_material.glsl"
#    ifdef HYBRID_RADIANCE_CACHE
#        include "gi_radiance_cache.glsl"
float hitCacheSegment = 0.0;
float hitCacheCone    = 0.0;
bool  CachedHitRadiance(HybridRayResult hit, vec3 normal, out vec3 radiance)
{
    int   level    = CacheLevel(hit.position, giCache.camera.xyz);
    float size     = CacheCellSize(level);
    bool  eligible = hitCacheSegment > sqrt(3.0) * size && (hitCacheCone < 0.0 || hitCacheSegment * hitCacheCone > size);
    for (int index = 0; eligible && index <= int(sceneUbo.lightInfo.x) && gi.lighting.x != 0.0; ++index)
    {
        SceneLight light = index == int(sceneUbo.lightInfo.x) ? sceneUbo.cameraLight : sceneUbo.lights[index];
        if (light.directionType.w != 0.0 && light.colorIntensity.w > 0.0
            && length(light.positionRange.xyz - hit.position) < max(4.0 * size, 0.25 * giCache.grid.y))
        {
            eligible = false;
        }
    }
    radiance   = vec3(0);
    bool found = eligible && CacheLookup(hit.position, normal, level, radiance);
#        ifdef HYBRID_COUNT_RAYS
    cacheTraceCounts += uvec2(1u, uint(found));
#        endif
    return found;
}
#    endif
#    ifdef NATIVE_WORLD_CACHE
#        include "world_cache.glsl"
#    endif

// Ray hits use their own UV sets and transforms, never fragment derivatives or voxel-owner UVs.
vec4 RayHitTexture(HybridRayResult hit, int textureIndex, int set, TextureTransform transform, vec2 sampling)
{
    vec3 weights     = vec3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics);
    vec2 uv          = RaySurfaceUV(hit.triangle, weights, set);
    vec2 transformed = vec2(dot(transform.row0.xyz, vec3(uv, 1)), dot(transform.row1.xyz, vec3(uv, 1)));
#    ifdef HYBRID_ABLATE_MATERIAL
    return vec4(1);
#    else
    return MaterialBindingTextureLod0(textureIndex, transform, sampling, transformed);
#    endif
}

#    ifdef HYBRID_RESTIR
// Per-pixel ReSTIR traces shade diffuse-ray hits with the interpolated vertex normal: the
// normal map changed no gated quality metric there and cost 12% of the pass (HybridTraceProfile.md,
// stage 4). Reflection hits and every other ray-hit shader keep it.
bool hitSkipsNormalMap = false;
#    endif
// The interpolated vertex normal on the side of the geometric hit normal.
vec3 RayHitVertexNormal(HybridRayResult hit, vec3 weights, out bool flipped)
{
    Vertex   a    = LoadRayHitVertex(indices[hit.triangle.x]);
    Vertex   b    = LoadRayHitVertex(indices[hit.triangle.x + 1u]);
    Vertex   c    = LoadRayHitVertex(indices[hit.triangle.x + 2u]);
    NodeData node = nodesData[hit.triangle.y];
    vec3 normal   = mat3(node.normalMatrix) * (a.normal.xyz * weights.x + b.normal.xyz * weights.y + c.normal.xyz * weights.z);
    normal        = dot(normal, normal) > 1e-12 ? normalize(normal) : hit.normal;
    flipped       = dot(normal, hit.normal) < 0.0;
    return flipped ? -normal : normal;
}
vec3 RayHitNormal(HybridRayResult hit, Material material, vec3 weights)
{
    bool flipped;
    vec3 normal = RayHitVertexNormal(hit, weights, flipped);
#    if !defined(HYBRID_ABLATE_NORMAL) && !defined(HYBRID_ABLATE_MATERIAL)
    Vertex   a    = LoadRayHitVertex(indices[hit.triangle.x]);
    Vertex   b    = LoadRayHitVertex(indices[hit.triangle.x + 1u]);
    Vertex   c    = LoadRayHitVertex(indices[hit.triangle.x + 2u]);
    NodeData node = nodesData[hit.triangle.y];
#    endif
#    if !defined(HYBRID_ABLATE_NORMAL) && !defined(HYBRID_ABLATE_MATERIAL)
#        ifdef HYBRID_RESTIR
    if (material.normalTexIndex >= 0 && !hitSkipsNormalMap)
#        else
    if (material.normalTexIndex >= 0)
#        endif
    {
        vec4 tangent      = a.tangent * weights.x + b.tangent * weights.y + c.tangent * weights.z;
        vec3 t            = mat3(node.modelMatrix) * tangent.xyz;
        t                -= normal * dot(normal, t);
        float handedness  = tangent.w * sign(determinant(mat3(node.modelMatrix)));
        if (dot(t, t) < 1e-12 || abs(handedness) < 0.5)
        {
            vec2             uva           = RaySurfaceUV(hit.triangle, vec3(1, 0, 0), material.normalTexSet);
            vec2             uvb           = RaySurfaceUV(hit.triangle, vec3(0, 1, 0), material.normalTexSet);
            vec2             uvc           = RaySurfaceUV(hit.triangle, vec3(0, 0, 1), material.normalTexSet);
            TextureTransform transform     = material.textureTransforms[2];
            vec2             du            = vec2(dot(transform.row0.xy, uvb - uva), dot(transform.row1.xy, uvb - uva));
            vec2             dv            = vec2(dot(transform.row0.xy, uvc - uva), dot(transform.row1.xy, uvc - uva));
            float            determinantUV = du.x * dv.y - du.y * dv.x;
            if (abs(determinantUV) > 1e-12)
            {
                vec3 edgeB      = mat3(node.modelMatrix) * (b.position.xyz - a.position.xyz);
                vec3 edgeC      = mat3(node.modelMatrix) * (c.position.xyz - a.position.xyz);
                t               = (edgeB * dv.y - edgeC * du.y) / determinantUV;
                vec3 bitangent  = (edgeC * du.x - edgeB * dv.x) / determinantUV;
                t              -= normal * dot(normal, t);
                handedness      = sign(dot(cross(normal, t), bitangent));
            }
        }
        else if (flipped)
        {
            handedness = -handedness;
        }
        vec3 axis    = abs(normal.z) < 0.999 ? vec3(0, 0, 1) : vec3(0, 1, 0);
        t            = dot(t, t) > 1e-12 ? normalize(t) : normalize(cross(axis, normal));
        t            = flipped ? -t : t;
        mat3 basis   = mat3(t, cross(normal, t) * (handedness < 0.0 ? -1.0 : 1.0), normal);
        vec3 mapped  = MaterialNormalVector(RayHitTexture(hit, material.normalTexIndex, material.normalTexSet,
                                                          material.textureTransforms[2], material.textureSampling.xy)
                                                .xyz,
                                            material.normalEncoding.x, material.surfaceProperties.z);
        vec3 shading = basis * mapped;
        normal       = dot(shading, shading) > 1e-12 ? normalize(shading) : normal;
    }
#    endif
    return normal;
}

// Decode both reflection lobes from the same material texture fetches.
vec3 RayHitReflectance(HybridRayResult hit, Material material, out vec3 emission, out vec4 f0Roughness)
{
    vec4 colorA = LoadRayHitVertex(indices[hit.triangle.x]).color;
    vec4 color  = colorA + (LoadRayHitVertex(indices[hit.triangle.x + 1u]).color - colorA) * hit.barycentrics.x
                + (LoadRayHitVertex(indices[hit.triangle.x + 2u]).color - colorA) * hit.barycentrics.y;
    vec3 reflectance;
    if (material.surfaceProperties.w > 0.5)
    {
        MaterialTextureData diffuse  = material.featureTextures[2];
        MaterialTextureData specular = material.featureTextures[3];
        vec3                albedo = material.diffuseFactor.rgb * color.rgb
                                   * RayHitTexture(hit, int(diffuse.properties.x), int(diffuse.properties.y), diffuse.transform,
                                                   material.textureSampling.xy)
                                         .rgb;
        vec4 specGloss = RayHitTexture(hit, int(specular.properties.x), int(specular.properties.y), specular.transform,
                                       material.textureSampling.xy);
        vec3 f0        = clamp(material.specularGlossiness.rgb * specGloss.rgb, vec3(0), vec3(1));
        f0Roughness    = vec4(f0, clamp(1.0 - material.specularGlossiness.w * specGloss.a, 0.04, 1.0));
        reflectance    = albedo * (1.0 - max(max(f0.r, f0.g), f0.b));
    }
    else
    {
        vec3  albedo = material.baseColorFactor.rgb * color.rgb
                     * RayHitTexture(hit, material.bcTexIndex, material.bcTexSet, material.textureTransforms[0],
                                     material.textureSampling.xy)
                           .rgb;
        vec2  mr     = RayHitTexture(hit, material.mrTexIndex, material.mrTexSet, material.textureTransforms[1],
                                     material.textureSampling.xy)
                           .gb;
        float metal  = clamp(material.metallicFactor * mr.y, 0.0, 1.0);
        f0Roughness =
            vec4(mix(vec3(0.04), clamp(albedo, vec3(0), vec3(1)), metal), clamp(material.roughnessFactor * mr.x, 0.04, 1.0));
        reflectance = albedo * (0.96 * (1.0 - metal));
    }
    emission = vec3(0);
    if (gi.lighting.z != 0.0 && any(notEqual(material.emissiveFactor.rgb, vec3(0))))
    {
        emission = material.emissiveFactor.rgb * gi.lighting.z
                 * RayHitTexture(hit, material.emissiveTexIndex, material.emissiveTexSet, material.textureTransforms[4],
                                 material.textureSampling.xy)
                       .rgb;
    }
    return reflectance;
}
#    ifndef HYBRID_RECORDED_HIT_SHADING
#        include "sampled_hit_diffuse.glsl"
// Evaluate the hit material for cache lookup or endpoint shading.
#        ifdef NATIVE_GLOSSY_CACHE
#            include "moving_light.glsl"
#            include "world_light_response.glsl"
#        endif
vec3 HardwareHitRadiance(HybridRayResult hit, uint seed)
{
    Material material = LoadRayHitMaterial(hit.triangle.z);
    vec3     weights  = vec3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics);
    vec3     emission;
    vec4     f0Roughness;
    vec3     reflectance = RayHitReflectance(hit, material, emission, f0Roughness);
    vec3     normal      = RayHitNormal(hit, material, weights);
#        ifdef HYBRID_COUNT_RAYS
    uint previousPurpose = issuedRayPurpose;
    issuedRayPurpose     = 1u;
#        endif
    vec3 diffuse = vec3(0);
#        ifdef NATIVE_GLOSSY_CACHE
    float response = float(WorldSurfaceChanged(gl_WorldRayOriginEXT, gl_WorldRayDirectionEXT, gl_HitTEXT, hit.triangle.z, 1u));
#        endif
#        if defined(NATIVE_WORLD_CACHE)
#            if defined(NATIVE_GLOSSY_CACHE)
    if ((hitPayload.skipNormalMap & 4u) == 0u)
    {
#            endif
        bool  worldFound, worldUsable;
        float residual;
        uint  sampledSlot;
        diffuse = WorldLookup(hit.position, hit.normal, normal, (hitPayload.skipNormalMap & 16u) != 0u,
                              WorldFeedbackSelection(seed), worldFound, worldUsable, residual, sampledSlot);
#            ifdef NATIVE_GLOSSY_CACHE
        response = max(response, residual);
#            endif
#            if defined(NATIVE_CACHE_MISS_TRANSPORT) || defined(NATIVE_WORLD_CACHE_PIXEL)
        if (!worldUsable)
        {
            diffuse = SampledHitDiffuse(hit, normal, seed, false, false);
        }
#            elif defined(NATIVE_GLOSSY_CACHE)
    // Reflections use the same bounded endpoint as the probe gather.
    if (!worldUsable)
    {
        diffuse = SampledHitDiffuse(hit, normal, seed, false, false, worldCache.lightCount.w != 0u);
    }
    if (worldCache.lightCount.w != 0u)
    {
        vec3 delta;
        diffuse  += MovingLighting(hit.position, hit.normal, normal, 1u, delta)
                  + WorldMovingIndirect(hit.position, hit.normal, sampledSlot, worldFound);
        response  = max(response, min(1.0, dot(abs(delta), vec3(.2126, .7152, .0722))
                                               / max(1e-6, dot(diffuse, vec3(.2126, .7152, .0722)))));
    }
#            endif
        if ((hitPayload.skipNormalMap & 7u) == 3u)
        {
            WorldCountProbe(seed, hit.position, hit.normal, worldFound, worldUsable);
        }
#            if defined(NATIVE_GLOSSY_CACHE)
    }
    else
    {
#            endif
#        endif
#        if defined(NATIVE_GI_CACHE_ONLY)
        // A miss stays dark. Probe traces never fall back to nested shadow or sky rays.
        CacheLookup(hit.position, normal, CacheLevel(hit.position, giCache.camera.xyz), diffuse);
#        elif !defined(NATIVE_WORLD_CACHE) || defined(NATIVE_GLOSSY_CACHE)
#            if defined(HYBRID_RADIANCE_CACHE) && !defined(GI_CACHE_WRITE)
    bool cacheHit = CachedHitRadiance(hit, normal, diffuse);
    if (!cacheHit)
    {
#            endif
#            ifdef NATIVE_GLOSSY_CACHE
        bool movers = worldCache.lightCount.w != 0u;
        diffuse     = SampledHitDiffuse(hit, normal, seed, true, true, movers);
        if (movers)
        {
            vec3 delta;
            diffuse  += MovingLighting(hit.position, hit.normal, normal, 1u, delta);
            response  = max(response, min(1.0, HybridLuminance(abs(delta)) / max(1e-6, HybridLuminance(diffuse))));
        }
#            else
    diffuse = SampledHitDiffuse(hit, normal, seed, true, false);
#            endif
#            if defined(HYBRID_RADIANCE_CACHE) && !defined(GI_CACHE_WRITE)
    }
#            elif defined(GI_CACHE_WRITE)
    CacheAccumulate(hit.position, normal, diffuse);
#            endif
#        endif
#        if defined(NATIVE_GLOSSY_CACHE)
    }
#        endif
#        ifdef HYBRID_COUNT_RAYS
    issuedRayPurpose = previousPurpose;
#        endif
#        ifdef NATIVE_GLOSSY_CACHE
    if ((hitPayload.skipNormalMap & 4u) != 0u && editLog.w != 0u)
    {
        vec3 delta;
        response = max(response, WorldLightResponse(1u, hit.position, normal, diffuse, 0xffffffffu, delta));
        if (!WorldMovingHistoryComplete(1u))
        {
            response = 1.0;
        }
        response = max(response, float(envLog.w == worldCache.schedule.x));
    }
    hitPayload.seed = floatBitsToUint(response);
#        endif
#        if defined(NATIVE_WORLD_CACHE) && !defined(NATIVE_WORLD_CACHE_PIXEL)
    return emission + reflectance * diffuse;
#        else
    return (emission + reflectance * diffuse) * gi.volume.w;
#        endif
}
#    endif
#endif

// Passes that only shade recorded hits trace nothing themselves.
#ifndef HYBRID_RECORDED_HIT_SHADING
vec3 TraceHitRadiance(HybridRayResult hit, vec3 direction, uint seed, out bool rejected)
{
    vec3 value;
#    ifdef HYBRID_HARDWARE_PROVIDER
#        if defined(HYBRID_RAY_PIPELINE) && !defined(HYBRID_CLOSEST_HIT)
    bool materialAvailable = hitPayload.material != 0xffffffffu;
#        else
    bool materialAvailable = hit.triangle.x != 0xffffffffu;
#        endif
    if (hit.hit && materialAvailable)
    {
        rejected = false;
#        if defined(HYBRID_RAY_PIPELINE) && !defined(HYBRID_CLOSEST_HIT)
        value = hitPayload.radiance;
#        else
        value = HardwareHitRadiance(hit, seed);
#        endif
    }
    else
#    endif
    {
#    if defined(HYBRID_FIELD_WORLD_CACHE)
        value = NativeFieldRadiance(hit, seed, rejected) * gi.volume.w;
#    elif !defined(HYBRID_HARDWARE_PROVIDER) || defined(HYBRID_FIELD_TRAVERSAL)
        value = HitRadiance(hitRadiance, hitNormal, hit, direction, false, rejected);
#    else
        // A HW hit without surface metadata is unavailable, never a voxel-cache fallback.
        value    = vec3(0);
        rejected = hit.hit;
#    endif
    }
    return value;
}

void PrepareRayHitLighting(uint seed, bool diffuse, uint feature)
{
#    if defined(HYBRID_RAY_PIPELINE) && !defined(HYBRID_CLOSEST_HIT)
    rayShading = uvec3(seed, 0u, feature);
#        ifdef HYBRID_RESTIR
    rayShading.y = uint(diffuse);
#        endif
#    elif defined(HYBRID_RESTIR)
    hitSkipsNormalMap = diffuse;
#    endif
}
#endif
#endif
