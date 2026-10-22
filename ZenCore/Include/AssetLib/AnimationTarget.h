#pragma once
#include <cstdint>

namespace zen::asset
{
enum class AnimatedObject : uint32_t
{
    Node,
    Material,
    Camera,
    Light,
    Environment
};

enum class AnimatedProperty : uint32_t
{
    Invalid,
    Translation,
    Rotation,
    Scale,
    Weights,
    Visible,
    Selectable,
    Hoverable,
    BaseColor,
    Metallic,
    Roughness,
    Emissive,
    AlphaCutoff,
    NormalScale,
    OcclusionStrength,
    EmissiveStrength,
    Ior,
    Dispersion,
    Specular,
    SpecularColor,
    Diffuse,
    SpecularGlossinessColor,
    Glossiness,
    Clearcoat,
    ClearcoatRoughness,
    ClearcoatNormalScale,
    SheenColor,
    SheenRoughness,
    Transmission,
    Thickness,
    AttenuationDistance,
    AttenuationColor,
    Iridescence,
    IridescenceIor,
    IridescenceThicknessMinimum,
    IridescenceThicknessMaximum,
    Anisotropy,
    AnisotropyRotation,
    DiffuseTransmission,
    DiffuseTransmissionColor,
    MultiscatterColor,
    Retroreflection,
    TextureOffset,
    TextureScale,
    TextureRotation,
    CameraAspect,
    CameraFov,
    CameraNear,
    CameraFar,
    CameraXMag,
    CameraYMag,
    LightColor,
    LightIntensity,
    LightRange,
    LightInnerCone,
    LightOuterCone,
    EnvironmentIntensity,
    EnvironmentRotation
};

// An engine property, resolved once at import. No source path is interpreted at playback.
struct AnimationTarget
{
    AnimatedObject   object{AnimatedObject::Node};
    uint32_t         index{0};
    AnimatedProperty property{AnimatedProperty::Invalid};
    uint32_t         dimensions{0};
    // Only weights support component addressing. UINT32_MAX means the whole value.
    uint32_t element{UINT32_MAX};
    // Core material slots 0..4, then MaterialFeatureTexture slots.
    uint32_t textureSlot{UINT32_MAX};
};

inline bool IsQuaternionTarget(const AnimationTarget& target)
{
    return target.property == AnimatedProperty::Rotation || target.property == AnimatedProperty::EnvironmentRotation;
}
} // namespace zen::asset
