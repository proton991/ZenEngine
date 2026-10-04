#pragma once
#include "Editor/Model/EditorIds.h"
#include "SceneGraph/Scene.h"
#include "SceneGraph/Material.h"
#include "SceneGraph/Mesh.h"

namespace zen::editor
{
// Typed list values; frontends choose wording and units.
struct SceneAssetItem
{
    SceneAssetId  id;
    std::string   name;
    uint32_t      users{0};
    uint32_t      width{0};
    uint32_t      height{0};
    asset::Format format{asset::Format::UNDEFINED};
    // Linear base color factor and the texture it modulates, if any.
    Vec4         baseColor{1.0f};
    SceneAssetId baseColorTexture;
    uint32_t     primitives{0};
    uint32_t     triangles{0};
    uint32_t     channels{0};
    float        duration{0.0f};
};

struct MaterialTextureSlot
{
    const char*  slot{""};
    SceneAssetId texture;
};

struct SceneAssetInspection
{
    bool           valid{false};
    SceneAssetItem item;
    uint32_t       mipLevels{0};
    // Imported CPU data, including authored mip levels.
    uint64_t      bytes{0};
    int           sampler{-1};
    sg::AlphaMode alphaMode{sg::AlphaMode::Opaque};
    float         metallic{0.0f};
    float         roughness{1.0f};
    float         alphaCutoff{0.5f};
    Vec3          emissive{0.0f};
    bool          doubleSided{false};
    bool          unlit{false};
    // Material texture slots; engine placeholder textures are omitted.
    HeapVector<MaterialTextureSlot> textures;
    sg::AABB                        bounds;
    HeapVector<SceneAssetId>        materials;
    // Materials that sample a texture, or meshes that draw a material.
    HeapVector<SceneAssetId> usedBy;
    // Nodes that instance a mesh or material, or that an animation targets.
    HeapVector<NodeId> nodes;
};

// Encoded RGBA8 preview in the texture's stored color encoding, for direct UI display.
struct TexturePreview
{
    uint32_t            width{0};
    uint32_t            height{0};
    HeapVector<uint8_t> pixels;
};

// Box-filters uncompressed RGBA8 textures; compressed and floating-point data return false.
bool MakeTexturePreview(const sg::Texture& texture, uint32_t maxSize, TexturePreview& preview);

// Resources of one loaded scene. Engine placeholder textures, linear-data copies of
// color textures and an unused default material are internal and are not listed.
class SceneAssetIndex
{
public:
    void Build(const sg::Scene* scene, uint64_t generation);

    void Clear();

    HeapVector<SceneAssetItem> Query(const std::string& search) const;

    // List values for one asset, built with the index; stale IDs return an item without a name.
    SceneAssetItem Describe(SceneAssetId id) const;

    SceneAssetInspection Inspect(SceneAssetId id) const;

    bool Contains(SceneAssetId id) const;

    const sg::Texture* ResolveTexture(SceneAssetId id) const;

    const sg::Mesh* ResolveMesh(SceneAssetId id) const;

    // The listed mesh asset for a scene mesh; empty for null or unknown meshes.
    SceneAssetId FindMesh(const sg::Mesh* mesh) const;

private:
    SceneAssetId FindTexture(const sg::Texture* texture) const;

    SceneAssetId FindMaterial(const sg::Material* material) const;

    SceneAssetItem MakeItem(SceneAssetId id) const;

    uint32_t GetCount(SceneAssetKind kind) const;

    const sg::Scene*                       m_scene{nullptr};
    uint64_t                               m_generation{0};
    HeapVector<const sg::Mesh*>            m_meshes;
    HeapVector<const sg::Material*>        m_materials;
    HeapVector<const sg::Texture*>         m_textures;
    HashMap<const sg::Texture*, uint32_t>  m_textureIndices;
    HashMap<const sg::Material*, uint32_t> m_materialIndices;
    HashMap<const sg::Mesh*, uint32_t>     m_meshIndices;
    HeapVector<HeapVector<uint32_t>>       m_textureMaterials;
    HeapVector<HeapVector<uint32_t>>       m_materialMeshes;
    HeapVector<HeapVector<NodeId>>         m_materialNodes;
    // Items in kind order; m_firstItem[kind] is the position of each kind's first item.
    HeapVector<SceneAssetItem> m_items;
    uint32_t                   m_firstItem[4]{};
};
} // namespace zen::editor
