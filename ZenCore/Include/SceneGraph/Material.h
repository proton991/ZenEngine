#pragma once
#include "Component.h"
#include "Utils/Errors.h"
#include "Texture.h"
#include "MaterialFeatures.h"
#include <cmath>

namespace zen::sg
{
enum class AlphaMode
{
    Opaque,
    Mask,
    Blend
};

struct MaterialData
{
    int   bcTexIndex{-1};
    int   mrTexIndex{-1};
    int   normalTexIndex{-1};
    int   occlusionTexIndex{-1};
    int   emissiveTexIndex{-1};
    int   bcTexSet{-1};
    int   mrTexSet{-1};
    int   normalTexSet{-1};
    int   aoTexSet{-1};
    int   emissiveTexSet{-1};
    float metallicFactor{0.0f};
    float roughnessFactor{1.0f};
    Vec4  baseColorFactor{1.0f};
    Vec4  emissiveFactor{0.0f};
    // Alpha cutoff, alpha mode, normal scale, specular/glossiness workflow.
    Vec4 surfaceProperties{0.5f, 0.0f, 1.0f, 0.0f};
    // Occlusion strength, unlit, double sided, advanced material shading.
    Vec4                 materialProperties{1.0f, 0.0f, 0.0f, 0.0f};
    TextureTransformData textureTransforms[5]{};
    Vec4                 specularColorIor{1.0f, 1.0f, 1.0f, 1.5f};
    Vec4                 specularGlossiness{0.0f, 0.0f, 0.0f, 1.0f};
    Vec4                 diffuseFactor{1.0f};
    // Clearcoat, clearcoat roughness, sheen roughness, specular strength.
    Vec4 clearcoatSheenSpecular{0.0f, 0.0f, 0.0f, 1.0f};
    Vec4 sheenColorTransmission{0.0f};
    // Thickness, attenuation distance (zero denotes infinity), iridescence, iridescence IOR.
    Vec4                volumeIridescence{0.0f, 0.0f, 0.0f, 1.3f};
    Vec4                attenuationColorDispersion{1.0f, 1.0f, 1.0f, 0.0f};
    Vec4                iridescenceAnisotropy{100.0f, 400.0f, 0.0f, 0.0f};
    Vec4                diffuseTransmissionColorFactor{1.0f, 1.0f, 1.0f, 0.0f};
    Vec4                volumeScatterColorRetroreflection{0.0f};
    MaterialTextureData featureTextures[static_cast<uint32_t>(MaterialFeatureTexture::Count)]{};
};

class Material : public Component
{
public:
    static UniquePtr<Material> CreateDefaultUnique()
    {
        return MakeUnique<Material>("DefaultMaterial");
    }

    Material(std::string name) : Component(std::move(name)) {}

    virtual TypeId GetTypeId() const override
    {
        return typeid(Material);
    }

