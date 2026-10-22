#pragma once
#include "AssetLib/AssetId.h"
#include "Math/Math.h"
#include <limits>
#include <array>
#include <string>

namespace zen::asset
{
enum class AlphaMode
{
    Opaque,
    Mask,
    Blend
};

struct TextureTransform
{
    Vec4  row0{1, 0, 0, 0};
    Vec4  row1{0, 1, 0, 0};
    Vec2  uvOffset{0.0f};
    Vec2  uvScale{1.0f};
    float uvRotation{0.0f};

    bool operator==(const TextureTransform&) const = default;
};

template <class TextureReference> struct MaterialTextureBindingT
{
    TextureReference texture{};
    uint32_t         texCoord{0};
    TextureTransform transform;
    float            scale{1.0f};

    bool operator==(const MaterialTextureBindingT&) const = default;
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

struct MaterialFeatureFactors
{
    float ior{1.5f};
    float dispersion{0.0f};
    float specular{1.0f};
    Vec3  specularColor{1.0f};
    float glossiness{1.0f};
    float clearcoat{0.0f};
    float clearcoatRoughness{0.0f};
    Vec3  sheenColor{0.0f};
    float sheenRoughness{0.0f};
    float transmission{0.0f};
    float thickness{0.0f};
    Vec3  attenuationColor{1.0f};
    float attenuationDistance{std::numeric_limits<float>::infinity()};
    float iridescence{0.0f};
    float iridescenceIor{1.3f};
    float iridescenceThicknessMin{100.0f};
    float iridescenceThicknessMax{400.0f};
    float anisotropy{0.0f};
    float anisotropyRotation{0.0f};
    float diffuseTransmission{0.0f};
    Vec3  diffuseTransmissionColor{1.0f};
    Vec3  multiscatterColor{0.0f};
    float retroreflection{0.0f};
    bool  operator==(const MaterialFeatureFactors&) const = default;
};

template <class TextureReference> struct MaterialFeaturesT : MaterialFeatureFactors
{
    MaterialTextureBindingT<TextureReference> specularTexture;
    MaterialTextureBindingT<TextureReference> specularColorTexture;
    MaterialTextureBindingT<TextureReference> diffuseTexture;
    MaterialTextureBindingT<TextureReference> specularGlossinessTexture;
    MaterialTextureBindingT<TextureReference> clearcoatTexture;
    MaterialTextureBindingT<TextureReference> clearcoatRoughnessTexture;
    MaterialTextureBindingT<TextureReference> clearcoatNormalTexture;
    MaterialTextureBindingT<TextureReference> sheenColorTexture;
    MaterialTextureBindingT<TextureReference> sheenRoughnessTexture;
    MaterialTextureBindingT<TextureReference> transmissionTexture;
    MaterialTextureBindingT<TextureReference> thicknessTexture;
    MaterialTextureBindingT<TextureReference> iridescenceTexture;
    MaterialTextureBindingT<TextureReference> iridescenceThicknessTexture;
    MaterialTextureBindingT<TextureReference> anisotropyTexture;
    MaterialTextureBindingT<TextureReference> diffuseTransmissionTexture;
    MaterialTextureBindingT<TextureReference> diffuseTransmissionColorTexture;
    MaterialTextureBindingT<TextureReference> retroreflectionTexture;

    using TextureMember = MaterialTextureBindingT<TextureReference>                                       MaterialFeaturesT::*;
    inline static constexpr std::array<TextureMember, static_cast<size_t>(MaterialFeatureTexture::Count)> textureMembers{
        &MaterialFeaturesT::specularTexture,
        &MaterialFeaturesT::specularColorTexture,
        &MaterialFeaturesT::diffuseTexture,
        &MaterialFeaturesT::specularGlossinessTexture,
        &MaterialFeaturesT::clearcoatTexture,
        &MaterialFeaturesT::clearcoatRoughnessTexture,
        &MaterialFeaturesT::clearcoatNormalTexture,
        &MaterialFeaturesT::sheenColorTexture,
        &MaterialFeaturesT::sheenRoughnessTexture,
        &MaterialFeaturesT::transmissionTexture,
        &MaterialFeaturesT::thicknessTexture,
        &MaterialFeaturesT::iridescenceTexture,
        &MaterialFeaturesT::iridescenceThicknessTexture,
        &MaterialFeaturesT::anisotropyTexture,
        &MaterialFeaturesT::diffuseTransmissionTexture,
        &MaterialFeaturesT::diffuseTransmissionColorTexture,
        &MaterialFeaturesT::retroreflectionTexture};

    bool operator==(const MaterialFeaturesT&) const = default;
};
// Semantic material state shared by the canonical asset and mutable runtime.
// Imported bindings hold asset IDs; runtime bindings resolve those IDs to textures.
template <class TextureReference> struct MaterialParametersT
{
    AlphaMode alphaMode{AlphaMode::Opaque};
    bool      doubleSided{false};
    // material factors
    float                               alphaCutoff{0.5f};
    float                               metallicFactor{1.0f};
    float                               roughnessFactor{1.0f};
    float                               normalScale{1.0f};
    float                               occlusionStrength{1.0f};
    TextureTransform                    textureTransforms[5]{};
    MaterialFeaturesT<TextureReference> features;
    Vec4                                baseColorFactor{1.0f};
    Vec4                                emissiveFactor{0.0f};
    // textures
    TextureReference m_pBaseColorTexture{};
    TextureReference m_pMetallicRoughnessTexture{};
    TextureReference m_pNormalTexture{};
    TextureReference m_pOcclusionTexture{};
    TextureReference m_pEmissiveTexture{};

    struct TexCoordSets
    {
        uint32_t baseColor{0};
        uint32_t metallicRoughness{0};
        uint32_t specularGlossiness{0};
        uint32_t normal{0};
        uint32_t occlusion{0};
        uint32_t emissive{0};
    } texCoordSets;
    struct Extension
    {
        TextureReference pSpecularGlossinessTexture{};
        TextureReference pDiffuseTexture{};
        Vec4             diffuseFactor{1.0f};
        Vec3             specularFactor{0.0f};
    } extension;
    struct PbrWorkflows
    {
        bool metallicRoughness{true};
        bool specularGlossiness{false};
    } pbrWorkflows;
    bool  unlit{false};
    float emissiveStrength{1.0f};
};

template <class Destination, class Source, class Resolve> void CopyMaterialParameters(MaterialParametersT<Destination>&  target,
                                                                                      const MaterialParametersT<Source>& source,
                                                                                      const Resolve& resolve)
{
    target.alphaMode         = source.alphaMode;
    target.doubleSided       = source.doubleSided;
    target.alphaCutoff       = source.alphaCutoff;
    target.metallicFactor    = source.metallicFactor;
    target.roughnessFactor   = source.roughnessFactor;
    target.normalScale       = source.normalScale;
    target.occlusionStrength = source.occlusionStrength;
    target.baseColorFactor   = source.baseColorFactor;
    target.emissiveFactor    = source.emissiveFactor;
    target.unlit             = source.unlit;
    target.emissiveStrength  = source.emissiveStrength;
    for (size_t index = 0; index < 5; ++index)
    {
        target.textureTransforms[index] = source.textureTransforms[index];
    }
    static_cast<MaterialFeatureFactors&>(target.features) = source.features;
    for (size_t index = 0; index < static_cast<size_t>(MaterialFeatureTexture::Count); ++index)
    {
        const MaterialTextureBindingT<Source>& binding = source.features.*MaterialFeaturesT<Source>::textureMembers[index];
        target.features.*MaterialFeaturesT<Destination>::textureMembers[index] = {resolve(binding.texture), binding.texCoord,
                                                                                  binding.transform, binding.scale};
    }
    target.m_pBaseColorTexture                  = resolve(source.m_pBaseColorTexture);
    target.m_pMetallicRoughnessTexture          = resolve(source.m_pMetallicRoughnessTexture);
    target.m_pNormalTexture                     = resolve(source.m_pNormalTexture);
    target.m_pOcclusionTexture                  = resolve(source.m_pOcclusionTexture);
    target.m_pEmissiveTexture                   = resolve(source.m_pEmissiveTexture);
    target.texCoordSets.baseColor               = source.texCoordSets.baseColor;
    target.texCoordSets.metallicRoughness       = source.texCoordSets.metallicRoughness;
    target.texCoordSets.specularGlossiness      = source.texCoordSets.specularGlossiness;
    target.texCoordSets.normal                  = source.texCoordSets.normal;
    target.texCoordSets.occlusion               = source.texCoordSets.occlusion;
    target.texCoordSets.emissive                = source.texCoordSets.emissive;
    target.extension.pSpecularGlossinessTexture = resolve(source.extension.pSpecularGlossinessTexture);
    target.extension.pDiffuseTexture            = resolve(source.extension.pDiffuseTexture);
    target.extension.diffuseFactor              = source.extension.diffuseFactor;
    target.extension.specularFactor             = source.extension.specularFactor;
    target.pbrWorkflows.metallicRoughness       = source.pbrWorkflows.metallicRoughness;
    target.pbrWorkflows.specularGlossiness      = source.pbrWorkflows.specularGlossiness;
}

using MaterialTextureBinding = MaterialTextureBindingT<TextureId>;
using MaterialFeatures       = MaterialFeaturesT<TextureId>;

struct MaterialAsset : MaterialParametersT<TextureId>
{
    std::string name;
};
} // namespace zen::asset
