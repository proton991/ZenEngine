layout(set = 1, binding = 128) uniform sampler2D secondaryBRDFMap;

vec3 RayHitBounceReflectance(HybridRayResult hit, Material material, float nDotV, out vec3 emission)
{
    vec4 f0Roughness;
    vec3 reflectance = RayHitReflectance(hit, material, emission, f0Roughness);
    if (giCache.grid.z > 0.0 && nDotV > 0.0)
    {
        // With world-cache feedback, approximate incident radiance across the GGX lobe
        // by the local irradiance / pi. The BRDF integral needs no additional ray.
        vec2 brdf     = textureLod(secondaryBRDFMap, vec2(min(nDotV, 1.0), f0Roughness.a), 0.0).rg;
        vec3 specular = clamp(f0Roughness.rgb * brdf.x + brdf.y, vec3(0), vec3(1));
        reflectance   = min(reflectance, 1.0 - specular) + specular;
    }
    return reflectance;
}
