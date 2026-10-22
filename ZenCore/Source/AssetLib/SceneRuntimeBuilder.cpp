#include "AssetLib/SceneRuntimeBuilderInternal.h"
#include "SceneGraph/SceneAnimation.h"
#include "SceneGraph/Transform.h"
#include "AssetLib/SurfaceGeometry.h"
#include "Systems/SceneEditor.h"
#include <algorithm>
#include <cmath>

namespace zen::asset
{
namespace
{
struct ResolveTextureReference
{
    const HeapVector<sg::Texture*>& textures;

    sg::Texture* operator()(TextureId id) const
    {
        return id.IsValid() ? textures[id.value] : nullptr;
    }
};

void ResolveMaterialSamplers(sg::Material& material)
{
    const sg::Texture* core[] = {material.m_pBaseColorTexture, material.m_pMetallicRoughnessTexture, material.m_pNormalTexture,
                                 material.m_pOcclusionTexture, material.m_pEmissiveTexture};
    for (size_t index = 0; index < 5; ++index)
    {
        material.textureTransforms[index].row0.w = static_cast<float>(core[index]->samplerIndex + 1);
    }
    for (sg::MaterialFeatures::TextureMember member : sg::MaterialFeatures::textureMembers)
    {
        sg::MaterialTextureBinding& binding = material.features.*member;
        binding.transform.row0.w = binding.texture != nullptr ? static_cast<float>(binding.texture->samplerIndex + 1) : 0.0f;
    }
}

void BuildImagesAndMaterials(const SceneAsset& asset, sg::Scene& scene)
{
    for (const SamplerAsset& source : asset.samplers)
    {
        UniquePtr<sg::Sampler> sampler            = MakeUnique<sg::Sampler>(source.name);
        static_cast<SamplerParameters&>(*sampler) = source;
        scene.AddComponent(std::move(sampler));
    }
    for (size_t index = 0; index < asset.textures.size(); ++index)
    {
        const TextureAsset&    source  = asset.textures[index];
        UniquePtr<sg::Texture> texture = MakeUnique<sg::Texture>(source.name);
        texture->index                 = static_cast<uint32_t>(index);
        texture->linearSourceIndex     = source.linearSource.value;
        texture->samplerIndex          = source.sampler.IsValid() ? static_cast<int32_t>(source.sampler.value) : -1;
        texture->pixels                = asset.images[source.image.value].pixels;
        texture->width                 = texture->pixels->width;
        texture->height                = texture->pixels->height;
        texture->format                = source.format;
        texture->flatNormal            = source.flatNormal;
        scene.AddComponent(std::move(texture));
    }
    const HeapVector<sg::Texture*> textures = scene.GetComponents<sg::Texture>();
    scene.LoadDefaultTextures(static_cast<uint32_t>(textures.size()));
    const sg::Scene::DefaultTextures defaults = scene.GetDefaultTextures();
    scene.AddComponent(UniquePtr<sg::Texture>(defaults.pBaseColor));
    scene.AddComponent(UniquePtr<sg::Texture>(defaults.pMetallicRoughness));
    scene.AddComponent(UniquePtr<sg::Texture>(defaults.pNormal));
    scene.AddComponent(UniquePtr<sg::Texture>(defaults.pEmissive));
    scene.AddComponent(UniquePtr<sg::Texture>(defaults.pOcclusion));
    for (size_t index = 0; index <= asset.materials.size(); ++index)
    {
        UniquePtr<sg::Material> material =
            MakeUnique<sg::Material>(index < asset.materials.size() ? asset.materials[index].name : "DefaultMaterial");
        if (index < asset.materials.size())
        {
            CopyMaterialParameters<sg::Texture*, TextureId>(*material, asset.materials[index],
                                                            ResolveTextureReference{textures});
        }
        material->index = static_cast<uint32_t>(index);
        if (material->m_pBaseColorTexture == nullptr)
        {
            material->m_pBaseColorTexture = defaults.pBaseColor;
        }
        if (material->m_pMetallicRoughnessTexture == nullptr)
        {
            material->m_pMetallicRoughnessTexture = defaults.pMetallicRoughness;
        }
        if (material->m_pNormalTexture == nullptr)
        {
            material->m_pNormalTexture = defaults.pNormal;
        }
        if (material->m_pEmissiveTexture == nullptr)
        {
            material->m_pEmissiveTexture = defaults.pEmissive;
        }
        if (material->m_pOcclusionTexture == nullptr)
        {
            material->m_pOcclusionTexture = defaults.pOcclusion;
        }
        ResolveMaterialSamplers(*material);
        material->SetData();
        scene.AddComponent(std::move(material));
    }
}

void BuildMeshes(const SceneAsset& asset, sg::Scene& scene)
{
    const HeapVector<sg::Material*> materials = scene.GetComponents<sg::Material>();
    for (uint32_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex)
    {
        const MeshAsset&    source = asset.meshes[meshIndex];
        UniquePtr<sg::Mesh> mesh   = MakeUnique<sg::Mesh>(source.name);
        for (uint32_t primitiveIndex = 0; primitiveIndex < source.primitives.size(); ++primitiveIndex)
        {
            const MeshPrimitiveAsset& primitive = source.primitives[primitiveIndex];
            if (primitive.vertexCount > 0)
            {
                UniquePtr<sg::SubMesh> subMesh =
                    MakeUnique<sg::SubMesh>(primitive.name, primitive.firstIndex, primitive.indexCount, primitive.vertexCount);
                const uint32_t material =
                    primitive.material.IsValid() ? primitive.material.value : static_cast<uint32_t>(asset.materials.size());
                subMesh->SetMaterial(material, materials[material]);
                subMesh->assetMesh      = meshIndex;
                subMesh->assetPrimitive = primitiveIndex;
                subMesh->topology       = primitive.topology;
                subMesh->SetBounds(primitive.bounds);
                mesh->SetAABB(primitive.bounds.GetMin(), primitive.bounds.GetMax());
                mesh->AddSubMesh(subMesh.Get());
                scene.AddComponent(std::move(subMesh));
            }
        }
        scene.AddComponent(std::move(mesh));
    }
}

bool HasDeformation(const SceneAsset& asset, const NodeAsset& node)
{
    bool result = node.skin.IsValid();
    for (const MorphPrimitiveAsset& morph : asset.morphPrimitives)
    {
        result |= morph.mesh == node.mesh.value && !morph.targets.empty();
    }
    return result;
}

void BuildInstanceMesh(const SceneAsset& asset,
                       const NodeAsset&  sourceNode,
                       sg::Node&         node,
                       sg::Mesh&         original,
                       LoadedScene&      loaded,
                       bool              deform)
{
    sg::Scene&          scene = *loaded.scene;
    sg::SceneAssetData& data  = scene.GetAssetData();
    UniquePtr<sg::Mesh> mesh  = MakeUnique<sg::Mesh>(original.GetName() + "_instance_" + std::to_string(node.GetIndex()));
    size_t              subMeshIndex          = 0;
    uint32_t            componentCount        = static_cast<uint32_t>(scene.GetComponents<sg::SubMesh>().size());
    const MeshAsset&    sourceMesh            = asset.meshes[sourceNode.mesh.value];
    const HeapVector<sg::Material*> materials = scene.GetComponents<sg::Material>();
    for (uint32_t primitiveIndex = 0; primitiveIndex < sourceMesh.primitives.size(); ++primitiveIndex)
    {
        const MeshPrimitiveAsset& primitive = sourceMesh.primitives[primitiveIndex];
        if (primitive.vertexCount > 0)
        {
            const sg::SubMesh&        source = *original.GetSubMeshes()[subMeshIndex++];
            DeformationPrimitiveAsset deformation;
            if (deform)
            {
                deformation.node           = node.GetIndex();
                deformation.firstVertex    = static_cast<uint32_t>(data.bindVertices.size());
                deformation.vertexCount    = primitive.vertexCount;
                deformation.subMesh        = componentCount++;
                deformation.flatNormals    = primitive.flatNormals;
                deformation.sourceVertices = primitive.sourceVertices;
                if (deformation.sourceVertices.empty())
                {
                    for (uint32_t vertex = 0; vertex < primitive.vertexCount; ++vertex)
                    {
                        deformation.sourceVertices.push_back(vertex);
                    }
                }
                deformation.influences = primitive.influences;
                for (uint32_t morph = 0; morph < data.morphPrimitives.size(); ++morph)
                {
                    if (data.morphPrimitives[morph].mesh == sourceNode.mesh.value
                        && (data.morphPrimitives[morph].primitive == primitiveIndex
                            || data.morphPrimitives[morph].primitive == UINT32_MAX))
                    {
                        deformation.morphPrimitive = static_cast<int32_t>(morph);
                    }
                }
                for (uint32_t vertex = 0; vertex < primitive.vertexCount; ++vertex)
                {
                    data.bindVertices.push_back(asset.vertices[primitive.firstVertex + vertex]);
                    data.vertexTexCoords.push_back(asset.vertexTexCoords.empty()
                                                       ? VertexTextureCoordinates()
                                                       : asset.vertexTexCoords[primitive.firstVertex + vertex]);
                }
            }
            const uint32_t firstIndex = deform ? static_cast<uint32_t>(loaded.indices.size()) : primitive.firstIndex;
            for (uint32_t index = 0; deform && index < primitive.indexCount; ++index)
            {
                loaded.indices.push_back(deformation.firstVertex + asset.indices[primitive.firstIndex + index]
                                         - primitive.firstVertex);
            }
            UniquePtr<sg::SubMesh> subMesh =
                MakeUnique<sg::SubMesh>(source.GetName() + "_instance_" + std::to_string(node.GetIndex()), firstIndex,
                                        primitive.indexCount, primitive.vertexCount);
            subMesh->SetMaterial(source.GetMaterialIndex(), source.GetMaterial());
            if (primitive.materialSlot < sourceNode.materials.size())
            {
                const MaterialId binding = sourceNode.materials[primitive.materialSlot];
                const uint32_t   materialIndex =
                    binding.IsValid() ? binding.value : static_cast<uint32_t>(asset.materials.size());
                subMesh->SetMaterial(materialIndex, materials[materialIndex]);
            }
            subMesh->assetMesh      = sourceNode.mesh.value;
            subMesh->assetPrimitive = primitiveIndex;
            subMesh->topology       = primitive.topology;
            subMesh->SetBounds(primitive.bounds);
            mesh->SetAABB(primitive.bounds.GetMin(), primitive.bounds.GetMax());
            mesh->AddSubMesh(subMesh.Get());
            scene.AddComponent(std::move(subMesh));
            if (deform)
            {
                data.deformations.push_back(std::move(deformation));
            }
        }
    }
    node.AddComponent(mesh.Get());
    mesh->AddNode(&node);
    scene.AddComponent(std::move(mesh));
}

void BuildCameraAndLight(const SceneAsset& asset, const NodeAsset& source, sg::Node& node, sg::Scene& scene)
{
    const Mat4& world = node.GetData().modelMatrix;
    if (source.camera.IsValid())
    {
        const CameraAsset&         definition   = asset.cameras[source.camera.value];
        UniquePtr<sg::SceneCamera> camera       = MakeUnique<sg::SceneCamera>(definition.name);
        static_cast<CameraParameters&>(*camera) = definition;
        camera->sourceIndex                     = source.camera.value;
        camera->worldMatrix                     = world;
        node.AddComponent(camera.Get());
        scene.AddComponent(std::move(camera));
    }
    if (source.light.IsValid())
    {
        const LightAsset&    definition = asset.lights[source.light.value];
        UniquePtr<sg::Light> light      = MakeUnique<sg::Light>(definition.name);
        light->sourceIndex              = source.light.value;
        light->SetType(static_cast<sg::LightType>(definition.kind));
        sg::LightProperties properties;
        properties.position = Vec3(world[3]);
        Vec3 direction      = definition.kind == LightKind::Point ? Vec3(0, 0, -1) : -Vec3(world[2]);
        if (!NormalizeSurfaceNormal(direction))
        {
            direction = Vec3(0, 0, -1);
        }
        properties.direction      = Vec4(direction, 0);
        properties.color          = Vec4(definition.color, 1);
        properties.intensity      = definition.intensity;
        properties.range          = definition.range;
        properties.innerConeAngle = definition.innerConeAngle;
        properties.outerConeAngle = definition.outerConeAngle;
        light->SetProperties(properties);
        node.AddComponent(light.Get());
        scene.AddComponent(std::move(light));
    }
}

void BuildNodes(const SceneAsset& asset, LoadedScene& loaded, SceneImportControl* control)
{
    sg::Scene&                               scene  = *loaded.scene;
    const HeapVector<sg::Mesh*>              meshes = scene.GetComponents<sg::Mesh>();
    HeapVector<UniquePtr<sg::Node>>          nodes;
    HeapVector<std::pair<NodeId, sg::Node*>> pending;
    for (size_t root = asset.roots.size(); root > 0; --root)
    {
        pending.push_back({asset.roots[root - 1], nullptr});
    }
    while (!pending.empty() && (control == nullptr || !control->IsCancelled()))
    {
        const std::pair<NodeId, sg::Node*> entry = pending.back();
        pending.pop_back();
        const NodeAsset&    source = asset.nodes[entry.first.value];
        UniquePtr<sg::Node> node   = MakeUnique<sg::Node>(entry.first.value, source.name);
        node->SetParent(entry.second);
        node->visible    = source.visible;
        node->selectable = source.selectable;
        node->hoverable  = source.hoverable;
        node->skinIndex  = source.skin.IsValid() ? static_cast<int32_t>(source.skin.value) : -1;
        node->morphWeightsSourceNode =
            source.morphWeightsSource.IsValid() ? static_cast<int32_t>(source.morphWeightsSource.value) : -1;
        node->morphWeights                 = source.morphWeights;
        UniquePtr<sg::Transform> transform = MakeUnique<sg::Transform>(*node);
        transform->SetTranslation(source.translation);
        transform->SetRotation(source.rotation);
        transform->SetScale(source.scale);
        transform->SetLocalMatrix(source.baseMatrix);
        const Mat4 world =
            (entry.second != nullptr ? entry.second->GetData().modelMatrix : Mat4(1)) * transform->GetLocalMatrix();
        node->AddComponent(transform.Get());
        node->SetData(static_cast<uint32_t>(scene.GetRenderableCount()), world);
        scene.AddComponent(std::move(transform));
        BuildCameraAndLight(asset, source, *node, scene);
        if (source.mesh.IsValid())
        {
            sg::Mesh& mesh = *meshes[source.mesh.value];
            node->AddComponent(&mesh);
            if (node->morphWeights.empty())
            {
                node->morphWeights = asset.meshes[source.mesh.value].morphWeights;
            }
            if (source.renderMesh && !mesh.GetSubMeshes().empty())
            {
                const bool deform = HasDeformation(asset, source);
                if (deform || !source.materials.empty())
                {
                    BuildInstanceMesh(asset, source, *node, mesh, loaded, deform);
                }
                else
                {
                    mesh.AddNode(node.Get());
                }
                scene.AddRenderableNode(node.Get());
            }
        }
        if (entry.second != nullptr)
        {
            entry.second->AddChild(node.Get());
        }
        for (size_t child = source.children.size(); child > 0; --child)
        {
            pending.push_back({source.children[child - 1], node.Get()});
        }
        nodes.push_back(std::move(node));
    }
    scene.SetNodes(std::move(nodes));
}

SharedPtr<const sg::SceneSourceInfo, MultiThreadCounter> DescribeSource(const SceneAsset& asset)
{
    SharedPtr<sg::SceneSourceInfo, MultiThreadCounter> source(new sg::SceneSourceInfo);
    source->document = asset.sourceDocument;
    source->nodeMeshes.reserve(asset.nodes.size());
    source->nodeMatrixTransforms.reserve(asset.nodes.size());
    for (const NodeAsset& node : asset.nodes)
    {
        source->nodeMeshes.push_back(node.mesh);
        source->nodeMatrixTransforms.push_back(node.matrixTransform ? 1 : 0);
    }
    source->materials        = static_cast<uint32_t>(asset.materials.size());
    source->cameras          = static_cast<uint32_t>(asset.cameras.size());
    source->lights           = static_cast<uint32_t>(asset.lights.size());
    source->imageBasedLights = static_cast<uint32_t>(asset.imageBasedLights.size());
    return source;
}
} // namespace

void PrepareScenePlacement(sg::Scene& scene, ScenePlacement placement)
{
    if (placement == ScenePlacement::CenterAndNormalize)
    {
        sys::SceneEditor::CenterAndNormalizeScene(&scene);
    }
}

SceneLoadResult BuildSceneRuntime(SharedPtr<const SceneAsset, MultiThreadCounter> asset, SceneImportControl* control)
{
    SceneLoadResult result;
    std::string     reason;
    if (control != nullptr && control->IsCancelled())
    {
        SetSceneImportFailure(result, SceneImportStatus::Cancelled, asset ? asset->sourcePath : "", "Scene build cancelled");
    }
    else if (!asset)
    {
        SetSceneImportFailure(result, SceneImportStatus::InvalidRequest, "", "A canonical scene asset is required");
    }
    else if (!ValidateSceneAsset(*asset, 16ull * 1024 * 1024 * 1024, &reason))
    {
        SetSceneImportFailure(result, SceneImportStatus::InvalidData, asset->sourcePath, "Invalid canonical scene: " + reason);
    }
    else
    {
        result = BuildValidatedSceneRuntime(asset, control);
    }
    return result;
}

SceneLoadResult BuildValidatedSceneRuntime(const SharedPtr<const SceneAsset, MultiThreadCounter>& asset,
                                           SceneImportControl*                                    control)
{
    SceneLoadResult result;
    if (control != nullptr && control->IsCancelled())
    {
        SetSceneImportFailure(result, SceneImportStatus::Cancelled, asset->sourcePath, "Scene build cancelled");
    }
    else
    {
        if (control != nullptr)
        {
            control->SetStage(SceneImportStage::BuildingScene);
        }
        UniquePtr<LoadedScene> loaded = MakeUnique<LoadedScene>();
        loaded->path                  = asset->sourcePath;
        loaded->indices               = asset->indices;
        loaded->scene                 = MakeUnique<sg::Scene>();
        loaded->scene->SetName(asset->name);
        sg::SceneAssetData& data = loaded->scene->GetAssetData();
        data.source              = DescribeSource(*asset);
        data.skins               = asset->skins;
        data.animations          = asset->animations;
        data.morphPrimitives     = asset->morphPrimitives;
        data.vertexTexCoords     = asset->vertexTexCoords;
        data.vertexTexCoords.resize(asset->vertices.size());
        data.vertexTexCoordSets    = asset->vertexTexCoordSets;
        data.bindVertices          = asset->vertices;
        data.materialVariants      = asset->materialVariants;
        data.variantPrimitives     = asset->variantPrimitives;
        data.extraVertexAttributes = asset->extraVertexAttributes;
        data.imageBasedLights      = asset->imageBasedLights;
        data.imageBasedLight       = asset->imageBasedLight;
        BuildImagesAndMaterials(*asset, *loaded->scene);
        BuildMeshes(*asset, *loaded->scene);
        BuildNodes(*asset, *loaded, control);
        data.bindVertexTexCoords = data.vertexTexCoords;
        if (control != nullptr && control->IsCancelled())
        {
            SetSceneImportFailure(result, SceneImportStatus::Cancelled, asset->sourcePath, "Scene build cancelled");
        }
        else if (!sg::ApplySceneDeformations(*loaded->scene,
                                             VectorView<const Vertex>(data.bindVertices.data(), data.bindVertices.size()),
                                             loaded->vertices))
        {
            SetSceneImportFailure(result, SceneImportStatus::InvalidData, asset->sourcePath,
                                  "Failed to apply initial skin or morph deformation");
        }
        else
        {
            loaded->scene->UpdateAABB();
            if (control != nullptr && control->IsCancelled())
            {
                SetSceneImportFailure(result, SceneImportStatus::Cancelled, asset->sourcePath, "Scene build cancelled");
            }
            else
            {
                result.scene  = std::move(loaded);
                result.status = SceneImportStatus::Success;
            }
        }
    }
    return result;
}
} // namespace zen::asset
