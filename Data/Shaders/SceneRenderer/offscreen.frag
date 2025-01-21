#version 450
#extension GL_ARB_separate_shader_objects : enable

layout (set = 1, binding = 0) uniform sampler2D uTextureArray[1024];

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec2 inUV;
layout (location = 2) in vec3 inColor;
layout (location = 3) in vec3 inWorldPos;

layout (location = 0) out vec4 outPosition;
layout (location = 1) out vec4 outNormal;
layout (location = 2) out vec4 outAlbedo;
layout (location = 3) out vec4 outMetallicRoughness;
layout (location = 4) out vec4 outEmissiveOcclusion;

struct Material
{
    int bcTexIndex;
    int mrTexIndex;
    int normalTexIndex;
    int occlusionTexIndex;
    int emissiveTexIndex;
    int bcTexSet;
    int mrTexSet;
    int normalTexSet;
    int aoTexSet;
    int emissiveTexSet;
    float metallicFactor;
    float roughnessFactor;
    vec4 baseColorFactor;
    vec4 emissiveFactor;
};

layout(std140, set = 0, binding = 2) readonly buffer MaterialBuffer {
    Material materialData[];
};

layout (push_constant) uniform uNodePushConstant
{
    uint uNodeIndex;
    uint uMaterialIndex;
};

struct SurfaceOut
{
    vec4 albedo;
    vec3 normal;
    vec3 emissive;
    float roughness;
    float metallic;
    float occlusion;
};

vec3 GetNormal() {
    vec3 pos_dx = dFdx(inWorldPos);
    vec3 pos_dy = dFdy(inWorldPos);
    vec3 st1    = dFdx(vec3(inUV, 0.0));
    vec3 st2    = dFdy(vec3(inUV, 0.0));
    vec3 T      = (st2.t * pos_dx - st1.t * pos_dy) / (st1.s * st2.t - st2.s * st1.t);
    vec3 N      = normalize(inNormal);
    T           = normalize(T - N * dot(N, T));
    vec3 B      = normalize(cross(N, T));
    mat3 TBN    = mat3(T, B, N);

    return normalize(TBN[2].xyz);
}

void SurfaceShaderTextured(out SurfaceOut surface)
{
    Material surfMat = materialData[uMaterialIndex];

    surface.normal = GetNormal();

    surface.albedo = surfMat.baseColorFactor;
    surface.albedo *= texture(uTextureArray[surfMat.bcTexIndex], inUV);

    surface.roughness = surfMat.roughnessFactor;
    surface.roughness *= texture(uTextureArray[surfMat.mrTexIndex], inUV).g;

    surface.metallic = surfMat.metallicFactor;
    surface.metallic *= texture(uTextureArray[surfMat.mrTexIndex], inUV).b;

    surface.emissive = surfMat.emissiveFactor.rbg;
    surface.emissive *= texture(uTextureArray[surfMat.emissiveTexIndex], inUV).rgb;

    surface.occlusion = 1.0f;
    surface.occlusion *= texture(uTextureArray[surfMat.occlusionTexIndex], inUV).r;

}

void main()
{
    outPosition = vec4(inWorldPos, 1.0);

    SurfaceOut surface;
    SurfaceShaderTextured(surface);

    outNormal = vec4(surface.normal, 1.0);
    outAlbedo = vec4(surface.albedo);
    outMetallicRoughness = vec4(surface.metallic, surface.roughness, vec2(0.0));
    outEmissiveOcclusion = vec4(surface.emissive, surface.occlusion);
}