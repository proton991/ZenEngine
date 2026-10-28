#pragma once
#include "Graphics/RenderCore/V2/VoxelGISettings.h"
#include "imgui.h"

namespace zen::ui
{
// The passes the GI tracing panel configures, each with one tracer.
enum class GIPass : uint32_t
{
    Diffuse,  // Sky light and diffuse bounce rays.
    Specular, // Specular occlusion and glossy reflection rays.
    SkyCache, // Sky visibility for the voxel irradiance cache.
    Shadows,  // Direct-light shadows.
    Count
};

enum class GITracer : uint32_t
{
    HardwareRT,
    Field,      // Compute: GPU-built sparse distance field; GI rays and the sky cache.
    ShadowMaps, // Direct shadows only; traces no rays.
    // The pass's GI is replaced by the unoccluded environment, as without GI. Diffuse GI keeps
    // tracing; specular GI, the sky cache and shadows trace no rays.
    Off,
    Mixed // The pass's ray features use different providers (set per feature in the config).
};

GITracer GetGIPassTracer(const rc::VoxelGISettings& settings, GIPass pass);
// Selecting a ray tracer for a pass that is off turns it on; other choices keep the pass's enables.
void SetGIPassTracer(rc::VoxelGISettings& settings, GIPass pass, GITracer tracer);
// Sets GI to HW RT or its current Compute tracer; global Compute defaults direct shadows to maps.
// Existing feature enables and explicit shadow-map choices are preserved.
void SetGIRayTracing(rc::VoxelGISettings& settings, bool hardware);

// Shared by the editor and runtime panel so tracing selection has the same semantics.
bool DrawLightingBackendControls(rc::VoxelGISettings& settings, bool hardwareReady, const char* reason);
bool DrawGIGatherControls(rc::VoxelGISettings& settings);
bool DrawGIPresetControls(rc::VoxelGIRuntimeSettings& settings, const char* selectionReason);

// The scene view's message while the requested GI builds its scene BVH in the background
// (RendererServer::IsPreparingGI), or nullptr when it is not preparing.
const char* GetGIPreparationStatus(bool preparing, bool hardwareReady);
// Draws that message in a box at the top center of the view [viewMin, viewMax], centered
// between insets of rightInset pixels so it stays clear of corner widgets. Views too small
// for it skip it. Returns whether it was drawn.
bool DrawGIPreparationStatus(ImDrawList& draw,
                             bool        preparing,
                             bool        hardwareReady,
                             ImVec2      viewMin,
                             ImVec2      viewMax,
                             float       rightInset = 0.0f);
} // namespace zen::ui
