#pragma once

#include "Templates/FrameTimeline.h"

namespace zen::rc
{
enum class RenderFrameNumber : uint64_t
{
    Invalid = ~0ull
};

enum class RenderFrameSlot : uint32_t
{
};

constexpr uint32_t ToIndex(RenderFrameSlot slot)
{
    return static_cast<uint32_t>(slot);
}

constexpr uint64_t ToValue(RenderFrameNumber frameNumber)
{
    return static_cast<uint64_t>(frameNumber);
}
} // namespace zen::rc

namespace zen
{
template <> struct FrameTimelineTraits<FrameTimelineId::Render>
{
    using Number = rc::RenderFrameNumber;
    using Slot   = rc::RenderFrameSlot;

    static constexpr uint32_t kMaxFramesInFlight = 4u;

    static constexpr uint32_t kDefaultFramesInFlight = 3u;

    static constexpr const char* kName = "RenderFrameState";
};
} // namespace zen

namespace zen::rc
{
using RenderFrameState = TFrameTimeline<FrameTimelineId::Render>;
} // namespace zen::rc

extern zen::rc::RenderFrameState GRenderFrameState;