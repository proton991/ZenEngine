#pragma once
#include "Graphics/RenderCore/V2/VoxelGISettings.h"

namespace zen::ui
{
// Shared by the editor and runtime panel so backend selection has the same semantics.
bool DrawLightingBackendControls(rc::VoxelGISettings& settings, bool hardwareReady, const char* reason);
bool DrawGIPresetControls(rc::VoxelGIRuntimeSettings& settings, const char* selectionReason);
} // namespace zen::ui
