#pragma once
#include "Component.h"
#include "Common/Types.h"
#include "Common/Math.h"
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

class Material : public Component
{
public:
    static UniquePtr<Material> CreateDefaultUnique()
    {
        return MakeUnique<Material>("DefaultMaterial");
    }

    Material(std::string name) : Component(std::move(name)) {}

    virtual TypeId GetTypeId() const override { return typeid(Material); }

    AlphaMode alphaMode{AlphaMode::Opaque};
    bool      doubleSided{false};
    // material factors
    float alphaCutoff{1.0f};
    float metallicFactor{1.0f};
    float roughnessFactor{1.0f};
    Vec4  baseColorFactor{1.0f};
    Vec4  emissiveFactor{0.0f};
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
};
} // namespace zen::sg