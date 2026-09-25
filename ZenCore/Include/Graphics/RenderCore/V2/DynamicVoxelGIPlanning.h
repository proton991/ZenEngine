#pragma once
#include "Graphics/RHI/RHICommon.h"
#include "Platform/ConfigLoader.h"

namespace zen::rc
{
enum class VoxelGIMethod
{
    eAuto,
    eCone,
    eDynamicVoxel,
    eNone // Resolved PBR fallback only; not a selectable GI method.
};
enum class VoxelGIQueryBackend
{
    eAuto,
    eVoxelDDA,
    eHardwareRT
};

enum class GITemporalMode : uint32_t
{
    eOff,
    eFixed,
    eElapsed
};

struct DynamicVoxelGISettings
{
    VoxelGIMethod method{VoxelGIMethod::eAuto};
    VoxelGIQueryBackend backend{VoxelGIQueryBackend::eAuto};
    uint32_t resolution{64};
    uint32_t raysPerFace{128};
    uint32_t neighborRadius{2};
    GITemporalMode temporal{GITemporalMode::eElapsed};
    bool spatialFilter{true};
    bool analyticLighting{true};
    bool environmentLighting{true};
    bool emissiveLighting{true};
    bool compactCache{false}; // DDA only; decoded cache also supports the reference provider.
    // No guessed shipping budget: an explicit cap is required by the preflight.
    uint64_t memoryBudgetBytes{0};
};

bool LoadDynamicVoxelGISettings(const platform::ConfigLoader& config,
                                DynamicVoxelGISettings& output);

struct VoxelGISelection
{
    VoxelGIMethod method{VoxelGIMethod::eCone};
    const char* backend{"none"};
    const char* reason{"legacy default; dynamic voxel GI requires explicit selection"};
};

// Base policy; RendererServer enables directional GI only after scene-profile and resource checks.
VoxelGISelection ResolveVoxelGISelection(const DynamicVoxelGISettings& settings);

enum class GIResourceStatus
{
    eSuccess,
    eInvalidInput,
    eUnavailableBackend,
    eOverflow,
    eBufferSize,
    eDescriptorRange,
    eBudget
};

struct GIResourceEstimate
{
    const char* name;
    bool storageBuffer;
    uint64_t elements;
    uint32_t stride;
    uint32_t copies;
    uint64_t bytesPerAllocation{0};
};

struct DynamicVoxelGIResourceRequest
{
    DynamicVoxelGISettings settings;
    uint64_t staticCapacity{0};
    uint64_t dynamicCapacity{0}; // Includes the receiver neighborhood, not just occupied cells.
    uint64_t retiringBytes{0};
    bool averagedReflectance{false}; // M2 uses independent class scratch and output allocations.
    bool coneTransition{false};
};

struct DynamicVoxelGIResourceEstimate
{
    GIResourceStatus status{GIResourceStatus::eSuccess};
    const char* rejectedResource{nullptr};
    uint64_t peakBytes{0};
    HeapVector<GIResourceEstimate> resources;
};

GIResourceStatus ValidateGIStorageBuffer(uint64_t elements,
                                         uint32_t stride,
                                         const RHIGPUInfo& gpu,
                                         uint64_t& bytes);
// Incremental V1 budget: one uint4 scratch and one RGBA8 output per cell,
// including any previous V1 allocations still awaiting retirement.
GIResourceStatus ValidateVoxelReflectanceResources(uint32_t resolution,
                                                   uint64_t triangles,
                                                   uint64_t budgetBytes,
                                                   uint64_t retiringBytes,
                                                   const RHIGPUInfo& gpu,
                                                   uint64_t& peakBytes);
// M2 class surfaces/lists plus the retained cone transition, without future hit caches.
GIResourceStatus ValidateVoxelConeResources(uint32_t resolution,
                                            bool averaged,
                                            uint64_t budgetBytes,
                                            uint64_t retiringBytes,
                                            const RHIGPUInfo& gpu,
                                            uint64_t& peakBytes);
GIResourceStatus ValidateVoxelClassResources(uint32_t resolution,
                                             bool averaged,
                                             uint64_t budgetBytes,
                                             uint64_t retiringBytes,
                                             const RHIGPUInfo& gpu,
                                             uint64_t& peakBytes);
// Six static GIHit caches; raw/final faces, float histories and per-cell metadata,
// masks, receiver lists/flags and bounded indirect arguments. The static
// capacity is bounded by both the total cap and the range of each face buffer.
GIResourceStatus PlanStaticVoxelGIResources(uint32_t resolution,
                                            bool averaged,
                                            uint64_t budgetBytes,
                                            const RHIGPUInfo& gpu,
                                            uint32_t& capacity,
                                            uint64_t& peakBytes,
                                            uint64_t receiverSurfaceBytes = 0,
                                            uint64_t retiringBytes        = 0,
                                            bool compactCache             = false,
                                            uint32_t raysPerFace          = 128);
DynamicVoxelGIResourceEstimate EstimateDynamicVoxelGIResources(
    const DynamicVoxelGIResourceRequest& request,
    const RHIGPUInfo& gpu);
} // namespace zen::rc
