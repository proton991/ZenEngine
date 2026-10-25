#pragma once
#include "AssetLib/MaterialAsset.h"
#include "Texture.h"
#include <bit>

namespace zen::sg
{
using TextureTransform       = asset::TextureTransform;
using MaterialFeatureTexture = asset::MaterialFeatureTexture;
using MaterialTextureBinding = asset::MaterialTextureBindingT<Texture*>;
using MaterialFeatures       = asset::MaterialFeaturesT<Texture*>;

struct TextureTransformData
{
    Vec4 row0{1, 0, 0, 0};
    // row1.w stores the runtime page-size code; xyz retain the authored affine row.
    Vec4 row1{0, 1, 0, 0};
    // Integer texel rectangle, separate from the authored UV transform. Zero means the full image.
    glm::uvec4 atlasRegion{0};

    bool operator==(const TextureTransformData&) const = default;
};

inline float GetAtlasPageSizeCode(const Texture* texture)
{
    float code = 0.0f;
    if (texture != nullptr && texture->GetAtlasRegion() != asset::ImageRegion{})
    {
        // The spatial profile requires power-of-two pages. Two five-bit exponents
        // fit exactly in a float and avoid bindless textureSize queries in shaders.
        // Invalid extents remain invalid; texture upload and material edits reject them.
        code = std::has_single_bit(texture->width) && std::has_single_bit(texture->height)
                 ? static_cast<float>(std::countr_zero(texture->width) | (std::countr_zero(texture->height) << 5))
                 : -1.0f;
    }
    return code;
}

inline TextureTransformData PublishTextureTransform(const TextureTransform& transform, const Texture* texture = nullptr)
{
    const asset::ImageRegion region = texture != nullptr ? texture->GetAtlasRegion() : asset::ImageRegion{};
    TextureTransformData     data{transform.row0, transform.row1, glm::uvec4(region.x, region.y, region.width, region.height)};
    data.row1.w = GetAtlasPageSizeCode(texture);
    return data;
}

// Vec4 fields deliberately match the std140 GPU representation.
struct MaterialTextureData
{
    Vec4                 properties{-1.0f, 0.0f, 1.0f, 0.0f};
    TextureTransformData transform;
};

} // namespace zen::sg
