#pragma once
#include "RHICommon.h"

namespace zen
{
class RHIShader;
class RHIShaderGroupSPIRV;

inline constexpr uint32_t kUnusedRayTracingShader = ~0u;

enum class RHIRayTracingShaderGroupType : uint32_t
{
    eRayGeneration,
    eMiss,
    eHit,
    eCallable
};

struct RHIRayTracingShaderGroup
{
    RHIRayTracingShaderGroupType type{RHIRayTracingShaderGroupType::eHit};
    uint32_t                     generalShader{kUnusedRayTracingShader};
    uint32_t                     closestHitShader{kUnusedRayTracingShader};
    uint32_t                     anyHitShader{kUnusedRayTracingShader};
};

struct RHIRayTracingPipelineCreateInfo
{
    RHIShader*                           pShader{nullptr};
    HeapVector<RHIRayTracingShaderGroup> groups;
    uint32_t                             maxRecursionDepth{1};
};

struct RHIShaderBindingTableRegion
{
    uint64_t offset{0};
    uint64_t stride{0};
    uint64_t size{0};
};

// Offsets are relative to a base address aligned to alignment. Empty regions stay zero.
// Records contain pipeline shader-group handles; materials are addressed through instances.
struct RHIShaderBindingTableLayout
{
    enum Region : uint32_t
    {
        RayGeneration,
        Miss,
        Hit,
        Callable,
        Count
    };
    std::array<RHIShaderBindingTableRegion, Count> regions{};
    uint64_t                                       size{0};
    uint32_t                                       alignment{0};
};

// Rejects unusable capabilities, anything other than one raygen record, and overflow.
// Failure clears output, so no partial layout can be submitted.
bool BuildShaderBindingTableLayout(const RHIRayTracingCapabilities&                                capabilities,
                                   const std::array<uint32_t, RHIShaderBindingTableLayout::Count>& counts,
                                   RHIShaderBindingTableLayout&                                    output);
bool ValidateRayTracingShaderGroups(const RHIShaderGroupSPIRV&                                modules,
                                    const HeapVector<RHIRayTracingShaderGroup>&               groups,
                                    std::array<uint32_t, RHIShaderBindingTableLayout::Count>& counts);
} // namespace zen
