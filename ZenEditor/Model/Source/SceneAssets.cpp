#include "Editor/Model/SceneAssets.h"
#include "Editor/Model/EditorText.h"
#include <algorithm>
#include <cmath>

namespace zen::editor
{
namespace
{
struct TextureBinding
{
    const char*        slot{""};
    const sg::Texture* texture{nullptr};
};

HeapVector<TextureBinding> GetTextureBindings(const sg::Material& material)
{
    const sg::MaterialFeatures& features = material.features;

    const TextureBinding bindings[]      = {{"Base Color", material.m_pBaseColorTexture},
                                            {"Metallic Roughness", material.m_pMetallicRoughnessTexture},
                                            {"Normal", material.m_pNormalTexture},
                                            {"Occlusion", material.m_pOcclusionTexture},
                                            {"Emissive", material.m_pEmissiveTexture},
                                            {"Diffuse", features.diffuseTexture.texture},
                                            {"Specular Glossiness", features.specularGlossinessTexture.texture},
                                            {"Specular", features.specularTexture.texture},
                                            {"Specular Color", features.specularColorTexture.texture},
                                            {"Clearcoat", features.clearcoatTexture.texture},
                                            {"Clearcoat Roughness", features.clearcoatRoughnessTexture.texture},
                                            {"Clearcoat Normal", features.clearcoatNormalTexture.texture},
                                            {"Sheen Color", features.sheenColorTexture.texture},
                                            {"Sheen Roughness", features.sheenRoughnessTexture.texture},
                                            {"Transmission", features.transmissionTexture.texture},
                                            {"Thickness", features.thicknessTexture.texture},
                                            {"Iridescence", features.iridescenceTexture.texture},
                                            {"Iridescence Thickness", features.iridescenceThicknessTexture.texture},
                                            {"Anisotropy", features.anisotropyTexture.texture},
                                            {"Diffuse Transmission", features.diffuseTransmissionTexture.texture},
                                            {"Diffuse Transmission Color", features.diffuseTransmissionColorTexture.texture},
                                            {"Retroreflection", features.retroreflectionTexture.texture}};

    HeapVector<TextureBinding> result;

    for (const TextureBinding& binding : bindings)
    {
        if (binding.texture != nullptr)
        {
            result.push_back(binding);
        }
    }

    return result;
}

bool IsPlaceholder(const sg::Texture* texture, const sg::Scene::DefaultTextures& defaults)
{
    return texture == defaults.pBaseColor || texture == defaults.pMetallicRoughness || texture == defaults.pNormal
        || texture == defaults.pEmissive || texture == defaults.pOcclusion;
}

// Only the importer can identify a generated linear view; display names are not identities.
const sg::Texture* FindLinearSource(const sg::Texture& texture, const HeapVector<sg::Texture*>& textures)
{
    const sg::Texture* result = nullptr;

    if (texture.linearSourceIndex != UINT32_MAX)
    {
        for (const sg::Texture* candidate : textures)
        {
            if (result == nullptr && candidate != &texture && candidate->index == texture.linearSourceIndex)
            {
                result = candidate;
            }
        }
    }

    return result;
}

template <class T> void AppendUnique(HeapVector<T>& values, const T& value)
{
    if (std::find(values.begin(), values.end(), value) == values.end())
    {
        values.push_back(value);
    }
}

std::string DisplayName(const std::string& name, const char* kind, uint32_t index)
{
    return name.empty() ? std::string(kind) + " " + std::to_string(index) : name;
}

const sg::Texture* GetBaseColorTexture(const sg::Material& material)
{
    return material.pbrWorkflows.specularGlossiness ? material.features.diffuseTexture.texture : material.m_pBaseColorTexture;
}

const uint8_t* GetLevelZero(const sg::Texture& texture, size_t& size)
{
    const uint8_t* result = nullptr;

    if (texture.mipBytes.empty())
    {
        result = texture.bytesData.data();

        size   = texture.bytesData.size();
    }
    else
    {
        result = texture.mipBytes[0].data();

        size   = texture.mipBytes[0].size();
    }

    return result;
}
} // namespace

bool MakeTexturePreview(const sg::Texture& texture, uint32_t maxSize, TexturePreview& preview)
{
    size_t size          = 0;

    const uint8_t* level = GetLevelZero(texture, size);

    const bool valid     = (texture.format == asset::Format::R8G8B8A8_UNORM || texture.format == asset::Format::R8G8B8A8_SRGB)
                    && texture.width > 0 && texture.height > 0 && maxSize > 0
                    && size >= size_t(texture.width) * texture.height * 4;

    preview = {};

    if (valid)
    {
        const float scale = std::max(1.0f, float(std::max(texture.width, texture.height)) / float(maxSize));

        preview.width     = std::max(1u, uint32_t(std::lround(texture.width / scale)));

        preview.height    = std::max(1u, uint32_t(std::lround(texture.height / scale)));

        preview.pixels.resize(size_t(preview.width) * preview.height * 4);

        const uint32_t taps = std::min(4u, uint32_t(std::ceil(scale)));

        for (uint32_t y = 0; y < preview.height; ++y)
        {
            for (uint32_t x = 0; x < preview.width; ++x)
            {
                uint32_t sum[3] = {0, 0, 0};

                for (uint32_t tapY = 0; tapY < taps; ++tapY)
                {
                    for (uint32_t tapX = 0; tapX < taps; ++tapX)
                    {
                        const float u           = (float(x) + (float(tapX) + 0.5f) / float(taps)) / float(preview.width);

                        const float v           = (float(y) + (float(tapY) + 0.5f) / float(taps)) / float(preview.height);

                        const uint32_t sourceX  = std::min(texture.width - 1, uint32_t(u * float(texture.width)));

                        const uint32_t sourceY  = std::min(texture.height - 1, uint32_t(v * float(texture.height)));

                        const uint8_t* texel    = level + (size_t(sourceY) * texture.width + sourceX) * 4;

                        sum[0]                 += texel[0];

                        sum[1]                 += texel[1];

                        sum[2]                 += texel[2];
                    }
                }

                uint8_t* output = preview.pixels.data() + (size_t(y) * preview.width + x) * 4;

                for (uint32_t channel = 0; channel < 3; ++channel)
                {
                    output[channel] = uint8_t((sum[channel] + taps * taps / 2) / (taps * taps));
                }

                // Previews show color only; transparent texels would otherwise be invisible.
                output[3] = 255;
            }
        }
    }

    return valid;
}

void SceneAssetIndex::Clear()
{
    m_scene      = nullptr;

    m_generation = 0;

    m_meshes.clear();

    m_materials.clear();

    m_textures.clear();

    m_textureIndices.clear();

    m_materialIndices.clear();

    m_meshIndices.clear();

    m_textureMaterials.clear();

    m_materialMeshes.clear();

    m_materialNodes.clear();

    m_items.clear();

    std::fill(std::begin(m_firstItem), std::end(m_firstItem), 0u);
}

void SceneAssetIndex::Build(const sg::Scene* scene, uint64_t generation)
{
    Clear();

    if (scene != nullptr)
    {
        m_scene                                   = scene;

        m_generation                              = generation;

        const sg::Scene::DefaultTextures defaults = scene->GetDefaultTextures();

        const HeapVector<sg::Texture*> textures   = scene->GetComponents<sg::Texture>();

        HeapVector<std::pair<const sg::Texture*, const sg::Texture*>> copies;

        for (const sg::Texture* texture : textures)
        {
            const sg::Texture* source = FindLinearSource(*texture, textures);

            if (source != nullptr)
            {
                copies.emplace_back(texture, source);
            }
            else if (!IsPlaceholder(texture, defaults))
            {
                m_textureIndices[texture] = uint32_t(m_textures.size());

                m_textures.push_back(texture);
            }
        }

        for (const std::pair<const sg::Texture*, const sg::Texture*>& copy : copies)
        {
            if (m_textureIndices.count(copy.second) != 0)
            {
                m_textureIndices[copy.first] = m_textureIndices[copy.second];
            }
        }

        for (const sg::Mesh* mesh : scene->GetComponents<sg::Mesh>())
        {
            m_meshIndices[mesh] = uint32_t(m_meshes.size());

            m_meshes.push_back(mesh);
        }

        HashMap<const sg::Material*, HeapVector<uint32_t>> meshesByMaterial;

        HashMap<const sg::Material*, HeapVector<NodeId>> nodesByMaterial;

        for (uint32_t meshIndex = 0; meshIndex < m_meshes.size(); ++meshIndex)
        {
            for (const sg::SubMesh* primitive : m_meshes[meshIndex]->GetSubMeshes())
            {
                const sg::Material* material = primitive->GetMaterial();

                if (material != nullptr)
                {
                    AppendUnique(meshesByMaterial[material], meshIndex);

                    for (const sg::Node* node : m_meshes[meshIndex]->GetNodes())
                    {
                        AppendUnique(nodesByMaterial[material], NodeId{generation, node->GetIndex()});
                    }
                }
            }
        }

        for (const sg::Material* material : scene->GetComponents<sg::Material>())
        {
            const bool used = meshesByMaterial.count(material) != 0;

            if (used || material->GetName() != "DefaultMaterial")
            {
                m_materialIndices[material] = uint32_t(m_materials.size());

                m_materials.push_back(material);

                m_materialMeshes.push_back(used ? meshesByMaterial[material] : HeapVector<uint32_t>());

                m_materialNodes.push_back(used ? nodesByMaterial[material] : HeapVector<NodeId>());
            }
        }

        m_textureMaterials.resize(m_textures.size());

        for (uint32_t materialIndex = 0; materialIndex < m_materials.size(); ++materialIndex)
        {
            for (const TextureBinding& binding : GetTextureBindings(*m_materials[materialIndex]))
            {
                const SceneAssetId texture = FindTexture(binding.texture);

                if (texture.generation != 0)
                {
                    AppendUnique(m_textureMaterials[texture.index], materialIndex);
                }
            }
        }

        for (uint32_t kind = 0; kind < 4; ++kind)
        {
            m_firstItem[kind] = uint32_t(m_items.size());

            for (uint32_t index = 0; index < GetCount(SceneAssetKind(kind)); ++index)
            {
                m_items.push_back(MakeItem({m_generation, SceneAssetKind(kind), index}));
            }
        }
    }
}

SceneAssetId SceneAssetIndex::FindTexture(const sg::Texture* texture) const
{
    SceneAssetId result;

    const HashMap<const sg::Texture*, uint32_t>::const_iterator found = m_textureIndices.find(texture);

    if (texture != nullptr && found != m_textureIndices.end())
    {
        result = {m_generation, SceneAssetKind::Texture, found->second};
    }

    return result;
}

SceneAssetId SceneAssetIndex::FindMaterial(const sg::Material* material) const
{
    SceneAssetId result;

    const HashMap<const sg::Material*, uint32_t>::const_iterator found = m_materialIndices.find(material);

    if (material != nullptr && found != m_materialIndices.end())
    {
        result = {m_generation, SceneAssetKind::Material, found->second};
    }

    return result;
}

uint32_t SceneAssetIndex::GetCount(SceneAssetKind kind) const
{
    size_t count = 0;

    if (m_scene != nullptr)
    {
        switch (kind)
        {
            case SceneAssetKind::Mesh: count = m_meshes.size(); break;
            case SceneAssetKind::Material: count = m_materials.size(); break;
            case SceneAssetKind::Texture: count = m_textures.size(); break;
            case SceneAssetKind::Animation: count = m_scene->GetAssetData().animations.size(); break;
        }
    }

    return uint32_t(count);
}

bool SceneAssetIndex::Contains(SceneAssetId id) const
{
    return m_scene != nullptr && id.generation == m_generation && id.index < GetCount(id.kind);
}

const sg::Texture* SceneAssetIndex::ResolveTexture(SceneAssetId id) const
{
    return id.kind == SceneAssetKind::Texture && Contains(id) ? m_textures[id.index] : nullptr;
}

const sg::Mesh* SceneAssetIndex::ResolveMesh(SceneAssetId id) const
{
    return id.kind == SceneAssetKind::Mesh && Contains(id) ? m_meshes[id.index] : nullptr;
}

SceneAssetId SceneAssetIndex::FindMesh(const sg::Mesh* mesh) const
{
    SceneAssetId result;

    const HashMap<const sg::Mesh*, uint32_t>::const_iterator found = m_meshIndices.find(mesh);

    if (mesh != nullptr && found != m_meshIndices.end())
    {
        result = {m_generation, SceneAssetKind::Mesh, found->second};
    }

    return result;
}

SceneAssetItem SceneAssetIndex::Describe(SceneAssetId id) const
{
    SceneAssetItem result;

    result.id = id;

    if (Contains(id))
    {
        result = m_items[m_firstItem[uint32_t(id.kind)] + id.index];
    }

    return result;
}

SceneAssetItem SceneAssetIndex::MakeItem(SceneAssetId id) const
{
    SceneAssetItem item;

    item.id            = id;

    const bool current = Contains(id);

    if (current && id.kind == SceneAssetKind::Mesh)
    {
        const sg::Mesh& mesh = *m_meshes[id.index];

        item.name            = DisplayName(mesh.GetName(), "Mesh", id.index);

        item.users           = uint32_t(mesh.GetNodes().size());

        item.primitives      = uint32_t(mesh.GetSubMeshes().size());

        for (const sg::SubMesh* primitive : mesh.GetSubMeshes())
        {
            item.triangles += primitive->topology == sg::MeshTopology::Triangles ? primitive->GetIndexCount() / 3 : 0;
        }
    }
    else if (current && id.kind == SceneAssetKind::Material)
    {
        const sg::Material& material = *m_materials[id.index];

        item.name                    = DisplayName(material.GetName(), "Material", id.index);

        item.users                   = uint32_t(m_materialNodes[id.index].size());

        item.baseColor = material.pbrWorkflows.specularGlossiness ? material.extension.diffuseFactor : material.baseColorFactor;

        item.baseColorTexture = FindTexture(GetBaseColorTexture(material));
    }
    else if (current && id.kind == SceneAssetKind::Texture)
    {
        const sg::Texture& texture = *m_textures[id.index];

        item.name                  = DisplayName(texture.GetName(), "Texture", id.index);

        item.users                 = uint32_t(m_textureMaterials[id.index].size());

        item.width                 = texture.width;

        item.height                = texture.height;

        item.format                = texture.format;
    }
    else if (current)
    {
        const sg::AnimationAsset& animation = m_scene->GetAssetData().animations[id.index];

        HeapVector<uint32_t> targets;

        for (const sg::AnimationChannel& channel : animation.channels)
        {
            AppendUnique(targets, channel.node);
        }

        for (const sg::AnimationSampler& sampler : animation.samplers)
        {
            item.duration = sampler.times.empty() ? item.duration : std::max(item.duration, sampler.times.back());
        }

        item.name     = DisplayName(animation.name, "Animation", id.index);

        item.users    = uint32_t(targets.size());

        item.channels = uint32_t(animation.channels.size());
    }

    return item;
}

HeapVector<SceneAssetItem> SceneAssetIndex::Query(const std::string& search) const
{
    HeapVector<SceneAssetItem> result;

    for (const SceneAssetItem& item : m_items)
    {
        if (MatchesSearch(item.name, search))
        {
            result.push_back(item);
        }
    }

    return result;
}

SceneAssetInspection SceneAssetIndex::Inspect(SceneAssetId id) const
{
    SceneAssetInspection result;

    result.valid = Contains(id);

    if (result.valid)
    {
        result.item = Describe(id);
    }

    if (result.valid && id.kind == SceneAssetKind::Mesh)
    {
        const sg::Mesh& mesh = *m_meshes[id.index];

        result.bounds        = mesh.GetAABB();

        for (const sg::SubMesh* primitive : mesh.GetSubMeshes())
        {
            const SceneAssetId material = FindMaterial(primitive->GetMaterial());

            if (material.generation != 0)
            {
                AppendUnique(result.materials, material);
            }
        }

        for (const sg::Node* node : mesh.GetNodes())
        {
            result.nodes.push_back({m_generation, node->GetIndex()});
        }
    }
    else if (result.valid && id.kind == SceneAssetKind::Material)
    {
        const sg::Material& material = *m_materials[id.index];

        result.alphaMode             = material.alphaMode;

        result.metallic              = material.metallicFactor;

        result.roughness             = material.roughnessFactor;

        result.alphaCutoff           = material.alphaCutoff;

        result.emissive              = Vec3(material.emissiveFactor) * material.emissiveStrength;

        result.doubleSided           = material.doubleSided;

        result.unlit                 = material.unlit;

        for (const TextureBinding& binding : GetTextureBindings(material))
        {
            const SceneAssetId texture = FindTexture(binding.texture);

            if (texture.generation != 0)
            {
                result.textures.push_back({binding.slot, texture});
            }
        }

        for (uint32_t mesh : m_materialMeshes[id.index])
        {
            result.usedBy.push_back({m_generation, SceneAssetKind::Mesh, mesh});
        }

        result.nodes = m_materialNodes[id.index];
    }
    else if (result.valid && id.kind == SceneAssetKind::Texture)
    {
        const sg::Texture& texture = *m_textures[id.index];

        size_t size                = 0;

        GetLevelZero(texture, size);

        // Without authored levels, upload generates the complete chain from level zero.
        result.mipLevels = texture.mipBytes.empty()
                             ? uint32_t(std::floor(std::log2(float(std::max(1u, std::max(texture.width, texture.height)))))) + 1
                             : uint32_t(texture.mipBytes.size());

        result.bytes     = size;

        for (size_t level = 1; level < texture.mipBytes.size(); ++level)
        {
            result.bytes += texture.mipBytes[level].size();
        }

        result.sampler = texture.samplerIndex;

        for (uint32_t material : m_textureMaterials[id.index])
        {
            result.usedBy.push_back({m_generation, SceneAssetKind::Material, material});
        }
    }
    else if (result.valid)
    {
        HashMap<uint32_t, bool> existing;

        for (const UniquePtr<sg::Node>& node : m_scene->GetNodes())
        {
            existing[node->GetIndex()] = true;
        }

        for (const sg::AnimationChannel& channel : m_scene->GetAssetData().animations[id.index].channels)
        {
            if (existing.count(channel.node) != 0)
            {
                AppendUnique(result.nodes, NodeId{m_generation, channel.node});
            }
        }
    }

    return result;
}
} // namespace zen::editor
