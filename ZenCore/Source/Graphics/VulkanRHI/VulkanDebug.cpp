#include "Graphics/VulkanRHI/VulkanTypes.h"
#include "Graphics/VulkanRHI/VulkanDebug.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"

namespace zen
{
VulkanDebug::VulkanDebug() {}

void VulkanDebug::SetPipelineDebugName(RHIPipeline* pipeline, NameID name)
{
    if (pipeline != nullptr)
    {
        GVulkanRHI->GetDevice()->SetObjectName(VK_OBJECT_TYPE_PIPELINE,
                                               reinterpret_cast<uint64_t>(TO_VK_PIPELINE(pipeline)->GetVkPipeline()), name);
    }
}

void VulkanDebug::SetTextureDebugName(RHITexture* texture, NameID name)
{
    if (texture != nullptr)
    {
        GVulkanRHI->GetDevice()->SetObjectName(VK_OBJECT_TYPE_IMAGE,
                                               reinterpret_cast<uint64_t>(TO_VK_TEXTURE(texture)->GetVkImage()), name);
    }
}
} // namespace zen
