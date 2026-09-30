#define MATERIAL_FRAGMENT
#include "../Common/material.glsl"
layout(location=0) in vec3 inNormal;
layout(location=1) in vec2 inUV;
layout(location=2) in vec4 inColor;
layout(location=3) in vec3 inWorldPos;
layout(location=4) in vec2 inUV1;
layout(location=5) in vec4 inTangent;
layout(location=0) out vec4 outPosition;
layout(location=1) out vec4 outNormal;
layout(location=2) out vec4 outAlbedo;
layout(location=3) out vec4 outMetallicRoughness;
layout(location=4) out vec4 outEmissiveOcclusion;
layout(std140,set=1,binding=2) readonly buffer MaterialBuffer { Material materialData[]; };
layout(push_constant) uniform Constants { uint uNodeIndex; uint uMaterialIndex;
};


vec3 SurfaceNormal(Material material)
{
    vec3 N = normalize(inNormal);
    vec2 uv = MaterialUV(material.normalTexSet, inUV, inUV1);
    // Evaluate derivatives before alpha discard, including meshes without authored tangents.
    vec3 dpdx = dFdx(inWorldPos);
    vec3 dpdy = dFdy(inWorldPos);
    vec2 duvdx = dFdx(uv);
    vec2 duvdy = dFdy(uv);
    vec3 T = inTangent.xyz - N * dot(N, inTangent.xyz);
    float handedness = inTangent.w;
    if (dot(T, T) < 1e-12 || abs(handedness) < 0.5)
    {
        float determinantUV = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
        T = vec3(0.0);
        if (abs(determinantUV) > 1e-12)
        {
            T = (dpdx * duvdy.y - dpdy * duvdx.y) / determinantUV;
            vec3 B = (dpdy * duvdx.x - dpdx * duvdy.x) / determinantUV;
            T -= N * dot(N, T);
            handedness = sign(dot(cross(N, T), B));
        }
    }
    if (material.normalTexIndex >= 0 && dot(T, T) > 1e-12 && abs(handedness) > 0.5)
    {
        T = normalize(T);
        vec3 B = cross(N, T) * sign(handedness);
        vec3 mapped = MaterialTexture(material.normalTexIndex, uv).xyz * 2.0 - 1.0;
        mapped.xy *= material.surfaceProperties.z;
        N = normalize(mat3(T, B, N) * mapped);
    }
    return N;
}

void main()
{
    Material material=materialData[uMaterialIndex];
    vec3 normal=SurfaceNormal(material);
    vec4 albedo=MaterialAlbedo(material,inUV,inUV1,inColor);
    if(!MaterialVisible(material,albedo.a)) discard;
    vec4 mr=MaterialTexture(material.mrTexIndex,MaterialUV(material.mrTexSet,inUV,inUV1));
    vec3 emission=material.emissiveFactor.rgb*MaterialTexture(material.emissiveTexIndex,
        MaterialUV(material.emissiveTexSet,inUV,inUV1)).rgb;
    float occlusion=MaterialTexture(material.occlusionTexIndex,
        MaterialUV(material.aoTexSet,inUV,inUV1)).r;
    outPosition=vec4(inWorldPos,1);
    outNormal=vec4(normal,1);
    outAlbedo=vec4(albedo.rgb,1);
    outMetallicRoughness=vec4(material.metallicFactor*mr.b,material.roughnessFactor*mr.g,0,0);
    outEmissiveOcclusion=vec4(emission,occlusion);
}
