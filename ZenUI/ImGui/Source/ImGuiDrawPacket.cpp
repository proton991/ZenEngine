#include "ImGui/ImGuiDrawPacket.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace zen::ui
{
namespace
{
bool IsFinite(const ImVec2& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool AppendCommands(const ImDrawData&          data,
                    const ImDrawList&          list,
                    uint32_t                   width,
                    uint32_t                   height,
                    uint32_t                   firstIndex,
                    uint32_t                   vertexOffset,
                    HeapVector<UIDrawCommand>& commands)
{
    bool valid = true;

    for (const ImDrawCmd& draw : list.CmdBuffer)
    {
        if (draw.UserCallback != nullptr)
        {
            valid = valid && draw.UserCallback == ImDrawCallback_ResetRenderState;
        }
        else if (draw.ElemCount != 0)
        {
            valid = valid && draw.GetTexID() != ImTextureID_Invalid
                 && draw.IdxOffset <= static_cast<uint32_t>(list.IdxBuffer.Size)
                 && draw.ElemCount <= static_cast<uint32_t>(list.IdxBuffer.Size) - draw.IdxOffset
                 && draw.VtxOffset < static_cast<uint32_t>(list.VtxBuffer.Size);

            const float x1 = (draw.ClipRect.x - data.DisplayPos.x) * data.FramebufferScale.x;

            const float y1 = (draw.ClipRect.y - data.DisplayPos.y) * data.FramebufferScale.y;

            const float x2 = (draw.ClipRect.z - data.DisplayPos.x) * data.FramebufferScale.x;

            const float y2 = (draw.ClipRect.w - data.DisplayPos.y) * data.FramebufferScale.y;

            valid          = valid && std::isfinite(x1) && std::isfinite(y1) && std::isfinite(x2) && std::isfinite(y2);

            if (valid)
            {
                UIDrawCommand command;

                command.texture.value = static_cast<uint64_t>(draw.GetTexID());

                command.minX          = static_cast<uint32_t>(std::clamp(std::floor(x1), 0.0f, float(width)));

                command.minY          = static_cast<uint32_t>(std::clamp(std::floor(y1), 0.0f, float(height)));

                command.maxX          = static_cast<uint32_t>(std::clamp(std::ceil(x2), 0.0f, float(width)));

                command.maxY          = static_cast<uint32_t>(std::clamp(std::ceil(y2), 0.0f, float(height)));

                command.count         = draw.ElemCount;

                command.firstIndex    = firstIndex + draw.IdxOffset;

                command.vertexOffset  = vertexOffset + draw.VtxOffset;

                if (command.minX < command.maxX && command.minY < command.maxY)
                {
                    commands.push_back(command);
                }
            }
        }
    }

    return valid;
}
} // namespace

bool BuildUIDrawPacket(const ImDrawData& data, uint32_t width, uint32_t height, UIDrawPacket& output)
{
    static_assert(sizeof(ImDrawVert) == sizeof(UIVertex) && offsetof(ImDrawVert, pos) == offsetof(UIVertex, position)
                  && offsetof(ImDrawVert, uv) == offsetof(UIVertex, uv)
                  && offsetof(ImDrawVert, col) == offsetof(UIVertex, color));

    static_assert(std::is_trivially_copyable_v<ImDrawVert> && std::is_trivially_copyable_v<UIVertex>);

    static_assert(IM_COL32_R_SHIFT == 0 && IM_COL32_G_SHIFT == 8 && IM_COL32_B_SHIFT == 16 && IM_COL32_A_SHIFT == 24);

    output     = {};

    bool valid = data.Valid && data.TotalVtxCount >= 0 && data.TotalIdxCount >= 0 && IsFinite(data.DisplayPos)
              && IsFinite(data.DisplaySize) && IsFinite(data.FramebufferScale) && data.FramebufferScale.x > 0
              && data.FramebufferScale.y > 0;

    const bool visible = width > 0 && height > 0 && data.DisplaySize.x > 0 && data.DisplaySize.y > 0;

    if (valid && visible && data.TotalVtxCount > 0 && data.TotalIdxCount > 0)
    {
        uint64_t vertexCount = 0;

        uint64_t indexCount  = 0;

        for (const ImDrawList* list : data.CmdLists)
        {
            vertexCount += static_cast<uint64_t>(list->VtxBuffer.Size);

            indexCount  += static_cast<uint64_t>(list->IdxBuffer.Size);
        }

        const uint64_t vertexBytes = vertexCount * sizeof(UIVertex);

        const uint64_t indexBytes  = indexCount * sizeof(uint32_t);

        valid                      = vertexCount == static_cast<uint64_t>(data.TotalVtxCount)
             && indexCount == static_cast<uint64_t>(data.TotalIdxCount) && vertexBytes <= UINT32_MAX && indexBytes <= UINT32_MAX
             && vertexCount <= INT32_MAX;

        if (valid)
        {
            output.vertices.resize(static_cast<size_t>(vertexCount));

            output.indices.resize(static_cast<size_t>(indexCount));

            output.projection[0]  = 2.0f / data.DisplaySize.x;

            output.projection[1]  = 2.0f / data.DisplaySize.y;

            output.projection[2]  = -1.0f - data.DisplayPos.x * output.projection[0];

            output.projection[3]  = -1.0f - data.DisplayPos.y * output.projection[1];

            uint32_t firstIndex   = 0;

            uint32_t vertexOffset = 0;

            for (const ImDrawList* list : data.CmdLists)
            {
                if (list->VtxBuffer.Size > 0)
                {
                    std::memcpy(output.vertices.data() + vertexOffset, list->VtxBuffer.Data,
                                list->VtxBuffer.Size * sizeof(UIVertex));
                }

                // Widen 16-bit ImGui indices to the packet's 32-bit indices.
                for (int index = 0; index < list->IdxBuffer.Size; ++index)
                {
                    output.indices[size_t(firstIndex) + index] = list->IdxBuffer[index];
                }

                valid         = AppendCommands(data, *list, width, height, firstIndex, vertexOffset, output.commands) && valid;

                firstIndex   += static_cast<uint32_t>(list->IdxBuffer.Size);

                vertexOffset += static_cast<uint32_t>(list->VtxBuffer.Size);
            }
        }
    }

    if (!valid)
    {
        output = {};
    }

    return valid;
}
} // namespace zen::ui
