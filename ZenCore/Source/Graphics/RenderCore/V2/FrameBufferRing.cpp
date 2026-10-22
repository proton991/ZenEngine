#include "Graphics/RenderCore/V2/FrameBufferRing.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include <algorithm>

namespace zen::rc
{
void FrameBufferRing::MarkDirty(uint32_t offset, uint32_t size)
{
    VERIFY_EXPR(size <= UINT32_MAX - offset);
    if (size != 0)
    {
        m_changed = true;
        for (Slot& slot : m_slots)
        {
            if (slot.buffer != nullptr)
            {
                Range                       range{offset, offset + size};
                HeapVector<Range>::iterator first = slot.dirty.begin();
                while (first != slot.dirty.end() && first->end < range.begin)
                {
                    ++first;
                }
                HeapVector<Range>::iterator last = first;
                while (last != slot.dirty.end() && last->begin <= range.end)
                {
                    range.begin = std::min(range.begin, last->begin);
                    range.end   = std::max(range.end, last->end);
                    ++last;
                }
                first = slot.dirty.erase(first, last);
                slot.dirty.insert(first, &range, &range + 1);
            }
        }
    }
}

bool FrameBufferRing::Update(RenderDevice& device, const void* data, uint32_t size, NameID name, bool vertexBuffer)
{
    bool valid = data != nullptr && size != 0 && !device.AreSubmissionsBlocked();
    if (valid && (m_changed || GetBuffer() == nullptr))
    {
        // Rotate past the newest generation rather than following the frame slot: after an
        // idle span, that slot can hold the buffer the previous frame and its history read.
        const uint32_t index = GetBuffer() == nullptr ? m_current : (m_current + 1) % GRenderFrameState.GetNumFramesInFlight();
        Slot&          slot  = m_slots[index];
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        if (slot.buffer == nullptr || slot.size < size)
        {
            RHIBuffer* replacement = m_indexBuffer ? device.CreateIndexBuffer(size, bytes)
                                   : vertexBuffer  ? device.CreateVertexBuffer(size, bytes)
                                                   : device.CreateStorageBuffer(size, bytes, name);
            valid                  = replacement != nullptr;
            if (valid)
            {
                device.DestroyBuffer(slot.buffer);
                slot.buffer = replacement;
                slot.size   = size;
            }
        }
        else
        {
            // A failed partial update must not leave an old generation valid.
            slot.generation = 0;
            for (const Range& range : slot.dirty)
            {
                const uint32_t end = std::min(range.end, size);
                if (valid && range.begin < end)
                {
                    valid = device.UpdateBuffer(slot.buffer, end - range.begin, bytes + range.begin, range.begin);
                }
            }
        }
        if (valid)
        {
            slot.dirty.clear();
            slot.generation = ++m_generation;
            m_current       = index;
            m_changed       = false;
        }
    }
    return valid;
}

bool FrameBufferRing::ContainsGeneration(const RHIBuffer* buffer, uint64_t generation) const
{
    bool found = false;
    for (const Slot& slot : m_slots)
    {
        found = found || (buffer != nullptr && slot.buffer == buffer && generation != 0 && slot.generation == generation);
    }
    return found;
}

void FrameBufferRing::Destroy(RenderDevice& device)
{
    for (Slot& slot : m_slots)
    {
        device.DestroyBuffer(slot.buffer);
        slot = Slot{};
    }
    m_current = 0;
    // Keep generations monotonic across replacement of the ring's resources.
    m_changed = true;
}
} // namespace zen::rc
