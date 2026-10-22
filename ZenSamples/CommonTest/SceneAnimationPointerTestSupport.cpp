#include "SceneAnimationPointerTestSupport.h"
#include "AssetLib/GltfAnimationBindings.h"
#include "SceneGraph/Scene.h"
#include "SceneGraph/SceneAnimation.h"

namespace zen::sg
{
namespace
{
asset::SceneAsset TargetDefinitions(const Scene& scene)
{
    asset::SceneAsset                                           definitions;
    const SharedPtr<const SceneSourceInfo, MultiThreadCounter>& source = scene.GetAssetData().source;
    if (source)
    {
        definitions.nodes.resize(source->nodeMeshes.size());
        for (size_t index = 0; index < source->nodeMeshes.size(); ++index)
        {
            definitions.nodes[index].mesh            = source->nodeMeshes[index];
            definitions.nodes[index].matrixTransform = source->nodeMatrixTransforms[index] != 0;
            if (source->nodeMeshes[index].IsValid() && definitions.meshes.size() <= source->nodeMeshes[index].value)
            {
                definitions.meshes.resize(size_t(source->nodeMeshes[index].value) + 1);
            }
        }
        definitions.materials.resize(source->materials);
        definitions.cameras.resize(source->cameras);
        definitions.lights.resize(source->lights);
        definitions.morphPrimitives = scene.GetAssetData().morphPrimitives;
    }
    else
    {
        definitions.materials.resize(scene.GetComponents<Material>().size());
    }
    definitions.imageBasedLights = scene.GetAssetData().imageBasedLights;
    for (const UniquePtr<Node>& node : scene.GetNodes())
    {
        if (definitions.nodes.size() <= node->GetIndex())
        {
            definitions.nodes.resize(size_t(node->GetIndex()) + 1);
        }
        asset::NodeAsset& definition = definitions.nodes[node->GetIndex()];
        if (node->HasComponent<Transform>())
        {
            definition.matrixTransform |= node->GetComponent<Transform>()->GetBaseMatrix() != Mat4(1);
        }
        const uint32_t dimensions = GetNodeMorphTargetCount(scene, *node);
        if (dimensions > 0)
        {
            definition.mesh = asset::MeshId{static_cast<uint32_t>(definitions.meshes.size())};
            definitions.meshes.emplace_back();
            asset::MorphPrimitiveAsset morph;
            morph.mesh = definition.mesh.value;
            morph.targets.resize(dimensions);
            definitions.morphPrimitives.push_back(std::move(morph));
        }
    }
    for (const SceneCamera* camera : scene.GetComponents<SceneCamera>())
    {
        if (camera->sourceIndex != UINT32_MAX && definitions.cameras.size() <= camera->sourceIndex)
        {
            definitions.cameras.resize(size_t(camera->sourceIndex) + 1);
        }
    }
    for (const Light* light : scene.GetComponents<Light>())
    {
        if (light->sourceIndex != UINT32_MAX && definitions.lights.size() <= light->sourceIndex)
        {
            definitions.lights.resize(size_t(light->sourceIndex) + 1);
        }
    }
    return definitions;
}
} // namespace

bool ValidateAnimationPointer(Scene& scene, const std::string& pointer, VectorView<const float> values)
{
    const asset::SceneAsset definitions = TargetDefinitions(scene);
    asset::AnimationTarget  target;
    return asset::CompileGltfAnimationTarget(definitions, pointer, target) && ValidateAnimationTarget(scene, target, values);
}

bool ApplyAnimationPointer(Scene& scene, const std::string& pointer, VectorView<const float> values)
{
    const asset::SceneAsset definitions = TargetDefinitions(scene);
    asset::AnimationTarget  target;
    return asset::CompileGltfAnimationTarget(definitions, pointer, target) && ApplyAnimationTarget(scene, target, values);
}
} // namespace zen::sg
