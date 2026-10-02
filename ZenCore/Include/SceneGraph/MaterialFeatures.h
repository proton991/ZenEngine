#pragma once

#include "Texture.h"

namespace zen::sg
{
struct TextureTransform
{
    Vec4  row0{1, 0, 0, 0};
    Vec4  row1{0, 1, 0, 0};
    Vec2  uvOffset{0.0f};
    Vec2  uvScale{1.0f};
    float uvRotation{0.0f};

    bool operator==(const TextureTransform&) const = default;
};

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

struct MaterialTextureBinding
{
    Texture*         texture{nullptr};
    uint32_t         texCoord{0};
    TextureTransform transform;
    float            scale{1.0f};

    bool operator==(const MaterialTextureBinding&) const = default;
};

enum class MaterialFeatureTexture : uint32_t
{
    Specular,
    SpecularColor,
    Diffuse,
    SpecularGlossiness,
    Clearcoat,
    ClearcoatRoughness,
    ClearcoatNormal,
    SheenColor,
    SheenRoughness,
    Transmission,
    Thickness,
    Iridescence,
    IridescenceThickness,
    Anisotropy,
    DiffuseTransmission,
    DiffuseTransmissionColor,
    Retroreflection,
    Count
};

// Vec4 fields deliberately match the std140 GPU representation.
struct MaterialTextureData
{
    Vec4                 properties{-1.0f, 0.0f, 1.0f, 0.0f};
    TextureTransformData transform;
};

struct MaterialFeatures
{
    float                  ior{1.5f};
    float                  dispersion{0.0f};
    float                  specular{1.0f};
    Vec3                   specularColor{1.0f};
    float                  glossiness{1.0f};
    float                  clearcoat{0.0f};
    float                  clearcoatRoughness{0.0f};
    Vec3                   sheenColor{0.0f};
    float                  sheenRoughness{0.0f};
    float                  transmission{0.0f};
    float                  thickness{0.0f};
    Vec3                   attenuationColor{1.0f};
    float                  attenuationDistance{std::numeric_limits<float>::infinity()};
    float                  iridescence{0.0f};
    float                  iridescenceIor{1.3f};
    float                  iridescenceThicknessMin{100.0f};
    float                  iridescenceThicknessMax{400.0f};
    float                  anisotropy{0.0f};
    float                  anisotropyRotation{0.0f};
    float                  diffuseTransmission{0.0f};
    Vec3                   diffuseTransmissionColor{1.0f};
    Vec3                   multiscatterColor{0.0f};
    float                  retroreflection{0.0f};
    MaterialTextureBinding specularTexture;
    MaterialTextureBinding specularColorTexture;
    MaterialTextureBinding diffuseTexture;
    MaterialTextureBinding specularGlossinessTexture;
    MaterialTextureBinding clearcoatTexture;
    MaterialTextureBinding clearcoatRoughnessTexture;
    MaterialTextureBinding clearcoatNormalTexture;
    MaterialTextureBinding sheenColorTexture;
    MaterialTextureBinding sheenRoughnessTexture;
    MaterialTextureBinding transmissionTexture;
    MaterialTextureBinding thicknessTexture;
    MaterialTextureBinding iridescenceTexture;
    MaterialTextureBinding iridescenceThicknessTexture;
    MaterialTextureBinding anisotropyTexture;
    MaterialTextureBinding diffuseTransmissionTexture;
    MaterialTextureBinding diffuseTransmissionColorTexture;
    MaterialTextureBinding retroreflectionTexture;

    bool operator==(const MaterialFeatures&) const = default;
};
} // namespace zen::sg
