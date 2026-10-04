#include "UI/UIDrawPacket.h"
#include <cmath>

namespace zen::ui
{
bool ValidateUIDrawPacket(const UIDrawPacket& packet, uint32_t width, uint32_t height)
{
    bool valid =
        packet.vertices.size() <= UINT32_MAX / sizeof(UIVertex) && packet.indices.size() <= UINT32_MAX / sizeof(uint32_t);

    for (float value : packet.projection)
    {
        valid = valid && std::isfinite(value);
    }

    const size_t vertices = packet.vertices.size();

    const size_t indices  = packet.indices.size();

    for (const UIDrawCommand& command : packet.commands)
    {
        valid = valid && command.texture.value != 0 && command.minX < command.maxX && command.minY < command.maxY
             && command.maxX <= width && command.maxY <= height && command.firstIndex <= indices
             && command.count <= indices - command.firstIndex;

        if (valid)
        {
            for (uint32_t offset = 0; offset < command.count; ++offset)
            {
                valid =
                    valid && uint64_t(packet.indices[size_t(command.firstIndex) + offset]) + command.vertexOffset < vertices;
            }
        }
    }

    return valid;
}
} // namespace zen::ui
