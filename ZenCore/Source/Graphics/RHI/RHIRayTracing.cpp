#include "Graphics/RHI/RHIRayTracing.h"
#include "Graphics/RHI/RHIResource.h"
#include <algorithm>
#include <limits>

namespace zen
{
namespace
{
bool HasStage(const RHIShaderGroupSPIRV& modules, uint32_t index, RHIShaderStage stage)
{
    return index < modules.GetModules().size() && modules.GetModules()[index].stage == stage;
}
} // namespace

bool ValidateRayTracingShaderGroups(const RHIShaderGroupSPIRV&                                modules,
                                    const HeapVector<RHIRayTracingShaderGroup>&               groups,
                                    std::array<uint32_t, RHIShaderBindingTableLayout::Count>& counts)
{
    std::array<uint32_t, RHIShaderBindingTableLayout::Count> result{};
    bool valid = !groups.empty() && groups.size() <= UINT32_MAX && modules.GetStageCount() != 0;
    for (const RHIShaderGroupSPIRV::Module& module : modules.GetModules())
    {
        valid = valid && module.stage >= RHIShaderStage::eRayGeneration && module.stage < RHIShaderStage::eMax;
    }
    for (const RHIRayTracingShaderGroup& group : groups)
    {
        if (!valid)
        {
            break;
        }
        const uint32_t type = ToUnderlying(group.type);
        if (group.type == RHIRayTracingShaderGroupType::eHit)
        {
            valid = group.generalShader == kUnusedRayTracingShader
                 && (group.closestHitShader != kUnusedRayTracingShader || group.anyHitShader != kUnusedRayTracingShader)
                 && (group.closestHitShader == kUnusedRayTracingShader
                     || HasStage(modules, group.closestHitShader, RHIShaderStage::eClosestHit))
                 && (group.anyHitShader == kUnusedRayTracingShader
                     || HasStage(modules, group.anyHitShader, RHIShaderStage::eAnyHit));
        }
        else
        {
            const RHIShaderStage stage = group.type == RHIRayTracingShaderGroupType::eRayGeneration
                                           ? RHIShaderStage::eRayGeneration
                                       : group.type == RHIRayTracingShaderGroupType::eMiss     ? RHIShaderStage::eMiss
                                       : group.type == RHIRayTracingShaderGroupType::eCallable ? RHIShaderStage::eCallable
                                                                                               : RHIShaderStage::eMax;
            valid                      = stage != RHIShaderStage::eMax && HasStage(modules, group.generalShader, stage)
                                      && group.closestHitShader == kUnusedRayTracingShader && group.anyHitShader == kUnusedRayTracingShader;
        }
        if (valid)
        {
            ++result[type];
        }
    }
    valid  = valid && result[RHIShaderBindingTableLayout::RayGeneration] == 1;
    counts = valid ? result : std::array<uint32_t, RHIShaderBindingTableLayout::Count>{};
    return valid;
}

bool BuildShaderBindingTableLayout(const RHIRayTracingCapabilities&                                capabilities,
                                   const std::array<uint32_t, RHIShaderBindingTableLayout::Count>& counts,
                                   RHIShaderBindingTableLayout&                                    output)
{
    RHIShaderBindingTableLayout layout;
    bool                        valid = capabilities.IsUsable() && counts[RHIShaderBindingTableLayout::RayGeneration] == 1;
    if (valid)
    {
        layout.alignment = std::max(capabilities.shaderGroupBaseAlignment, capabilities.shaderGroupHandleAlignment);
        const uint64_t handleAlignment = capabilities.shaderGroupHandleAlignment;
        const uint64_t stride = (uint64_t(capabilities.shaderGroupHandleSize) + handleAlignment - 1) & ~(handleAlignment - 1);
        valid                 = stride <= capabilities.maxShaderGroupStride;
        constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
        for (uint32_t region = 0; valid && region < RHIShaderBindingTableLayout::Count; ++region)
        {
            if (counts[region] != 0)
            {
                const uint64_t padding = layout.alignment - 1;
                valid                  = layout.size <= maximum - padding;
                if (valid)
                {
                    const uint64_t offset = (layout.size + padding) & ~padding;
                    valid                 = counts[region] <= (maximum - offset) / stride;
                    if (valid)
                    {
                        const uint64_t size    = stride * counts[region];
                        layout.regions[region] = {offset, stride, size};
                        layout.size            = offset + size;
                    }
                }
            }
        }
    }
    output = valid ? layout : RHIShaderBindingTableLayout{};
    return valid;
}
} // namespace zen
