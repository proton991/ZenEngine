#ifndef ZEN_RAY_HIT_MATERIAL
#define ZEN_RAY_HIT_MATERIAL
// ABI mirrors Graphics/Shared/RayHitShading.h.
struct RayHitVertex
{
    vec4 normal;
    vec4 tangent;
    vec4 color;
};
struct RayHitMaterial
{
    vec4                baseColorFactor;
    vec4                emissiveFactor;
    vec4                surfaceProperties;
    vec4                specularFactor;
    uvec4               normalEncoding;
    vec4                textureSampling;
    vec4                factors;
    MaterialTextureData textures[4];
};
layout(std430, set = 6, binding = 7) readonly buffer RayHitVertices
{
    RayHitVertex rayHitVertices[];
};
layout(std430, set = 6, binding = 8) readonly buffer RayHitMaterials
{
    RayHitMaterial rayHitMaterials[];
};

// Present the existing shading fields without changing its arithmetic. The non-stream
// program reads the original records directly; the stream program expands only used fields.
Vertex LoadRayHitVertex(uint index)
{
    Vertex result;
#ifdef HYBRID_HIT_STREAM
    RayHitVertex source = rayHitVertices[index];
    result.position     = vertices[index].position;
    result.normal       = source.normal;
    result.tangent      = source.tangent;
    result.color        = source.color;
#else
    result = vertices[index];
#endif
    return result;
}

Material LoadRayHitMaterial(uint index)
{
    Material result;
#ifdef HYBRID_HIT_STREAM
    RayHitMaterial source       = rayHitMaterials[index];
    result.baseColorFactor      = source.baseColorFactor;
    result.diffuseFactor        = source.baseColorFactor;
    result.emissiveFactor       = source.emissiveFactor;
    result.surfaceProperties    = source.surfaceProperties;
    result.specularGlossiness   = source.specularFactor;
    result.normalEncoding       = source.normalEncoding;
    result.textureSampling      = source.textureSampling;
    result.metallicFactor       = source.factors.x;
    result.bcTexIndex           = int(source.textures[0].properties.x);
    result.bcTexSet             = int(source.textures[0].properties.y);
    result.mrTexIndex           = int(source.textures[1].properties.x);
    result.mrTexSet             = int(source.textures[1].properties.y);
    result.normalTexIndex       = int(source.textures[2].properties.x);
    result.normalTexSet         = int(source.textures[2].properties.y);
    result.emissiveTexIndex     = int(source.textures[3].properties.x);
    result.emissiveTexSet       = int(source.textures[3].properties.y);
    result.textureTransforms[0] = source.textures[0].transform;
    result.textureTransforms[1] = source.textures[1].transform;
    result.textureTransforms[2] = source.textures[2].transform;
    result.textureTransforms[4] = source.textures[3].transform;
    result.featureTextures[2]   = source.textures[0];
    result.featureTextures[3]   = source.textures[1];
#else
    result = materialData[index];
#endif
    return result;
}
#endif
