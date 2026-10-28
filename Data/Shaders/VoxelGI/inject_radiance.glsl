#include "../Common/bindless_heap.glsl"
#include "../Common/hdr_storage.glsl"
#include "gi_common.glsl"
#include "Graphics/Shared/VoxelGI.h"
#include "light_visibility_cache.glsl"
// Written by every variant so the graph always knows its contents; only mesh light visibility fills it.
layout(std430, set = 1, binding = 8) writeonly buffer LightVisibilityCache
{
    uvec4      lightCount;
    SceneLight cacheLights[MAX_SCENE_LIGHTS];
    uint       classMasks[];
}
lightCache;
#ifdef VOXEL_MESH_SHADOWS
#    define VOXEL_GEOMETRY_SET 5
#    include "voxel_geometry.glsl"
#    include "../ShadowMapping/scene_shadows.glsl"
#    include "gi_ray_totals.glsl"
#    include "voxel_surface_class.glsl"
layout(set = 1, binding = 6, r32ui) uniform readonly uimage3D voxelOwner;
layout(set = 1, binding = 7, r32ui) uniform readonly uimage3D voxelClassOwner;
// Which of the first `count` scene lights reach the surfaces of one class of class cell p.
uint ClassLightMask(ivec3 p, int surfaceClass, int resolution, int count)
{
    uint mask  = 0u;
    uint owner = imageLoad(voxelClassOwner, VoxelClassTexel(p, surfaceClass, resolution)).r;
    if (owner < triangles.length())
    {
        Vertex a, b, c;
        vec3   pa, pb, pc;
        TriangleVertices(owner, a, b, c, pa, pb, pc);
        // The winding normal, turned to the class's side as voxelization oriented it.
        vec3 normal = normalize(cross(pb - pa, pc - pa));
        if ((normal[surfaceClass % 3] > 0.0) != (surfaceClass < 3))
        {
            normal = -normal;
        }
        // The owner point nearest the cell center stays within the cell's reach, unlike a clamped
        // projection onto a long triangle that only touches the cell.
        float size   = gi.gridMinVoxelSize.w * gi.volume.x / float(resolution);
        vec3  center = gi.gridMinVoxelSize.xyz + (vec3(p) + 0.5) * size;
        vec3  origin = TriangleClosestPoint(center, pa, pb, pc) + normal * (ZEN_LIGHT_CACHE_ORIGIN_OFFSET * size);
        mask         = ZEN_LIGHT_CACHE_PRESENT;
        for (int i = 0; i < count; ++i)
        {
            vec3  direction;
            float distanceToLight;
            vec3  incoming = EvaluateLight(sceneUbo.lights[i], origin, direction, distanceToLight);
            // Hits trace near lights themselves. A light far behind the owner's plane reaches none
            // of the class's surfaces.
            if (LightCacheNear(sceneUbo.lights[i], center, size)
                || (any(greaterThan(incoming, vec3(0))) && dot(normal, direction) > -0.8
                    && SceneLightVisibility(i, origin, normal) > 0.5))
            {
                mask |= 1u << uint(i);
            }
        }
    }
    return mask;
}
#endif
layout(local_size_x    = ZEN_VOXEL_VOLUME_GROUP_SIZE,
       local_size_x_id = ZEN_VOXEL_VOLUME_GROUP_X_ID,
       local_size_y    = ZEN_VOXEL_VOLUME_GROUP_SIZE,
       local_size_y_id = ZEN_VOXEL_VOLUME_GROUP_Y_ID,
       local_size_z    = ZEN_VOXEL_VOLUME_GROUP_SIZE,
       local_size_z_id = ZEN_VOXEL_VOLUME_GROUP_Z_ID) in;
