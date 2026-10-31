#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RDGDebugSnapshot.h"
#include <chrono>

namespace zen::editor
{
struct RenderGraphCaptureSource
{
    bool        play{false};
    uint64_t    scene{0};
    uint64_t    revision{0};
    std::string path;
    bool        operator==(const RenderGraphCaptureSource&) const = default;
};

// Capture policy and owned values only; never calls a device, window, or UI toolkit.
class RenderGraphCaptureState
{
public:
    void Synchronize(const RenderGraphCaptureSource& source, const std::string& unavailable, bool awaitingApply);
    rc::RDGDebugSnapshot* RequestCapture(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    void CompleteCapture(bool submitted, const std::string& error, std::string algorithm, std::string fallback);
    void Refresh();
    void SetFrozen(bool frozen);

    const rc::RDGDebugSnapshot& GetSnapshot() const
    {
        return m_snapshot;
    }
    const RenderGraphCaptureSource& GetCapturedSource() const
    {
        return m_captured;
    }
    const std::string& GetUnavailable() const
    {
        return m_unavailable;
    }
    const std::string& GetError() const
    {
        return m_error;
    }
    const std::string& GetAlgorithm() const
    {
        return m_algorithm;
    }
    const std::string& GetFallback() const
    {
        return m_fallback;
    }
    bool IsFrozen() const
    {
        return m_frozen;
    }
    bool IsAwaitingApply() const
    {
        return m_awaitingApply;
    }
    bool     IsStale() const;
    uint64_t GetChangeSerial() const
    {
        return m_changeSerial;
    }

private:
    RenderGraphCaptureSource              m_source;
    RenderGraphCaptureSource              m_requested;
    RenderGraphCaptureSource              m_captured;
    rc::RDGDebugSnapshot                  m_snapshot;
    rc::RDGDebugSnapshot                  m_pending;
    std::chrono::steady_clock::time_point m_nextCapture;
    std::string                           m_unavailable;
    std::string                           m_error;
    std::string                           m_algorithm;
    std::string                           m_fallback;
    bool                                  m_frozen{false};
    bool                                  m_refresh{true};
    bool                                  m_inFlight{false};
    bool                                  m_awaitingApply{false};
    uint64_t                              m_changeSerial{0};
};
} // namespace zen::editor
