#include "AssetLib/GltfAnimationBindings.h"
#include <algorithm>
#include <charconv>

namespace zen::asset
{
namespace
{
struct PropertyName
{
    const char*      name;
    AnimatedProperty property;
    uint32_t         dimensions;
};
bool ParseIndex(const std::string& token, uint32_t& index)
{
    const std::from_chars_result parsed = std::from_chars(token.data(), token.data() + token.size(), index);

    const bool valid = !token.empty() && (token.size() == 1 || token.front() != '0') && parsed.ec == std::errc{}
                    && parsed.ptr == token.data() + token.size();

    return valid;
}

bool SplitPointer(const std::string& pointer, HeapVector<std::string>& tokens)
{
    bool valid   = !pointer.empty() && pointer.front() == '/';

    size_t begin = 1;

    while (valid && begin <= pointer.size())
    {
        const size_t end          = pointer.find('/', begin);

        const std::string encoded = pointer.substr(begin, end - begin);

        std::string decoded;

        for (size_t index = 0; valid && index < encoded.size(); ++index)
        {
            if (encoded[index] == '~')
            {
                valid = index + 1 < encoded.size() && (encoded[index + 1] == '0' || encoded[index + 1] == '1');

                if (valid)
                {
                    decoded.push_back(encoded[++index] == '0' ? '~' : '/');
                }
            }
            else
            {
                decoded.push_back(encoded[index]);
            }
        }

        // Standard glTF property names contain no slashes; an escaped slash is a
        // literal property-name character, never another path separator.
        valid &= decoded.find('/') == std::string::npos;

        tokens.push_back(std::move(decoded));

        if (end == std::string::npos)
        {
            break;
        }

        begin = end + 1;
    }

    return valid;
}

std::string RelativePath(const HeapVector<std::string>& tokens, size_t begin)
{
    std::string result;

    for (size_t index = begin; index < tokens.size(); ++index)
    {
        if (!result.empty())
        {
            result += '/';
        }

        result += tokens[index];
    }

    return result;
}


bool FindProperty(const std::string& path, VectorView<const PropertyName> properties, AnimationTarget& target)
{
    bool found = false;
    for (const PropertyName& property : properties)
    {
        if (path == property.name)
        {
            target.property   = property.property;
            target.dimensions = property.dimensions;
            found             = true;
            break;
        }
    }
    return found;
}

bool CompileMaterial(const std::string& path, AnimationTarget& target)
{
    static const PropertyName properties[]{
        {"pbrMetallicRoughness/baseColorFactor", AnimatedProperty::BaseColor, 4},
        {"pbrMetallicRoughness/metallicFactor", AnimatedProperty::Metallic, 1},
        {"pbrMetallicRoughness/roughnessFactor", AnimatedProperty::Roughness, 1},
        {"emissiveFactor", AnimatedProperty::Emissive, 3},
        {"alphaCutoff", AnimatedProperty::AlphaCutoff, 1},
        {"normalTexture/scale", AnimatedProperty::NormalScale, 1},
        {"occlusionTexture/strength", AnimatedProperty::OcclusionStrength, 1},
        {"extensions/KHR_materials_emissive_strength/emissiveStrength", AnimatedProperty::EmissiveStrength, 1},
        {"extensions/KHR_materials_ior/ior", AnimatedProperty::Ior, 1},
        {"extensions/KHR_materials_dispersion/dispersion", AnimatedProperty::Dispersion, 1},
        {"extensions/KHR_materials_specular/specularFactor", AnimatedProperty::Specular, 1},
        {"extensions/KHR_materials_specular/specularColorFactor", AnimatedProperty::SpecularColor, 3},
        {"extensions/KHR_materials_pbrSpecularGlossiness/diffuseFactor", AnimatedProperty::Diffuse, 4},
        {"extensions/KHR_materials_pbrSpecularGlossiness/specularFactor", AnimatedProperty::SpecularGlossinessColor, 3},
        {"extensions/KHR_materials_pbrSpecularGlossiness/glossinessFactor", AnimatedProperty::Glossiness, 1},
        {"extensions/KHR_materials_clearcoat/clearcoatFactor", AnimatedProperty::Clearcoat, 1},
        {"extensions/KHR_materials_clearcoat/clearcoatRoughnessFactor", AnimatedProperty::ClearcoatRoughness, 1},
        {"extensions/KHR_materials_clearcoat/clearcoatNormalTexture/scale", AnimatedProperty::ClearcoatNormalScale, 1},
        {"extensions/KHR_materials_sheen/sheenColorFactor", AnimatedProperty::SheenColor, 3},
        {"extensions/KHR_materials_sheen/sheenRoughnessFactor", AnimatedProperty::SheenRoughness, 1},
        {"extensions/KHR_materials_transmission/transmissionFactor", AnimatedProperty::Transmission, 1},
        {"extensions/KHR_materials_volume/thicknessFactor", AnimatedProperty::Thickness, 1},
        {"extensions/KHR_materials_volume/attenuationDistance", AnimatedProperty::AttenuationDistance, 1},
        {"extensions/KHR_materials_volume/attenuationColor", AnimatedProperty::AttenuationColor, 3},
        {"extensions/KHR_materials_iridescence/iridescenceFactor", AnimatedProperty::Iridescence, 1},
        {"extensions/KHR_materials_iridescence/iridescenceIor", AnimatedProperty::IridescenceIor, 1},
        {"extensions/KHR_materials_iridescence/iridescenceThicknessMinimum", AnimatedProperty::IridescenceThicknessMinimum, 1},
        {"extensions/KHR_materials_iridescence/iridescenceThicknessMaximum", AnimatedProperty::IridescenceThicknessMaximum, 1},
        {"extensions/KHR_materials_anisotropy/anisotropyStrength", AnimatedProperty::Anisotropy, 1},
        {"extensions/KHR_materials_anisotropy/anisotropyRotation", AnimatedProperty::AnisotropyRotation, 1},
        {"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionFactor", AnimatedProperty::DiffuseTransmission, 1},
        {"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor",
         AnimatedProperty::DiffuseTransmissionColor, 3},
        {"extensions/KHR_materials_volume_scatter/multiscatterColorFactor", AnimatedProperty::MultiscatterColor, 3},
        {"extensions/KHR_materials_retroreflection/retroreflectionFactor", AnimatedProperty::Retroreflection, 1},
    };
    bool              valid    = FindProperty(path, properties, target);
    const std::string marker   = "/extensions/KHR_texture_transform/";
    const size_t      position = path.find(marker);
    if (!valid && position != std::string::npos)
    {
        static const char* textureNames[]{
            "pbrMetallicRoughness/baseColorTexture",
            "pbrMetallicRoughness/metallicRoughnessTexture",
            "normalTexture",
            "occlusionTexture",
            "emissiveTexture",
            "extensions/KHR_materials_specular/specularTexture",
            "extensions/KHR_materials_specular/specularColorTexture",
            "extensions/KHR_materials_pbrSpecularGlossiness/diffuseTexture",
            "extensions/KHR_materials_pbrSpecularGlossiness/specularGlossinessTexture",
            "extensions/KHR_materials_clearcoat/clearcoatTexture",
            "extensions/KHR_materials_clearcoat/clearcoatRoughnessTexture",
            "extensions/KHR_materials_clearcoat/clearcoatNormalTexture",
            "extensions/KHR_materials_sheen/sheenColorTexture",
            "extensions/KHR_materials_sheen/sheenRoughnessTexture",
            "extensions/KHR_materials_transmission/transmissionTexture",
            "extensions/KHR_materials_volume/thicknessTexture",
            "extensions/KHR_materials_iridescence/iridescenceTexture",
            "extensions/KHR_materials_iridescence/iridescenceThicknessTexture",
            "extensions/KHR_materials_anisotropy/anisotropyTexture",
            "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionTexture",
            "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorTexture",
            "extensions/KHR_materials_retroreflection/retroreflectionTexture",
        };
        const std::string texture = path.substr(0, position);
        for (uint32_t slot = 0; slot < std::size(textureNames); ++slot)
        {
            if (texture == textureNames[slot])
            {
                target.textureSlot = slot;
            }
        }
        static const PropertyName transformProperties[]{{"offset", AnimatedProperty::TextureOffset, 2},
                                                        {"scale", AnimatedProperty::TextureScale, 2},
                                                        {"rotation", AnimatedProperty::TextureRotation, 1}};
        valid = target.textureSlot != UINT32_MAX
             && FindProperty(path.substr(position + marker.size()), transformProperties, target);
    }
    return valid;
}

bool CompileNode(const SceneAsset& asset, const std::string& path, AnimationTarget& target)
{
    static const PropertyName properties[]{{"translation", AnimatedProperty::Translation, 3},
                                           {"rotation", AnimatedProperty::Rotation, 4},
                                           {"scale", AnimatedProperty::Scale, 3},
                                           {"extensions/KHR_node_visibility/visible", AnimatedProperty::Visible, 1},
                                           {"extensions/KHR_node_selectability/selectable", AnimatedProperty::Selectable, 1},
                                           {"extensions/KHR_node_hoverability/hoverable", AnimatedProperty::Hoverable, 1}};
    bool                      valid = FindProperty(path, properties, target);
    if (target.property == AnimatedProperty::Rotation || target.property == AnimatedProperty::Scale)
    {
        valid &= !asset.nodes[target.index].matrixTransform;
    }
    if (!valid && (path == "weights" || path.starts_with("weights/")))
    {
        target.property       = AnimatedProperty::Weights;
        const NodeAsset& node = asset.nodes[target.index];
        for (const MorphPrimitiveAsset& morph : asset.morphPrimitives)
        {
            if (morph.mesh == node.mesh.value)
            {
                target.dimensions = std::max(target.dimensions, static_cast<uint32_t>(morph.targets.size()));
            }
        }
        valid = target.dimensions > 0;
        if (path != "weights")
        {
            valid             &= ParseIndex(path.substr(8), target.element) && target.element < target.dimensions;
            target.dimensions  = 1;
        }
    }
    return valid;
}
} // namespace

bool CompileGltfAnimationTarget(const SceneAsset& asset, const std::string& pointer, AnimationTarget& target)
{
    target = AnimationTarget{};
    HeapVector<std::string> tokens;
    bool                    valid = SplitPointer(pointer, tokens) && tokens.size() >= 3;
    if (valid && (tokens[0] == "materials" || tokens[0] == "nodes" || tokens[0] == "cameras"))
    {
        valid                  = ParseIndex(tokens[1], target.index);
        const std::string path = RelativePath(tokens, 2);
        if (valid && tokens[0] == "materials")
        {
            target.object = AnimatedObject::Material;
            valid         = target.index < asset.materials.size() && CompileMaterial(path, target);
        }
        else if (valid && tokens[0] == "nodes")
        {
            target.object = AnimatedObject::Node;
            valid         = target.index < asset.nodes.size() && CompileNode(asset, path, target);
        }
        else if (valid)
        {
            target.object = AnimatedObject::Camera;
            static const PropertyName properties[]{{"perspective/aspectRatio", AnimatedProperty::CameraAspect, 1},
                                                   {"perspective/yfov", AnimatedProperty::CameraFov, 1},
                                                   {"perspective/znear", AnimatedProperty::CameraNear, 1},
                                                   {"perspective/zfar", AnimatedProperty::CameraFar, 1},
                                                   {"orthographic/xmag", AnimatedProperty::CameraXMag, 1},
                                                   {"orthographic/ymag", AnimatedProperty::CameraYMag, 1},
                                                   {"orthographic/znear", AnimatedProperty::CameraNear, 1},
                                                   {"orthographic/zfar", AnimatedProperty::CameraFar, 1}};
            valid = target.index < asset.cameras.size() && FindProperty(path, properties, target);
        }
    }
    else if (valid && tokens.size() == 5 && tokens[0] == "extensions" && tokens[1] == "EXT_lights_image_based"
             && tokens[2] == "lights")
    {
        target.object = AnimatedObject::Environment;
        static const PropertyName properties[]{{"intensity", AnimatedProperty::EnvironmentIntensity, 1},
                                               {"rotation", AnimatedProperty::EnvironmentRotation, 4}};
        valid = ParseIndex(tokens[3], target.index) && target.index < asset.imageBasedLights.size()
             && FindProperty(tokens[4], properties, target);
    }
    else if (valid && tokens.size() >= 5 && tokens[0] == "extensions" && tokens[1] == "KHR_lights_punctual"
             && tokens[2] == "lights")
    {
        target.object = AnimatedObject::Light;
        static const PropertyName properties[]{{"color", AnimatedProperty::LightColor, 3},
                                               {"intensity", AnimatedProperty::LightIntensity, 1},
                                               {"range", AnimatedProperty::LightRange, 1},
                                               {"spot/innerConeAngle", AnimatedProperty::LightInnerCone, 1},
                                               {"spot/outerConeAngle", AnimatedProperty::LightOuterCone, 1}};
        valid = ParseIndex(tokens[3], target.index) && target.index < asset.lights.size()
             && FindProperty(RelativePath(tokens, 4), properties, target);
    }
    else
    {
        valid = false;
    }
    if (!valid)
    {
        target = AnimationTarget{};
    }
    return valid;
}
} // namespace zen::asset