layout(set = 1, binding = 0) uniform sampler3D voxelAlbedo;
layout(set = 1, binding = 1) uniform sampler3D voxelNormal;
layout(set = 1, binding = 2) uniform sampler3D voxelEmissive;
layout(set = 1, binding = 3, rgba16f) uniform writeonly image3D voxelRadiance;
layout(set = 1, binding = 4) uniform sampler3D skyIrradiance;
#ifdef AVERAGED_REFLECTANCE
layout(set = 1, binding = 5) uniform sampler3D voxelReflectance;
#endif
void main()
{
    ivec3 p = ivec3(gl_GlobalInvocationID);
    if (any(greaterThanEqual(p, imageSize(voxelRadiance))))
    {
        return;
    }
    vec4 albedo   = texelFetch(voxelAlbedo, p, 0);
    vec4 radiance = vec4(0);
    // Scene lights below this index are cached per class rather than in the isotropic radiance.
    int cached     = 0;
    int resolution = 0;
#ifdef VOXEL_MESH_SHADOWS
    if (gi.limits.w > 0.5)
    {
        cached     = min(int(sceneUbo.lightInfo.x), ZEN_LIGHT_CACHE_LIGHTS) * int(gi.lighting.x);
        resolution = imageSize(voxelClassOwner).x;
    }
#endif
    if (gi.limits.w > 0.5 && all(equal(p, ivec3(0))))
    {
#ifdef AVERAGED_REFLECTANCE
        lightCache.lightCount = uvec4(cached, 1, resolution, 0);
#else
        lightCache.lightCount = uvec4(cached, 0, resolution, 0);
#endif
        for (int i = 0; i < MAX_SCENE_LIGHTS; ++i)
        {
            lightCache.cacheLights[i] = sceneUbo.lights[i];
        }
    }
#ifdef VOXEL_MESH_SHADOWS
    if (gi.limits.w > 0.5)
    {
        if (all(lessThan(p, ivec3(resolution))))
        {
            for (int surfaceClass = 0; surfaceClass < int(ZEN_VOXEL_SURFACE_CLASSES); ++surfaceClass)
            {
                lightCache.classMasks[LightCacheIndex(p, surfaceClass, resolution)] =
                    ClassLightMask(p, surfaceClass, resolution, cached);
            }
        }
    }
#endif
    if (albedo.a > 0.5)
    {
        vec4 normalDiffuse   = texelFetch(voxelNormal, p, 0);
        vec3 normal          = normalize(normalDiffuse.rgb * 2.0 - 1.0);
        vec3 position        = gi.gridMinVoxelSize.xyz + (vec3(p) + 0.5) * gi.gridMinVoxelSize.w;
        vec3 origin          = TraceOrigin(position, normal);
        vec3 geometricNormal = normal;
#ifdef VOXEL_MESH_SHADOWS
        // Evaluate incident light and its visibility at the same owner surface point.
        // A voxel center may lie inside the wall represented by this cell.
        uint owner        = imageLoad(voxelOwner, p).r;
        bool surfaceValid = false;
        if (gi.lighting.x > 0 && owner < triangles.length())
        {
            Vertex a, b, c;
            vec3   pa, pb, pc;
            TriangleVertices(owner, a, b, c, pa, pb, pc);
            vec3 surfaceNormal = cross(pb - pa, pc - pa);
            if (dot(surfaceNormal, surfaceNormal) > 1e-20)
            {
                geometricNormal = normalize(surfaceNormal);
                vec3 reference  = mat3(nodesData[triangles[owner].y].normalMatrix) * a.normal.xyz;
                if (dot(geometricNormal, reference) < 0)
                {
                    geometricNormal = -geometricNormal;
                }
                vec3 weights = TriangleBarycentrics(pa, pb, pc, position);
                position     = pa * weights.x + pb * weights.y + pc * weights.z;
                surfaceValid = true;
            }
        }
#endif
        vec3 irradiance = texelFetch(skyIrradiance, p, 0).rgb;
        for (int i = cached; i < (int(sceneUbo.lightInfo.x) + 1) * int(gi.lighting.x); ++i)
        {
            vec3       direction;
            float      distanceToLight;
            SceneLight light    = i == int(sceneUbo.lightInfo.x) ? sceneUbo.cameraLight : sceneUbo.lights[i];
            vec3       incoming = EvaluateLight(light, position, direction, distanceToLight);
            float      cosine   = max(dot(normal, direction), 0.0);
            if (cosine > 0 && any(greaterThan(incoming, vec3(0))))
            {
#ifdef VOXEL_MESH_SHADOWS
                float visibility = surfaceValid ? SceneLightVisibility(i == int(sceneUbo.lightInfo.x) ? MAX_SCENE_LIGHTS : i,
                                                                       position, geometricNormal)
                                                : VoxelLightVisibility(voxelAlbedo, light, origin);
#else
                float visibility = VoxelLightVisibility(voxelAlbedo, light, origin);
#endif
                irradiance += incoming * cosine * visibility;
            }
        }
#ifdef AVERAGED_REFLECTANCE
        vec3 diffuseReflectance = texelFetch(voxelReflectance, p, 0).rgb;
#else
        vec3 diffuseReflectance = albedo.rgb * normalDiffuse.a;
#endif
        // Sky-cache kind 3 has no sky estimate (sky_irradiance.glsl): the cell is not a valid source.
        radiance = vec4(texelFetch(voxelEmissive, p, 0).rgb * gi.lighting.z + diffuseReflectance * irradiance / 3.14159265359,
                        texelFetch(skyIrradiance, p, 0).a > 2.5 ? 0.0 : 1.0);
    }
    imageStore(voxelRadiance, p, vec4(ClampHDRStorage(radiance.rgb), radiance.a));
#ifdef GI_COUNT_RAY_SLOT
    RecordGIRays();
#endif
}
