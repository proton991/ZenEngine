#include "Graphics/RHI/DynamicRHI.h"
#include "Graphics/RHI/RHIDebug.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Utils/Errors.h"
#include "Graphics/VulkanRHI/VulkanDebug.h"

zen::DynamicRHI*   GDynamicRHI = nullptr;
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
        VERIFY_EXPR_MSG_F(false, "Dynamic RHI creation failed: unsupported graphics API {}", static_cast<uint32_t>(type));
    }

    pRHI->Init();

    GDynamicRHI = pRHI;

    return pRHI;
}

RHIDebug* RHIDebug::Create()
{
    RHIDebug* result{};

    if (GDynamicRHI != nullptr && GDynamicRHI->GetAPIType() == RHIAPIType::eVulkan)
    {
        result = ZEN_NEW() VulkanDebug();
    }
    else
    {
        LOGE("RHI debug creation requires a supported, initialized backend");

        result = nullptr;
    }

    return result;
}

} // namespace zen
