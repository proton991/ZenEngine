#include "UI/UIDrawPacket.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

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
                    ImTextureID                fontTexture,
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
            valid = valid && draw.GetTexID() == fontTexture && draw.IdxOffset <= static_cast<uint32_t>(list.IdxBuffer.Size)
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

                command.minX         = static_cast<uint32_t>(std::clamp(std::floor(x1), 0.0f, float(width)));

                command.minY         = static_cast<uint32_t>(std::clamp(std::floor(y1), 0.0f, float(height)));

                command.maxX         = static_cast<uint32_t>(std::clamp(std::ceil(x2), 0.0f, float(width)));

                command.maxY         = static_cast<uint32_t>(std::clamp(std::ceil(y2), 0.0f, float(height)));

                command.count        = draw.ElemCount;

                command.firstIndex   = firstIndex + draw.IdxOffset;

                command.vertexOffset = vertexOffset + draw.VtxOffset;

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

bool BuildUIDrawPacket(const ImDrawData& data, ImTextureID fontTexture, uint32_t width, uint32_t height, UIDrawPacket& output)
{
    output     = {};

    bool valid = data.Valid && fontTexture != ImTextureID_Invalid && data.TotalVtxCount >= 0 && data.TotalIdxCount >= 0
              && IsFinite(data.DisplayPos) && IsFinite(data.DisplaySize) && IsFinite(data.FramebufferScale)
              && data.FramebufferScale.x > 0 && data.FramebufferScale.y > 0;

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

        const uint64_t vertexBytes = vertexCount * sizeof(ImDrawVert);

        const uint64_t indexBytes  = (indexCount * sizeof(ImDrawIdx) + 3) & ~uint64_t(3);

        valid                      = vertexCount == static_cast<uint64_t>(data.TotalVtxCount)
             && indexCount == static_cast<uint64_t>(data.TotalIdxCount) && vertexBytes <= UINT32_MAX && indexBytes <= UINT32_MAX
             && vertexCount <= INT32_MAX;

        if (valid)
        {
            output.vertices.resize(static_cast<size_t>(vertexBytes));

            output.indices.resize(static_cast<size_t>(indexBytes));

            std::memset(output.indices.data(), 0, output.indices.size());

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
                    std::memcpy(output.vertices.data() + vertexOffset * sizeof(ImDrawVert), list->VtxBuffer.Data,
                                list->VtxBuffer.Size * sizeof(ImDrawVert));
                }

                if (list->IdxBuffer.Size > 0)
                {
                    std::memcpy(output.indices.data() + firstIndex * sizeof(ImDrawIdx), list->IdxBuffer.Data,
                                list->IdxBuffer.Size * sizeof(ImDrawIdx));
                }

                valid =
                    AppendCommands(data, *list, fontTexture, width, height, firstIndex, vertexOffset, output.commands) && valid;

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
