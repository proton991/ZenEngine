#pragma once

#include "Templates/FrameTimeline.h"

namespace zen
{
enum class RHIFrameNumber : uint64_t
{
    Invalid = ~0ull
};

enum class RHIFrameSlot : uint32_t
{
};

constexpr uint32_t ToIndex(RHIFrameSlot slot)
{
    return static_cast<uint32_t>(slot);
}

constexpr uint64_t ToValue(RHIFrameNumber frameNumber)
{
    return static_cast<uint64_t>(frameNumber);
}

template <> struct FrameTimelineTraits<FrameTimelineId::RHI>
{
    using Number = RHIFrameNumber;
    using Slot   = RHIFrameSlot;

    static constexpr uint32_t kMaxFramesInFlight = 4u;

    static constexpr uint32_t kDefaultFramesInFlight = 3u;

    static constexpr const char* kName = "RHIFrameState";
};

using RHIFrameState = TFrameTimeline<FrameTimelineId::RHI>;
} // namespace zen

extern zen::RHIFrameState GRHIFrameState;