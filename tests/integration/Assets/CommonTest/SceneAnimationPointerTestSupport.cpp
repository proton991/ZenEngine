#include "SceneAnimationPointerTestSupport.h"
#include "ZenAsset/Processor/AnimationTargets.h"
#include "AssetLib/ProcessedAnimationAdapter.h"
#include "SceneGraph/Scene.h"
#include "SceneGraph/SceneAnimation.h"

namespace zen::sg
{
namespace
{
zenasset::Scene TargetDefinitions(const Scene& scene)
{
    zenasset::Scene                                             definitions;
    const SharedPtr<const SceneSourceInfo, MultiThreadCounter>& source = scene.GetAssetData().source;
    if (source)
    {
        definitions.nodes.resize(source->nodeMeshes.size());
        for (size_t index = 0; index < source->nodeMeshes.size(); ++index)
        {
            definitions.nodes[index].mesh            = zenasset::MeshId{source->nodeMeshes[index].value};
            definitions.nodes[index].matrixTransform = source->nodeMatrixTransforms[index] != 0;
            if (source->nodeMeshes[index].IsValid() && definitions.meshes.size() <= source->nodeMeshes[index].value)
            {
                definitions.meshes.resize(size_t(source->nodeMeshes[index].value) + 1);
            }
        }
        definitions.materials.resize(source->materials);
        definitions.cameras.resize(source->cameras);
        definitions.lights.resize(source->lights);
        for (const asset::MorphPrimitiveAsset& sourceMorph : scene.GetAssetData().morphPrimitives)
        {
            zenasset::MorphPrimitiveAsset morph;
            morph.mesh = sourceMorph.mesh;
            morph.targets.resize(sourceMorph.targets.size());
            definitions.morphPrimitives.push_back(std::move(morph));
        }
    }
    else
    {
        definitions.materials.resize(scene.GetComponents<Material>().size());
    }
    definitions.imageBasedLights.resize(scene.GetAssetData().imageBasedLights.size());
    for (const UniquePtr<Node>& node : scene.GetNodes())
    {
        if (definitions.nodes.size() <= node->GetIndex())
        {
            definitions.nodes.resize(size_t(node->GetIndex()) + 1);
        }
        zenasset::NodeAsset& definition = definitions.nodes[node->GetIndex()];
        if (node->HasComponent<Transform>())
        {
            definition.matrixTransform |= node->GetComponent<Transform>()->GetBaseMatrix() != Mat4(1);
        }
        const uint32_t dimensions = GetNodeMorphTargetCount(scene, *node);
        if (dimensions > 0)
        {
            definition.mesh = zenasset::MeshId{static_cast<uint32_t>(definitions.meshes.size())};
            definitions.meshes.emplace_back();
            zenasset::MorphPrimitiveAsset morph;
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
    const zenasset::Scene     definitions = TargetDefinitions(scene);
    zenasset::AnimationTarget sourceTarget;
    asset::AnimationTarget    target;
    return zenasset::CompileGltfAnimationTarget(definitions, pointer, sourceTarget)
        && asset::AdaptProcessedAnimationTarget(sourceTarget, target) && ValidateAnimationTarget(scene, target, values);
}

bool ApplyAnimationPointer(Scene& scene, const std::string& pointer, VectorView<const float> values)
{
    const zenasset::Scene     definitions = TargetDefinitions(scene);
    zenasset::AnimationTarget sourceTarget;
    asset::AnimationTarget    target;
    return zenasset::CompileGltfAnimationTarget(definitions, pointer, sourceTarget)
        && asset::AdaptProcessedAnimationTarget(sourceTarget, target) && ApplyAnimationTarget(scene, target, values);
}
} // namespace zen::sg
