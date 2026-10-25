#ifndef ZEN_MATERIAL_GLSL
#define ZEN_MATERIAL_GLSL
#include "bindless_heap.glsl"
#if defined(MATERIAL_BINDING_UV) && !defined(MATERIAL_BINDING_UV_VALUE)
#    define MATERIAL_BINDING_UV_VALUE(slot) inBindingUV[slot]
#endif
struct TextureTransform
{
    vec4  row0;
    vec4  row1;
    uvec4 atlasRegion;
};
struct MaterialTextureData
{
    vec4             properties;
    TextureTransform transform;
};
struct Material
{
    int   bcTexIndex;
    int   mrTexIndex;
    int   normalTexIndex;
    int   occlusionTexIndex;
    int   emissiveTexIndex;
    int   bcTexSet;
    int   mrTexSet;
    int   normalTexSet;
    int   aoTexSet;
    int   emissiveTexSet;
    float metallicFactor;
    float roughnessFactor;
    // surfaceProperties: alpha cutoff, alpha mode, normal scale, specular/glossiness workflow.
    vec4                baseColorFactor;
    vec4                emissiveFactor;
    vec4                surfaceProperties;
    vec4                materialProperties;
    TextureTransform    textureTransforms[5];
    vec4                specularColorIor;
    vec4                specularGlossiness;
    vec4                diffuseFactor;
    vec4                clearcoatSheenSpecular;
    vec4                sheenColorTransmission;
    vec4                volumeIridescence;
    vec4                attenuationColorDispersion;
    vec4                iridescenceAnisotropy;
    vec4                diffuseTransmissionColorFactor;
    vec4                volumeScatterColorRetroreflection;
    MaterialTextureData featureTextures[17];
    uvec4               normalEncoding;
    vec4                textureSampling;
};
vec3 MaterialNormalVector(vec3 sampled, uint encoding, float scale)
{
    vec3 mapped = sampled * 2.0 - 1.0;
    if (encoding == 1u)
    {
        mapped.z = sqrt(max(0.0, 1.0 - dot(mapped.xy, mapped.xy)));
    }
    mapped.xy *= scale;
    return mapped;
}
vec2 MaterialUV(int uvSet, vec2 uv0, vec2 uv1)
{
    return uvSet == 1 ? uv1 : uv0;
}
vec2 MaterialTransformedUV(Material material, int slot, int uvSet, vec2 uv0, vec2 uv1)
{
    vec3 uv = vec3(MaterialUV(uvSet, uv0, uv1), 1.0);
#ifdef MATERIAL_BINDING_UV
    uv = vec3(MATERIAL_BINDING_UV_VALUE(slot), 1.0);
#endif
    return vec2(dot(material.textureTransforms[slot].row0.xyz, uv), dot(material.textureTransforms[slot].row1.xyz, uv));
}
vec4 MaterialTexture(int index, vec2 uv)
{
    if (index < 0)
    {
        return vec4(1.0);
    }
#ifdef MATERIAL_FRAGMENT
    return texture(sampler2D(uTexture2DHeap[nonuniformEXT(index)], uSamplerHeap[0]), uv);
#else
    return textureLod(sampler2D(uTexture2DHeap[nonuniformEXT(index)], uSamplerHeap[0]), uv, 0.0);
#endif
}
vec2 MaterialAtlasPageSize(TextureTransform transform)
{
    // CPU publication stores two five-bit power-of-two exponents in this unused affine lane.
    uint code = uint(transform.row1.w);
    return vec2(uvec2(1u << (code & 31u), 1u << (code >> 5u)));
}
// The spatial profile has one physical level, linear clamp sampling and no anisotropy.
// Quantize in logical texel space before adding the page offset. Keep authored UVs
// untouched for tangent reconstruction and use their original gradients below.
vec2 MaterialAtlasUV(TextureTransform transform, vec2 sampling, vec2 uv)
{
    precise vec2 scaled     = clamp(uv, vec2(0), vec2(1)) * vec2(transform.atlasRegion.zw);
    precise vec2 coordinate = scaled * sampling.x;
    // The device qualification selects the native rule before spatial materials are published.
    precise vec2 rounded    = (sampling.y == 2.0 ? roundEven(coordinate) : floor(coordinate + 0.5)) / sampling.x;
    precise vec2 offset     = vec2(transform.atlasRegion.xy) + rounded;
    precise vec2 normalized = offset / MaterialAtlasPageSize(transform);
    return normalized;
}
vec4 MaterialBindingTextureLod0(int index, TextureTransform transform, vec2 sampling, vec2 uv)
{
    if (index < 0)
    {
        return vec4(1.0);
    }
    vec2 coordinates = transform.atlasRegion.z != 0u ? MaterialAtlasUV(transform, sampling, uv) : uv;
    return textureLod(sampler2D(uTexture2DHeap[nonuniformEXT(index)], uSamplerHeap[nonuniformEXT(int(transform.row0.w))]),
                      coordinates, 0.0);
}
vec4 MaterialBindingTexture(int index, TextureTransform transform, vec2 sampling, vec2 uv)
{
    if (index < 0)
    {
        return vec4(1.0);
    }
#ifdef MATERIAL_FRAGMENT
    // Evaluate derivatives before the spatial branch, in the authored coordinate domain.
    vec2 dx = dFdx(uv);
    vec2 dy = dFdy(uv);
    if (transform.atlasRegion.z != 0u)
    {
        vec2 scale = vec2(transform.atlasRegion.zw) / MaterialAtlasPageSize(transform);
        return textureGrad(sampler2D(uTexture2DHeap[nonuniformEXT(index)], uSamplerHeap[nonuniformEXT(int(transform.row0.w))]),
                           MaterialAtlasUV(transform, sampling, uv), dx * scale, dy * scale);
    }
    return texture(sampler2D(uTexture2DHeap[nonuniformEXT(index)], uSamplerHeap[nonuniformEXT(int(transform.row0.w))]), uv);
#else
    return MaterialBindingTextureLod0(index, transform, sampling, uv);
#endif
}
vec4 MaterialSlotTexture(Material material, int slot, int index, vec2 uv)
{
    if (index < 0)
    {
        return vec4(1.0);
    }
    return MaterialBindingTexture(index, material.textureTransforms[slot], material.textureSampling.xy, uv);
}
vec4 MaterialAlbedo(Material material, vec2 uv0, vec2 uv1, vec4 color)
{
    return material.baseColorFactor * color
         * MaterialSlotTexture(material, 0, material.bcTexIndex,
                               MaterialTransformedUV(material, 0, material.bcTexSet, uv0, uv1));
}
bool MaterialVisible(Material material, float alpha)
{
    return material.surfaceProperties.y != 1.0 || alpha >= material.surfaceProperties.x;
}
// GI occlusion. Blended surfaces block where their coverage reaches one half, so decals and
// cutout cards do not occlude as solid quads. Transmission keeps the opaque-blocker approximation.
bool MaterialOccludes(Material material, float alpha)
{
    return material.surfaceProperties.y == 0.0
        || alpha >= (material.surfaceProperties.y == 1.0 ? material.surfaceProperties.x : 0.5);
}
vec4 MaterialFeatureTexture(Material material, int slot, vec2 uv0, vec2 uv1)
{
    MaterialTextureData binding = material.featureTextures[slot];
    int                 index   = int(binding.properties.x);
    if (index < 0)
    {
        return vec4(1.0);
    }
    vec3 uv = vec3(MaterialUV(int(binding.properties.y), uv0, uv1), 1.0);
#ifdef MATERIAL_BINDING_UV
    uv = vec3(MATERIAL_BINDING_UV_VALUE(slot + 5), 1.0);
#endif
    vec2 transformed = vec2(dot(binding.transform.row0.xyz, uv), dot(binding.transform.row1.xyz, uv));
    return MaterialBindingTexture(index, binding.transform, material.textureSampling.xy, transformed);
}
#endif
