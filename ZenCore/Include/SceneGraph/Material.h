#pragma once
#include "Component.h"
#include "Common/Math.h"
#include "Common/Errors.h"
#include "Texture.h"
#include <vector>

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
        VERIFY_EXPR(baseColorTexture != nullptr);
        VERIFY_EXPR(metallicRoughnessTexture != nullptr);
        VERIFY_EXPR(normalTexture != nullptr);
        VERIFY_EXPR(occlusionTexture != nullptr);
        VERIFY_EXPR(emissiveTexture != nullptr);

        data.bcTexSet       = texCoordSets.baseColor;
        data.mrTexSet       = texCoordSets.metallicRoughness;
        data.normalTexSet   = texCoordSets.normal;
        data.aoTexSet       = texCoordSets.occlusion;
        data.emissiveTexSet = texCoordSets.emissive;

        data.bcTexIndex        = static_cast<int>(baseColorTexture->index);
        data.mrTexIndex        = static_cast<int>(metallicRoughnessTexture->index);
        data.normalTexIndex    = static_cast<int>(normalTexture->index);
        data.occlusionTexIndex = static_cast<int>(occlusionTexture->index);
        data.emissiveTexIndex  = static_cast<int>(emissiveTexture->index);

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
    Texture* baseColorTexture{nullptr};
    Texture* metallicRoughnessTexture{nullptr};
    Texture* normalTexture{nullptr};
    Texture* occlusionTexture{nullptr};
    Texture* emissiveTexture{nullptr};

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
        Texture* specularGlossinessTexture;
        Texture* diffuseTexture;
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

inline bool operator==(const Material& lhs, const Material& rhs)
{
    // Compare the name
    if (lhs.GetName() != rhs.GetName())
        return false;

    // Compare alphaMode
    if (lhs.alphaMode != rhs.alphaMode)
        return false;

    // Compare doubleSided flag
    if (lhs.doubleSided != rhs.doubleSided)
        return false;

    // Compare material factors
    if (lhs.alphaCutoff != rhs.alphaCutoff)
        return false;

    if (lhs.metallicFactor != rhs.metallicFactor)
        return false;

    if (lhs.roughnessFactor != rhs.roughnessFactor)
        return false;

    if (lhs.baseColorFactor != rhs.baseColorFactor)
        return false;

    if (lhs.emissiveFactor != rhs.emissiveFactor)
        return false;

    // Compare textures (check if the texture pointers are the same)
    if (*lhs.baseColorTexture != *rhs.baseColorTexture)
        return false;

    if (*lhs.metallicRoughnessTexture != *rhs.metallicRoughnessTexture)
        return false;

    if (*lhs.normalTexture != *rhs.normalTexture)
        return false;

    if (*lhs.occlusionTexture != *rhs.occlusionTexture)
        return false;

    if (*lhs.emissiveTexture != *rhs.emissiveTexture)
        return false;

    // Compare texCoordSets
    if (lhs.texCoordSets.baseColor != rhs.texCoordSets.baseColor ||
        lhs.texCoordSets.metallicRoughness != rhs.texCoordSets.metallicRoughness ||
        lhs.texCoordSets.specularGlossiness != rhs.texCoordSets.specularGlossiness ||
        lhs.texCoordSets.normal != rhs.texCoordSets.normal ||
        lhs.texCoordSets.occlusion != rhs.texCoordSets.occlusion ||
        lhs.texCoordSets.emissive != rhs.texCoordSets.emissive)
        return false;

    // Compare extension fields
    if (lhs.extension.specularGlossinessTexture != rhs.extension.specularGlossinessTexture ||
        lhs.extension.diffuseTexture != rhs.extension.diffuseTexture ||
        lhs.extension.diffuseFactor != rhs.extension.diffuseFactor ||
        lhs.extension.specularFactor != rhs.extension.specularFactor)
        return false;

    // Compare PBR workflows
    if (lhs.pbrWorkflows.metallicRoughness != rhs.pbrWorkflows.metallicRoughness ||
        lhs.pbrWorkflows.specularGlossiness != rhs.pbrWorkflows.specularGlossiness)
        return false;

    // Compare other scalar values and flags
    if (lhs.index != rhs.index || lhs.unlit != rhs.unlit ||
        lhs.emissiveStrength != rhs.emissiveStrength)
        return false;

    return true;
}

inline bool operator!=(const Material& lhs, const Material& rhs)
{
    return !(lhs == rhs);
}
} // namespace zen::sg