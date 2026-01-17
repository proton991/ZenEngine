#include "Graphics/RHI/DynamicRHI.h"
#include "Graphics/RHI/RHIDebug.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Utils/Errors.h"
#include "Graphics/VulkanRHI/VulkanCommands.h"
#include "Graphics/VulkanRHI/VulkanDebug.h"

zen::DynamicRHI* GDynamicRHI = nullptr;

namespace zen
{
DynamicRHI* DynamicRHI::Create(RHIAPIType type)
{
    DynamicRHI* RHI = nullptr;

    if (type == RHIAPIType::eVulkan)
    {
        RHI = ZEN_NEW() VulkanRHI();
    }
    else
    {
        LOGE("Dynamic RHI creation failed! Unsupported Graphics API type!");
    }

    RHI->Init();

    GDynamicRHI = RHI;

    return RHI;
}

RHIDebug* RHIDebug::Create()
{
    // VERIFY_EXPR(RHI != nullptr);
    if (GDynamicRHI != nullptr && GDynamicRHI->GetAPIType() == RHIAPIType::eVulkan)
    {
        return ZEN_NEW() VulkanDebug();
    }
    LOGE("Dynamic RHI creation failed! Unsupported Graphics API type!");

    return nullptr;
}

RHICommandList* RHICommandList::Create(RHIAPIType type, RHICommandListContext* context)
{
    VERIFY_EXPR(context != nullptr);
    if (context != nullptr && type == RHIAPIType::eVulkan)
    {
        return ZEN_NEW() VulkanCommandList(dynamic_cast<VulkanCommandListContext*>(context));
    }
    LOGE("Dynamic RHI creation failed! Unsupported Graphics API type!");

    return nullptr;
}
} // namespace zen