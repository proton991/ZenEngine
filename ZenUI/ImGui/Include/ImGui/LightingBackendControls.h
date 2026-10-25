#pragma once
#include "Graphics/RenderCore/V2/VoxelGISettings.h"

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
    Bricks,      // Compute: sparse occupancy bricks, fast and approximate.
    SoftwareBVH, // Compute: software triangle BVH, exact and slow.
    ShadowMaps,  // Direct shadows only; traces no rays.
    // The pass's GI is replaced by the unoccluded environment, as without GI. Diffuse GI keeps
    // tracing; specular GI, the sky cache and shadows trace no rays.
    Off,
    Mixed // The pass's ray features use different providers (set per feature in the config).
};

GITracer GetGIPassTracer(const rc::VoxelGISettings& settings, GIPass pass);
// Selecting a ray tracer for a pass that is off turns it on; other choices keep the pass's enables.
void SetGIPassTracer(rc::VoxelGISettings& settings, GIPass pass, GITracer tracer);
// Sets every pass to HW RT or to Compute with its current Compute tracer.
void SetGIRayTracing(rc::VoxelGISettings& settings, bool hardware);

// Shared by the editor and runtime panel so tracing selection has the same semantics.
bool DrawLightingBackendControls(rc::VoxelGISettings& settings, bool hardwareReady, const char* reason);
bool DrawGIGatherControls(rc::VoxelGISettings& settings);
bool DrawGIPresetControls(rc::VoxelGIRuntimeSettings& settings, const char* selectionReason);
} // namespace zen::ui
