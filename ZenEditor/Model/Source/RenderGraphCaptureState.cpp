#include "Editor/Model/RenderGraphCaptureState.h"

namespace zen::editor
{
void RenderGraphCaptureState::Synchronize(const RenderGraphCaptureSource& source,
                                          const std::string&              unavailable,
                                          bool                            awaitingApply)
{
    if (!(m_source == source) || m_unavailable != unavailable || m_awaitingApply != awaitingApply)
    {
        const bool stale = IsStale();
        const bool sourceChanged =
            m_source.scene != source.scene || m_source.path != source.path || m_source.play != source.play;
        const bool displayChanged = m_unavailable != unavailable || m_awaitingApply != awaitingApply;
        if (sourceChanged && !m_frozen)
        {
            m_snapshot = {};
            m_error.clear();
            m_algorithm.clear();
            m_fallback.clear();
        }
        // Live settings edits coalesce at the normal capture rate. A different
        // scene/source or resumed rendering can request its first frame immediately.
        if (sourceChanged || (!m_unavailable.empty() && unavailable.empty()))
        {
            m_nextCapture = {};
        }
        m_source        = source;
        m_unavailable   = unavailable;
        m_awaitingApply = awaitingApply;
        if (displayChanged || (sourceChanged && !m_frozen) || stale != IsStale())
        {
            ++m_changeSerial;
        }
    }
}

rc::RDGDebugSnapshot* RenderGraphCaptureState::RequestCapture(std::chrono::steady_clock::time_point now)
{
    rc::RDGDebugSnapshot* result = nullptr;
    // Freeze retains an accepted snapshot; it must not prevent acquiring the
    // first one after an unavailable scene/source transition or failed capture.
    if (!m_inFlight && m_unavailable.empty() && (m_refresh || ((!m_frozen || !m_snapshot.accepted) && now >= m_nextCapture)))
    {
        m_requested   = m_source;
        m_pending     = {};
        m_inFlight    = true;
        m_refresh     = false;
        m_nextCapture = now + std::chrono::milliseconds(250);
        result        = &m_pending;
    }
    return result;
}

void RenderGraphCaptureState::CompleteCapture(bool               submitted,
                                              const std::string& error,
                                              std::string        algorithm,
                                              std::string        fallback)
{
    if (m_inFlight)
    {
        if (submitted && m_pending.accepted && m_requested == m_source)
        {
            m_snapshot  = std::move(m_pending);
            m_captured  = m_requested;
            m_algorithm = std::move(algorithm);
            m_fallback  = std::move(fallback);
            m_error.clear();
        }
        else
        {
            m_error = error.empty() ? "Capture declined; waiting for the next scene submission." : error;
        }
        m_pending  = {};
        m_inFlight = false;
        ++m_changeSerial;
    }
}

void RenderGraphCaptureState::Refresh()
{
    m_refresh = true;
    ++m_changeSerial;
}

void RenderGraphCaptureState::SetFrozen(bool frozen)
{
    m_frozen = frozen;
    if (!frozen)
    {
        m_nextCapture = {};
    }
    ++m_changeSerial;
}

bool RenderGraphCaptureState::IsStale() const
{
    return m_snapshot.accepted && (!(m_captured == m_source) || !m_unavailable.empty() || !m_error.empty());
}
} // namespace zen::editor
