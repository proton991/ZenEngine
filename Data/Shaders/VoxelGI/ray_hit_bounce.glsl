layout(set = 1, binding = 128) uniform sampler2D secondaryBRDFMap;

vec3 RayHitBounceReflectance(HybridRayResult hit,
                             Material        material,
                             vec3            normal,
                             vec3            view,
                             out vec3        emission,
                             out vec4        cacheNormal)
{
    vec4 f0Roughness;
    vec3 reflectance = RayHitReflectance(hit, material, emission, f0Roughness);
    cacheNormal      = vec4(normal, 1.0);
    float nDotV      = dot(normal, view);
    if (giCache.grid.z > 0.0 && nDotV > 0.0)
    {
        // The split-sum integral gives the energy of the secondary GGX lobe.
        vec2 brdf     = textureLod(secondaryBRDFMap, vec2(min(nDotV, 1.0), f0Roughness.a), 0.0).rg;
        vec3 specular = clamp(f0Roughness.rgb * brdf.x + brdf.y, vec3(0), vec3(1));
        reflectance   = min(reflectance, 1.0 - specular) + specular;
        // Tilt the cache lookup toward the mirror direction in proportion to specular energy.
        // Irradiance from a uniform geometric hemisphere scales with 1 + dot(geometric, axis).
        // Dividing out that tilt response preserves the original energy under uniform light.
        vec3 reflection = reflect(-view, normal);
        if (dot(reflection, hit.normal) > 0.0)
        {
            float share     = HybridLuminance(specular) / max(HybridLuminance(reflectance), 1e-8);
            float alpha     = f0Roughness.a * f0Roughness.a;
            cacheNormal.xyz = normalize(mix(normal, reflection, share * (1.0 - alpha)));
            cacheNormal.w   = (1.0 + dot(hit.normal, normal)) / (1.0 + dot(hit.normal, cacheNormal.xyz));
        }
    }
    return reflectance;
}
