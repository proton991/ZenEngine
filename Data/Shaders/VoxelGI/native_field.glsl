#ifndef ZEN_NATIVE_FIELD
#define ZEN_NATIVE_FIELD
#define HYBRID_FIELD_TRAVERSAL
#ifndef WRC_LIGHTING_WRITE
#    define WRC_LIGHTING_WRITE
#endif
#include "hybrid_provider.glsl"
#include "world_cache.glsl"
#include "scene_ray_geometry.glsl"
struct NativeDynamicRecord
{
    vec4  minimum;
    vec4  maximum;
    uvec4 triangles;
};
layout(std430, set = 1, binding = 111) readonly buffer NativeDynamicGeometry
{
    NativeDynamicRecord nativeDynamic[];
};
layout(std430, set = 1, binding = 112) readonly buffer TriangleRecords
{
    uvec4 nativeTriangles[];
};
layout(set = 1, binding = 107) uniform sampler3D nativeReflectance;
layout(set = 1, binding = 108) uniform sampler3D nativeNormal;
layout(set = 1, binding = 109) uniform sampler3D nativeEmission;
layout(std140, set = 1, binding = 110) uniform uNativeFieldData
{
    uvec4 material;
}
nativeField;
HybridRayResult NativeTraceRay(vec3 position, vec3 normal, vec3 direction, float maximumDistance, bool normals)
{
    vec3            origin           = HybridTraceOrigin(position, normal, 0.0);
    vec3            inverseDirection = 1.0 / mix(direction, vec3(1), equal(direction, vec3(0)));
    float           closest          = maximumDistance;
    HybridRayResult dynamicHit       = MakeHybridRayResult(false, origin, vec3(0), ivec3(-1), true);
    for (uint instance = 0u; instance < nativeField.material.y; ++instance)
    {
        NativeDynamicRecord record = nativeDynamic[instance];
        float               enter, leave;
        if (DistanceFieldBoxSpan(origin, direction, inverseDirection, record.minimum.xyz, record.maximum.xyz, 0.0, closest,
                                 enter, leave))
        {
            for (uint item = 0u; item < record.triangles.y; ++item)
            {
                uvec4 triangle = nativeTriangles[record.triangles.x + item];
                uvec3 vertex   = RayVertexIndices(triangle);
                mat4  model    = nodesData[triangle.y].modelMatrix;
                vec3  a        = (model * vec4(vertices[vertex.x].position.xyz, 1)).xyz;
                vec3  edge1    = mat3(model) * (vertices[vertex.y].position.xyz - vertices[vertex.x].position.xyz);
                vec3  edge2    = mat3(model) * (vertices[vertex.z].position.xyz - vertices[vertex.x].position.xyz);
                vec3  p = cross(direction, edge2), offset = origin - a;
                float determinant = dot(edge1, p);
                if (abs(determinant) > 1e-8 * length(edge1) * length(edge2))
                {
                    vec3  q        = cross(offset, edge1);
                    vec2  uv       = vec2(dot(offset, p), dot(direction, q)) / determinant;
                    float distance = dot(edge2, q) / determinant;
                    if (all(greaterThanEqual(uv, vec2(0))) && uv.x + uv.y <= 1.0 && distance > 0.0 && distance < closest
                        && RayCandidateAccepted(triangle, uv))
                    {
                        closest           = distance;
                        vec3 hitNormal    = normalize(cross(edge1, edge2));
                        hitNormal        *= dot(hitNormal, direction) > 0.0 ? -1.0 : 1.0;
                        vec3 hitPosition  = a + edge1 * uv.x + edge2 * uv.y;
                        dynamicHit = MakeHybridRayResult(true, hitPosition, hitNormal, HybridCacheCell(hitPosition), true);
                        dynamicHit.triangle     = triangle;
                        dynamicHit.barycentrics = uv;
                    }
                }
            }
        }
    }
    vec3            fieldOrigin  = FieldTraceOrigin(position, normal);
    float           fieldMaximum = max(closest - dot(fieldOrigin - origin, direction), 0.0);
    HybridRayResult hit          = FieldTraceRay(fieldOrigin, direction, fieldMaximum, normals);
    if (hit.valid && !hit.hit && dynamicHit.hit)
    {
        hit = dynamicHit;
    }
    return hit;
}
float NativeFieldVisibility(vec3 position, vec3 normal, SceneLight light)
{
    vec3  direction;
    float distance;
    EvaluateLight(light, position, direction, distance);
    vec3            origin  = HybridTraceOrigin(position, normal, 0.0);
    float           maximum = light.directionType.w == 0.0 ? 1e20 : max(length(light.positionRange.xyz - origin) - 1e-6, 0.0);
    HybridRayResult hit     = NativeTraceRay(position, normal, direction, maximum, false);
    return hit.valid && !hit.hit ? 1.0 : 0.0;
}

#if defined(NATIVE_CACHE_MISS_TRANSPORT) || defined(NATIVE_FIELD_ENDPOINT)
#    include "sampled_hit_diffuse.glsl"
#endif
bool NativeFieldMaterial(HybridRayResult hit, out vec3 diffuse, out vec3 emission)
{
    diffuse     = vec3(0);
    emission    = vec3(0);
    bool  found = false;
    ivec3 size  = textureSize(nativeReflectance, 0);
    uint  axis  = CacheNormalBin(hit.normal) / 2u;
    ivec3 cell  = hit.cell;
    for (uint attempt = 0u; attempt < 3u && !found; ++attempt)
    {
        if (all(greaterThanEqual(cell, ivec3(0))) && all(lessThan(cell, size)))
        {
            vec4 reflectance = texelFetch(nativeReflectance, cell, 0);
            vec4 normal      = texelFetch(nativeNormal, cell, 0);
            if (reflectance.a > .5 && dot(normal.xyz * 2.0 - 1.0, hit.normal) > 0.0)
            {
                diffuse  = reflectance.rgb * (nativeField.material.x != 0u ? 1.0 : normal.a);
                emission = texelFetch(nativeEmission, cell, 0).rgb * gi.lighting.z;
                found    = true;
            }
        }
        // The distance-field hit band can lie just outside the occupied voxel.
        cell        = hit.cell;
        cell[axis] += (attempt == 0u ? -1 : 1) * (hit.normal[axis] > 0.0 ? 1 : -1);
    }
    return found;
}
vec3 NativeFieldRadiance(HybridRayResult hit, uint seed, out bool rejected)
{
    vec3 reflectance, emission, value = vec3(0);
    bool hasMaterial = hit.hit && hit.valid && NativeFieldMaterial(hit, reflectance, emission);
    rejected         = hit.hit && !hasMaterial;
    if (hasMaterial)
    {
        bool exact, usable;
        vec3 incoming = WorldLookup(hit.position, hit.normal, hit.normal, exact, usable);
#if defined(NATIVE_CACHE_MISS_TRANSPORT) || defined(NATIVE_FIELD_ENDPOINT)
        if (!usable)
        {
            incoming = SampledHitDiffuse(hit, hit.normal, seed, false, false);
        }
#    ifdef NATIVE_CACHE_MISS_TRANSPORT
        WorldCountProbe(seed, hit.position, hit.normal, exact, usable);
#    endif
#endif
        value = reflectance * incoming + emission;
    }
    return value;
}
vec3 NativeFieldRadiance(HybridRayResult hit, uint seed)
{
    bool rejected;
    return NativeFieldRadiance(hit, seed, rejected);
}
#endif
