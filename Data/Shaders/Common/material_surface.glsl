#ifndef ZEN_MATERIAL_SURFACE_GLSL
#define ZEN_MATERIAL_SURFACE_GLSL
layout(location=0) in vec3 inNormal;
layout(location=1) in vec4 inColor;
layout(location=2) in vec3 inWorldPos;
layout(location=3) in vec4 inTangent;
layout(location=4) flat in float inOrientation;
layout(location=5) in vec4 inPackedBindingUV[11];
#include "material_binding_uv.glsl"
#define inUV MaterialBindingUV(0)
#define inUV1 MaterialBindingUV(1)
#define MATERIAL_FRAGMENT
#include "material.glsl"

vec3 SafeNormalize(vec3 value, vec3 fallback)
{
    return dot(value, value) > 1e-12 ? normalize(value) : fallback;
}

vec3 GeometricNormal(Material material)
{
    vec3 normal = SafeNormalize(inNormal, SafeNormalize(cross(dFdx(inWorldPos), dFdy(inWorldPos)), vec3(0, 0, 1)));
    if (material.materialProperties.z > 0.5 && !(gl_FrontFacing == (inOrientation >= 0.0))) normal = -normal;
    return normal;
}

mat3 SurfaceBasis(vec3 normal, vec2 uv, bool useAuthoredTangent, bool flipTangent)
{
    vec3 T = useAuthoredTangent ? inTangent.xyz - normal * dot(normal, inTangent.xyz) : vec3(0.0);
    float handedness = inTangent.w;
    bool authored = dot(T, T) >= 1e-12 && abs(handedness) >= 0.5;
    if (authored && flipTangent) handedness = -handedness;
    vec3 dpdx = dFdx(inWorldPos);
    vec3 dpdy = dFdy(inWorldPos);
    vec2 duvdx = dFdx(uv);
    vec2 duvdy = dFdy(uv);
    if (dot(T, T) < 1e-12 || abs(handedness) < 0.5)
    {
        float determinantUV = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
        if (abs(determinantUV) > 1e-12)
        {
            T = (dpdx * duvdy.y - dpdy * duvdx.y) / determinantUV;
            vec3 B = (dpdy * duvdx.x - dpdx * duvdy.x) / determinantUV;
            T -= normal * dot(normal, T);
            handedness = sign(dot(cross(normal, T), B));
        }
    }
    vec3 axis = abs(normal.z) < 0.999 ? vec3(0, 0, 1) : vec3(0, 1, 0);
    T = SafeNormalize(T, normalize(cross(axis, normal)));
    if (flipTangent) T = -T;
    return mat3(T, cross(normal, T) * (handedness < 0.0 ? -1.0 : 1.0), normal);
}

vec3 SurfaceNormal(Material material)
{
    vec3 N = GeometricNormal(material);
    vec2 uv = MaterialTransformedUV(material, 2, material.normalTexSet, inUV, inUV1);
    bool backface = material.materialProperties.z > 0.5 && !(gl_FrontFacing == (inOrientation >= 0.0));
    // Explicit TANGENT is the authored tangent space. Texture coordinates only
    // derive the frame when that attribute is absent.
    mat3 basis = SurfaceBasis(N, uv, true, backface);
    if (material.normalTexIndex >= 0)
    {
        vec3 mapped = MaterialSlotTexture(material, 2, material.normalTexIndex, uv).xyz * 2.0 - 1.0;
        mapped.xy *= material.surfaceProperties.z;
        N = SafeNormalize(basis * mapped, N);
    }
    return N;
}

vec3 ClearcoatNormal(Material material)
{
    vec3 N = GeometricNormal(material);
    MaterialTextureData binding = material.featureTextures[6];
    // Clearcoat samples its own UVs but shares the base normal map's tangent frame.
    vec2 uv = MaterialTransformedUV(material, 2, material.normalTexSet, inUV, inUV1);
    bool backface = material.materialProperties.z > 0.5 && !(gl_FrontFacing == (inOrientation >= 0.0));
    mat3 basis = SurfaceBasis(N, uv, true, backface);
    if (binding.properties.x >= 0.0)
    {
        vec3 mapped = MaterialFeatureTexture(material, 6, inUV, inUV1).xyz * 2.0 - 1.0;
        mapped.xy *= binding.properties.z;
        N = SafeNormalize(basis * mapped, N);
    }
    return N;
}
#endif
