#pragma once
#include "Platform/ConfigLoader.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include <span>
#include <string>
#include <string_view>

namespace zen::rc
{
// Apply on the render/main thread between frame recordings. Resource changes may stall.
struct VoxelGIRuntimeSettings
{
    uint32_t                   resolution{256};
    VoxelGISettings            cone;
    platform::AsyncComputeMode asyncCompute{platform::AsyncComputeMode::eDisabled};
    bool                       averagedReflectance{false};
    uint64_t                   reflectanceBudgetBytes{0};
    uint32_t                   shadowMapResolution{1024};
    // The GI configuration (method, tracing, resolution, denoiser, shadow method) is chosen from
    // device capability whenever the settings are applied (ResolveVoxelGIAutomaticProvider). Lighting
    // content (intensity, light sources, diffuse/specular/reflection and cast-shadow toggles) is kept.
    // Editing a configuration field makes the settings custom (IsOptimalVoxelGIConfiguration).
    // Off by default so setups saved before this option keep their explicit configuration.
    bool optimal{false};

    bool operator==(const VoxelGIRuntimeSettings&) const = default;
};

enum class VoxelGIPreset : uint32_t
{
    Custom,
    Low,
    Medium,
    High,
    Ultra,
    Native,
    Balanced,
    Optimal
};

// Every GI method the renderer implements. Each one sets restirGI, worldRadianceCache and screenProbeGI together.
enum class GIMethod : uint32_t
{
    Pixel,            // One trace per GI texel with exact hit shading.
    PixelReSTIR,      // Plus temporal and spatial reservoir reuse of the traced hits. Hardware RT.
    PixelCache,       // Hits shaded from the world radiance cache, which adds bounces. RT pipeline or Compute.
    PixelReSTIRCache, // Reservoir reuse with cached hit shading. RT pipeline.
    ScreenProbes,     // World-anchored screen probes gathered at full resolution, with the world cache.
    Count
};

// Automatic uses hardware when the requested tracing features are available, and Compute otherwise.
enum class GITracing : uint32_t
{
    Automatic,
    Compute,            // GPU-built distance field; direct shadows use shadow maps.
    RayQueries,         // Hardware ray queries from compute passes.
    RayTracingPipeline, // Hardware RT pipeline.
    Count
};

// The GI denoiser stages that run. Per-pixel methods use the ReLAX-style pixel denoiser; screen probes
// filter in probe space. Spatial includes the history repair; the output stabilizer needs both stages.
enum class GIDenoiser : uint32_t
{
    Spatiotemporal,
    Temporal,
    Spatial,
    Off,
    Count
};

// Direct-light visibility when shadows are cast. Ray-traced shadows need hardware RT and use maps on
// Compute. Reuse traces a 4x4 coarse grid and refines only receivers whose neighbours disagree; it needs
// the RT pipeline and otherwise traces every receiver.
enum class GIShadows : uint32_t
{
    ShadowMaps,
    RayTraced,
    RayTracedReuse,
    Count
};

GIMethod    GetGIMethod(const VoxelGISettings& settings);
void        SetGIMethod(VoxelGISettings& settings, GIMethod method);
const char* GetGIMethodName(GIMethod method); // Configuration value, e.g. "per-pixel-restir".

GITracing   GetGITracing(const VoxelGISettings& settings);
void        SetGITracing(VoxelGISettings& settings, GITracing tracing);
const char* GetGITracingName(GITracing tracing);

GIDenoiser  GetGIDenoiser(const VoxelGISettings& settings);
void        SetGIDenoiser(VoxelGISettings& settings, GIDenoiser denoiser);
const char* GetGIDenoiserName(GIDenoiser denoiser);

GIShadows   GetGIShadows(const VoxelGISettings& settings);
void        SetGIShadows(VoxelGISettings& settings, GIShadows shadows);
const char* GetGIShadowsName(GIShadows shadows);

// Why a method cannot run on a resolved tracing tier (Compute, RayQueries or RayTracingPipeline), or nullptr.
// An unavailable method still renders: ReSTIR is skipped, and cache methods trace exact hits.
const char* GetGIMethodUnavailableReason(GIMethod method, GITracing tracing);

// Why a tracing tier cannot run on this device, or nullptr. Automatic and Compute always run.
const char* GetGITracingUnavailableReason(GITracing tracing, const RHIGPUInfo& gpu);

// Optimal gathers screen probes at native resolution using the RT pipeline or the compute distance field.
// A forced hardware provider can use per-pixel ReSTIR on a ray-query-only device.
void      ApplyOptimalVoxelGIConfiguration(VoxelGIRuntimeSettings& settings, const RHIGPUInfo& gpu);
GITracing GetOptimalGITracing(bool rayQueries, bool rayTracingPipeline);
GIMethod  GetOptimalGIMethod(GITracing tracing);

// Copies the lighting content the optimal configuration keeps: indirect diffuse and its intensity and
// light sources, specular GI and reflections, cast shadows, async compute and the reflectance policy.
void CopyVoxelGIContent(const VoxelGIRuntimeSettings& source, VoxelGIRuntimeSettings& target);

// The optimal flag is set and every configuration field still has its optimal value; a forced provider
// may differ. A caller that edits a configuration field gets that configuration, as with engine.cfg keys.
bool IsOptimalVoxelGIConfiguration(const VoxelGIRuntimeSettings& settings);

const char*   GetVoxelGIPresetName(VoxelGIPreset preset);
VoxelGIPreset GetVoxelGIPreset(const VoxelGIRuntimeSettings& settings);
// Optimal sets the flag and the configuration chosen for hardware RT, with the provider left automatic;
// the device-specific choice happens when the settings are applied. Content is kept for Optimal only.
bool ApplyVoxelGIPreset(VoxelGIPreset preset, VoxelGIRuntimeSettings& settings);
// Resolve the optimal configuration and inherited Auto requests from device capability: the HW tier where
// it can run (GetHardwareTierUnavailableReason), Compute otherwise. Explicit requests remain authoritative.
// Returns "explicit_provider", "ray_query_available" or the unavailable reason.
const char* ResolveVoxelGIAutomaticProvider(VoxelGIRuntimeSettings& settings, const RHIGPUInfo& gpu);

bool ValidateVoxelGIRuntimeSettings(const VoxelGIRuntimeSettings& settings);

// Parsing is transactional: a rejected configuration leaves output untouched. Any configuration key
// (method, tracing, resolution, denoiser, shadow method, ...) turns the optimal configuration off.
bool LoadVoxelGIRuntimeSettings(const platform::ConfigLoader& config,
                                VoxelGIRuntimeSettings&       output,
                                const VoxelGIRuntimeSettings* inherited = nullptr);

bool RequiresVoxelGIRebuild(const VoxelGIRuntimeSettings& previous, const VoxelGIRuntimeSettings& next);

// The GI options shared by engine.cfg, ZenApp and the editor's GI settings panel. ZenApp accepts each one
// as --gi-<name>=<value>, where <name> is the key without "voxel_gi_" and with '-' for '_'.
struct GIConfigOption
{
    const char* key;
    const char* values;        // Accepted values, for help text.
    bool        configuration; // Chosen by the optimal configuration; content options are kept.
};

std::span<const GIConfigOption> GetGIConfigOptions();
std::string                     GetGIConfigArgumentName(const GIConfigOption& option);
const GIConfigOption*           FindGIConfigArgument(std::string_view name);

// engine.cfg lines for every option, which LoadVoxelGIRuntimeSettings reads back to the same settings.
// The optimal configuration writes only the preset and the content options.
std::string FormatGIConfig(const VoxelGIRuntimeSettings& settings, bool enabled);
// The same options as ZenApp arguments on one line.
std::string FormatGIArguments(const VoxelGIRuntimeSettings& settings, bool enabled);
// Replaces the GI options in an engine.cfg text with FormatGIConfig's lines, at the first GI option's line
// (or at the end). Other lines, comments and unrelated keys are kept.
std::string UpdateGIConfigText(std::string_view text, const VoxelGIRuntimeSettings& settings, bool enabled);

} // namespace zen::rc
