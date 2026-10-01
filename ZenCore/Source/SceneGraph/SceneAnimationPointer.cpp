#include "SceneGraph/SceneAnimationPointer.h"
#include "SceneGraph/SceneAnimation.h"
#include "SceneGraph/Scene.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <simdjson.h>

namespace zen::sg
{
namespace
{
struct NumericProperty
{
    const char* name;
    float* values;
    uint32_t dimensions;
};

struct TextureProperty
{
    const char* name;
    MaterialTextureBinding* binding;
};

bool ParseIndex(const std::string& token, uint32_t& index)
{
    const std::from_chars_result parsed =
        std::from_chars(token.data(), token.data() + token.size(), index);

    const bool valid = !token.empty() && (token.size() == 1 || token.front() != '0') &&
        parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();

    return valid;
}

bool SplitPointer(const std::string& pointer, HeapVector<std::string>& tokens)
{
    bool valid = !pointer.empty() && pointer.front() == '/';

    size_t begin = 1;

    while (valid && begin <= pointer.size())
    {
        const size_t end = pointer.find('/', begin);

        const std::string encoded = pointer.substr(begin, end - begin);

        std::string decoded;

        for (size_t index = 0; valid && index < encoded.size(); ++index)
        {
            if (encoded[index] == '~')
            {
                valid = index + 1 < encoded.size() &&
                    (encoded[index + 1] == '0' || encoded[index + 1] == '1');

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

bool NumericUpdate(const NumericProperty& property,
                   const std::string& path,
                   VectorView<const float> values,
                   bool apply,
                   float scale        = 1.0f,
                   bool arrayElements = false)
{
    uint32_t first = 0;

    uint32_t dimensions = property.dimensions;

    bool valid = path == property.name;

    const std::string componentPrefix = std::string(property.name) + '/';

    if (!valid && arrayElements && path.starts_with(componentPrefix))
    {
        valid = ParseIndex(path.substr(componentPrefix.size()), first) && first < dimensions;

        dimensions = 1;
    }

    valid &= values.size() == dimensions;

    for (float value : values)
    {
        valid &= std::isfinite(value) && std::isfinite(value * scale);
    }

    if (valid && apply && property.values != nullptr)
    {
        for (uint32_t index = 0; index < dimensions; ++index)
        {
            property.values[first + index] = values[index] * scale;
        }
    }

    return valid;
}

bool SourceObjectExists(const Scene& scene, const char* collection, uint32_t index)
{
    bool exists = false;

    if (!scene.GetAssetData().sourceDocument.empty())
    {
        simdjson::dom::parser parser;

        const simdjson::padded_string json(scene.GetAssetData().sourceDocument);

        simdjson::dom::element source;

        if (parser.parse(json).get(source) == simdjson::SUCCESS)
        {
            simdjson::dom::array objects;

            exists = source.at_pointer(collection).get(objects) == simdjson::SUCCESS &&
                index < objects.size();
        }
    }

    return exists;
}

bool SourcePropertyExists(const Scene& scene, const std::string& pointer)
{
    bool exists = false;

    if (!scene.GetAssetData().sourceDocument.empty())
    {
        simdjson::dom::parser parser;

        const simdjson::padded_string json(scene.GetAssetData().sourceDocument);

        simdjson::dom::element source;

        if (parser.parse(json).get(source) == simdjson::SUCCESS)
        {
            simdjson::dom::element property;

            exists = source.at_pointer(pointer).get(property) == simdjson::SUCCESS;
        }
    }

    return exists;
}

void RebuildTextureTransform(TextureTransform& transform)
{
    const float cosine = std::cos(transform.uvRotation);

    const float sine = std::sin(transform.uvRotation);

    const float sampler = transform.row0.w;

    transform.row0 = Vec4(cosine * transform.uvScale.x, -sine * transform.uvScale.y,
                          transform.uvOffset.x, sampler);

    transform.row1 = Vec4(sine * transform.uvScale.x, cosine * transform.uvScale.y,
                          transform.uvOffset.y, transform.row1.w);
}

TextureTransform* FindTextureTransform(Material& material, const std::string& texture)
{
    TextureTransform* result = nullptr;

    const char* coreNames[] = {"pbrMetallicRoughness/baseColorTexture",
                               "pbrMetallicRoughness/metallicRoughnessTexture", "normalTexture",
                               "occlusionTexture", "emissiveTexture"};

    for (uint32_t index = 0; index < 5; ++index)
    {
        if (texture == coreNames[index])
        {
            result = &material.textureTransforms[index];
        }
    }

    const TextureProperty features[] = {
        {"extensions/KHR_materials_specular/specularTexture", &material.features.specularTexture},
        {"extensions/KHR_materials_specular/specularColorTexture",
         &material.features.specularColorTexture},
        {"extensions/KHR_materials_pbrSpecularGlossiness/diffuseTexture",
         &material.features.diffuseTexture},
        {"extensions/KHR_materials_pbrSpecularGlossiness/specularGlossinessTexture",
         &material.features.specularGlossinessTexture},
        {"extensions/KHR_materials_clearcoat/clearcoatTexture",
         &material.features.clearcoatTexture},
        {"extensions/KHR_materials_clearcoat/clearcoatRoughnessTexture",
         &material.features.clearcoatRoughnessTexture},
        {"extensions/KHR_materials_clearcoat/clearcoatNormalTexture",
         &material.features.clearcoatNormalTexture},
        {"extensions/KHR_materials_sheen/sheenColorTexture", &material.features.sheenColorTexture},
        {"extensions/KHR_materials_sheen/sheenRoughnessTexture",
         &material.features.sheenRoughnessTexture},
        {"extensions/KHR_materials_transmission/transmissionTexture",
         &material.features.transmissionTexture},
        {"extensions/KHR_materials_volume/thicknessTexture", &material.features.thicknessTexture},
        {"extensions/KHR_materials_iridescence/iridescenceTexture",
         &material.features.iridescenceTexture},
        {"extensions/KHR_materials_iridescence/iridescenceThicknessTexture",
         &material.features.iridescenceThicknessTexture},
        {"extensions/KHR_materials_anisotropy/anisotropyTexture",
         &material.features.anisotropyTexture},
        {"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionTexture",
         &material.features.diffuseTransmissionTexture},
        {"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorTexture",
         &material.features.diffuseTransmissionColorTexture},
        {"extensions/KHR_materials_retroreflection/retroreflectionTexture",
         &material.features.retroreflectionTexture}};

    for (const TextureProperty& feature : features)
    {
        if (texture == feature.name)
        {
            result = &feature.binding->transform;
        }
    }

    return result;
}

bool UpdateMaterial(Material& material,
                    const std::string& path,
                    VectorView<const float> values,
                    bool apply)
{
    const bool iorProperty = path == "extensions/KHR_materials_ior/ior";

    const bool iorValid =
        !iorProperty || (values.size() == 1 && std::isfinite(values[0]) && values[0] >= 1.0f);

    // Authored zero permanently denotes infinite IOR; valid animation updates are ignored.
    const bool ignoreIor = iorProperty && material.features.ior == 0.0f;

    const NumericProperty properties[] = {
        {"pbrMetallicRoughness/baseColorFactor", &material.baseColorFactor[0], 4},
        {"pbrMetallicRoughness/metallicFactor", &material.metallicFactor, 1},
        {"pbrMetallicRoughness/roughnessFactor", &material.roughnessFactor, 1},
        {"emissiveFactor", &material.emissiveFactor[0], 3},
        {"alphaCutoff", &material.alphaCutoff, 1},
        {"normalTexture/scale", &material.normalScale, 1},
        {"occlusionTexture/strength", &material.occlusionStrength, 1},
        {"extensions/KHR_materials_emissive_strength/emissiveStrength", &material.emissiveStrength,
         1},
        {"extensions/KHR_materials_ior/ior", &material.features.ior, 1},
        {"extensions/KHR_materials_dispersion/dispersion", &material.features.dispersion, 1},
        {"extensions/KHR_materials_specular/specularFactor", &material.features.specular, 1},
        {"extensions/KHR_materials_specular/specularColorFactor",
         &material.features.specularColor[0], 3},
        {"extensions/KHR_materials_pbrSpecularGlossiness/diffuseFactor",
         &material.extension.diffuseFactor[0], 4},
        {"extensions/KHR_materials_pbrSpecularGlossiness/specularFactor",
         &material.extension.specularFactor[0], 3},
        {"extensions/KHR_materials_pbrSpecularGlossiness/glossinessFactor",
         &material.features.glossiness, 1},
        {"extensions/KHR_materials_clearcoat/clearcoatFactor", &material.features.clearcoat, 1},
        {"extensions/KHR_materials_clearcoat/clearcoatRoughnessFactor",
         &material.features.clearcoatRoughness, 1},
        {"extensions/KHR_materials_clearcoat/clearcoatNormalTexture/scale",
         &material.features.clearcoatNormalTexture.scale, 1},
        {"extensions/KHR_materials_sheen/sheenColorFactor", &material.features.sheenColor[0], 3},
        {"extensions/KHR_materials_sheen/sheenRoughnessFactor", &material.features.sheenRoughness,
         1},
        {"extensions/KHR_materials_transmission/transmissionFactor",
         &material.features.transmission, 1},
        {"extensions/KHR_materials_volume/thicknessFactor", &material.features.thickness, 1},
        {"extensions/KHR_materials_volume/attenuationDistance",
         &material.features.attenuationDistance, 1},
        {"extensions/KHR_materials_volume/attenuationColor", &material.features.attenuationColor[0],
         3},
        {"extensions/KHR_materials_iridescence/iridescenceFactor", &material.features.iridescence,
         1},
        {"extensions/KHR_materials_iridescence/iridescenceIor", &material.features.iridescenceIor,
         1},
        {"extensions/KHR_materials_iridescence/iridescenceThicknessMinimum",
         &material.features.iridescenceThicknessMin, 1},
        {"extensions/KHR_materials_iridescence/iridescenceThicknessMaximum",
         &material.features.iridescenceThicknessMax, 1},
        {"extensions/KHR_materials_anisotropy/anisotropyStrength", &material.features.anisotropy,
         1},
        {"extensions/KHR_materials_anisotropy/anisotropyRotation",
         &material.features.anisotropyRotation, 1},
        {"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionFactor",
         &material.features.diffuseTransmission, 1},
        {"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor",
         &material.features.diffuseTransmissionColor[0], 3},
        {"extensions/KHR_materials_volume_scatter/multiscatterColorFactor",
         &material.features.multiscatterColor[0], 3},
        {"extensions/KHR_materials_retroreflection/retroreflectionFactor",
         &material.features.retroreflection, 1}};

    bool valid = false;

    for (const NumericProperty& property : properties)
    {
        if (NumericUpdate(property, path, values, apply && iorValid && !ignoreIor))
        {
            valid = true;

            break;
        }
    }

    const std::string transformMarker = "/extensions/KHR_texture_transform/";

    const size_t marker = path.find(transformMarker);

    if (!valid && marker != std::string::npos)
    {
        TextureTransform* transform = FindTextureTransform(material, path.substr(0, marker));

        if (transform != nullptr)
        {
            const std::string propertyPath = path.substr(marker + transformMarker.size());

            const NumericProperty transformProperties[] = {{"offset", &transform->uvOffset[0], 2},
                                                           {"scale", &transform->uvScale[0], 2},
                                                           {"rotation", &transform->uvRotation, 1}};

            for (const NumericProperty& property : transformProperties)
            {
                if (NumericUpdate(property, propertyPath, values, apply))
                {
                    valid = true;

                    break;
                }
            }

            if (valid && apply)
            {
                RebuildTextureTransform(*transform);
            }
        }
    }

    valid &= iorValid;

    if (valid && apply)
    {
        material.SetData();
    }

    return valid;
}

Node* FindNode(Scene& scene, uint32_t sourceIndex)
{
    Node* result = nullptr;

    for (const UniquePtr<Node>& node : scene.GetNodes())
    {
        if (node->GetIndex() == sourceIndex)
        {
            result = node.Get();

            break;
        }
    }

    return result;
}

uint32_t MorphDimensions(const Scene& scene, const Node* node)
{
    const uint32_t dimensions = node != nullptr ? GetNodeMorphTargetCount(scene, *node) : 0;

    return dimensions;
}

bool UpdateNode(Scene& scene,
                uint32_t sourceIndex,
                const std::string& path,
                VectorView<const float> values,
                bool apply)
{
    Node* node = FindNode(scene, sourceIndex);

    bool valid = node != nullptr || SourceObjectExists(scene, "/nodes", sourceIndex);

    if (valid &&
        (path == "extensions/KHR_node_visibility/visible" ||
         path == "extensions/KHR_node_selectability/selectable" ||
         path == "extensions/KHR_node_hoverability/hoverable"))
    {
        valid = values.size() == 1 && std::isfinite(values[0]);

        if (valid && apply && node != nullptr)
        {
            if (path == "extensions/KHR_node_visibility/visible")
            {
                node->visible = values[0] != 0.0f;
            }
            else if (path == "extensions/KHR_node_selectability/selectable")
            {
                node->selectable = values[0] != 0.0f;
            }
            else
            {
                node->hoverable = values[0] != 0.0f;
            }
        }
    }
    else if (valid && (path == "weights" || path.starts_with("weights/")))
    {
        const uint32_t dimensions = MorphDimensions(scene, node);

        HeapVector<float> weights(dimensions, 0.0f);

        for (const DeformationPrimitiveAsset& deformation : scene.GetAssetData().deformations)
        {
            if (deformation.node == sourceIndex && deformation.morphPrimitive >= 0 &&
                static_cast<size_t>(deformation.morphPrimitive) <
                    scene.GetAssetData().morphPrimitives.size())
            {
                const HeapVector<float>& defaults =
                    scene.GetAssetData().morphPrimitives[deformation.morphPrimitive].weights;

                for (size_t index = 0; index < std::min(weights.size(), defaults.size()); ++index)
                {
                    weights[index] = defaults[index];
                }

                break;
            }
        }

        if (node != nullptr)
        {
            for (size_t index = 0; index < std::min(weights.size(), node->morphWeights.size());
                 ++index)
            {
                weights[index] = node->morphWeights[index];
            }
        }

        valid = node == nullptr ||
            NumericUpdate({"weights", weights.data(), dimensions}, path, values, apply, 1.0f, true);

        if (valid && apply && node != nullptr)
        {
            node->morphWeights = std::move(weights);
        }
    }
    else if (valid)
    {
        Transform* transform = node != nullptr && node->HasComponent<Transform>() ?
            node->GetComponent<Transform>() :
            nullptr;

        const Vec3 baseTranslation =
            transform != nullptr ? Vec3(transform->GetBaseMatrix()[3]) : Vec3(0);

        Vec3 translation =
            transform != nullptr ? transform->GetTranslation() + baseTranslation : Vec3(0);

        Vec3 scale = transform != nullptr ? transform->GetScale() : Vec3(1);

        const Quat rotation = transform != nullptr ? transform->GetRotation() : Quat(1, 0, 0, 0);

        Vec4 quaternion(rotation.x, rotation.y, rotation.z, rotation.w);

        const NumericProperty properties[] = {{"translation", &translation[0], 3},
                                              {"scale", &scale[0], 3},
                                              {"rotation", &quaternion[0], 4}};

        valid = false;

        for (const NumericProperty& property : properties)
        {
            if (NumericUpdate(property, path, values, true))
            {
                valid = true;

                break;
            }
        }

        if (valid && (path == "rotation" || path == "scale"))
        {
            valid =
                !SourcePropertyExists(scene, "/nodes/" + std::to_string(sourceIndex) + "/matrix");
        }

        if (valid && path.starts_with("rotation"))
        {
            const float squared = glm::dot(quaternion, quaternion);

            valid = std::isfinite(squared) && squared > 1e-20f;
        }

        if (valid && apply && transform != nullptr)
        {
            transform->SetTranslation(translation - baseTranslation);

            transform->SetScale(scale);

            transform->SetRotation(
                glm::normalize(Quat(quaternion.w, quaternion.x, quaternion.y, quaternion.z)));
        }
    }

    return valid;
}

bool UpdateCamera(SceneCamera& camera,
                  const std::string& path,
                  VectorView<const float> values,
                  bool apply)
{
    const NumericProperty properties[] = {{"perspective/aspectRatio", &camera.aspect, 1},
                                          {"perspective/yfov", &camera.verticalFov, 1},
                                          {"perspective/znear", &camera.nearPlane, 1},
                                          {"perspective/zfar", &camera.farPlane, 1},
                                          {"orthographic/xmag", &camera.xmag, 1},
                                          {"orthographic/ymag", &camera.ymag, 1},
                                          {"orthographic/znear", &camera.nearPlane, 1},
                                          {"orthographic/zfar", &camera.farPlane, 1}};

    bool valid = false;

    for (const NumericProperty& property : properties)
    {
        const bool scaled = std::string(property.name).ends_with("znear") ||
            std::string(property.name).ends_with("zfar") ||
            std::string(property.name).ends_with("xmag") ||
            std::string(property.name).ends_with("ymag");

        if (NumericUpdate(property, path, values, apply, scaled ? camera.unitScale : 1.0f))
        {
            valid = true;

            if (apply && path == "perspective/aspectRatio")
            {
                camera.fixedAspect = true;
            }

            if (apply && path == "perspective/zfar")
            {
                camera.infiniteFar = false;
            }

            break;
        }
    }

    return valid;
}

bool UpdateLight(Light& light, const std::string& path, VectorView<const float> values, bool apply)
{
    LightProperties properties = light.GetProperties();

    const NumericProperty fields[] = {{"color", &properties.color[0], 3},
                                      {"intensity", &properties.intensity, 1},
                                      {"range", &properties.range, 1},
                                      {"spot/innerConeAngle", &properties.innerConeAngle, 1},
                                      {"spot/outerConeAngle", &properties.outerConeAngle, 1}};

    bool valid = false;

    for (const NumericProperty& field : fields)
    {
        const float scale = path == "range" ? light.unitScale :
            path == "intensity" && light.GetType() != Directional ?
                                              light.unitScale * light.unitScale :
                                              1.0f;

        if (NumericUpdate(field, path, values, true, scale))
        {
            valid = true;

            break;
        }
    }

    if (valid && apply)
    {
        light.SetProperties(properties);
    }

    return valid;
}

bool ResolveAnimationPointer(Scene& scene,
                             const std::string& pointer,
                             VectorView<const float> values,
                             bool apply)
{
    HeapVector<std::string> tokens;

    bool valid = SplitPointer(pointer, tokens) && tokens.size() >= 3;

    for (float value : values)
    {
        valid &= std::isfinite(value);
    }

    uint32_t sourceIndex = 0;

    if (valid && (tokens[0] == "materials" || tokens[0] == "nodes" || tokens[0] == "cameras"))
    {
        valid = ParseIndex(tokens[1], sourceIndex);

        const std::string path = RelativePath(tokens, 2);

        if (valid && tokens[0] == "materials")
        {
            const std::vector<Material*> materials = scene.GetComponents<Material>();

            const size_t count =
                !scene.GetAssetData().sourceDocument.empty() && !materials.empty() ?
                materials.size() - 1 :
                materials.size();

            valid =
                sourceIndex < count && UpdateMaterial(*materials[sourceIndex], path, values, apply);
        }
        else if (valid && tokens[0] == "nodes")
        {
            valid = UpdateNode(scene, sourceIndex, path, values, apply);
        }
        else if (valid)
        {
            const std::vector<SceneCamera*> cameras = scene.GetComponents<SceneCamera>();

            bool found = false;

            for (SceneCamera* camera : cameras)
            {
                if (camera->sourceIndex == sourceIndex)
                {
                    found = true;

                    valid &= UpdateCamera(*camera, path, values, apply);
                }
            }

            if (!found)
            {
                SceneCamera camera("InactiveCamera");

                valid = SourceObjectExists(scene, "/cameras", sourceIndex) &&
                    UpdateCamera(camera, path, values, false);
            }
        }
    }
    else if (valid && tokens.size() == 5 && tokens[0] == "extensions" &&
             tokens[1] == "EXT_lights_image_based" && tokens[2] == "lights")
    {
        valid = ParseIndex(tokens[3], sourceIndex) &&
            sourceIndex < scene.GetAssetData().imageBasedLights.size();

        if (valid)
        {
            ImageBasedLightAsset& light = scene.GetAssetData().imageBasedLights[sourceIndex];

            if (tokens[4] == "intensity")
            {
                valid = values.size() == 1 && values[0] >= 0.0f;

                if (valid && apply)
                {
                    light.intensity = values[0];
                }
            }
            else if (tokens[4] == "rotation")
            {
                valid = values.size() == 4;

                Vec4 quaternion(0, 0, 0, 1);

                if (valid)
                {
                    quaternion = Vec4(values[0], values[1], values[2], values[3]);

                    const float squared = glm::dot(quaternion, quaternion);

                    valid = std::isfinite(squared) && squared > 1e-20f;
                }

                if (valid && apply)
                {
                    light.rotation = glm::normalize(
                        Quat(quaternion.w, quaternion.x, quaternion.y, quaternion.z));
                }
            }
            else
            {
                valid = false;
            }
        }
    }
    else if (valid && tokens.size() >= 5 && tokens[0] == "extensions" &&
             tokens[1] == "KHR_lights_punctual" && tokens[2] == "lights")
    {
        valid = ParseIndex(tokens[3], sourceIndex);

        const std::string path = RelativePath(tokens, 4);

        const std::vector<Light*> lights = scene.GetComponents<Light>();

        bool found = false;

        for (Light* light : lights)
        {
            if (valid && light->sourceIndex == sourceIndex)
            {
                found = true;

                valid &= UpdateLight(*light, path, values, apply);
            }
        }

        if (valid && !found)
        {
            Light light("InactiveLight");

            valid =
                SourceObjectExists(scene, "/extensions/KHR_lights_punctual/lights", sourceIndex) &&
                UpdateLight(light, path, values, false);
        }
    }
    else
    {
        valid = false;
    }

    return valid;
}
} // namespace

bool ValidateAnimationPointer(Scene& scene,
                              const std::string& pointer,
                              VectorView<const float> values)
{
    const bool valid = ResolveAnimationPointer(scene, pointer, values, false);

    return valid;
}

bool ApplyAnimationPointer(Scene& scene, const std::string& pointer, VectorView<const float> values)
{
    const bool applied = ResolveAnimationPointer(scene, pointer, values, true);

    return applied;
}
} // namespace zen::sg
