#pragma once
#include "AssetLib/MaterialAsset.h"
#include "Texture.h"

namespace zen::sg
{
using TextureTransform       = asset::TextureTransform;
using MaterialFeatureTexture = asset::MaterialFeatureTexture;
using MaterialTextureBinding = asset::MaterialTextureBindingT<Texture*>;
using MaterialFeatures       = asset::MaterialFeaturesT<Texture*>;

struct TextureTransformData
{
    Vec4 row0{1, 0, 0, 0};
    Vec4 row1{0, 1, 0, 0};

    bool operator==(const TextureTransformData&) const = default;
};

inline TextureTransformData PublishTextureTransform(const TextureTransform& transform)
{
    return {transform.row0, transform.row1};
}

// Vec4 fields deliberately match the std140 GPU representation.
struct MaterialTextureData
{
    Vec4                 properties{-1.0f, 0.0f, 1.0f, 0.0f};
    TextureTransformData transform;
};

} // namespace zen::sg
