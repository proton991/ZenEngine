#pragma once
// Shader registration shared by the editor's render services. Private to ZenEditorRender.
#include "Graphics/RenderCore/V2/RenderDevice.h"

namespace zen::editor
{
// Registers a graphics program (vertex and fragment) or, without a fragment stage, a
// compute program, unless one with this name exists. SPIR-V paths are relative to the
// engine's shader directory.
bool RegisterEditorShader(rc::RenderDevice& device, NameID name, const char* vertex, const char* fragment);
} // namespace zen::editor
