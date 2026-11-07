#include "AssetLib/SceneAssembly.h"
#include "AssetLib/ProcessedSceneAdapter.h"
#include "AssetLib/ProcessedSceneIO.h"
#include "AssetLib/AssetPaths.h"
#include "ZenAsset/Reader/Assembly.h"
#include <algorithm>

namespace zen::asset
{
namespace
{
struct ModelOffsets
{
    uint32_t vertex, index, mesh, material, image, texture, sampler, camera, light, environment;
};

// A model read ahead of its manifest turn: its geometry is already appended at vertex/index.
struct AssemblyModelResources
{
    SharedPtr<const SceneAsset, MultiThreadCounter> asset;
    HeapVector<SceneImportDiagnostic>               diagnostics;
    HeapVector<uint32_t>                            slots;
    uint32_t                                        vertex{0};
    uint32_t                                        index{0};
    bool                                            ready{false};
};

struct LargerModelGeometry
{
    const zenasset::SceneAssembly& assembly;

    bool operator()(uint32_t left, uint32_t right) const
    {
        const zenasset::AssemblyCounts& a = assembly.models[left].counts;
        const zenasset::AssemblyCounts& b = assembly.models[right].counts;
        return a.vertices * sizeof(zenasset::Vertex) + a.indices * sizeof(uint32_t)
             > b.vertices * sizeof(zenasset::Vertex) + b.indices * sizeof(uint32_t);
    }
};

template <class Tag> AssetId<Tag> OffsetId(AssetId<Tag> id, uint32_t offset)
{
    if (id.IsValid())
    {
        id.value += offset;
    }
    return id;
}

struct OffsetTexture
{
    uint32_t  offset;
    TextureId operator()(TextureId id) const
    {
        return OffsetId(id, offset);
    }
};

bool Cancelled(const SceneImportControl* control)
{
    return control != nullptr && control->IsCancelled();
}

bool UsesModel(const zenasset::SceneAssembly& assembly, const std::string& key)
{
    bool used = false;
    for (const zenasset::AssemblyInstance& instance : assembly.instances)
    {
        used |= instance.model == key;
    }
    return used;
}

// uv0 and uv1 carry up to two sets. Only more sets, or morphs that offset their base
// coordinates (MorphVertex), need the per-vertex list.
bool KeepsVertexTexCoords(const SceneAsset& target, const zenasset::SceneAssembly& assembly)
{
    return target.vertexTexCoordSets.size() > 2 || assembly.counts.morphPrimitives != 0;
}

void AppendModelDiagnostics(HeapVector<SceneImportDiagnostic>& target,
                            const zenasset::BuildMetadata&     metadata,
                            const std::string&                 modelKey)
{
    for (const zenasset::Diagnostic& source : metadata.diagnostics)
    {
        SceneImportDiagnostic diagnostic = AdaptProcessedDiagnostic(source);
        diagnostic.object                = modelKey + "/" + diagnostic.object;
        target.push_back(std::move(diagnostic));
    }
}

bool ReserveAssembly(SceneAsset& target, const zenasset::SceneAssembly& assembly, const SceneAssetReadLimits& limits)
{
    uint64_t vertices = 0, indices = 0, meshes = 0, materials = 0, images = 0, textures = 0, samplers = 0;
    bool     valid = assembly.instances.size() <= INT32_MAX && assembly.counts.nodes <= INT32_MAX - assembly.instances.size()
                  && assembly.counts.skins <= INT32_MAX && assembly.counts.animations <= INT32_MAX;
    for (const zenasset::AssemblyModel& model : assembly.models)
    {
        if (UsesModel(assembly, model.key))
        {
            vertices  += model.counts.vertices;
            indices   += model.counts.indices;
            meshes    += model.counts.meshes;
            materials += model.counts.materials;
            images    += model.counts.images;
            textures  += model.counts.textures;
            samplers  += model.counts.samplers;
        }
    }
    valid = valid && vertices <= UINT32_MAX && indices <= UINT32_MAX && meshes <= INT32_MAX && materials < INT32_MAX
         && images <= INT32_MAX && textures <= INT32_MAX && samplers <= INT32_MAX && assembly.counts.cameras <= INT32_MAX
         && assembly.counts.lights <= INT32_MAX && assembly.counts.imageBasedLights <= INT32_MAX
         && assembly.counts.morphPrimitives <= INT32_MAX;
    const uint64_t counts[] = {vertices,
                               indices,
                               meshes,
                               materials,
                               images,
                               textures,
                               samplers,
                               assembly.counts.nodes + assembly.instances.size(),
                               assembly.counts.skins,
                               assembly.counts.animations,
                               assembly.counts.cameras,
                               assembly.counts.lights,
                               assembly.counts.imageBasedLights,
                               assembly.counts.morphPrimitives};
    for (uint64_t count : counts)
    {
        valid = valid && count <= limits.maxArrayElements;
    }
    for (uint64_t set : assembly.texCoordSets)
    {
        target.vertexTexCoordSets.push_back(set);
    }
    for (const zenasset::AssemblyModel& model : assembly.models)
    {
        if (UsesModel(assembly, model.key) && model.texCoordSets.empty())
        {
            for (uint64_t set = 0; set < 2; ++set)
            {
                if (std::find(target.vertexTexCoordSets.begin(), target.vertexTexCoordSets.end(), set)
                    == target.vertexTexCoordSets.end())
                {
                    target.vertexTexCoordSets.push_back(set);
                }
            }
        }
    }
    if (target.vertexTexCoordSets.size() > 1)
    {
        std::sort(target.vertexTexCoordSets.begin(), target.vertexTexCoordSets.end());
    }
    // Counts are validated metadata hints. Full model reads below must agree before appending.
    const uint64_t vertexStride =
        sizeof(Vertex) + sizeof(VertexTextureCoordinates) + target.vertexTexCoordSets.size() * sizeof(Vec2);
    valid = valid && vertexStride <= limits.maxRuntimeGeometryBytes && vertices <= limits.maxRuntimeGeometryBytes / vertexStride
         && indices <= (limits.maxRuntimeGeometryBytes - vertices * vertexStride) / sizeof(uint32_t);
    if (valid)
    {
        const uint64_t reserveBytes = vertices * vertexStride + indices * sizeof(uint32_t) + meshes * sizeof(MeshAsset)
                                    + materials * sizeof(MaterialAsset) + images * sizeof(ImageAsset)
                                    + textures * sizeof(TextureAsset) + samplers * sizeof(SamplerAsset)
                                    + (assembly.counts.nodes + assembly.instances.size()) * sizeof(NodeAsset);
        valid                       = reserveBytes <= limits.maxAllocationBytes;
    }
    if (valid)
    {
        target.vertices.reserve(static_cast<size_t>(vertices));
        target.indices.reserve(static_cast<size_t>(indices));
        if (KeepsVertexTexCoords(target, assembly))
        {
            target.vertexTexCoords.reserve(static_cast<size_t>(vertices));
        }
        target.meshes.reserve(static_cast<size_t>(meshes));
        target.materials.reserve(static_cast<size_t>(materials));
        target.images.reserve(static_cast<size_t>(images));
        target.textures.reserve(static_cast<size_t>(textures));
        target.samplers.reserve(static_cast<size_t>(samplers));
        target.nodes.reserve(static_cast<size_t>(assembly.counts.nodes + assembly.instances.size()));
    }
    return valid;
}

HeapVector<uint32_t> MapUVSets(const SceneAsset& source, const SceneAsset& target)
{
    HeapVector<uint32_t> slots;
    for (uint32_t index = 0; index < (source.vertexTexCoordSets.empty() ? 2 : source.vertexTexCoordSets.size()); ++index)
    {
        const uint64_t set   = source.vertexTexCoordSets.empty() ? index : source.vertexTexCoordSets[index];
        const auto     found = std::find(target.vertexTexCoordSets.begin(), target.vertexTexCoordSets.end(), set);
        slots.push_back(static_cast<uint32_t>(found - target.vertexTexCoordSets.begin()));
    }
    return slots;
}

uint32_t MapUV(uint32_t slot, const HeapVector<uint32_t>& slots)
{
    return slot < slots.size() ? slots[slot] : 0;
}

void RemapMaterialUVs(MaterialAsset& material, const HeapVector<uint32_t>& slots)
{
    material.texCoordSets.baseColor          = MapUV(material.texCoordSets.baseColor, slots);
    material.texCoordSets.metallicRoughness  = MapUV(material.texCoordSets.metallicRoughness, slots);
    material.texCoordSets.specularGlossiness = MapUV(material.texCoordSets.specularGlossiness, slots);
    material.texCoordSets.normal             = MapUV(material.texCoordSets.normal, slots);
    material.texCoordSets.occlusion          = MapUV(material.texCoordSets.occlusion, slots);
    material.texCoordSets.emissive           = MapUV(material.texCoordSets.emissive, slots);
    for (MaterialFeatures::TextureMember member : MaterialFeatures::textureMembers)
    {
        MaterialTextureBinding& binding = material.features.*member;
        binding.texCoord                = MapUV(binding.texCoord, slots);
    }
}

bool AppendGeometry(SceneAsset&                 target,
                    const zenasset::Scene&      source,
                    const ModelOffsets&         offsets,
                    const HeapVector<uint32_t>& slots,
                    bool                        keepCoordinates,
                    SceneImportControl*         control)
{
    bool valid = true;
    for (uint32_t index = 0; valid && index < source.vertices.size(); ++index)
    {
        if ((index & 4095u) == 0)
        {
            valid = !Cancelled(control);
        }
        if (valid)
        {
            target.vertices.push_back(AdaptProcessedVertex(source.vertices[index]));
            VertexTextureCoordinates coordinates;
            coordinates.resize(target.vertexTexCoordSets.size());
            for (uint32_t slot = 0; slot < std::min(size_t(2), slots.size()); ++slot)
            {
                const zenasset::Vec2 uv  = slot == 0 ? source.vertices[index].uv0 : source.vertices[index].uv1;
                coordinates[slots[slot]] = Vec2(uv.x, uv.y);
            }
            if (!source.vertexTexCoords.empty())
            {
                for (uint32_t slot = 0; slot < std::min(slots.size(), source.vertexTexCoords[index].size()); ++slot)
                {
                    const zenasset::Vec2 uv  = source.vertexTexCoords[index][slot];
                    coordinates[slots[slot]] = Vec2(uv.x, uv.y);
                }
            }
            target.vertices.back().uv0 = !coordinates.empty() ? coordinates[0] : Vec2(0);
            target.vertices.back().uv1 = coordinates.size() > 1 ? coordinates[1] : Vec2(0);
            if (keepCoordinates)
            {
                target.vertexTexCoords.push_back(std::move(coordinates));
            }
        }
    }
    for (uint32_t index = 0; valid && index < source.indices.size(); ++index)
    {
        if ((index & 4095u) == 0)
        {
            valid = !Cancelled(control);
        }
        if (valid)
        {
            target.indices.push_back(source.indices[index] + offsets.vertex);
        }
    }
    return valid;
}

void AppendModelResources(SceneAsset&                 target,
                          const SceneAsset&           source,
                          const ModelOffsets&         offsets,
                          const HeapVector<uint32_t>& slots,
                          const std::string&          prefix)
{
    for (const MeshAsset& original : source.meshes)
    {
        MeshAsset mesh = original;
        mesh.name      = prefix + mesh.name;
        for (MeshPrimitiveAsset& primitive : mesh.primitives)
        {
            primitive.firstVertex += offsets.vertex;
            primitive.firstIndex  += offsets.index;
            primitive.material     = OffsetId(primitive.material, offsets.material);
        }
        target.meshes.push_back(std::move(mesh));
    }
    for (const MaterialAsset& original : source.materials)
    {
        MaterialAsset material;
        material.name = prefix + original.name;
        CopyMaterialParameters(material, original, OffsetTexture{offsets.texture});
        RemapMaterialUVs(material, slots);
        target.materials.push_back(std::move(material));
    }
    for (const ImageAsset& image : source.images)
    {
        target.images.push_back({prefix + image.name, image.pixels});
    }
    for (const TextureAsset& original : source.textures)
    {
        TextureAsset texture = original;
        texture.name         = prefix + texture.name;
        texture.image        = OffsetId(texture.image, offsets.image);
        texture.sampler      = OffsetId(texture.sampler, offsets.sampler);
        texture.linearSource = OffsetId(texture.linearSource, offsets.texture);
        target.textures.push_back(std::move(texture));
    }
    for (const SamplerAsset& sampler : source.samplers)
    {
        target.samplers.push_back(sampler);
        target.samplers.back().name = prefix + sampler.name;
    }
    for (const CameraAsset& camera : source.cameras)
    {
        target.cameras.push_back(camera);
        target.cameras.back().name = prefix + camera.name;
    }
    for (const LightAsset& light : source.lights)
    {
        target.lights.push_back(light);
        target.lights.back().name = prefix + light.name;
    }
    for (const ImageBasedLightAsset& environment : source.imageBasedLights)
    {
        target.imageBasedLights.push_back(environment);
        target.imageBasedLights.back().name = prefix + environment.name;
    }
    for (const MorphPrimitiveAsset& original : source.morphPrimitives)
    {
        MorphPrimitiveAsset morph  = original;
        morph.mesh                += offsets.mesh;
        for (MorphTargetAsset& morphTarget : morph.targets)
        {
            HeapVector<HeapVector<Vec2>> coordinates(target.vertexTexCoordSets.size());
            for (uint32_t slot = 0; slot < morphTarget.texCoords.size(); ++slot)
            {
                coordinates[slots[slot]] = std::move(morphTarget.texCoords[slot]);
            }
            morphTarget.texCoords = std::move(coordinates);
        }
        target.morphPrimitives.push_back(std::move(morph));
    }
    for (const VertexAttributeAsset& original : source.extraVertexAttributes)
    {
        target.extraVertexAttributes.push_back(original);
        target.extraVertexAttributes.back().mesh += offsets.mesh;
    }
    const size_t variantBase = target.materialVariants.size();
    for (const std::string& name : source.materialVariants)
    {
        target.materialVariants.push_back(prefix + name);
    }
    for (MaterialVariantPrimitiveAsset& variant : target.variantPrimitives)
    {
        while (variant.materials.size() < target.materialVariants.size())
        {
            variant.materials.push_back(-1);
        }
    }
    for (const MaterialVariantPrimitiveAsset& original : source.variantPrimitives)
    {
        MaterialVariantPrimitiveAsset variant;
        variant.mesh      = original.mesh + offsets.mesh;
        variant.primitive = original.primitive;
        for (size_t index = 0; index < target.materialVariants.size(); ++index)
        {
            const int32_t material = index < variantBase ? -1 : original.materials[index - variantBase];
            variant.materials.push_back(material < 0 ? -1 : material + static_cast<int32_t>(offsets.material));
        }
        target.variantPrimitives.push_back(std::move(variant));
    }
}

void RemapAnimationTarget(AnimationTarget& target, const ModelOffsets& offsets, uint32_t nodeOffset)
{
    switch (target.object)
    {
        case AnimatedObject::Node: target.index += nodeOffset; break;
        case AnimatedObject::Material: target.index += offsets.material; break;
        case AnimatedObject::Camera: target.index += offsets.camera; break;
        case AnimatedObject::Light: target.index += offsets.light; break;
        case AnimatedObject::Environment: target.index += offsets.environment; break;
    }
}

bool IsInstantiatedNode(const SceneAsset& source, NodeId node)
{
    while (source.nodes[node.value].parent.IsValid())
    {
        node = source.nodes[node.value].parent;
    }
    return std::find(source.roots.begin(), source.roots.end(), node) != source.roots.end();
}

NodeId AppendInstance(SceneAsset&                       target,
                      const SceneAsset&                 source,
                      const ModelOffsets&               offsets,
                      const zenasset::AssemblyInstance& instance,
                      const zenasset::SceneAssembly&    assembly)
{
    const std::string prefix     = instance.key + "/";
    const uint32_t    rootIndex  = static_cast<uint32_t>(target.nodes.size());
    const uint32_t    nodeOffset = rootIndex + 1;
    const uint32_t    skinOffset = static_cast<uint32_t>(target.skins.size());
    NodeAsset         placement;
    placement.name            = instance.key;
    placement.matrixTransform = true;
    for (uint32_t column = 0; column < 4; ++column)
    {
        for (uint32_t row = 0; row < 4; ++row)
        {
            placement.baseMatrix[column][row] = instance.transform[column][row];
        }
    }
    for (NodeId root : source.roots)
    {
        placement.children.push_back(OffsetId(root, nodeOffset));
    }
    target.nodes.push_back(std::move(placement));
    for (const NodeAsset& original : source.nodes)
    {
        NodeAsset node = original;
        node.name      = prefix + original.name;
        node.parent    = OffsetId(original.parent, nodeOffset);
        const NodeId originalId{static_cast<uint32_t>(target.nodes.size()) - nodeOffset};
        if (!original.parent.IsValid() && std::find(source.roots.begin(), source.roots.end(), originalId) != source.roots.end())
        {
            node.parent = NodeId{rootIndex};
        }
        for (NodeId& child : node.children)
        {
            child = OffsetId(child, nodeOffset);
        }
        node.mesh               = OffsetId(node.mesh, offsets.mesh);
        node.skin               = OffsetId(node.skin, skinOffset);
        node.camera             = OffsetId(node.camera, offsets.camera);
        node.light              = OffsetId(node.light, offsets.light);
        node.morphWeightsSource = OffsetId(node.morphWeightsSource, nodeOffset);
        node.submitLight        = instance.submitLights && original.submitLight;
        for (MaterialId& material : node.materials)
        {
            material = OffsetId(material, offsets.material);
        }
        if (assembly.camera.instance == instance.key && original.camera.value == assembly.camera.resource
            && target.preferredCameraNode < 0 && IsInstantiatedNode(source, originalId))
        {
            target.preferredCameraNode = static_cast<int32_t>(target.nodes.size());
        }
        target.nodes.push_back(std::move(node));
    }
    for (const SkinAsset& original : source.skins)
    {
        SkinAsset skin = original;
        skin.name      = prefix + skin.name;
        if (skin.skeleton >= 0)
        {
            skin.skeleton += static_cast<int32_t>(nodeOffset);
        }
        for (uint32_t& joint : skin.joints)
        {
            joint += nodeOffset;
        }
        target.skins.push_back(std::move(skin));
    }
    for (const AnimationAsset& original : source.animations)
    {
        AnimationAsset animation = original;
        animation.name           = prefix + animation.name;
        for (AnimationChannel& channel : animation.channels)
        {
            if (channel.node < source.nodes.size())
            {
                channel.node += nodeOffset;
            }
            RemapAnimationTarget(channel.target, offsets, nodeOffset);
        }
        target.animations.push_back(std::move(animation));
    }
    if (assembly.environment.instance == instance.key)
    {
        target.imageBasedLight = static_cast<int32_t>(offsets.environment + assembly.environment.resource);
    }
    return NodeId{rootIndex};
}

// Appends one staged model's resources and instances in manifest order, then releases them.
void ComposeModel(SceneAsset&                        target,
                  AssemblyModelResources&            staged,
                  const zenasset::SceneAssembly&     assembly,
                  uint32_t                           modelIndex,
                  HeapVector<SceneImportDiagnostic>& diagnostics)
{
    if (staged.asset)
    {
        const zenasset::AssemblyModel& model  = assembly.models[modelIndex];
        const SceneAsset&              source = *staged.asset;
        const ModelOffsets             offsets{staged.vertex,
                                               staged.index,
                                               static_cast<uint32_t>(target.meshes.size()),
                                               static_cast<uint32_t>(target.materials.size()),
                                               static_cast<uint32_t>(target.images.size()),
                                               static_cast<uint32_t>(target.textures.size()),
                                               static_cast<uint32_t>(target.samplers.size()),
                                               static_cast<uint32_t>(target.cameras.size()),
                                               static_cast<uint32_t>(target.lights.size()),
                                               static_cast<uint32_t>(target.imageBasedLights.size())};
        AppendModelResources(target, source, offsets, staged.slots, model.key + "/");
        for (uint32_t instanceIndex = 0; instanceIndex < assembly.instances.size(); ++instanceIndex)
        {
            const zenasset::AssemblyInstance& instance = assembly.instances[instanceIndex];
            if (instance.model == model.key)
            {
                target.roots[instanceIndex] = AppendInstance(target, source, offsets, instance, assembly);
            }
        }
        for (SceneImportDiagnostic& diagnostic : staged.diagnostics)
        {
            diagnostics.push_back(std::move(diagnostic));
        }
    }
    staged = AssemblyModelResources{};
}

zenasset::Status ReadModel(const std::string&               path,
                           const zenasset::AssemblyModel&   model,
                           const zenasset::SceneReadLimits& limits,
                           const zenasset::Cancellation&    cancellation,
                           zenasset::SceneReadResult&       result)
{
    const zenasset::PackageResult package = zenasset::OpenPackage(path, limits.package, &cancellation);
    zenasset::Status              status  = package.status;
    if (status.Succeeded() && package.package->Identity() != model.identity)
    {
        status = {zenasset::StatusCode::InvalidData, "Assembly model identity changed before reading"};
    }
    if (status.Succeeded())
    {
        result = zenasset::ReadSceneDeferredImages(package.package, limits, &cancellation);
        status = result.status;
    }
    if (status.Succeeded())
    {
        zenasset::AssemblyModel summary;
        status = zenasset::SummarizeAssemblyModel(*result.scene, summary, limits.maxAllocationBytes, &cancellation);
        if (status.Succeeded()
            && (summary.counts != model.counts || summary.texCoordSets != model.texCoordSets
                || summary.hasBounds != model.hasBounds || summary.bounds != model.bounds))
        {
            status = {zenasset::StatusCode::InvalidData, "Decoded model disagrees with assembly metadata"};
        }
    }
    return status;
}
} // namespace

SceneImportResult ReadSceneAssemblyAsset(const std::string&                         path,
                                         const SceneAssetReadLimits&                limits,
                                         SceneImportControl*                        control,
                                         SharedPtr<SceneAsset, MultiThreadCounter>* runtimeOwner)
{
    SceneImportResult result;
    if (runtimeOwner != nullptr)
    {
        runtimeOwner->Reset();
    }
    const zenasset::Cancellation      cancellation(&ProcessedOperationCancelled, control);
    const zenasset::SceneReadLimits   readLimits = ProcessedLimits(limits);
    HeapVector<SceneImportDiagnostic> modelDiagnostics;
    if (control != nullptr)
    {
        control->SetStage(SceneImportStage::Reading);
    }
    const zenasset::AssemblyReadResult        loaded = zenasset::ReadAssembly(path, readLimits, &cancellation);
    zenasset::Status                          status = loaded.status;
    SharedPtr<SceneAsset, MultiThreadCounter> combined;
    if (status.Succeeded())
    {
        status = zenasset::VerifyAssemblyDependencies(path, *loaded.assembly, readLimits.package, &cancellation);
    }
    if (status.Succeeded())
    {
        combined                          = SharedPtr<SceneAsset, MultiThreadCounter>(new SceneAsset);
        combined->name                    = loaded.assembly->name;
        combined->sourcePath              = path;
        combined->explicitCameraSelection = true;
        // There is no single source document whose local IDs describe the composed asset.
        if (!ReserveAssembly(*combined, *loaded.assembly, limits))
        {
            status = {zenasset::StatusCode::ResourceLimit, "Assembly exceeds runtime geometry or index limits"};
        }
    }
    if (status.Succeeded())
    {
        const zenasset::SceneAssembly& assembly = *loaded.assembly;
        combined->roots.resize(assembly.instances.size());
        const std::filesystem::path        directory = std::filesystem::u8path(path).parent_path();
        HeapVector<AssemblyModelResources> resources(assembly.models.size());
        HeapVector<uint32_t>               readOrder;
        for (uint32_t index = 0; index < assembly.models.size(); ++index)
        {
            if (UsesModel(assembly, assembly.models[index].key))
            {
                readOrder.push_back(index);
            }
            else
            {
                resources[index].ready = true;
            }
        }
        // Decode large models before the destination fills up. Keep resource/node
        // composition in manifest order so IDs and draw-order ties stay stable.
        if (readOrder.size() > 1)
        {
            std::stable_sort(readOrder.begin(), readOrder.end(), LargerModelGeometry{assembly});
        }
        uint32_t composed = 0;
        for (uint32_t modelIndex : readOrder)
        {
            const zenasset::AssemblyModel& model  = assembly.models[modelIndex];
            AssemblyModelResources&        staged = resources[modelIndex];
            if (status.Succeeded())
            {
                zenasset::SceneReadResult native;
                status = ReadModel(PathToUtf8(directory / std::filesystem::u8path(model.path)), model, readLimits, cancellation,
                                   native);
                if (status.Succeeded())
                {
                    AppendModelDiagnostics(staged.diagnostics, native.metadata, model.key);
                    SceneImportResult adapted = AdaptProcessedSceneResources(*native.scene, control);
                    if (adapted.asset)
                    {
                        staged.vertex = static_cast<uint32_t>(combined->vertices.size());
                        staged.index  = static_cast<uint32_t>(combined->indices.size());
                        staged.slots  = MapUVSets(*adapted.asset, *combined);
                        const ModelOffsets offsets{staged.vertex, staged.index};
                        if (AppendGeometry(*combined, *native.scene, offsets, staged.slots,
                                           KeepsVertexTexCoords(*combined, assembly), control))
                        {
                            staged.asset = std::move(adapted.asset);
                            staged.ready = true;
                        }
                        else
                        {
                            status = {zenasset::StatusCode::Cancelled, "Assembly composition cancelled"};
                        }
                    }
                    else
                    {
                        result = std::move(adapted);
                        status = {zenasset::StatusCode::InvalidData, "Failed to adapt assembly model"};
                    }
                }
            }
            // Compose every model whose manifest predecessors are composed, so resources read
            // ahead of their turn are retained only until then.
            while (status.Succeeded() && composed < resources.size() && resources[composed].ready)
            {
                ComposeModel(*combined, resources[composed], assembly, composed, modelDiagnostics);
                ++composed;
                if (Cancelled(control))
                {
                    status = {zenasset::StatusCode::Cancelled, "Assembly composition cancelled"};
                }
            }
        }
    }
    if (Cancelled(control))
    {
        result = {};
        status = {zenasset::StatusCode::Cancelled, "Assembly load cancelled"};
    }
    if (status.Succeeded() && !loaded.assembly->camera.instance.empty() && combined->preferredCameraNode < 0)
    {
        status = {zenasset::StatusCode::InvalidData, "Selected assembly camera has no instantiated node"};
    }
    std::string failure;
    if (status.Succeeded() && !ValidateSceneAsset(*combined, limits.maxRuntimeGeometryBytes, &failure))
    {
        status = {zenasset::StatusCode::InvalidData, "Invalid composed scene: " + failure};
    }
    if (status.Succeeded())
    {
        result.asset = combined;
        if (runtimeOwner != nullptr)
        {
            *runtimeOwner = combined;
        }
        result.status      = SceneImportStatus::Success;
        result.diagnostics = std::move(modelDiagnostics);
    }
    else if (result.diagnostics.empty())
    {
        SetSceneImportFailure(result, AdaptProcessedStatus(status.code), path, status.message);
    }
    result.format          = "zenscene";
    result.importerVersion = zenasset::PackageSchemaVersion;
    return result;
}
} // namespace zen::asset
