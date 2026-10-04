#pragma once
#include "UI/UIDrawPacket.h"
#include "imgui.h"

namespace zen::ui
{
// Reject arbitrary callbacks. Texture handles are validated by UIRenderer before recording.
bool BuildUIDrawPacket(const ImDrawData& data, uint32_t width, uint32_t height, UIDrawPacket& output);
} // namespace zen::ui
