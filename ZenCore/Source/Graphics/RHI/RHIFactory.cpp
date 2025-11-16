#include "Graphics/RHI/DynamicRHI.h"
#include "Graphics/RHI/RHIDebug.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Utils/Errors.h"
#include "Graphics/VulkanRHI/VulkanCommands.h"
#include "Graphics/VulkanRHI/VulkanDebug.h"

namespace zen::rhi
{
DynamicRHI* GDynamicRHI = nullptr;

DynamicRHI* DynamicRHI::Create(GraphicsAPIType type)
{
    DynamicRHI* RHI = nullptr;

    if (type == GraphicsAPIType::eVulkan)
    {
        RHI = new VulkanRHI();
    }
    else
    {
        LOGE("Dynamic RHI creation failed! Unsupported Graphics API type!");
    }

    GDynamicRHI = RHI;

    return RHI;
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