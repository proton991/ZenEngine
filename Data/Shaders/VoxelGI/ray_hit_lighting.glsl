#ifndef ZEN_RAY_HIT_LIGHTING
#define ZEN_RAY_HIT_LIGHTING
#ifdef HYBRID_HARDWARE_PROVIDER
#    include "environment_sampling.glsl"

// Ray hits use their own UV sets and transforms, never fragment derivatives or voxel-owner UVs.
vec4 RayHitTexture(HybridRayResult hit, int textureIndex, int set, TextureTransform transform, vec2 sampling)
{
    vec3 weights     = vec3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics);
    vec2 uv          = RaySurfaceUV(hit.triangle, weights, set);
    vec2 transformed = vec2(dot(transform.row0.xyz, vec3(uv, 1)), dot(transform.row1.xyz, vec3(uv, 1)));
    return MaterialBindingTextureLod0(textureIndex, transform, sampling, transformed);
}

vec3 RayHitNormal(HybridRayResult hit, Material material, vec3 weights)
{
    Vertex   a    = vertices[indices[hit.triangle.x]];
    Vertex   b    = vertices[indices[hit.triangle.x + 1u]];
    Vertex   c    = vertices[indices[hit.triangle.x + 2u]];
    NodeData node = nodesData[hit.triangle.y];
    vec3 normal   = mat3(node.normalMatrix) * (a.normal.xyz * weights.x + b.normal.xyz * weights.y + c.normal.xyz * weights.z);
    normal        = dot(normal, normal) > 1e-12 ? normalize(normal) : hit.normal;
    bool flipped  = dot(normal, hit.normal) < 0.0;
    normal        = flipped ? -normal : normal;
    if (material.normalTexIndex >= 0)
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
    return normal;
}

// One diffuse secondary surface, lit by exact RT visibility. This is shared by diffuse bounce
// and reflected hit radiance; it does not recursively read the radiance of another cached cell.
vec3 HardwareHitRadiance(HybridRayResult hit, uint seed)
{
    Material material = materialData[hit.triangle.z];
    vec3     weights  = vec3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics);
    vec4     color    = vec4(0);
    for (uint corner = 0u; corner < 3u; ++corner)
    {
        color += vertices[indices[hit.triangle.x + corner]].color * weights[corner];
    }
    vec3 reflectance;
    if (material.surfaceProperties.w > 0.5)
    {
        MaterialTextureData diffuse  = material.featureTextures[2];
        MaterialTextureData specular = material.featureTextures[3];
        vec3                albedo = material.diffuseFactor.rgb * color.rgb
                                   * RayHitTexture(hit, int(diffuse.properties.x), int(diffuse.properties.y), diffuse.transform,
                                                   material.textureSampling.xy)
                                         .rgb;
        vec3                f0 = material.specularGlossiness.rgb
                               * RayHitTexture(hit, int(specular.properties.x), int(specular.properties.y), specular.transform,
                                               material.textureSampling.xy)
                                     .rgb;
        reflectance            = albedo * (1.0 - clamp(max(max(f0.r, f0.g), f0.b), 0.0, 1.0));
    }
    else
    {
        vec3  albedo = material.baseColorFactor.rgb * color.rgb
                     * RayHitTexture(hit, material.bcTexIndex, material.bcTexSet, material.textureTransforms[0],
                                     material.textureSampling.xy)
                           .rgb;
        float metal  = clamp(material.metallicFactor
                                 * RayHitTexture(hit, material.mrTexIndex, material.mrTexSet, material.textureTransforms[1],
                                                 material.textureSampling.xy)
                                       .b,
                             0.0, 1.0);
        reflectance  = albedo * (0.96 * (1.0 - metal));
    }
    vec3 normal  = RayHitNormal(hit, material, weights);
    vec3 diffuse = vec3(0);
    for (int i = 0; i < (int(sceneUbo.lightInfo.x) + 1) * int(gi.lighting.x); ++i)
    {
        SceneLight light = i == int(sceneUbo.lightInfo.x) ? sceneUbo.cameraLight : sceneUbo.lights[i];
        vec3       direction;
        float      distanceToLight;
        vec3       incoming = EvaluateLight(light, hit.position, direction, distanceToLight);
        float      cosine   = max(dot(normal, direction), 0.0);
        if (cosine > 0.0 && dot(hit.normal, direction) > 0.0 && any(greaterThan(incoming, vec3(0))))
        {
            float visibility  = gi.limits.z > 0.5 && (i == int(sceneUbo.lightInfo.x) || light.coneShadow.z > 0.0)
                                  ? HardwareLightVisibility(hit.position, hit.normal, light)
                                  : 1.0;
            diffuse          += incoming * cosine * visibility / HYBRID_PI;
        }
    }
    float skyScale = sceneUbo.environment.x * sceneUbo.environment.z * gi.lighting.y;
    if (skyScale > 0.0)
    {
        // Independent counter dimensions avoid correlating the secondary light direction with
        // the primary bounce direction. The seed changes for each ray and successful frame.
        vec3 u =
            vec3(uvec3(HybridHash(seed ^ 0xa511e9b3u), HybridHash(seed ^ 0x63d83595u), HybridHash(seed ^ 0x9e3779b9u)) >> 8)
            / 16777216.0;
        float pdf;
        vec3  direction = HybridSkySample(normal, u.xy, u.z < HYBRID_ENVIRONMENT_SHARE, pdf);
        float cosine    = max(dot(normal, direction), 0.0);
        if (cosine > 0.0 && dot(hit.normal, direction) > 0.0 && pdf > 0.0)
        {
            vec3 origin = HybridTraceOrigin(hit.position, hit.normal, 0.0);
            if (!HardwareTraceRay(origin, direction, 1e20, false).hit)
            {
                diffuse += textureLod(coneEnvironmentMap, EnvironmentDirection(direction), 0).rgb
                         * (skyScale * cosine / (HYBRID_PI * pdf));
            }
        }
    }
    vec3 emission = material.emissiveFactor.rgb * gi.lighting.z
                  * RayHitTexture(hit, material.emissiveTexIndex, material.emissiveTexSet, material.textureTransforms[4],
                                  material.textureSampling.xy)
                        .rgb;
    return (emission + reflectance * diffuse) * gi.volume.w;
}
#endif

vec3 TraceHitRadiance(HybridRayResult hit, vec3 direction, uint seed, out bool rejected)
{
    vec3 value;
#ifdef HYBRID_HARDWARE_PROVIDER
    if (hit.hit && hit.triangle.x != 0xffffffffu)
    {
        rejected = false;
        value    = HardwareHitRadiance(hit, seed);
    }
    else
#endif
    {
#if !defined(HYBRID_HARDWARE_PROVIDER) || defined(HYBRID_FIELD_TRAVERSAL)
        value = HitRadiance(hitRadiance, hitNormal, hit, direction, false, rejected);
#else
        // A HW hit without surface metadata is unavailable, never a voxel-cache fallback.
        value    = vec3(0);
        rejected = hit.hit;
#endif
    }
    return value;
}
#endif
