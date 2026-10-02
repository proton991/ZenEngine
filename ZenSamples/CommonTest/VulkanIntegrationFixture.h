#pragma once
#include <filesystem>
#include "Graphics/VulkanRHI/VulkanRHI.h"

namespace zen::test
{
// Set zero is reserved for the global heap in every Vulkan pipeline layout.
inline constexpr uint32_t kLocalResourceSet = kGlobalBindlessHeapIndex + 1;

inline void SetShaderStage(RHIShaderCreateInfo& info, RHIShaderStage stage, const char* file)
{
    info.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));
    info.spirvFileName[ToUnderlying(stage)] =
        std::filesystem::relative(std::filesystem::path(RDG_REFLECTION_TEST_PATH) / file, SPV_SHADER_PATH).generic_string();
}

// Used only by the explicitly invoked VulkanRHIIntegrationTest executable.
// Objects are initialized through the same public entry points as the renderer.
class VulkanSession
{
public:
    VulkanSession()
    {
        GDynamicRHI = &rhi;
        rhi.Init();
    }

    ~VulkanSession()
    {
        rhi.Destroy();
        GDynamicRHI = nullptr;
        GVulkanRHI  = nullptr;
    }

    VulkanRHI rhi;
};

} // namespace zen::test
