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
    // VERIFY_EXPR(RHI != nullptr);
    if (GDynamicRHI != nullptr && GDynamicRHI->GetAPIType() == RHIAPIType::eVulkan)
    {
        return ZEN_NEW() VulkanDebug();
    }
    LOGE("Dynamic RHI creation failed! Unsupported Graphics API type!");

    return nullptr;
}

LegacyRHICommandList* LegacyRHICommandList::Create(RHIAPIType type,
                                                   LegacyRHICommandListContext* pContext)
{
    VERIFY_EXPR(pContext != nullptr);
    if (pContext != nullptr && type == RHIAPIType::eVulkan)
    {
        return ZEN_NEW() LegacyVulkanCommandList(
            dynamic_cast<LegacyVulkanCommandListContext*>(pContext));
    }
    LOGE("Dynamic RHI creation failed! Unsupported Graphics API type!");

    return nullptr;
}
} // namespace zen
