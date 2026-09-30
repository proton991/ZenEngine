#pragma once

#include "Templates/HeapVector.h"
#include "imgui.h"
#include <cstdint>

namespace zen::ui
{
struct UIDrawCommand
{
    uint32_t minX{0};
    uint32_t minY{0};
    uint32_t maxX{0};
    uint32_t maxY{0};
    uint32_t count{0};
    uint32_t firstIndex{0};
    uint32_t vertexOffset{0};
};

struct UIDrawPacket
{
    HeapVector<uint8_t> vertices;
    // Padded to four bytes for RHI staging copies; draw counts exclude padding.
    HeapVector<uint8_t> indices;
    HeapVector<UIDrawCommand> commands;
    float projection[4]{};
};

// Static-font backend contract: reject unsupported textures/custom callbacks.
// ResetRenderState is accepted because each draw restores all mutable UI state.
bool BuildUIDrawPacket(const ImDrawData& data,
                       ImTextureID fontTexture,
                       uint32_t width,
                       uint32_t height,
                       UIDrawPacket& output);
} // namespace zen::ui
