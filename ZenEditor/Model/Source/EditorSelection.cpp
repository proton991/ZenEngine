#include "Editor/Model/EditorSelection.h"

namespace zen::editor
{
EditorSelection::EditorSelection(const EditorScene& scene) : m_scene(scene) {}

void EditorSelection::SelectNode(NodeId id)
{
    m_node  = m_scene.Resolve(id) != nullptr ? id : NodeId{};

    m_asset = {};

    ++m_revision;
}

void EditorSelection::SelectAsset(SceneAssetId id)
{
    m_asset = m_scene.GetAssets().Contains(id) ? id : SceneAssetId{};

    m_node  = {};

    ++m_revision;
}

void EditorSelection::Clear()
{
    m_node  = {};

    m_asset = {};

    ++m_revision;
}

NodeId EditorSelection::GetNode() const
{
    return m_node.generation == m_scene.GetGeneration() ? m_node : NodeId{};
}

SceneAssetId EditorSelection::GetAsset() const
{
    return m_asset.generation == m_scene.GetGeneration() ? m_asset : SceneAssetId{};
}

uint64_t EditorSelection::GetRevision() const
{
    return m_revision;
}

PickStamp MakePickStamp(const EditorScene& scene, const EditorCamera& camera, const EditorSelection& selection, uint64_t target)
{
    return {scene.GetGeneration(), camera.GetRevision(), target, selection.GetRevision()};
}
} // namespace zen::editor