    void SetData()
    {
        VERIFY_EXPR(m_pBaseColorTexture != nullptr);
        VERIFY_EXPR(m_pMetallicRoughnessTexture != nullptr);
        VERIFY_EXPR(m_pNormalTexture != nullptr);
        VERIFY_EXPR(m_pOcclusionTexture != nullptr);
        VERIFY_EXPR(m_pEmissiveTexture != nullptr);

        data.bcTexSet           = texCoordSets.baseColor;
        data.mrTexSet           = texCoordSets.metallicRoughness;
        data.normalTexSet       = texCoordSets.normal;
        data.aoTexSet           = texCoordSets.occlusion;
        data.emissiveTexSet     = texCoordSets.emissive;

        data.bcTexIndex         = static_cast<int>(m_pBaseColorTexture->index);
        data.mrTexIndex         = static_cast<int>(m_pMetallicRoughnessTexture->index);
        data.normalTexIndex     = static_cast<int>(m_pNormalTexture->index);
        data.occlusionTexIndex  = static_cast<int>(m_pOcclusionTexture->index);
        data.emissiveTexIndex   = static_cast<int>(m_pEmissiveTexture->index);

        data.metallicFactor     = metallicFactor;
        data.roughnessFactor    = roughnessFactor;
        data.baseColorFactor    = baseColorFactor;
        data.emissiveFactor     = emissiveFactor * emissiveStrength;
        data.surfaceProperties  = Vec4(alphaCutoff, static_cast<float>(alphaMode), normalScale, 0.0f);
        data.materialProperties = Vec4(occlusionStrength, unlit ? 1.0f : 0.0f, doubleSided ? 1.0f : 0.0f, 0.0f);

        for (uint32_t index = 0; index < 5; ++index)
        {
            data.textureTransforms[index] = PublishTextureTransform(textureTransforms[index]);
        }

        data.surfaceProperties.w = pbrWorkflows.specularGlossiness ? 1.0f : 0.0f;
        data.specularColorIor    = Vec4(features.specularColor, features.ior);
        data.specularGlossiness  = Vec4(extension.specularFactor, features.glossiness);
        data.diffuseFactor       = extension.diffuseFactor;
        data.clearcoatSheenSpecular =
            Vec4(features.clearcoat, features.clearcoatRoughness, features.sheenRoughness, features.specular);
        data.sheenColorTransmission = Vec4(features.sheenColor, features.transmission);
        data.volumeIridescence =
            Vec4(features.thickness, std::isfinite(features.attenuationDistance) ? features.attenuationDistance : 0.0f,
                 features.iridescence, features.iridescenceIor);
        data.attenuationColorDispersion          = Vec4(features.attenuationColor, features.dispersion);
        data.iridescenceAnisotropy               = Vec4(features.iridescenceThicknessMin, features.iridescenceThicknessMax,
                                                        features.anisotropy, features.anisotropyRotation);
        data.diffuseTransmissionColorFactor      = Vec4(features.diffuseTransmissionColor, features.diffuseTransmission);
        data.volumeScatterColorRetroreflection   = Vec4(features.multiscatterColor, features.retroreflection);

        const MaterialTextureBinding* bindings[] = {&features.specularTexture,
                                                    &features.specularColorTexture,
                                                    &features.diffuseTexture,
                                                    &features.specularGlossinessTexture,
                                                    &features.clearcoatTexture,
                                                    &features.clearcoatRoughnessTexture,
                                                    &features.clearcoatNormalTexture,
                                                    &features.sheenColorTexture,
                                                    &features.sheenRoughnessTexture,
                                                    &features.transmissionTexture,
                                                    &features.thicknessTexture,
                                                    &features.iridescenceTexture,
                                                    &features.iridescenceThicknessTexture,
                                                    &features.anisotropyTexture,
                                                    &features.diffuseTransmissionTexture,
                                                    &features.diffuseTransmissionColorTexture,
                                                    &features.retroreflectionTexture};

        for (uint32_t index = 0; index < static_cast<uint32_t>(MaterialFeatureTexture::Count); ++index)
        {
            const MaterialTextureBinding& binding = *bindings[index];

            data.featureTextures[index].properties =
                Vec4(binding.texture == nullptr ? -1.0f : static_cast<float>(binding.texture->index),
                     static_cast<float>(binding.texCoord), binding.scale, 0.0f);
            data.featureTextures[index].transform = PublishTextureTransform(binding.transform);
        }

        data.materialProperties.w = features == MaterialFeatures{} && !pbrWorkflows.specularGlossiness ? 0.0f : 1.0f;

        if (pbrWorkflows.specularGlossiness)
        {
            data.baseColorFactor = extension.diffuseFactor;
            data.bcTexIndex =
                features.diffuseTexture.texture == nullptr ? -1 : static_cast<int>(features.diffuseTexture.texture->index);
            data.bcTexSet             = static_cast<int>(features.diffuseTexture.texCoord);
            data.textureTransforms[0] = PublishTextureTransform(features.diffuseTexture.transform);
        }
    }

