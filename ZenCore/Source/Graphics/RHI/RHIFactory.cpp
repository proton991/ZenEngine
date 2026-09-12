#include "Graphics/RHI/DynamicRHI.h"
#include "Graphics/RHI/RHIDebug.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Utils/Errors.h"
#include "Graphics/VulkanRHI/VulkanDebug.h"

zen::DynamicRHI* GDynamicRHI = nullptr;
zen::RHIFrameState GRHIFrameState;

namespace zen
{
DynamicRHI* DynamicRHI::Create(RHIAPIType type)
{
    DynamicRHI* pRHI = nullptr;

    if (type == RHIAPIType::eVulkan)
    {
        pRHI = ZEN_NEW() VulkanRHI();
    }
    else
    {
        LOGE("Dynamic RHI creation failed! Unsupported Graphics API type!");
    }

    pRHI->Init();

    GDynamicRHI = pRHI;

    return pRHI;
}

RHIDebug* RHIDebug::Create()
{
    RHIDebug* result{};

    // VERIFY_EXPR(RHI != nullptr);
    if (GDynamicRHI != nullptr && GDynamicRHI->GetAPIType() == RHIAPIType::eVulkan)
    {
        result = ZEN_NEW() VulkanDebug();
    }
    else
    {
        LOGE("Dynamic RHI creation failed! Unsupported Graphics API type!");

        result = nullptr;
    }

    return result;
}

} // namespace zen
