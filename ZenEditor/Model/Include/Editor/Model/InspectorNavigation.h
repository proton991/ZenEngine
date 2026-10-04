#pragma once
#include "Editor/Model/EditorActions.h"
#include "Editor/Model/EditorSelection.h"

namespace zen::editor
{
// One Inspector page. Exactly one identity is populated; both empty means no selection.
struct InspectionTarget
{
    NodeId       node;
    SceneAssetId asset;

    bool operator==(const InspectionTarget&) const = default;
};

struct InspectorTab
{
    // Zero is the permanent tab that follows scene/asset selection.
    uint32_t                     id{0};
    HeapVector<InspectionTarget> history;
    size_t                       cursor{0};

    InspectionTarget GetTarget() const;

    bool CanGoBack() const;

    bool CanGoForward() const;
};

// Toolkit-independent browsing state. Inspecting references never changes selection.
// The Selection tab follows external selection; other tabs retain their own history.
class InspectorNavigation
{
public:
    InspectorNavigation(const EditorScene& scene, const EditorSelection& selection);

    // Registers commands once; this navigation object must outlive the registry.
    void RegisterActions(EditorActions& registry);

    // Explicit update before reading pages/command availability. Scene replacement
    // discards tabs containing old identities. Getters never perform this update.
    void Synchronize();

    // Links from Selection open a tab; links in other tabs append to their history.
    // newTab explicitly opens/focuses a separate tab. New tabs retain the source page
    // as their first history entry, so Back can return to the originating node/asset.
    void Open(InspectionTarget target, bool newTab = false);

    void Back();

    void Forward();

    void Activate(uint32_t id);

    void Close(uint32_t id);

    const HeapVector<InspectorTab>& GetTabs() const;

    const InspectorTab& GetActiveTab() const;

private:
    bool IsSynchronized() const;

    bool IsValid(InspectionTarget target) const;

    const EditorScene&       m_scene;
    const EditorSelection&   m_selection;
    HeapVector<InspectorTab> m_tabs;
    size_t                   m_active{0};
    uint32_t                 m_nextId{1};
    uint64_t                 m_generation{0};
    uint64_t                 m_selectionRevision{UINT64_MAX};
};
} // namespace zen::editor
