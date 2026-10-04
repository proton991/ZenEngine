#include "Editor/Model/InspectorNavigation.h"

namespace zen::editor
{
namespace
{
constexpr size_t kHistoryLimit = 64;

void Visit(InspectorTab& tab, InspectionTarget target)
{
    if (tab.GetTarget() != target)
    {
        // Navigating after Back replaces the forward branch, as in a browser.
        tab.history.resize(tab.cursor + 1);

        tab.history.push_back(target);

        if (tab.history.size() > kHistoryLimit)
        {
            tab.history.erase(tab.history.begin());
        }

        tab.cursor = tab.history.size() - 1;
    }
}
} // namespace

InspectionTarget InspectorTab::GetTarget() const
{
    return history.empty() ? InspectionTarget{} : history[cursor];
}

bool InspectorTab::CanGoBack() const
{
    return id != 0 && cursor > 0;
}

bool InspectorTab::CanGoForward() const
{
    return id != 0 && cursor + 1 < history.size();
}

InspectorNavigation::InspectorNavigation(const EditorScene& scene, const EditorSelection& selection) :
    m_scene(scene), m_selection(selection)
{
    Synchronize();
}

void InspectorNavigation::RegisterActions(EditorActions& registry)
{
    const uint16_t alt = uint16_t(platform::KeyModifier::Alt);

    registry.Register({actions::InspectorBack,
                       "Back",
                       {platform::Key::Left, alt},
                       "No previous inspection page.",
                       [this]() { return IsSynchronized() && GetActiveTab().CanGoBack(); },
                       [this]() { Back(); },
                       EditorShortcutScope::Inspector});

    registry.Register({actions::InspectorForward,
                       "Forward",
                       {platform::Key::Right, alt},
                       "No next inspection page.",
                       [this]() { return IsSynchronized() && GetActiveTab().CanGoForward(); },
                       [this]() { Forward(); },
                       EditorShortcutScope::Inspector});
}

bool InspectorNavigation::IsSynchronized() const
{
    return m_generation == m_scene.GetGeneration() && m_selectionRevision == m_selection.GetRevision();
}

void InspectorNavigation::Synchronize()
{
    if (m_tabs.empty() || m_generation != m_scene.GetGeneration())
    {
        m_tabs.clear();

        m_tabs.emplace_back();

        m_active            = 0;

        m_generation        = m_scene.GetGeneration();

        m_selectionRevision = UINT64_MAX;
    }

    if (m_selectionRevision != m_selection.GetRevision())
    {
        m_tabs[0].history   = {{m_selection.GetNode(), m_selection.GetAsset()}};

        m_selectionRevision = m_selection.GetRevision();

        // An explicit selection brings its Inspector page into view without replacing
        // any open reference tabs, including a click on the already selected item.
        m_active = 0;
    }
}

bool InspectorNavigation::IsValid(InspectionTarget target) const
{
    const bool node  = target.node.generation != 0;

    const bool asset = target.asset.generation != 0;

    return node != asset && (node ? m_scene.Resolve(target.node) != nullptr : m_scene.GetAssets().Contains(target.asset));
}

void InspectorNavigation::Open(InspectionTarget target, bool newTab)
{
    Synchronize();

    if (IsValid(target) && target != GetActiveTab().GetTarget())
    {
        if (m_active == 0 || newTab)
        {
            size_t existing = m_tabs.size();

            for (size_t index = 1; index < m_tabs.size(); ++index)
            {
                if (m_tabs[index].GetTarget() == target)
                {
                    existing = index;

                    break;
                }
            }

            if (existing == m_tabs.size())
            {
                InspectorTab tab;

                tab.id                        = m_nextId++;

                const InspectionTarget source = GetActiveTab().GetTarget();

                if (IsValid(source))
                {
                    tab.history.push_back(source);
                }

                tab.history.push_back(target);

                tab.cursor = tab.history.size() - 1;

                m_tabs.push_back(std::move(tab));
            }

            m_active = existing;
        }
        else
        {
            Visit(m_tabs[m_active], target);
        }
    }
}

void InspectorNavigation::Back()
{
    Synchronize();

    if (GetActiveTab().CanGoBack())
    {
        --m_tabs[m_active].cursor;
    }
}

void InspectorNavigation::Forward()
{
    Synchronize();

    if (GetActiveTab().CanGoForward())
    {
        ++m_tabs[m_active].cursor;
    }
}

void InspectorNavigation::Activate(uint32_t id)
{
    Synchronize();

    for (size_t index = 0; index < m_tabs.size(); ++index)
    {
        if (m_tabs[index].id == id)
        {
            m_active = index;

            break;
        }
    }
}

void InspectorNavigation::Close(uint32_t id)
{
    Synchronize();

    for (size_t index = 1; index < m_tabs.size(); ++index)
    {
        if (m_tabs[index].id == id)
        {
            m_tabs.erase(m_tabs.begin() + index);

            if (m_active >= index)
            {
                --m_active;
            }

            break;
        }
    }
}

const HeapVector<InspectorTab>& InspectorNavigation::GetTabs() const
{
    return m_tabs;
}

const InspectorTab& InspectorNavigation::GetActiveTab() const
{
    return m_tabs[m_active];
}
} // namespace zen::editor
