#pragma once
#include "Editor/Model/EditorCamera.h"
#include "Editor/Model/EditorScene.h"

namespace zen::editor
{
// Node and asset selection are exclusive; selecting one clears the other. Every change
// advances the revision, so an asynchronous result can tell that the user moved on.
class EditorSelection
{
public:
    explicit EditorSelection(const EditorScene& scene);

    // IDs that do not resolve in the current scene clear the selection.
    void SelectNode(NodeId id);

    void SelectAsset(SceneAssetId id);

    void Clear();

    // Empty after the scene is replaced, even before Clear() runs.
    NodeId GetNode() const;

    SceneAssetId GetAsset() const;

    uint64_t GetRevision() const;

private:
    const EditorScene& m_scene;
    NodeId             m_node;
    SceneAssetId       m_asset;
    uint64_t           m_revision{0};
};

// Inputs that decide whether a GPU pick result still describes what the user sees.
struct PickStamp
{
    uint64_t scene{0};
    uint64_t camera{0};
    uint64_t target{0};
    uint64_t selection{0};

    bool operator==(const PickStamp&) const = default;
};

PickStamp MakePickStamp(const EditorScene&     scene,
                        const EditorCamera&    camera,
                        const EditorSelection& selection,
                        uint64_t               target);
} // namespace zen::editor
