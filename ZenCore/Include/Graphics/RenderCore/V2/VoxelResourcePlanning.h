#pragma once
#include "Graphics/RHI/RHICommon.h"
#include "Graphics/RHI/RHIGPUMemoryStats.h"
#include "Templates/ArrayView.h"

namespace zen::rc
{
enum class GIResourceStatus
{
    eSuccess,
    eInvalidInput,
    eOverflow,
    eBufferSize,
    eDescriptorRange,
    eBudget
};

// Validate the payload and room for RenderDevice's allocation alignment before narrowing.
GIResourceStatus ValidateGIStorageBuffer(uint64_t elements, uint32_t stride, const RHIGPUInfo& gpu, uint64_t& bytes);

// Cache uses 40 bytes per entry, at most a quarter of available headroom after a 10% reserve.
// Existing cache bytes are reusable; unavailable budget information means exact shading.
uint32_t PlanGIRadianceCache(const RHIGPUMemoryStats& memory,
                             const RHIGPUInfo&        gpu,
                             const Vec3&              sceneExtent,
                             uint64_t                 existingBytes);
uint32_t PlanGIRadianceCache(const RHIGPUMemoryStats&  memory,
                             const RHIGPUInfo&         gpu,
                             const Vec3&               sceneExtent,
                             uint64_t                  existingBytes,
                             ArrayView<const uint32_t> strides,
                             uint64_t                  fixedBytes      = 0,
                             uint32_t                  patchesPerAxis  = 300,
                             uint32_t                  entriesPerPatch = 8);

// Incremental scratch and output storage; zero for an unsupported resolution.
uint64_t GetVoxelReflectanceRequiredBytes(uint32_t resolution);

// Incremental V1 budget: one uint4 scratch and one RGBA8 output per cell,
// including any previous V1 allocations still awaiting retirement.
GIResourceStatus ValidateVoxelReflectanceResources(uint32_t          resolution,
                                                   uint64_t          triangles,
                                                   uint64_t          budgetBytes,
                                                   uint64_t          retiringBytes,
                                                   const RHIGPUInfo& gpu,
                                                   uint64_t&         peakBytes);

// Throughput distance-field construction pacing (DistanceFieldSceneSettings::throughputFrameBudgetMicroseconds).
struct FieldThroughputPacing
{
    uint32_t batchCap{0};         // Triangle batch cap, kept across generations.
    uint32_t cycles{1};           // Construction cycles per frame.
    uint32_t probeFrames{0};      // Frames left that record no construction while the render interval settles.
    double   renderIntervalUs{0}; // Frame interval without construction; zero until measured.
    bool     probed{false};       // The current generation has measured the render interval.
};

struct FieldThroughputLimits
{
    double   budgetUs{0};    // Frame-interval budget that construction may always fill.
    double   budgetShare{0}; // Construction may also lengthen the render-only interval by this share.
    uint32_t maxCycles{1};
    uint32_t batchCeiling{1}; // Device batch ceiling.
    uint32_t triangleCount{0};
    uint32_t probeFrames{0}; // Construction-free frames that measure the render-only interval.
};

// Starts a generation: one cycle, the retained batch cap and a fresh render-interval probe.
void StartFieldThroughputGeneration(FieldThroughputPacing& pacing);

// Paces one frame of a building generation from the smoothed frame interval; `submitted` is whether
// this generation has submitted construction, so that the interval reflects it. Within the budget the
// batch cap doubles until it covers the generation, then the cycles. Over it, a generation without a
// render-interval measurement first records no construction for `probeFrames` frames: rendering alone
// may exceed the absolute budget, and shrinking construction would then never finish it. Otherwise the
// cycles halve, then the batch cap. Returns whether this frame records construction.
bool PaceFieldThroughput(FieldThroughputPacing& pacing, const FieldThroughputLimits& limits, double intervalUs, bool submitted);

} // namespace zen::rc
