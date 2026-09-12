#pragma once

#include <cstdint>

namespace zen
{
enum class FrameTimelineId : uint8_t
{
    RHI,
    Render
};

template <FrameTimelineId Id> struct FrameTimelineTraits;

template <FrameTimelineId Id> class TFrameTimeline
{
    using Traits = FrameTimelineTraits<Id>;

public:
    using Number = typename Traits::Number;
    using Slot   = typename Traits::Slot;

    static constexpr uint32_t kMaxFramesInFlight = Traits::kMaxFramesInFlight;

    static constexpr uint32_t kDefaultFramesInFlight = Traits::kDefaultFramesInFlight;

    void Init(uint32_t numFramesInFlight)
    {
        if (numFramesInFlight == 0)
        {
            numFramesInFlight = 1;
        }
        else if (numFramesInFlight > kMaxFramesInFlight)
        {
            numFramesInFlight = kMaxFramesInFlight;
        }

        m_numFramesInflight = numFramesInFlight;
        m_frameNumber       = 0;
    }

    void Advance()
    {
        m_frameNumber++;
    }

    Number GetFrameNumber()
    {
        return Number(m_frameNumber);
    }

    Slot GetFrameSlot()
    {
        return Slot(static_cast<uint32_t>(m_frameNumber % m_numFramesInflight));
    }

    uint32_t GetNumFramesInFlight() const
    {
        return m_numFramesInflight;
    }

    uint64_t NumFramesSince(Number frame) const
    {
        uint64_t since = Number::Invalid;

        if (frame != Number::Invalid)
        {
            since = m_frameNumber - static_cast<uint64_t>(frame);
        }

        return since;
    }

private:
    uint64_t m_frameNumber{0};

    uint32_t m_numFramesInflight{1};
};
} // namespace zen