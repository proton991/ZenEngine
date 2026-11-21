#pragma once
#include "Graphics/RenderCore/V2/VoxelGISettings.h"
#include "imgui.h"

namespace zen::ui
{
// What the GI settings panel needs from the renderer and the device.
struct GIControlsContext
{
    // Settings in use, with the optimal configuration and automatic tracing resolved. Null uses the input.
    const rc::VoxelGIRuntimeSettings* effective{nullptr};
    bool                              hardwareReady{false};      // Hardware RT is active.
    const char*                       reason{""};                // Why hardware RT is not active.
    bool                              rayQueries{false};         // The device supports ray queries.
    bool                              rayTracingPipeline{false}; // The device supports the RT pipeline.
};

// The GI settings shared by the editor and the runtime UI. Each control edits one GIConfigOption, so
// FormatGIConfig and ZenApp's --gi-* arguments reproduce what is shown. In the optimal configuration
// the controls show the resolved choice; editing one of them switches to a custom configuration.
// Returns true when settings changed.
bool DrawGIControls(rc::VoxelGIRuntimeSettings& settings, const GIControlsContext& context);

const char* GetGIMethodLabel(rc::GIMethod method);
const char* GetGITracingLabel(rc::GITracing tracing);
const char* GetGIDenoiserLabel(rc::GIDenoiser denoiser);
const char* GetGIShadowsLabel(rc::GIShadows shadows);

enum class GILightingState
{
    Inactive,
    Preparing,
    Refining,
    Ready,
    Unavailable
};
// Inactive scenes have no status. Display in the scene toolbar, outside the rendered image.
const char* GetGILightingStatus(GILightingState state);
} // namespace zen::ui
