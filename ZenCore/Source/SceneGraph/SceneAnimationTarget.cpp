#include "SceneGraph/SceneAnimationTarget.h"
#include "SceneGraph/SceneAnimation.h"
#include "SceneGraph/Scene.h"
#include "AssetLib/AnimatedMaterial.h"
#include <algorithm>
#include <cmath>

namespace zen::sg
{
namespace
{
using asset::AnimatedObject;
using asset::AnimatedProperty;
using asset::AnimationTarget;

bool WriteValues(VectorView<float> destination, VectorView<const float> values, bool apply, float scale = 1.0f)
{
    bool valid = !destination.empty() && destination.size() == values.size();
    for (float value : values)
    {
        valid &= std::isfinite(value) && std::isfinite(value * scale);
    }
    if (valid && apply)
    {
        for (size_t index = 0; index < values.size(); ++index)
        {
            destination[index] = values[index] * scale;
        }
    }
    return valid;
}

Node* FindTargetNode(Scene& scene, uint32_t index)
{
    Node* result = nullptr;
    for (const UniquePtr<Node>& node : scene.GetNodes())
    {
        if (node->GetIndex() == index)
        {
            result = node.Get();
            break;
        }
    }
    return result;
}

bool SourceTargetExists(const Scene& scene, const AnimationTarget& target)
{
    bool                                                        exists = false;
    const SharedPtr<const SceneSourceInfo, MultiThreadCounter>& source = scene.GetAssetData().source;
    if (source)
    {
        switch (target.object)
        {
            case AnimatedObject::Node: exists = target.index < source->nodeMeshes.size(); break;
            case AnimatedObject::Material: exists = target.index < source->materials; break;
            case AnimatedObject::Camera: exists = target.index < source->cameras; break;
            case AnimatedObject::Light: exists = target.index < source->lights; break;
            case AnimatedObject::Environment: exists = target.index < source->imageBasedLights; break;
        }
    }
    return exists;
}

void RebuildTextureTransform(TextureTransform& transform)
{
    const float cosine = std::cos(transform.uvRotation);
    const float sine   = std::sin(transform.uvRotation);
    transform.row0 = Vec4(cosine * transform.uvScale.x, -sine * transform.uvScale.y, transform.uvOffset.x, transform.row0.w);
    transform.row1 = Vec4(sine * transform.uvScale.x, cosine * transform.uvScale.y, transform.uvOffset.y, transform.row1.w);
}

bool UpdateMaterial(Material& material, const AnimationTarget& target, VectorView<const float> values, bool apply)
{
    bool valid =
        target.property != AnimatedProperty::Ior || (values.size() == 1 && std::isfinite(values[0]) && values[0] >= 1.0f);
    const bool ignoreIor = target.property == AnimatedProperty::Ior && material.features.ior == 0.0f;
    if (valid
        && (target.property == AnimatedProperty::TextureOffset || target.property == AnimatedProperty::TextureScale
            || target.property == AnimatedProperty::TextureRotation))
    {
        TextureTransform* transform = nullptr;
        if (target.textureSlot < 5)
        {
            transform = &material.textureTransforms[target.textureSlot];
        }
        else if (target.textureSlot - 5 < MaterialFeatures::textureMembers.size())
        {
            transform = &(material.features.*MaterialFeatures::textureMembers[target.textureSlot - 5]).transform;
        }
        valid = transform != nullptr;
        if (valid)
        {
            VectorView<float> property =
                target.property == AnimatedProperty::TextureOffset  ? VectorView<float>(&transform->uvOffset[0], 2)
                : target.property == AnimatedProperty::TextureScale ? VectorView<float>(&transform->uvScale[0], 2)
                                                                    : VectorView<float>(&transform->uvRotation, 1);
            valid = WriteValues(property, values, apply);
            if (valid && apply)
            {
                RebuildTextureTransform(*transform);
            }
        }
    }
    else if (valid)
    {
        valid =
            WriteValues(asset::GetAnimatedMaterialProperty<Texture*>(material, target.property), values, apply && !ignoreIor);
    }
    if (valid && apply)
    {
        material.SetData();
    }
    return valid;
}

uint32_t TargetMorphDimensions(const Scene& scene, const Node* node, uint32_t index)
{
    uint32_t dimensions = node != nullptr ? GetNodeMorphTargetCount(scene, *node) : 0;
    const SharedPtr<const SceneSourceInfo, MultiThreadCounter>& source = scene.GetAssetData().source;
    if (node == nullptr && source && index < source->nodeMeshes.size())
    {
        for (const MorphPrimitiveAsset& morph : scene.GetAssetData().morphPrimitives)
        {
            if (morph.mesh == source->nodeMeshes[index].value)
            {
                dimensions = std::max(dimensions, static_cast<uint32_t>(morph.targets.size()));
            }
        }
    }
    return dimensions;
}

bool UpdateWeights(Scene& scene, Node* node, const AnimationTarget& target, VectorView<const float> values, bool apply)
{
    const uint32_t    dimensions = TargetMorphDimensions(scene, node, target.index);
    HeapVector<float> weights(dimensions, 0.0f);
    for (const DeformationPrimitiveAsset& deformation : scene.GetAssetData().deformations)
    {
        if (deformation.node == target.index && deformation.morphPrimitive >= 0
            && static_cast<size_t>(deformation.morphPrimitive) < scene.GetAssetData().morphPrimitives.size())
        {
            const HeapVector<float>& defaults = scene.GetAssetData().morphPrimitives[deformation.morphPrimitive].weights;
            for (size_t index = 0; index < std::min(weights.size(), defaults.size()); ++index)
            {
                weights[index] = defaults[index];
            }
            break;
        }
    }
    if (node != nullptr)
    {
        for (size_t index = 0; index < std::min(weights.size(), node->morphWeights.size()); ++index)
        {
            weights[index] = node->morphWeights[index];
        }
    }
    bool valid = target.element == UINT32_MAX || target.element < weights.size();
    if (valid)
    {
        const VectorView<float> property = target.element == UINT32_MAX ? VectorView<float>(weights.data(), weights.size())
                                                                        : VectorView<float>(weights.data() + target.element, 1);
        valid                            = WriteValues(property, values, apply);
    }
    if (valid && apply && node != nullptr)
    {
        node->morphWeights = std::move(weights);
    }
    return valid;
}

bool UpdateNode(Scene& scene, const AnimationTarget& target, VectorView<const float> values, bool apply)
{
    Node* node  = FindTargetNode(scene, target.index);
    bool  valid = node != nullptr || SourceTargetExists(scene, target);
    if (valid
        && (target.property == AnimatedProperty::Visible || target.property == AnimatedProperty::Selectable
            || target.property == AnimatedProperty::Hoverable))
    {
        valid = values.size() == 1;
        if (valid && apply && node != nullptr)
        {
            bool* field = target.property == AnimatedProperty::Visible    ? &node->visible
                        : target.property == AnimatedProperty::Selectable ? &node->selectable
                                                                          : &node->hoverable;
            *field      = values[0] != 0.0f;
        }
    }
    else if (valid && target.property == AnimatedProperty::Weights)
    {
        valid = UpdateWeights(scene, node, target, values, apply);
    }
    else if (valid)
    {
        Transform* transform   = node != nullptr && node->HasComponent<Transform>() ? node->GetComponent<Transform>() : nullptr;
        const Vec3 base        = transform != nullptr ? Vec3(transform->GetBaseMatrix()[3]) : Vec3(0);
        Vec3       translation = transform != nullptr ? transform->GetTranslation() + base : Vec3(0);
        Vec3       scale       = transform != nullptr ? transform->GetScale() : Vec3(1);
        const Quat rotation    = transform != nullptr ? transform->GetRotation() : Quat(1, 0, 0, 0);
        Vec4       quaternion(rotation.x, rotation.y, rotation.z, rotation.w);
        VectorView<float> property;
        switch (target.property)
        {
            case AnimatedProperty::Translation: property = VectorView<float>(&translation[0], 3); break;
            case AnimatedProperty::Scale: property = VectorView<float>(&scale[0], 3); break;
            case AnimatedProperty::Rotation: property = VectorView<float>(&quaternion[0], 4); break;
            default: break;
        }
        valid                                                              = WriteValues(property, values, true);
        const SharedPtr<const SceneSourceInfo, MultiThreadCounter>& source = scene.GetAssetData().source;
        if (valid && source && target.index < source->nodeMatrixTransforms.size()
            && (target.property == AnimatedProperty::Rotation || target.property == AnimatedProperty::Scale))
        {
            valid &= source->nodeMatrixTransforms[target.index] == 0;
        }
        if (valid && target.property == AnimatedProperty::Rotation)
        {
            const float squared = glm::dot(quaternion, quaternion);
            valid               = std::isfinite(squared) && squared > 1e-20f;
        }
        if (valid && apply && transform != nullptr)
        {
            transform->SetTranslation(translation - base);
            transform->SetScale(scale);
            transform->SetRotation(glm::normalize(Quat(quaternion.w, quaternion.x, quaternion.y, quaternion.z)));
        }
    }
    return valid;
}

bool UpdateCamera(SceneCamera& camera, const AnimationTarget& target, VectorView<const float> values, bool apply)
{
    float* property = nullptr;
    float  scale    = camera.unitScale;
    switch (target.property)
    {
        case AnimatedProperty::CameraAspect:
            property = &camera.aspect;
            scale    = 1;
            break;
        case AnimatedProperty::CameraFov:
            property = &camera.verticalFov;
            scale    = 1;
            break;
        case AnimatedProperty::CameraNear: property = &camera.nearPlane; break;
        case AnimatedProperty::CameraFar: property = &camera.farPlane; break;
        case AnimatedProperty::CameraXMag: property = &camera.xmag; break;
        case AnimatedProperty::CameraYMag: property = &camera.ymag; break;
        default: break;
    }
    const bool valid = property != nullptr && WriteValues(VectorView<float>(property, 1), values, apply, scale);
    if (valid && apply)
    {
        if (target.property == AnimatedProperty::CameraAspect)
        {
            camera.fixedAspect = true;
        }
        if (target.property == AnimatedProperty::CameraFar && !camera.orthographic)
        {
            camera.infiniteFar = false;
        }
    }
    return valid;
}

bool UpdateLight(Light& light, const AnimationTarget& target, VectorView<const float> values, bool apply)
{
    LightProperties   fields = light.GetProperties();
    VectorView<float> property;
    float             scale = 1;
    switch (target.property)
    {
        case AnimatedProperty::LightColor: property = VectorView<float>(&fields.color[0], 3); break;
        case AnimatedProperty::LightIntensity:
            property = VectorView<float>(&fields.intensity, 1);
            scale    = light.GetType() == Directional ? 1 : light.unitScale * light.unitScale;
            break;
        case AnimatedProperty::LightRange:
            property = VectorView<float>(&fields.range, 1);
            scale    = light.unitScale;
            break;
        case AnimatedProperty::LightInnerCone: property = VectorView<float>(&fields.innerConeAngle, 1); break;
        case AnimatedProperty::LightOuterCone: property = VectorView<float>(&fields.outerConeAngle, 1); break;
        default: break;
    }
    const bool valid = WriteValues(property, values, true, scale);
    if (valid && apply)
    {
        light.SetProperties(fields);
    }
    return valid;
}

bool ResolveAnimationTarget(Scene& scene, const AnimationTarget& target, VectorView<const float> values, bool apply)
{
    bool valid = target.dimensions > 0 && target.dimensions == values.size()
              && (target.property == AnimatedProperty::Weights || target.element == UINT32_MAX);
    for (float value : values)
    {
        valid &= std::isfinite(value);
    }
    if (valid)
    {
        switch (target.object)
        {
            case AnimatedObject::Material:
            {
                const HeapVector<Material*> materials = scene.GetComponents<Material>();
                valid = target.index < materials.size() && (!scene.GetAssetData().source || SourceTargetExists(scene, target))
                     && UpdateMaterial(*materials[target.index], target, values, apply);
                break;
            }
            case AnimatedObject::Node: valid = UpdateNode(scene, target, values, apply); break;
            case AnimatedObject::Camera:
            {
                bool found = false;
                for (SceneCamera* camera : scene.GetComponents<SceneCamera>())
                {
                    if (camera->sourceIndex == target.index)
                    {
                        found  = true;
                        valid &= UpdateCamera(*camera, target, values, apply);
                    }
                }
                if (!found)
                {
                    SceneCamera inactive("InactiveCamera");
                    valid = SourceTargetExists(scene, target) && UpdateCamera(inactive, target, values, false);
                }
                break;
            }
            case AnimatedObject::Light:
            {
                bool found = false;
                for (Light* light : scene.GetComponents<Light>())
                {
                    if (light->sourceIndex == target.index)
                    {
                        found  = true;
                        valid &= UpdateLight(*light, target, values, apply);
                    }
                }
                if (!found)
                {
                    Light inactive("InactiveLight");
                    valid = SourceTargetExists(scene, target) && UpdateLight(inactive, target, values, false);
                }
                break;
            }
            case AnimatedObject::Environment:
            {
                valid = target.index < scene.GetAssetData().imageBasedLights.size();
                if (valid && target.property == AnimatedProperty::EnvironmentIntensity)
                {
                    valid = values.size() == 1 && values[0] >= 0;
                    if (valid && apply)
                    {
                        scene.GetAssetData().imageBasedLights[target.index].intensity = values[0];
                    }
                }
                else if (valid && target.property == AnimatedProperty::EnvironmentRotation)
                {
                    valid = values.size() == 4;
                    Vec4 quaternion(0, 0, 0, 1);
                    if (valid)
                    {
                        quaternion          = Vec4(values[0], values[1], values[2], values[3]);
                        const float squared = glm::dot(quaternion, quaternion);
                        valid               = std::isfinite(squared) && squared > 1e-20f;
                    }
                    if (valid && apply)
                    {
                        scene.GetAssetData().imageBasedLights[target.index].rotation =
                            glm::normalize(Quat(quaternion.w, quaternion.x, quaternion.y, quaternion.z));
                    }
                }
                else
                {
                    valid = false;
                }
                break;
            }
            default: valid = false; break;
        }
    }
    return valid;
}
} // namespace

bool ValidateAnimationTarget(Scene& scene, const asset::AnimationTarget& target, VectorView<const float> values)
{
    return ResolveAnimationTarget(scene, target, values, false);
}

bool ApplyAnimationTarget(Scene& scene, const asset::AnimationTarget& target, VectorView<const float> values)
{
    return ResolveAnimationTarget(scene, target, values, false) && ResolveAnimationTarget(scene, target, values, true);
}

bool ApplyValidatedAnimationTarget(Scene& scene, const asset::AnimationTarget& target, VectorView<const float> values)
{
    return ResolveAnimationTarget(scene, target, values, true);
}
} // namespace zen::sg
