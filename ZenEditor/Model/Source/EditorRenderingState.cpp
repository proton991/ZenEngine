#include "Editor/Model/EditorRenderingState.h"
#include <algorithm>

namespace zen::editor
{
void EditorRenderingState::Initialize(const rc::RenderingSettings& settings)
{
    if (Stage(settings))
    {
        Commit();
    }
}

bool EditorRenderingState::Stage(const rc::RenderingSettings& settings)
{
    const bool resized = settings.gi.resolution != m_draft.gi.resolution;

    m_draft            = settings;

    if (resized && m_draft.gi.resolution > 0)
    {
        // Keep the selected slice inside the replacement grid before validating the draft.
        m_draft.debug.slice = std::min(m_draft.debug.slice, m_draft.gi.resolution - 1);
    }

    ++m_revision;

    return rc::ValidateRenderingSettings(m_draft, m_error);
}

void EditorRenderingState::Commit()
{
    if (rc::ValidateRenderingSettings(m_draft, m_error))
    {
        m_applied         = m_draft;

        m_appliedRevision = m_revision;

        m_error.clear();
    }
}

rc::RenderingSettings EditorRenderingState::GetPreview() const
{
    rc::RenderingSettings preview = m_draft;

    rc::HoldRenderingResources(m_applied, preview);

    return preview;
}

void EditorRenderingState::CommitPreview()
{
    m_applied = GetPreview();

    // The applied revision advances while the draft stays one ahead with its resource changes.
    m_appliedRevision = ++m_revision;

    ++m_revision;

    m_error.clear();
}

void EditorRenderingState::Revert()
{
    m_draft = m_applied;

    ++m_revision;

    m_appliedRevision = m_revision;

    m_error.clear();
}

void EditorRenderingState::CommitEnvironmentTexture(const std::string& path)
{
    const bool pending                = IsPending();

    m_draft.environment.texturePath   = path;

    m_applied.environment.texturePath = path;

    m_appliedRevision                 = ++m_revision;

    if (pending)
    {
        ++m_revision;
    }
}

void EditorRenderingState::ChangeScene(const std::string&                    path,
                                       Vec3                                  center,
                                       float                                 scale,
                                       const HeapVector<rc::RenderingLight>& lights)
{
    const bool pending  = IsPending();

    m_applied.scenePath = m_draft.scenePath = path;

    m_applied.normalizationCenter = m_draft.normalizationCenter = center;

    m_applied.normalizationScale = m_draft.normalizationScale = scale;

    m_applied.lights = m_draft.lights = lights;

    m_applied.debug.lightId = m_draft.debug.lightId = lights.empty() ? 0 : lights.front().id;

    m_appliedRevision                               = ++m_revision;

    if (pending)
    {
        ++m_revision;
    }

    rc::ValidateRenderingSettings(m_draft, m_error);
}
} // namespace zen::editor
