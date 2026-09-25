#ifndef ZEN_MATERIAL_GLSL
#define ZEN_MATERIAL_GLSL
#include "bindless_heap.glsl"
struct Material
{
    int bcTexIndex; int mrTexIndex; int normalTexIndex; int occlusionTexIndex;
    int emissiveTexIndex; int bcTexSet; int mrTexSet; int normalTexSet;
    int aoTexSet; int emissiveTexSet; float metallicFactor; float roughnessFactor;
    // surfaceProperties: alpha cutoff, alpha mode, normal scale, reserved.
    vec4 baseColorFactor; vec4 emissiveFactor; vec4 surfaceProperties;
};
vec2 MaterialUV(int uvSet, vec2 uv0, vec2 uv1) { return uvSet == 1 ? uv1 : uv0; }
vec4 MaterialTexture(int index, vec2 uv)
{
    if (index < 0) return vec4(1.0);
#ifdef MATERIAL_FRAGMENT
    return texture(sampler2D(uTexture2DHeap[nonuniformEXT(index)], uSamplerHeap[0]), uv);
#else
    return textureLod(sampler2D(uTexture2DHeap[nonuniformEXT(index)], uSamplerHeap[0]), uv, 0.0);
#endif
}
vec4 MaterialAlbedo(Material material, vec2 uv0, vec2 uv1, vec4 color)
{
    return material.baseColorFactor * color * MaterialTexture(material.bcTexIndex,
        MaterialUV(material.bcTexSet, uv0, uv1));
}
bool MaterialVisible(Material material, float alpha)
{
    return material.surfaceProperties.y == 0.0 || alpha >= material.surfaceProperties.x;
}
#endif
