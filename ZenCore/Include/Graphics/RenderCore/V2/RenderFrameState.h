#pragma once

#include "ZenFoundation/Templates/FrameTimeline.h"

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

namespace detail
{
struct RenderFrameTraits
{
    using Number                                     = RenderFrameNumber;
    using Slot                                       = RenderFrameSlot;

    static constexpr uint32_t kMaxFramesInFlight     = 4u;

    static constexpr uint32_t kDefaultFramesInFlight = 3u;

    static constexpr const char* kName               = "RenderFrameState";
};
} // namespace detail

using RenderFrameState = TFrameTimeline<detail::RenderFrameTraits>;
} // namespace zen::rc

extern zen::rc::RenderFrameState GRenderFrameState;