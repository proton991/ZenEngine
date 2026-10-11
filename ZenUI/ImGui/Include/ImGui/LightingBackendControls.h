#pragma once
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"

namespace zen::ui
{
// Shared by the editor and runtime panel so backend selection has the same semantics.
bool DrawLightingBackendControls(rc::VoxelGISettings& settings, bool hardwareReady, const char* reason);
} // namespace zen::ui
