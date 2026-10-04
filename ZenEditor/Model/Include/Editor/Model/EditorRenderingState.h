#pragma once
#include "Graphics/RenderCore/V2/RenderingSettings.h"

namespace zen::editor
{
// Invalid drafts remain editable. Only a successful runtime application publishes
// an applied revision, including applications which later fall back on the GPU.
class EditorRenderingState
{
public:
    void Initialize(const rc::RenderingSettings& settings);

    bool Stage(const rc::RenderingSettings& settings);

    void Commit();

    // The draft with Apply-only resource values held at their applied state.
    rc::RenderingSettings GetPreview() const;

    // Publishes the preview; resource changes stay pending until Commit.
    void CommitPreview();

    void Revert();

    void CommitEnvironmentTexture(const std::string& path);

    void ChangeScene(const std::string& path, Vec3 center, float scale, const HeapVector<rc::RenderingLight>& lights);

    const rc::RenderingSettings& GetDraft() const
    {
        return m_draft;
    }

    const rc::RenderingSettings& GetApplied() const
    {
        return m_applied;
    }

    uint64_t GetRevision() const
    {
        return m_revision;
    }

    uint64_t GetAppliedRevision() const
    {
        return m_appliedRevision;
    }

    bool IsPending() const
    {
        return m_revision != m_appliedRevision;
    }

    bool NeedsResourceApply() const
    {
        return rc::RequiresRenderingResourceApply(m_applied, m_draft);
    }

    const std::string& GetError() const
    {
        return m_error;
    }

    void SetError(std::string error)
    {
        m_error = std::move(error);
    }

private:
    rc::RenderingSettings m_draft;
    rc::RenderingSettings m_applied;
    uint64_t              m_revision{0};
    uint64_t              m_appliedRevision{0};
    std::string           m_error;
};
} // namespace zen::editor
