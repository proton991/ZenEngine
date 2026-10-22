#pragma once

#include "Graphics/RenderCore/V2/RenderFrameState.h"
#include "Graphics/RenderCore/V2/RenderCoreDefs.h"

namespace zen::rc
{
class RenderDevice;

// Persistent GPU buffers for CPU-authored scene data. Updates are staged copies into the
// slot after the newest generation; RenderDevice's upload history orders them after every
// previous GPU reader. Call after BeginFrame, before recording consumers of the contents.
class FrameBufferRing
{
public:
    FrameBufferRing() = default;
    // An index ring creates index buffers; Update's vertexBuffer flag then has no effect.
    explicit FrameBufferRing(bool indexBuffer) : m_indexBuffer(indexBuffer) {}
    FrameBufferRing(const FrameBufferRing&)            = delete;
    FrameBufferRing& operator=(const FrameBufferRing&) = delete;
    void             MarkDirty(uint32_t offset, uint32_t size);
    bool             Update(RenderDevice& device, const void* data, uint32_t size, NameID name, bool vertexBuffer = false);
    void             Destroy(RenderDevice& device);

    RHIBuffer* GetBuffer() const
    {
        return m_slots[m_current].buffer;
    }

    uint64_t GetGeneration() const
    {
        return m_generation;
    }

    bool ContainsGeneration(const RHIBuffer* buffer, uint64_t generation) const;

private:
    struct Range
    {
        uint32_t begin;
        uint32_t end;
    };

    struct Slot
    {
        RHIBuffer*        buffer{nullptr};
        uint32_t          size{0};
        uint64_t          generation{0};
        HeapVector<Range> dirty;
    };

    Slot     m_slots[RenderFrameState::kMaxFramesInFlight];
    uint32_t m_current{0};
    uint64_t m_generation{0};
    bool     m_changed{true};
    bool     m_indexBuffer{false};
};
} // namespace zen::rc
