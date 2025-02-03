#include "Graphics/RHI/DynamicRHI.h"
#include "Graphics/RHI/RHIDebug.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Common/Errors.h"
#include "Graphics/VulkanRHI/VulkanCommands.h"
#include "Graphics/VulkanRHI/VulkanDebug.h"

namespace zen::rhi
{
DynamicRHI* DynamicRHI::Create(GraphicsAPIType type)
{
    if (type == GraphicsAPIType::eVulkan)
    {
        return new VulkanRHI();
    }

    LOGE("Dynamic RHI creation failed! Unsupported Graphics API type!");

    return nullptr;
}

RHIDebug* RHIDebug::Create(DynamicRHI* RHI)
{
    VERIFY_EXPR(RHI != nullptr);
    if (RHI != nullptr && RHI->GetAPIType() == GraphicsAPIType::eVulkan)
    {
        return new VulkanDebug(RHI);
    }
    LOGE("Dynamic RHI creation failed! Unsupported Graphics API type!");

    return nullptr;
}

RHICommandList* RHICommandList::Create(GraphicsAPIType type, RHICommandListContext* context)
{
    VERIFY_EXPR(context != nullptr);
    if (context != nullptr && type == GraphicsAPIType::eVulkan)
    {
        return new VulkanCommandList(dynamic_cast<rhi::VulkanCommandListContext*>(context));
    }
    LOGE("Dynamic RHI creation failed! Unsupported Graphics API type!");

    return nullptr;
}
} // namespace zen::rhi