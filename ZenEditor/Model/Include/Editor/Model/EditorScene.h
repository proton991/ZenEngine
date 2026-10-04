#pragma once
#include "Editor/Model/NodeInspection.h"
#include "Editor/Model/SceneAssets.h"
#include "AssetLib/Types.h"

namespace zen::editor
{
struct LoadedScene
{
    UniquePtr<sg::Scene>      scene;
    HeapVector<asset::Vertex> vertices;
    HeapVector<uint32_t>      indices;
    std::string               path;
    Vec3                      normalizationCenter{0.0f};
    float                     normalizationScale{1.0f};
};

// Parses a glTF or GLB file on the calling thread. Invalid input returns null and an error.
UniquePtr<LoadedScene> ParseScene(const std::string& path, std::string& error);

// The open scene and its read-only queries. Replace() advances the generation, which
// invalidates every NodeId and SceneAssetId issued for the previous scene. Lookups and
// child lists are built once per scene, so queries do not scan every node.
class EditorScene
{
public:
    void Replace(UniquePtr<LoadedScene> scene);

    const LoadedScene* Get() const;

    uint64_t GetGeneration() const;

    const sg::Node* Resolve(NodeId id) const;

    // Authored name, or a component-based label with the node index. Display only;
    // the imported name and stable identity remain unchanged.
    std::string GetNodeDisplayName(NodeId id) const;

    const HeapVector<NodeId>& GetRoots() const;

    const HeapVector<NodeId>& GetChildren(NodeId id) const;

    // Nodes whose display names match, with their ancestors; keyed by node index.
    HashMap<uint32_t, bool> FilterHierarchy(const std::string& search) const;

    size_t GetNodeCount() const;

    NodeInspection Inspect(NodeId id) const;

    // World bounds of the node and its descendants, or a small box at a node without meshes.
    bool GetBounds(NodeId id, sg::AABB& bounds) const;

    bool GetSceneBounds(sg::AABB& bounds) const;

    // Current pose bounds in mesh vertex space, including normalization baked into skinned vertices.
    bool GetMeshBounds(SceneAssetId mesh, sg::AABB& bounds) const;

    // Nearest visible, selectable mesh node whose bounds the ray crosses.
    NodeId PickBounds(Vec3 origin, Vec3 direction) const;

    const SceneAssetIndex& GetAssets() const;

private:
    UniquePtr<LoadedScene>                m_scene;
    uint64_t                              m_generation{0};
    HashMap<uint32_t, const sg::Node*>    m_nodes;
    HashMap<uint32_t, HeapVector<NodeId>> m_children;
    HeapVector<NodeId>                    m_roots;
    SceneAssetIndex                       m_assets;
};
} // namespace zen::editor