    AlphaMode alphaMode{AlphaMode::Opaque};
    bool      doubleSided{false};
    // material factors
    float            alphaCutoff{0.5f};
    float            metallicFactor{1.0f};
    float            roughnessFactor{1.0f};
    float            normalScale{1.0f};
    float            occlusionStrength{1.0f};
    TextureTransform textureTransforms[5]{};
    MaterialFeatures features;
    Vec4             baseColorFactor{1.0f};
    Vec4             emissiveFactor{0.0f};
    // textures
    Texture* m_pBaseColorTexture{nullptr};
    Texture* m_pMetallicRoughnessTexture{nullptr};
    Texture* m_pNormalTexture{nullptr};
    Texture* m_pOcclusionTexture{nullptr};
    Texture* m_pEmissiveTexture{nullptr};

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
        Texture* pSpecularGlossinessTexture{nullptr};
        Texture* pDiffuseTexture{nullptr};
        Vec4     diffuseFactor{1.0f};
        Vec3     specularFactor{0.0f};
    } extension;
    struct PbrWorkflows
    {
        bool metallicRoughness{true};
        bool specularGlossiness{false};
    } pbrWorkflows;
    uint32_t index{0};
    bool     unlit{false};
    float    emissiveStrength{1.0f};

    MaterialData data;
};

inline bool EqualMaterialTexture(const Texture* left, const Texture* right)
{
    return left == right || (left != nullptr && right != nullptr && *left == *right);
}

inline bool operator==(const Material& lhs, const Material& rhs)
{
    return lhs.GetName() == rhs.GetName() && lhs.alphaMode == rhs.alphaMode && lhs.doubleSided == rhs.doubleSided
        && lhs.alphaCutoff == rhs.alphaCutoff && lhs.metallicFactor == rhs.metallicFactor
        && lhs.roughnessFactor == rhs.roughnessFactor && lhs.normalScale == rhs.normalScale
        && lhs.occlusionStrength == rhs.occlusionStrength && lhs.features == rhs.features
        && lhs.textureTransforms[0] == rhs.textureTransforms[0] && lhs.textureTransforms[1] == rhs.textureTransforms[1]
        && lhs.textureTransforms[2] == rhs.textureTransforms[2] && lhs.textureTransforms[3] == rhs.textureTransforms[3]
        && lhs.textureTransforms[4] == rhs.textureTransforms[4] && lhs.baseColorFactor == rhs.baseColorFactor
        && lhs.emissiveFactor == rhs.emissiveFactor && EqualMaterialTexture(lhs.m_pBaseColorTexture, rhs.m_pBaseColorTexture)
        && EqualMaterialTexture(lhs.m_pMetallicRoughnessTexture, rhs.m_pMetallicRoughnessTexture)
        && EqualMaterialTexture(lhs.m_pNormalTexture, rhs.m_pNormalTexture)
        && EqualMaterialTexture(lhs.m_pOcclusionTexture, rhs.m_pOcclusionTexture)
        && EqualMaterialTexture(lhs.m_pEmissiveTexture, rhs.m_pEmissiveTexture)
        && lhs.texCoordSets.baseColor == rhs.texCoordSets.baseColor
        && lhs.texCoordSets.metallicRoughness == rhs.texCoordSets.metallicRoughness
        && lhs.texCoordSets.specularGlossiness == rhs.texCoordSets.specularGlossiness
        && lhs.texCoordSets.normal == rhs.texCoordSets.normal && lhs.texCoordSets.occlusion == rhs.texCoordSets.occlusion
        && lhs.texCoordSets.emissive == rhs.texCoordSets.emissive
        && lhs.extension.pSpecularGlossinessTexture == rhs.extension.pSpecularGlossinessTexture
        && lhs.extension.pDiffuseTexture == rhs.extension.pDiffuseTexture
        && lhs.extension.diffuseFactor == rhs.extension.diffuseFactor
        && lhs.extension.specularFactor == rhs.extension.specularFactor
        && lhs.pbrWorkflows.metallicRoughness == rhs.pbrWorkflows.metallicRoughness
        && lhs.pbrWorkflows.specularGlossiness == rhs.pbrWorkflows.specularGlossiness && lhs.index == rhs.index
        && lhs.unlit == rhs.unlit && lhs.emissiveStrength == rhs.emissiveStrength;
}

inline bool operator!=(const Material& lhs, const Material& rhs)
{
    return !(lhs == rhs);
}
} // namespace zen::sg
