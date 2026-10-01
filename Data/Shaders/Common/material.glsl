#ifndef ZEN_MATERIAL_GLSL
#define ZEN_MATERIAL_GLSL
#include "bindless_heap.glsl"
#if defined(MATERIAL_BINDING_UV) && !defined(MATERIAL_BINDING_UV_VALUE)
#define MATERIAL_BINDING_UV_VALUE(slot) inBindingUV[slot]
#endif
struct TextureTransform { vec4 row0; vec4 row1; };
struct MaterialTextureData { vec4 properties; TextureTransform transform; };
struct Material
{
    int bcTexIndex; int mrTexIndex; int normalTexIndex; int occlusionTexIndex;
    int emissiveTexIndex; int bcTexSet; int mrTexSet; int normalTexSet;
    int aoTexSet; int emissiveTexSet; float metallicFactor; float roughnessFactor;
    // surfaceProperties: alpha cutoff, alpha mode, normal scale, reserved.
    vec4 baseColorFactor; vec4 emissiveFactor; vec4 surfaceProperties;
    vec4 materialProperties; TextureTransform textureTransforms[5];
    vec4 specularColorIor; vec4 specularGlossiness; vec4 diffuseFactor;
    vec4 clearcoatSheenSpecular; vec4 sheenColorTransmission; vec4 volumeIridescence;
    vec4 attenuationColorDispersion; vec4 iridescenceAnisotropy;
    vec4 diffuseTransmissionColorFactor; vec4 volumeScatterColorRetroreflection;
    MaterialTextureData featureTextures[17];
};
vec2 MaterialUV(int uvSet, vec2 uv0, vec2 uv1) { return uvSet == 1 ? uv1 : uv0; }
vec2 MaterialTransformedUV(Material material, int slot, int uvSet, vec2 uv0, vec2 uv1)
{
    vec3 uv = vec3(MaterialUV(uvSet, uv0, uv1), 1.0);
#ifdef MATERIAL_BINDING_UV
    uv = vec3(MATERIAL_BINDING_UV_VALUE(slot), 1.0);
#endif
    return vec2(dot(material.textureTransforms[slot].row0.xyz, uv),
                dot(material.textureTransforms[slot].row1.xyz, uv));
}
vec4 MaterialTexture(int index, vec2 uv)
{
    if (index < 0) return vec4(1.0);
#ifdef MATERIAL_FRAGMENT
    return texture(sampler2D(uTexture2DHeap[nonuniformEXT(index)], uSamplerHeap[0]), uv);
#else
    return textureLod(sampler2D(uTexture2DHeap[nonuniformEXT(index)], uSamplerHeap[0]), uv, 0.0);
#endif
}
vec4 MaterialSlotTexture(Material material, int slot, int index, vec2 uv)
{
    if (index < 0) return vec4(1.0);
    int samplerIndex = int(material.textureTransforms[slot].row0.w);
#ifdef MATERIAL_FRAGMENT
    return texture(sampler2D(uTexture2DHeap[nonuniformEXT(index)], uSamplerHeap[nonuniformEXT(samplerIndex)]), uv);
#else
    return textureLod(sampler2D(uTexture2DHeap[nonuniformEXT(index)], uSamplerHeap[nonuniformEXT(samplerIndex)]), uv, 0.0);
#endif
}
vec4 MaterialAlbedo(Material material, vec2 uv0, vec2 uv1, vec4 color)
{
    return material.baseColorFactor * color * MaterialSlotTexture(material, 0, material.bcTexIndex,
        MaterialTransformedUV(material, 0, material.bcTexSet, uv0, uv1));
}
bool MaterialVisible(Material material, float alpha)
{
    return material.surfaceProperties.y != 1.0 || alpha >= material.surfaceProperties.x;
}
vec4 MaterialFeatureTexture(Material material, int slot, vec2 uv0, vec2 uv1)
{
    MaterialTextureData binding = material.featureTextures[slot];
    int index = int(binding.properties.x);
    if (index < 0) return vec4(1.0);
    vec3 uv = vec3(MaterialUV(int(binding.properties.y), uv0, uv1), 1.0);
#ifdef MATERIAL_BINDING_UV
    uv = vec3(MATERIAL_BINDING_UV_VALUE(slot + 5), 1.0);
#endif
    vec2 transformed = vec2(dot(binding.transform.row0.xyz, uv), dot(binding.transform.row1.xyz, uv));
    int samplerIndex = int(binding.transform.row0.w);
#ifdef MATERIAL_FRAGMENT
    return texture(sampler2D(uTexture2DHeap[nonuniformEXT(index)], uSamplerHeap[nonuniformEXT(samplerIndex)]), transformed);
#else
    return textureLod(sampler2D(uTexture2DHeap[nonuniformEXT(index)], uSamplerHeap[nonuniformEXT(samplerIndex)]), transformed, 0.0);
#endif
}
#endif
