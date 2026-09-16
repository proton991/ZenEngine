#pragma once
#include "Component.h"
#include "Utils/Errors.h"
#include "Texture.h"

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
    int bcTexIndex{-1};
    int mrTexIndex{-1};
    int normalTexIndex{-1};
    int occlusionTexIndex{-1};
    int emissiveTexIndex{-1};
    int bcTexSet{-1};
    int mrTexSet{-1};
    int normalTexSet{-1};
    int aoTexSet{-1};
    int emissiveTexSet{-1};
    float metallicFactor{0.0f};
    float roughnessFactor{1.0f};
    Vec4 baseColorFactor{1.0f};
    Vec4 emissiveFactor{0.0f};
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

        data.bcTexSet       = texCoordSets.baseColor;
        data.mrTexSet       = texCoordSets.metallicRoughness;
        data.normalTexSet   = texCoordSets.normal;
        data.aoTexSet       = texCoordSets.occlusion;
        data.emissiveTexSet = texCoordSets.emissive;

        data.bcTexIndex        = static_cast<int>(m_pBaseColorTexture->index);
        data.mrTexIndex        = static_cast<int>(m_pMetallicRoughnessTexture->index);
        data.normalTexIndex    = static_cast<int>(m_pNormalTexture->index);
        data.occlusionTexIndex = static_cast<int>(m_pOcclusionTexture->index);
        data.emissiveTexIndex  = static_cast<int>(m_pEmissiveTexture->index);

        data.metallicFactor  = metallicFactor;
        data.roughnessFactor = roughnessFactor;
        data.emissiveFactor  = emissiveFactor;
    }

    AlphaMode alphaMode{AlphaMode::Opaque};
    bool doubleSided{false};
    // material factors
    float alphaCutoff{1.0f};
    float metallicFactor{1.0f};
    float roughnessFactor{1.0f};
    Vec4 baseColorFactor{1.0f};
    Vec4 emissiveFactor{0.0f};
    // textures
    Texture* m_pBaseColorTexture{nullptr};
    Texture* m_pMetallicRoughnessTexture{nullptr};
    Texture* m_pNormalTexture{nullptr};
    Texture* m_pOcclusionTexture{nullptr};
    Texture* m_pEmissiveTexture{nullptr};

    struct TexCoordSets
    {
        uint8_t baseColor{0};
        uint8_t metallicRoughness{0};
        uint8_t specularGlossiness{0};
        uint8_t normal{0};
        uint8_t occlusion{0};
        uint8_t emissive{0};
    } texCoordSets;
    struct Extension
    {
        Texture* pSpecularGlossinessTexture{nullptr};
        Texture* pDiffuseTexture{nullptr};
        Vec4 diffuseFactor{1.0f};
        Vec3 specularFactor{0.0f};
    } extension;
    struct PbrWorkflows
    {
        bool metallicRoughness{true};
        bool specularGlossiness{false};
    } pbrWorkflows;
    uint32_t index{0};
    bool unlit{false};
    float emissiveStrength{1.0f};

    MaterialData data;
};

inline bool EqualMaterialTexture(const Texture* left, const Texture* right)
{
    return left == right || (left != nullptr && right != nullptr && *left == *right);
}

inline bool operator==(const Material& lhs, const Material& rhs)
{
    return lhs.GetName() == rhs.GetName() && lhs.alphaMode == rhs.alphaMode &&
        lhs.doubleSided == rhs.doubleSided && lhs.alphaCutoff == rhs.alphaCutoff &&
        lhs.metallicFactor == rhs.metallicFactor && lhs.roughnessFactor == rhs.roughnessFactor &&
        lhs.baseColorFactor == rhs.baseColorFactor && lhs.emissiveFactor == rhs.emissiveFactor &&
        EqualMaterialTexture(lhs.m_pBaseColorTexture, rhs.m_pBaseColorTexture) &&
        EqualMaterialTexture(lhs.m_pMetallicRoughnessTexture, rhs.m_pMetallicRoughnessTexture) &&
        EqualMaterialTexture(lhs.m_pNormalTexture, rhs.m_pNormalTexture) &&
        EqualMaterialTexture(lhs.m_pOcclusionTexture, rhs.m_pOcclusionTexture) &&
        EqualMaterialTexture(lhs.m_pEmissiveTexture, rhs.m_pEmissiveTexture) &&
        lhs.texCoordSets.baseColor == rhs.texCoordSets.baseColor &&
        lhs.texCoordSets.metallicRoughness == rhs.texCoordSets.metallicRoughness &&
        lhs.texCoordSets.specularGlossiness == rhs.texCoordSets.specularGlossiness &&
        lhs.texCoordSets.normal == rhs.texCoordSets.normal &&
        lhs.texCoordSets.occlusion == rhs.texCoordSets.occlusion &&
        lhs.texCoordSets.emissive == rhs.texCoordSets.emissive &&
        lhs.extension.pSpecularGlossinessTexture == rhs.extension.pSpecularGlossinessTexture &&
        lhs.extension.pDiffuseTexture == rhs.extension.pDiffuseTexture &&
        lhs.extension.diffuseFactor == rhs.extension.diffuseFactor &&
        lhs.extension.specularFactor == rhs.extension.specularFactor &&
        lhs.pbrWorkflows.metallicRoughness == rhs.pbrWorkflows.metallicRoughness &&
        lhs.pbrWorkflows.specularGlossiness == rhs.pbrWorkflows.specularGlossiness &&
        lhs.index == rhs.index && lhs.unlit == rhs.unlit &&
        lhs.emissiveStrength == rhs.emissiveStrength;
}

inline bool operator!=(const Material& lhs, const Material& rhs)
{
    return !(lhs == rhs);
}
} // namespace zen::sg
