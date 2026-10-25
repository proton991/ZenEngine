#include "AssetLib/SceneAsset.h"
#include "AssetLib/AnimatedMaterial.h"
#include "AssetLib/Kernels/AssetKernels.h"
#include "Math/Finite.h"
#include "ZenAsset/Format/Image.h"
#include <algorithm>
#include <bit>
#include <cmath>

namespace zen::asset
{
namespace
{
template <class Tag, class T> bool ValidReference(AssetId<Tag> id, const HeapVector<T>& objects)
{
    return !id.IsValid() || id.value < objects.size();
}

struct ValidateTextureReference
{
    const SceneAsset& asset;
    bool&             valid;

    TextureId operator()(TextureId id) const
    {
        valid &= ValidReference(id, asset.textures);
        return id;
    }
};

bool ValidateReferences(const SceneAsset& asset)
{
    bool valid = asset.nodes.size() <= UINT32_MAX && asset.vertices.size() <= UINT32_MAX && asset.indices.size() <= UINT32_MAX
              && (asset.vertexTexCoords.empty() || asset.vertexTexCoords.size() == asset.vertices.size());
    for (const TextureAsset& texture : asset.textures)
    {
        valid &= texture.image.IsValid() && ValidReference(texture.image, asset.images)
              && ValidReference(texture.sampler, asset.samplers) && ValidReference(texture.linearSource, asset.textures);
    }
    for (const ImageAsset& image : asset.images)
    {
        valid &= image.pixels && image.pixels->width > 0 && image.pixels->height > 0 && !image.pixels->bytes.empty();
    }
    for (const MaterialAsset& material : asset.materials)
    {
        MaterialParametersT<TextureId> checked;
        CopyMaterialParameters(checked, material, ValidateTextureReference{asset, valid});
    }
    for (const MeshAsset& mesh : asset.meshes)
    {
        for (const MeshPrimitiveAsset& primitive : mesh.primitives)
        {
            valid &= primitive.firstVertex <= asset.vertices.size()
                  && primitive.vertexCount <= asset.vertices.size() - primitive.firstVertex
                  && primitive.firstIndex <= asset.indices.size()
                  && primitive.indexCount <= asset.indices.size() - primitive.firstIndex
                  && ValidReference(primitive.material, asset.materials)
                  && (primitive.sourceVertices.empty() || primitive.sourceVertices.size() == primitive.vertexCount)
                  && (primitive.influences.empty() || primitive.influences.size() == primitive.vertexCount);
            valid  = valid
                  && kernels::IndicesInRange(asset.indices.data() + primitive.firstIndex, primitive.indexCount,
                                             primitive.firstVertex, primitive.vertexCount);
        }
    }
    HeapVector<uint32_t> parents(asset.nodes.size(), UINT32_MAX);
    for (size_t index = 0; valid && index < asset.nodes.size(); ++index)
    {
        const NodeAsset& node  = asset.nodes[index];
        valid                 &= ValidReference(node.parent, asset.nodes) && ValidReference(node.mesh, asset.meshes)
                              && ValidReference(node.skin, asset.skins) && ValidReference(node.camera, asset.cameras)
                              && ValidReference(node.light, asset.lights) && ValidReference(node.morphWeightsSource, asset.nodes);
        for (MaterialId material : node.materials)
        {
            valid &= ValidReference(material, asset.materials);
        }
        if (valid && node.mesh.IsValid() && !node.materials.empty())
        {
            for (const MeshPrimitiveAsset& primitive : asset.meshes[node.mesh.value].primitives)
            {
                valid &= primitive.materialSlot == UINT32_MAX || primitive.materialSlot < node.materials.size();
            }
        }
        for (NodeId child : node.children)
        {
            if (!child.IsValid() || child.value >= asset.nodes.size())
            {
                valid = false;
            }
            else
            {
                valid                &= parents[child.value] == UINT32_MAX && asset.nodes[child.value].parent.value == index;
                parents[child.value]  = static_cast<uint32_t>(index);
            }
        }
    }
    // Validate the entire definition graph, including nodes outside the selected scene.
    HeapVector<uint8_t> visited(asset.nodes.size(), 0);
    for (size_t index = 0; valid && index < asset.nodes.size(); ++index)
    {
        valid            &= parents[index] == asset.nodes[index].parent.value;
        uint32_t current  = static_cast<uint32_t>(index);
        while (valid && current != UINT32_MAX && visited[current] == 0)
        {
            visited[current] = 1;
            current          = parents[current];
        }
        valid   &= current == UINT32_MAX || visited[current] != 1;
        current  = static_cast<uint32_t>(index);
        while (current != UINT32_MAX && visited[current] == 1)
        {
            visited[current] = 2;
            current          = parents[current];
        }
    }
    HeapVector<uint8_t> rootSeen(asset.nodes.size(), 0);
    for (NodeId root : asset.roots)
    {
        if (!root.IsValid() || root.value >= asset.nodes.size())
        {
            valid = false;
        }
        else
        {
            valid                &= !asset.nodes[root.value].parent.IsValid() && rootSeen[root.value] == 0;
            rootSeen[root.value]  = 1;
        }
    }
    return valid;
}


template <class T> bool Finite(const T& value)
{
    return IsFinite(value);
}

bool ValidRotation(const Quat& value)
{
    const float squared = glm::dot(value, value);
    return std::isfinite(squared) && squared > 1e-20f;
}

// Float-based payloads are scanned as one flat array by the optimized kernel.
template <class T> bool FiniteValues(const T* values, size_t count)
{
    bool valid = true;
    if constexpr (std::is_same_v<T, double>)
    {
        valid = kernels::AllFinite(values, count);
    }
    else if constexpr (std::is_trivially_copyable_v<T> && sizeof(T) % sizeof(float) == 0
                       && (std::is_same_v<T, float> || std::is_same_v<T, Vec2> || std::is_same_v<T, Vec3>
                           || std::is_same_v<T, Vec4> || std::is_same_v<T, Mat4> || std::is_same_v<T, Vertex>))
    {
        valid = kernels::AllFinite(reinterpret_cast<const float*>(values), count * (sizeof(T) / sizeof(float)));
    }
    else
    {
        for (size_t index = 0; index < count; ++index)
        {
            valid &= Finite(values[index]);
        }
    }
    return valid;
}

template <class T> bool FiniteArray(const T& values)
{
    return FiniteValues(values.data(), values.size());
}

bool ValidTransform(const TextureTransform& transform)
{
    return Finite(transform.row0) && Finite(transform.row1) && Finite(transform.uvOffset) && Finite(transform.uvScale)
        && Finite(transform.uvRotation);
}

uint32_t MipCount(uint32_t width, uint32_t height)
{
    uint32_t count = 0;
    for (uint32_t extent = std::max(width, height); extent > 0; extent >>= 1)
    {
        ++count;
    }
    return count;
}

zenasset::PixelFormat ImageStorageFormat(Format format)
{
    zenasset::PixelFormat result = zenasset::PixelFormat::Undefined;
    switch (format)
    {
        case Format::R8G8B8A8_UNORM: result = zenasset::PixelFormat::RGBA8Unorm; break;
        case Format::R8G8B8A8_SRGB: result = zenasset::PixelFormat::RGBA8Srgb; break;
        case Format::BC4_UNORM_BLOCK: result = zenasset::PixelFormat::BC4Unorm; break;
        case Format::BC5_UNORM_BLOCK: result = zenasset::PixelFormat::BC5Unorm; break;
        case Format::BC7_UNORM_BLOCK: result = zenasset::PixelFormat::BC7Unorm; break;
        case Format::BC7_SRGB_BLOCK: result = zenasset::PixelFormat::BC7Srgb; break;
        default: break;
    }
    return result;
}

bool ValidAtlasTexture(const SceneAsset& asset, const TextureAsset& texture)
{
    const ImagePixels& pixels = *asset.images[texture.image.value].pixels;
    const ImageRegion& region = texture.atlasRegion;
    bool valid = zenasset::IsValidImageRegion({region.x, region.y, region.width, region.height}, pixels.width, pixels.height);
    if (valid && region != ImageRegion{})
    {
        valid = (texture.format == Format::R8G8B8A8_UNORM || texture.format == Format::R8G8B8A8_SRGB) && pixels.arrayLayers == 1
             && texture.arrayLayer == 0 && pixels.mips.size() == 1 && pixels.width <= 16384 && pixels.height <= 16384
             && std::has_single_bit(pixels.width) && std::has_single_bit(pixels.height) && std::has_single_bit(region.width)
             && std::has_single_bit(region.height) && region.x >= 4 && region.y >= 4 && region.x % 4 == 0 && region.y % 4 == 0
             && pixels.width - region.x - region.width >= 4 && pixels.height - region.y - region.height >= 4
             && texture.sampler.IsValid();
        if (valid)
        {
            const SamplerAsset& sampler = asset.samplers[texture.sampler.value];
            valid = sampler.minFilter == TextureFilter::Linear && sampler.magFilter == TextureFilter::Linear
                 && sampler.mipFilter == TextureFilter::Linear && sampler.wrapS == SamplerAddressMode::ClampToEdge
                 && sampler.wrapT == SamplerAddressMode::ClampToEdge;
        }
    }
    return valid;
}

bool ValidImages(const SceneAsset& asset)
{
    bool valid = true;
    for (const ImageAsset& image : asset.images)
    {
        const ImagePixels&          pixels = *image.pixels;
        const zenasset::PixelFormat format = ImageStorageFormat(pixels.format);
        uint64_t                    bytes  = 0;
        valid &= zenasset::GetImageArrayLevelByteSize(format, pixels.width, pixels.height, pixels.arrayLayers, bytes)
              && bytes == pixels.bytes.size() && (pixels.arrayLayers == 1 || !pixels.mips.empty())
              && pixels.mips.size() <= MipCount(pixels.width, pixels.height)
              && (pixels.mips.empty()
                  || std::equal(pixels.mips[0].begin(), pixels.mips[0].end(), pixels.bytes.begin(), pixels.bytes.end()));
        for (size_t level = 0; valid && level < pixels.mips.size(); ++level)
        {
            valid &= zenasset::GetImageArrayLevelByteSize(format, std::max(1u, pixels.width >> level),
                                                          std::max(1u, pixels.height >> level), pixels.arrayLayers, bytes)
                  && bytes == pixels.mips[level].size();
        }
    }
    for (const TextureAsset& texture : asset.textures)
    {
        valid &= ValidAtlasTexture(asset, texture);
        valid &= texture.interpretation <= TextureInterpretation::NormalXYPositiveZ
              && texture.arrayLayer < asset.images[texture.image.value].pixels->arrayLayers
              && ((texture.interpretation == TextureInterpretation::NormalXYPositiveZ)
                  == (texture.format == Format::BC5_UNORM_BLOCK));
        valid &= zenasset::ArePixelFormatsViewCompatible(ImageStorageFormat(asset.images[texture.image.value].pixels->format),
                                                         ImageStorageFormat(texture.format));
    }
    for (const SamplerAsset& sampler : asset.samplers)
    {
        const TextureFilter filters[]{sampler.minFilter, sampler.magFilter, sampler.mipFilter};
        for (TextureFilter filter : filters)
        {
            valid &= filter == TextureFilter::Nearest || filter == TextureFilter::Linear || filter == TextureFilter::MaxEnum;
        }
        const SamplerAddressMode addresses[]{sampler.wrapS, sampler.wrapT};
        for (SamplerAddressMode address : addresses)
        {
            valid &= address <= SamplerAddressMode::MirrorClampToEdge || address == SamplerAddressMode::MaxEnum;
        }
    }
    return valid;
}

bool ValidMaterials(const SceneAsset& asset)
{
    bool valid = true;
    for (const MaterialAsset& material : asset.materials)
    {
        valid &= material.alphaMode >= AlphaMode::Opaque && material.alphaMode <= AlphaMode::Blend
              && Finite(material.baseColorFactor) && Finite(material.emissiveFactor) && Finite(material.extension.diffuseFactor)
              && Finite(material.extension.specularFactor);
        // Reuse the semantic property map so validation includes new supported
        // material factors when their animation binding is added.
        MaterialAsset properties = material;
        for (uint32_t property = uint32_t(AnimatedProperty::BaseColor); property < uint32_t(AnimatedProperty::TextureOffset);
             ++property)
        {
            const AnimatedProperty field = static_cast<AnimatedProperty>(property);
            for (float value : GetAnimatedMaterialProperty<TextureId>(properties, field))
            {
                valid &= std::isfinite(value)
                      || (field == AnimatedProperty::AttenuationDistance && value == std::numeric_limits<float>::infinity());
            }
        }
        for (const TextureTransform& transform : material.textureTransforms)
        {
            valid &= ValidTransform(transform);
        }
        for (MaterialFeatures::TextureMember member : MaterialFeatures::textureMembers)
        {
            const MaterialTextureBinding& binding  = material.features.*member;
            valid                                 &= Finite(binding.scale) && ValidTransform(binding.transform);
        }
        const uint32_t  coordinates[]{material.texCoordSets.baseColor, material.texCoordSets.metallicRoughness,
                                      material.texCoordSets.normal, material.texCoordSets.occlusion,
                                      material.texCoordSets.emissive};
        const TextureId textures[]{material.m_pBaseColorTexture, material.m_pMetallicRoughnessTexture,
                                   material.m_pNormalTexture, material.m_pOcclusionTexture, material.m_pEmissiveTexture};
        for (size_t slot = 0; slot < 5; ++slot)
        {
            valid &= !textures[slot].IsValid() || coordinates[slot] < asset.vertexTexCoordSets.size();
        }
        for (MaterialFeatures::TextureMember member : MaterialFeatures::textureMembers)
        {
            const MaterialTextureBinding& binding = material.features.*member;
            valid &= !binding.texture.IsValid() || binding.texCoord < asset.vertexTexCoordSets.size();
        }
    }
    return valid;
}

bool ValidGeometry(const SceneAsset& asset)
{
    bool valid = FiniteArray(asset.vertices);
    for (const VertexTextureCoordinates& coordinates : asset.vertexTexCoords)
    {
        valid &= coordinates.size() <= asset.vertexTexCoordSets.size()
              && kernels::AllFinite(reinterpret_cast<const float*>(coordinates.data()), coordinates.size() * 2);
    }
    HeapVector<uint64_t> sets = asset.vertexTexCoordSets;
    std::sort(sets.begin(), sets.end());
    valid &= std::adjacent_find(sets.begin(), sets.end()) == sets.end();
    for (const MeshAsset& mesh : asset.meshes)
    {
        valid &= FiniteArray(mesh.morphWeights) && mesh.primitives.size() <= UINT32_MAX;
        for (const MeshPrimitiveAsset& primitive : mesh.primitives)
        {
            const uint32_t width  = primitive.topology == MeshTopology::Points    ? 1
                                  : primitive.topology == MeshTopology::Lines     ? 2
                                  : primitive.topology == MeshTopology::Triangles ? 3
                                                                                  : 0;
            valid                &= width > 0 && primitive.indexCount % width == 0;
            if (primitive.vertexCount > 0)
            {
                valid &= Finite(primitive.bounds.GetMin()) && Finite(primitive.bounds.GetMax())
                      && glm::all(glm::lessThanEqual(primitive.bounds.GetMin(), primitive.bounds.GetMax()));
            }
            for (const HeapVector<VertexJointInfluence>& influences : primitive.influences)
            {
                for (const VertexJointInfluence& influence : influences)
                {
                    valid &= Finite(influence.weight) && influence.weight >= 0;
                }
            }
        }
    }
    return valid;
}

uint64_t SourceVertexCount(const MeshPrimitiveAsset& primitive)
{
    uint64_t count = primitive.sourceVertices.empty() ? primitive.vertexCount : 0;
    for (uint32_t source : primitive.sourceVertices)
    {
        count = std::max(count, uint64_t(source) + 1);
    }
    return count;
}

template <class T> bool ValidMorphArray(const HeapVector<T>& values, uint64_t count)
{
    return (values.empty() || values.size() >= count) && FiniteArray(values);
}

bool ValidMorphs(const SceneAsset& asset)
{
    bool valid = true;
    for (const MorphPrimitiveAsset& morph : asset.morphPrimitives)
    {
        valid                &= morph.mesh < asset.meshes.size() && morph.targets.size() <= UINT32_MAX
                             && (morph.weights.empty() || morph.weights.size() == morph.targets.size()) && FiniteArray(morph.weights);
        uint64_t sourceCount  = 0;
        if (valid)
        {
            const MeshAsset& mesh  = asset.meshes[morph.mesh];
            valid                 &= morph.primitive == UINT32_MAX || morph.primitive < mesh.primitives.size();
            for (size_t index = 0; index < mesh.primitives.size(); ++index)
            {
                if (morph.primitive == UINT32_MAX || morph.primitive == index)
                {
                    sourceCount = std::max(sourceCount, SourceVertexCount(mesh.primitives[index]));
                }
            }
        }
        for (const MorphTargetAsset& target : morph.targets)
        {
            valid &= ValidMorphArray(target.positions, sourceCount) && ValidMorphArray(target.normals, sourceCount)
                  && ValidMorphArray(target.tangents, sourceCount) && ValidMorphArray(target.colors, sourceCount)
                  && target.texCoords.size() <= asset.vertexTexCoordSets.size();
            for (const HeapVector<Vec2>& coordinates : target.texCoords)
            {
                valid &= ValidMorphArray(coordinates, sourceCount);
            }
            for (const MorphColorAttributeAsset& colors : target.extraColors)
            {
                valid &= ValidMorphArray(colors.values, sourceCount);
            }
        }
    }
    return valid;
}

uint32_t MorphCount(const SceneAsset& asset, uint32_t mesh)
{
    uint32_t count = 0;
    for (const MorphPrimitiveAsset& morph : asset.morphPrimitives)
    {
        if (morph.mesh == mesh)
        {
            count = std::max(count, static_cast<uint32_t>(morph.targets.size()));
        }
    }
    return count;
}

bool ConsumeGeometryBudget(uint64_t& budget, uint64_t count, uint64_t stride)
{
    const bool valid = count <= budget / stride;
    if (valid)
    {
        budget -= count * stride;
    }
    return valid;
}

bool ValidNodesAndSkins(const SceneAsset& asset, uint64_t geometryBudget)
{
    // Bind/output vertices and the current/bind/copied coordinate streams coexist.
    const uint64_t vertexBytes = 2 * sizeof(Vertex) + 3 * sizeof(VertexTextureCoordinates);
    bool           valid       = ConsumeGeometryBudget(geometryBudget, asset.vertices.size(), vertexBytes)
                              && ConsumeGeometryBudget(geometryBudget, asset.indices.size(), sizeof(uint32_t));
    for (const VertexTextureCoordinates& coordinates : asset.vertexTexCoords)
    {
        valid &= ConsumeGeometryBudget(geometryBudget, coordinates.size(), 3 * sizeof(Vec2));
    }
    for (const SkinAsset& skin : asset.skins)
    {
        valid &= skin.skeleton >= -1 && (skin.skeleton == -1 || size_t(skin.skeleton) < asset.nodes.size())
              && !skin.joints.empty() && skin.joints.size() <= UINT32_MAX
              && (skin.inverseBindMatrices.empty() || skin.inverseBindMatrices.size() == skin.joints.size())
              && FiniteArray(skin.inverseBindMatrices);
        for (uint32_t joint : skin.joints)
        {
            valid &= joint < asset.nodes.size();
        }
    }
    // Runtime instances duplicate deforming geometry. Bound the expanded counts
    // before any uint32_t vertex/index/submesh offsets can wrap during construction.
    uint64_t vertices  = asset.vertices.size();
    uint64_t indices   = asset.indices.size();
    uint64_t submeshes = 0;
    for (const MeshAsset& mesh : asset.meshes)
    {
        submeshes += mesh.primitives.size();
    }
    for (size_t index = 0; valid && index < asset.nodes.size(); ++index)
    {
        const NodeAsset& node = asset.nodes[index];
        valid &= Finite(node.translation) && ValidRotation(node.rotation) && Finite(node.scale) && Finite(node.baseMatrix)
              && FiniteArray(node.morphWeights);
        if (node.mesh.IsValid())
        {
            const uint32_t morphs  = MorphCount(asset, node.mesh.value);
            valid                 &= node.morphWeights.empty() || node.morphWeights.size() == morphs;
            const MeshAsset& mesh  = asset.meshes[node.mesh.value];
            valid                 &= mesh.morphWeights.empty() || mesh.morphWeights.size() == morphs;
            for (const MeshPrimitiveAsset& primitive : mesh.primitives)
            {
                if (node.renderMesh && (node.skin.IsValid() || morphs > 0))
                {
                    vertices += primitive.vertexCount;
                    indices  += primitive.indexCount;
                    valid    &= ConsumeGeometryBudget(geometryBudget, primitive.vertexCount,
                                                      vertexBytes + sizeof(uint32_t) + sizeof(HeapVector<VertexJointInfluence>))
                             && ConsumeGeometryBudget(geometryBudget, primitive.indexCount, sizeof(uint32_t));
                    for (const HeapVector<VertexJointInfluence>& influences : primitive.influences)
                    {
                        valid &= ConsumeGeometryBudget(geometryBudget, influences.size(), sizeof(VertexJointInfluence));
                    }
                    for (uint32_t vertex = 0; valid && !asset.vertexTexCoords.empty() && vertex < primitive.vertexCount;
                         ++vertex)
                    {
                        valid &= ConsumeGeometryBudget(
                            geometryBudget, asset.vertexTexCoords[primitive.firstVertex + vertex].size(), 3 * sizeof(Vec2));
                    }
                }
                if (node.renderMesh && (node.skin.IsValid() || morphs > 0 || !node.materials.empty()))
                {
                    ++submeshes;
                }
                if (valid && node.skin.IsValid())
                {
                    for (const HeapVector<VertexJointInfluence>& influences : primitive.influences)
                    {
                        double total = 0;
                        for (const VertexJointInfluence& influence : influences)
                        {
                            valid &= influence.joint < asset.skins[node.skin.value].joints.size();
                            total += influence.weight;
                        }
                        valid &= total > 0 && std::isfinite(total);
                    }
                }
            }
        }
        valid &= vertices <= UINT32_MAX && indices <= UINT32_MAX && submeshes <= UINT32_MAX;
    }
    return valid;
}

bool ValidAttributesAndVariants(const SceneAsset& asset)
{
    bool valid = true;
    for (const MaterialVariantPrimitiveAsset& variant : asset.variantPrimitives)
    {
        valid &= variant.mesh < asset.meshes.size() && variant.primitive < asset.meshes[variant.mesh].primitives.size()
              && variant.materials.size() == asset.materialVariants.size();
        for (int32_t material : variant.materials)
        {
            valid &= material >= -1 && (material == -1 || size_t(material) < asset.materials.size());
        }
    }
    for (const VertexAttributeAsset& attribute : asset.extraVertexAttributes)
    {
        valid &= attribute.mesh < asset.meshes.size() && attribute.primitive < asset.meshes[attribute.mesh].primitives.size()
              && attribute.components > 0 && attribute.components <= 16
              && (attribute.domain == VertexAttributeDomain::SourceVertex
                  || attribute.domain == VertexAttributeDomain::PrimitiveVertex)
              && attribute.morphTarget >= -1 && FiniteArray(attribute.values);
        if (valid)
        {
            const MeshPrimitiveAsset& primitive = asset.meshes[attribute.mesh].primitives[attribute.primitive];
            const uint64_t            count =
                attribute.domain == VertexAttributeDomain::SourceVertex ? SourceVertexCount(primitive) : primitive.vertexCount;
            valid &=
                attribute.values.size() % attribute.components == 0 && attribute.values.size() / attribute.components >= count;
            if (attribute.morphTarget >= 0)
            {
                bool found = false;
                for (const MorphPrimitiveAsset& morph : asset.morphPrimitives)
                {
                    found |= morph.mesh == attribute.mesh
                          && (morph.primitive == attribute.primitive || morph.primitive == UINT32_MAX)
                          && size_t(attribute.morphTarget) < morph.targets.size();
                }
                valid &= found;
            }
        }
    }
    return valid;
}

uint32_t TargetDimensions(const SceneAsset& asset, const AnimationTarget& target)
{
    uint32_t dimensions = 0;
    switch (target.object)
    {
        case AnimatedObject::Node:
            if (target.index < asset.nodes.size())
            {
                const NodeAsset& node = asset.nodes[target.index];
                switch (target.property)
                {
                    case AnimatedProperty::Translation: dimensions = 3; break;
                    case AnimatedProperty::Scale: dimensions = node.matrixTransform ? 0 : 3; break;
                    case AnimatedProperty::Rotation: dimensions = node.matrixTransform ? 0 : 4; break;
                    case AnimatedProperty::Weights:
                    {
                        const uint32_t count = MorphCount(asset, node.mesh.value);
                        dimensions           = target.element == UINT32_MAX ? count : target.element < count ? 1 : 0;
                        break;
                    }
                    case AnimatedProperty::Visible:
                    case AnimatedProperty::Selectable:
                    case AnimatedProperty::Hoverable: dimensions = 1; break;
                    default: break;
                }
            }
            break;
        case AnimatedObject::Material:
            if (target.index < asset.materials.size())
            {
                MaterialAsset fields;
                dimensions = static_cast<uint32_t>(GetAnimatedMaterialProperty<TextureId>(fields, target.property).size());
                if (target.property == AnimatedProperty::TextureOffset || target.property == AnimatedProperty::TextureScale
                    || target.property == AnimatedProperty::TextureRotation)
                {
                    dimensions = target.textureSlot < 5 + MaterialFeatures::textureMembers.size()
                                   ? (target.property == AnimatedProperty::TextureRotation ? 1 : 2)
                                   : 0;
                }
            }
            break;
        case AnimatedObject::Camera:
            dimensions = target.index < asset.cameras.size() && target.property >= AnimatedProperty::CameraAspect
                              && target.property <= AnimatedProperty::CameraYMag
                           ? 1
                           : 0;
            break;
        case AnimatedObject::Light:
            if (target.index < asset.lights.size() && target.property >= AnimatedProperty::LightColor
                && target.property <= AnimatedProperty::LightOuterCone)
            {
                dimensions = target.property == AnimatedProperty::LightColor ? 3 : 1;
            }
            break;
        case AnimatedObject::Environment:
            if (target.index < asset.imageBasedLights.size())
            {
                dimensions = target.property == AnimatedProperty::EnvironmentIntensity ? 1
                           : target.property == AnimatedProperty::EnvironmentRotation  ? 4
                                                                                       : 0;
            }
            break;
        default: break;
    }
    if (target.property != AnimatedProperty::Weights && target.element != UINT32_MAX)
    {
        dimensions = 0;
    }
    return dimensions;
}

void Reject(bool& valid, std::string& failure, const char* reason)
{
    if (valid)
    {
        valid   = false;
        failure = reason;
    }
}

uint32_t ChannelDimensions(const SceneAsset& asset, const AnimationChannel& channel)
{
    uint32_t dimensions = 0;
    if (channel.path == AnimationPath::Property)
    {
        dimensions = TargetDimensions(asset, channel.target);
        dimensions = channel.target.dimensions == dimensions ? dimensions : 0;
    }
    else if (channel.node < asset.nodes.size())
    {
        switch (channel.path)
        {
            case AnimationPath::Translation:
            case AnimationPath::Scale: dimensions = 3; break;
            case AnimationPath::Rotation: dimensions = 4; break;
            case AnimationPath::Weights: dimensions = MorphCount(asset, asset.nodes[channel.node].mesh.value); break;
            default: break;
        }
    }
    return dimensions;
}

void ValidateChannel(const SceneAsset&       asset,
                     const AnimationAsset&   animation,
                     const AnimationChannel& channel,
                     bool&                   valid,
                     std::string&            failure)
{
    const uint32_t dimensions = ChannelDimensions(asset, channel);
    if (channel.sampler >= animation.samplers.size())
    {
        Reject(valid, failure, "a channel references a missing sampler");
    }
    else if (dimensions == 0)
    {
        Reject(valid, failure, "a channel targets a missing or non-animatable property");
    }
    else
    {
        const AnimationSampler& sampler = animation.samplers[channel.sampler];
        const uint64_t          triples = sampler.interpolation == AnimationInterpolation::CubicSpline ? 3 : 1;
        if (sampler.times.size() > UINT64_MAX / dimensions / triples
            || uint64_t(sampler.times.size()) * dimensions * triples != sampler.values.size())
        {
            Reject(valid, failure, "a sampler's value count does not match its keys and target");
        }
        const bool rotation = channel.path == AnimationPath::Rotation
                           || (channel.path == AnimationPath::Property && IsQuaternionTarget(channel.target));
        for (size_t key = 0; valid && rotation && key < sampler.times.size(); ++key)
        {
            const size_t offset = (key * triples + (triples == 3 ? 1 : 0)) * 4;
            if (!ValidRotation(Quat(sampler.values[offset + 3], sampler.values[offset], sampler.values[offset + 1],
                                    sampler.values[offset + 2])))
            {
                Reject(valid, failure, "a rotation key has zero length");
            }
        }
    }
}

bool ValidAnimations(const SceneAsset& asset, std::string& failure)
{
    bool valid = true;
    for (const AnimationAsset& animation : asset.animations)
    {
        std::string reason;
        if (valid && !ValidateAnimationAsset(asset, animation, reason))
        {
            valid   = false;
            failure = "animation '" + animation.name + "': " + reason;
        }
    }
    return valid;
}

bool ValidLightsAndCameras(const SceneAsset& asset)
{
    bool valid = asset.imageBasedLight >= -1
              && (asset.imageBasedLight == -1 || size_t(asset.imageBasedLight) < asset.imageBasedLights.size());
    for (const CameraAsset& camera : asset.cameras)
    {
        valid &= Finite(camera.aspect) && Finite(camera.verticalFov) && Finite(camera.nearPlane) && Finite(camera.farPlane)
              && Finite(camera.xmag) && Finite(camera.ymag);
    }
    for (const LightAsset& light : asset.lights)
    {
        valid &= light.kind >= LightKind::Directional && light.kind <= LightKind::Spot && Finite(light.color)
              && Finite(light.intensity) && Finite(light.range) && Finite(light.innerConeAngle) && Finite(light.outerConeAngle);
    }
    for (const ImageBasedLightAsset& light : asset.imageBasedLights)
    {
        const bool compressed = light.specularFormat == Format::BC6H_UFLOAT_BLOCK;
        valid &= Finite(light.intensity) && ValidRotation(light.rotation) && light.irradianceCoefficients.size() == 9
              && FiniteArray(light.irradianceCoefficients) && light.size > 0 && light.mipLevels > 0
              && light.mipLevels <= MipCount(light.size, light.size)
              && (compressed ? light.specularMipFaces.empty() && light.specularBlockMipFaces
                                   && uint64_t(light.mipLevels) * 6 == light.specularBlockMipFaces->size()
                             : light.specularFormat == Format::R32G32B32A32_SFLOAT && !light.specularBlockMipFaces
                                   && uint64_t(light.mipLevels) * 6 == light.specularMipFaces.size());
        for (uint32_t face = 0; valid && face < light.mipLevels * 6; ++face)
        {
            const uint32_t extent = std::max(1u, light.size >> (face / 6));
            if (compressed)
            {
                const HeapVector<uint8_t>& blocks = (*light.specularBlockMipFaces)[face];
                uint64_t                   bytes  = 0;
                valid = zenasset::GetImageLevelByteSize(zenasset::PixelFormat::BC6HUfloat, extent, extent, bytes)
                     && bytes == blocks.size();
                for (size_t offset = 0; valid && offset < blocks.size(); offset += 16)
                {
                    const uint8_t mode = blocks[offset] & 31;
                    valid              = mode != 19 && mode != 23 && mode != 27 && mode != 31;
                }
            }
            else
            {
                valid &= light.specularMipFaces[face].size() == uint64_t(extent) * extent
                      && FiniteArray(light.specularMipFaces[face]);
            }
        }
    }
    return valid;
}
} // namespace

bool ValidateAnimationAsset(const SceneAsset& asset, const AnimationAsset& animation, std::string& failure)
{
    bool valid = true;
    for (const AnimationSampler& sampler : animation.samplers)
    {
        if (sampler.interpolation < AnimationInterpolation::Linear
            || sampler.interpolation > AnimationInterpolation::CubicSpline || sampler.components == 0 || sampler.times.empty()
            || !FiniteArray(sampler.values))
        {
            Reject(valid, failure, "a sampler has an invalid interpolation, no keys or non-finite values");
        }
        for (size_t key = 0; valid && key < sampler.times.size(); ++key)
        {
            if (!Finite(sampler.times[key]) || sampler.times[key] < 0
                || (key > 0 && sampler.times[key] <= sampler.times[key - 1]))
            {
                Reject(valid, failure, "key times are not finite, non-negative and strictly increasing");
            }
        }
    }
    for (const AnimationChannel& channel : animation.channels)
    {
        if (valid)
        {
            ValidateChannel(asset, animation, channel, valid, failure);
        }
    }
    return valid;
}

bool ValidateSceneAsset(const SceneAsset& asset, uint64_t maxRuntimeGeometryBytes, std::string* failure)
{
    // Signed runtime indices reserve -1, and texture/material construction adds
    // default records. These checks precede every narrowing conversion.
    const bool counts =
        asset.nodes.size() <= INT32_MAX && asset.meshes.size() <= INT32_MAX && asset.materials.size() < INT32_MAX
        && asset.images.size() <= INT32_MAX && asset.textures.size() < INT32_MAX - 5 && asset.samplers.size() <= INT32_MAX
        && asset.skins.size() <= INT32_MAX && asset.animations.size() <= INT32_MAX && asset.morphPrimitives.size() <= INT32_MAX
        && asset.cameras.size() <= INT32_MAX && asset.lights.size() <= INT32_MAX && asset.imageBasedLights.size() <= INT32_MAX;
    std::string reason;
    bool        valid = true;
    if (!counts)
    {
        Reject(valid, reason, "object counts exceed runtime index limits");
    }
    else if (!ValidateReferences(asset))
    {
        Reject(valid, reason, "invalid object references, primitive ranges or node hierarchy");
    }
    else if (!ValidImages(asset))
    {
        Reject(valid, reason, "invalid image, texture or sampler data");
    }
    else if (!ValidMaterials(asset))
    {
        Reject(valid, reason, "invalid material values or texture coordinate sets");
    }
    else if (!ValidGeometry(asset))
    {
        Reject(valid, reason, "invalid vertex, index or primitive data");
    }
    else if (!ValidMorphs(asset))
    {
        Reject(valid, reason, "invalid morph targets");
    }
    else if (!ValidNodesAndSkins(asset, maxRuntimeGeometryBytes))
    {
        Reject(valid, reason, "invalid node transforms, morph weights, skins or runtime geometry budget");
    }
    else if (!ValidAttributesAndVariants(asset))
    {
        Reject(valid, reason, "invalid custom vertex attributes or material variants");
    }
    else if (!ValidAnimations(asset, reason))
    {
        valid = false;
    }
    else if (!ValidLightsAndCameras(asset))
    {
        Reject(valid, reason, "invalid lights, cameras or image-based lighting");
    }
    if (failure != nullptr)
    {
        *failure = std::move(reason);
    }
    return valid;
}
} // namespace zen::asset
