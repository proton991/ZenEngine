#include "Utils/Errors.h"
#include "Graphics/Val/VulkanDebug.h"

namespace zen::val
{
void SetObjectName(VkDevice device,
                   uint64_t objectHandle,
                   VkObjectType objectType,
                   const char* pName)
{
    // Check for valid function pointer (may not be present if not running in a debug mode)
    VkDebugUtilsObjectNameInfoEXT ObjectNameInfo{};
    ObjectNameInfo.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    ObjectNameInfo.pNext        = nullptr;
    ObjectNameInfo.objectType   = objectType;
    ObjectNameInfo.objectHandle = objectHandle;
    ObjectNameInfo.pObjectName  = pName;

    VkResult res = vkSetDebugUtilsObjectNameEXT(device, &ObjectNameInfo);
    VERIFY_EXPR(res == VK_SUCCESS);
    (void)res;
}

void SetDeviceName(VkDevice device, const char* pName)
{
    SetObjectName(device, reinterpret_cast<uint64_t>(device), VK_OBJECT_TYPE_DEVICE, pName);
}

void SetCommandPoolName(VkDevice device, VkCommandPool cmdPool, const char* pName)
{
    SetObjectName(device, (uint64_t)cmdPool, VK_OBJECT_TYPE_COMMAND_POOL, pName);
}

void SetCommandBufferName(VkDevice device, VkCommandBuffer cmdBuffer, const char* pName)
{
    SetObjectName(device, (uint64_t)cmdBuffer, VK_OBJECT_TYPE_COMMAND_BUFFER, pName);
}

void SetQueueName(VkDevice device, VkQueue queue, const char* pName)
{
    SetObjectName(device, (uint64_t)queue, VK_OBJECT_TYPE_QUEUE, pName);
}

void SetImageName(VkDevice device, VkImage image, const char* pName)
{
    SetObjectName(device, (uint64_t)image, VK_OBJECT_TYPE_IMAGE, pName);
}

void SetImageViewName(VkDevice device, VkImageView imageView, const char* pName)
{
    SetObjectName(device, (uint64_t)imageView, VK_OBJECT_TYPE_IMAGE_VIEW, pName);
}

void SetSamplerName(VkDevice device, VkSampler sampler, const char* pName)
{
    SetObjectName(device, (uint64_t)sampler, VK_OBJECT_TYPE_SAMPLER, pName);
}

void SetBufferName(VkDevice device, VkBuffer buffer, const char* pName)
{
    SetObjectName(device, (uint64_t)buffer, VK_OBJECT_TYPE_BUFFER, pName);
}

void SetBufferViewName(VkDevice device, VkBufferView bufferView, const char* pName)
{
    SetObjectName(device, (uint64_t)bufferView, VK_OBJECT_TYPE_BUFFER_VIEW, pName);
}

void SetDeviceMemoryName(VkDevice device, VkDeviceMemory memory, const char* pName)
{
    SetObjectName(device, (uint64_t)memory, VK_OBJECT_TYPE_DEVICE_MEMORY, pName);
}

void SetShaderModuleName(VkDevice device, VkShaderModule shaderModule, const char* pName)
{
    SetObjectName(device, (uint64_t)shaderModule, VK_OBJECT_TYPE_SHADER_MODULE, pName);
}

void SetPipelineName(VkDevice device, VkPipeline pipeline, const char* pName)
{
    SetObjectName(device, (uint64_t)pipeline, VK_OBJECT_TYPE_PIPELINE, pName);
}

void SetPipelineLayoutName(VkDevice device, VkPipelineLayout pipelineLayout, const char* pName)
{
    SetObjectName(device, (uint64_t)pipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT, pName);
}

void SetRenderPassName(VkDevice device, VkRenderPass renderPass, const char* pName)
{
    SetObjectName(device, (uint64_t)renderPass, VK_OBJECT_TYPE_RENDER_PASS, pName);
}

void SetFramebufferName(VkDevice device, VkFramebuffer framebuffer, const char* pName)
{
    SetObjectName(device, (uint64_t)framebuffer, VK_OBJECT_TYPE_FRAMEBUFFER, pName);
}

void SetDescriptorSetLayoutName(VkDevice device,
                                VkDescriptorSetLayout descriptorSetLayout,
                                const char* pName)
{
    SetObjectName(device, (uint64_t)descriptorSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                  pName);
}

void SetDescriptorSetName(VkDevice device, VkDescriptorSet descriptorSet, const char* pName)
{
    SetObjectName(device, (uint64_t)descriptorSet, VK_OBJECT_TYPE_DESCRIPTOR_SET, pName);
}

void SetDescriptorPoolName(VkDevice device, VkDescriptorPool descriptorPool, const char* pName)
{
    SetObjectName(device, (uint64_t)descriptorPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL, pName);
}

void SetSemaphoreName(VkDevice device, VkSemaphore semaphore, const char* pName)
{
    SetObjectName(device, (uint64_t)semaphore, VK_OBJECT_TYPE_SEMAPHORE, pName);
}

void SetFenceName(VkDevice device, VkFence fence, const char* pName)
{
    SetObjectName(device, (uint64_t)fence, VK_OBJECT_TYPE_FENCE, pName);
}

void SetEventName(VkDevice device, VkEvent _event, const char* pName)
{
    SetObjectName(device, (uint64_t)_event, VK_OBJECT_TYPE_EVENT, pName);
}

void SetQueryPoolName(VkDevice device, VkQueryPool queryPool, const char* pName)
{
    SetObjectName(device, (uint64_t)queryPool, VK_OBJECT_TYPE_QUERY_POOL, pName);
}

void SetAccelStructName(VkDevice device, VkAccelerationStructureKHR accelStruct, const char* pName)
{
    SetObjectName(device, (uint64_t)accelStruct, VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, pName);
}

void SetPipelineCacheName(VkDevice device, VkPipelineCache pipeCache, const char* pName)
{
    SetObjectName(device, (uint64_t)pipeCache, VK_OBJECT_TYPE_PIPELINE_CACHE, pName);
}
enum class VulkanHandleTypeId : uint32_t
{
    CommandPool,
    CommandBuffer,
    Buffer,
    BufferView,
    Image,
    ImageView,
    DeviceMemory,
    Fence,
    RenderPass,
    Pipeline,
    ShaderModule,
    PipelineLayout,
    Sampler,
    Framebuffer,
    DescriptorPool,
    DescriptorSetLayout,
    DescriptorSet,
    Semaphore,
    Queue,
    Event,
    QueryPool,
    AccelerationStructureKHR,
    PipelineCache
};


template <>
void SetVulkanObjectName<VkCommandPool, VulkanHandleTypeId::CommandPool>(VkDevice device,
                                                                         VkCommandPool cmdPool,
                                                                         const char* pName)
{
    SetCommandPoolName(device, cmdPool, pName);
}

template <> void SetVulkanObjectName<VkCommandBuffer, VulkanHandleTypeId::CommandBuffer>(
    VkDevice device,
    VkCommandBuffer cmdBuffer,
    const char* pName)
{
    SetCommandBufferName(device, cmdBuffer, pName);
}

template <> void SetVulkanObjectName<VkQueue, VulkanHandleTypeId::Queue>(VkDevice device,
                                                                         VkQueue queue,
                                                                         const char* pName)
{
    SetQueueName(device, queue, pName);
}

template <> void SetVulkanObjectName<VkImage, VulkanHandleTypeId::Image>(VkDevice device,
                                                                         VkImage image,
                                                                         const char* pName)
{
    SetImageName(device, image, pName);
}

template <>
void SetVulkanObjectName<VkImageView, VulkanHandleTypeId::ImageView>(VkDevice device,
                                                                     VkImageView imageView,
                                                                     const char* pName)
{
    SetImageViewName(device, imageView, pName);
}

template <> void SetVulkanObjectName<VkSampler, VulkanHandleTypeId::Sampler>(VkDevice device,
                                                                             VkSampler sampler,
                                                                             const char* pName)
{
    SetSamplerName(device, sampler, pName);
}

template <> void SetVulkanObjectName<VkBuffer, VulkanHandleTypeId::Buffer>(VkDevice device,
                                                                           VkBuffer buffer,
                                                                           const char* pName)
{
    SetBufferName(device, buffer, pName);
}

template <>
void SetVulkanObjectName<VkBufferView, VulkanHandleTypeId::BufferView>(VkDevice device,
                                                                       VkBufferView bufferView,
                                                                       const char* pName)
{
    SetBufferViewName(device, bufferView, pName);
}

template <>
void SetVulkanObjectName<VkDeviceMemory, VulkanHandleTypeId::DeviceMemory>(VkDevice device,
                                                                           VkDeviceMemory memory,
                                                                           const char* pName)
{
    SetDeviceMemoryName(device, memory, pName);
}

template <> void SetVulkanObjectName<VkShaderModule, VulkanHandleTypeId::ShaderModule>(
    VkDevice device,
    VkShaderModule shaderModule,
    const char* pName)
{
    SetShaderModuleName(device, shaderModule, pName);
}

template <> void SetVulkanObjectName<VkPipeline, VulkanHandleTypeId::Pipeline>(VkDevice device,
                                                                               VkPipeline pipeline,
                                                                               const char* pName)
{
    SetPipelineName(device, pipeline, pName);
}

template <> void SetVulkanObjectName<VkPipelineLayout, VulkanHandleTypeId::PipelineLayout>(
    VkDevice device,
    VkPipelineLayout pipelineLayout,
    const char* pName)
{
    SetPipelineLayoutName(device, pipelineLayout, pName);
}

template <>
void SetVulkanObjectName<VkRenderPass, VulkanHandleTypeId::RenderPass>(VkDevice device,
                                                                       VkRenderPass renderPass,
                                                                       const char* pName)
{
    SetRenderPassName(device, renderPass, pName);
}

template <>
void SetVulkanObjectName<VkFramebuffer, VulkanHandleTypeId::Framebuffer>(VkDevice device,
                                                                         VkFramebuffer framebuffer,
                                                                         const char* pName)
{
    SetFramebufferName(device, framebuffer, pName);
}

template <>
void SetVulkanObjectName<VkDescriptorSetLayout, VulkanHandleTypeId::DescriptorSetLayout>(
    VkDevice device,
    VkDescriptorSetLayout descriptorSetLayout,
    const char* pName)
{
    SetDescriptorSetLayoutName(device, descriptorSetLayout, pName);
}

template <> void SetVulkanObjectName<VkDescriptorSet, VulkanHandleTypeId::DescriptorSet>(
    VkDevice device,
    VkDescriptorSet descriptorSet,
    const char* pName)
{
    SetDescriptorSetName(device, descriptorSet, pName);
}

template <> void SetVulkanObjectName<VkDescriptorPool, VulkanHandleTypeId::DescriptorPool>(
    VkDevice device,
    VkDescriptorPool descriptorPool,
    const char* pName)
{
    SetDescriptorPoolName(device, descriptorPool, pName);
}

template <>
void SetVulkanObjectName<VkSemaphore, VulkanHandleTypeId::Semaphore>(VkDevice device,
                                                                     VkSemaphore semaphore,
                                                                     const char* pName)
{
    SetSemaphoreName(device, semaphore, pName);
}

template <> void SetVulkanObjectName<VkFence, VulkanHandleTypeId::Fence>(VkDevice device,
                                                                         VkFence fence,
                                                                         const char* pName)
{
    SetFenceName(device, fence, pName);
}

template <> void SetVulkanObjectName<VkEvent, VulkanHandleTypeId::Event>(VkDevice device,
                                                                         VkEvent _event,
                                                                         const char* pName)
{
    SetEventName(device, _event, pName);
}

template <>
void SetVulkanObjectName<VkQueryPool, VulkanHandleTypeId::QueryPool>(VkDevice device,
                                                                     VkQueryPool queryPool,
                                                                     const char* pName)
{
    SetQueryPoolName(device, queryPool, pName);
}

template <>
void SetVulkanObjectName<VkAccelerationStructureKHR, VulkanHandleTypeId::AccelerationStructureKHR>(
    VkDevice device,
    VkAccelerationStructureKHR accelStruct,
    const char* pName)
{
    SetAccelStructName(device, accelStruct, pName);
}

template <> void SetVulkanObjectName<VkPipelineCache, VulkanHandleTypeId::PipelineCache>(
    VkDevice device,
    VkPipelineCache pipeCache,
    const char* pName)
{
    SetPipelineCacheName(device, pipeCache, pName);
}


const char* VkResultToString(VkResult errorCode)
{
    switch (errorCode)
    {
        // clang-format off
#define STR(r) case VK_ ## r: return #r
        STR(NOT_READY);
        STR(TIMEOUT);
        STR(EVENT_SET);
        STR(EVENT_RESET);
        STR(INCOMPLETE);
        STR(ERROR_OUT_OF_HOST_MEMORY);
        STR(ERROR_OUT_OF_DEVICE_MEMORY);
        STR(ERROR_INITIALIZATION_FAILED);
        STR(ERROR_DEVICE_LOST);
        STR(ERROR_MEMORY_MAP_FAILED);
        STR(ERROR_LAYER_NOT_PRESENT);
        STR(ERROR_EXTENSION_NOT_PRESENT);
        STR(ERROR_FEATURE_NOT_PRESENT);
        STR(ERROR_INCOMPATIBLE_DRIVER);
        STR(ERROR_TOO_MANY_OBJECTS);
        STR(ERROR_FORMAT_NOT_SUPPORTED);
        STR(ERROR_SURFACE_LOST_KHR);
        STR(ERROR_NATIVE_WINDOW_IN_USE_KHR);
        STR(SUBOPTIMAL_KHR);
        STR(ERROR_OUT_OF_DATE_KHR);
        STR(ERROR_INCOMPATIBLE_DISPLAY_KHR);
        STR(ERROR_VALIDATION_FAILED_EXT);
        STR(ERROR_INVALID_SHADER_NV);
        STR(ERROR_FRAGMENTED_POOL);
        STR(ERROR_UNKNOWN);
        STR(ERROR_OUT_OF_POOL_MEMORY);
        STR(ERROR_INVALID_EXTERNAL_HANDLE);
        STR(ERROR_FRAGMENTATION);
        STR(ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS);
        STR(ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT);
        STR(ERROR_NOT_PERMITTED_EXT);
        STR(ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT);
        STR(THREAD_IDLE_KHR);
        STR(THREAD_DONE_KHR);
        STR(OPERATION_DEFERRED_KHR);
        STR(OPERATION_NOT_DEFERRED_KHR);
        STR(PIPELINE_COMPILE_REQUIRED_EXT);
#undef STR
            // clang-format on
        default: return "UNKNOWN_ERROR";
    }
}

const char* VkAccessFlagBitToString(VkAccessFlagBits Bit)
{
    LOG_ERROR(Bit != 0 && (Bit & (Bit - 1)) == 0, "Single bit is expected");
    switch (Bit)
    {
        // clang-format off
#define ACCESS_FLAG_BIT_TO_STRING(ACCESS_FLAG_BIT)case ACCESS_FLAG_BIT: return #ACCESS_FLAG_BIT;
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_INDIRECT_COMMAND_READ_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_INDEX_READ_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_UNIFORM_READ_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_INPUT_ATTACHMENT_READ_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_SHADER_READ_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_SHADER_WRITE_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_TRANSFER_READ_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_TRANSFER_WRITE_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_HOST_READ_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_HOST_WRITE_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_MEMORY_READ_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_MEMORY_WRITE_BIT)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR)
        ACCESS_FLAG_BIT_TO_STRING(VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR)
#undef ACCESS_FLAG_BIT_TO_STRING
        default: LOGE("Unexpected bit"); return "";
            // clang-format on
    }
}

const char* VkImageLayoutToString(VkImageLayout Layout)
{
    switch (Layout)
    {
        // clang-format off
#define IMAGE_LAYOUT_TO_STRING(LAYOUT)case LAYOUT: return #LAYOUT;
        IMAGE_LAYOUT_TO_STRING(VK_IMAGE_LAYOUT_UNDEFINED)
        IMAGE_LAYOUT_TO_STRING(VK_IMAGE_LAYOUT_GENERAL)
        IMAGE_LAYOUT_TO_STRING(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        IMAGE_LAYOUT_TO_STRING(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
        IMAGE_LAYOUT_TO_STRING(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
        IMAGE_LAYOUT_TO_STRING(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        IMAGE_LAYOUT_TO_STRING(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        IMAGE_LAYOUT_TO_STRING(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        IMAGE_LAYOUT_TO_STRING(VK_IMAGE_LAYOUT_PREINITIALIZED)
        IMAGE_LAYOUT_TO_STRING(VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL)
        IMAGE_LAYOUT_TO_STRING(VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL)
        IMAGE_LAYOUT_TO_STRING(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        IMAGE_LAYOUT_TO_STRING(VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR)
#undef IMAGE_LAYOUT_TO_STRING
        default: LOGE("Unknown layout"); return "";
            // clang-format on
    }
}

std::string VkAccessFlagsToString(VkAccessFlags Flags)
{
    std::string FlagsString;
    while (Flags != 0)
    {
        auto Bit = Flags & ~(Flags - 1);
        if (!FlagsString.empty())
            FlagsString += ", ";
        FlagsString += VkAccessFlagBitToString(static_cast<VkAccessFlagBits>(Bit));
        Flags = Flags & (Flags - 1);
    }
    return FlagsString;
}

const char* VkObjectTypeToString(VkObjectType ObjectType)
{
    switch (ObjectType)
    {
            // clang-format off
        case VK_OBJECT_TYPE_UNKNOWN:                        return "unknown";
        case VK_OBJECT_TYPE_INSTANCE:                       return "instance";
        case VK_OBJECT_TYPE_PHYSICAL_DEVICE:                return "physical device";
        case VK_OBJECT_TYPE_DEVICE:                         return "device";
        case VK_OBJECT_TYPE_QUEUE:                          return "queue";
        case VK_OBJECT_TYPE_SEMAPHORE:                      return "semaphore";
        case VK_OBJECT_TYPE_COMMAND_BUFFER:                 return "cmd buffer";
        case VK_OBJECT_TYPE_FENCE:                          return "fence";
        case VK_OBJECT_TYPE_DEVICE_MEMORY:                  return "memory";
        case VK_OBJECT_TYPE_BUFFER:                         return "buffer";
        case VK_OBJECT_TYPE_IMAGE:                          return "image";
        case VK_OBJECT_TYPE_EVENT:                          return "event";
        case VK_OBJECT_TYPE_QUERY_POOL:                     return "query pool";
        case VK_OBJECT_TYPE_BUFFER_VIEW:                    return "buffer view";
        case VK_OBJECT_TYPE_IMAGE_VIEW:                     return "image view";
        case VK_OBJECT_TYPE_SHADER_MODULE:                  return "shader module";
        case VK_OBJECT_TYPE_PIPELINE_CACHE:                 return "pipeline cache";
        case VK_OBJECT_TYPE_PIPELINE_LAYOUT:                return "pipeline layout";
        case VK_OBJECT_TYPE_RENDER_PASS:                    return "render pass";
        case VK_OBJECT_TYPE_PIPELINE:                       return "pipeline";
        case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT:          return "descriptor set layout";
        case VK_OBJECT_TYPE_SAMPLER:                        return "sampler";
        case VK_OBJECT_TYPE_DESCRIPTOR_POOL:                return "descriptor pool";
        case VK_OBJECT_TYPE_DESCRIPTOR_SET:                 return "descriptor set";
        case VK_OBJECT_TYPE_FRAMEBUFFER:                    return "framebuffer";
        case VK_OBJECT_TYPE_COMMAND_POOL:                   return "command pool";
        case VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION:       return "sampler ycbcr conversion";
        case VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE:     return "descriptor update template";
        case VK_OBJECT_TYPE_SURFACE_KHR:                    return "surface KHR";
        case VK_OBJECT_TYPE_SWAPCHAIN_KHR:                  return "swapchain KHR";
        case VK_OBJECT_TYPE_DISPLAY_KHR:                    return "display KHR";
        case VK_OBJECT_TYPE_DISPLAY_MODE_KHR:               return "display mode KHR";
        case VK_OBJECT_TYPE_DEBUG_REPORT_CALLBACK_EXT:      return "debug report callback";
        case VK_OBJECT_TYPE_DEBUG_UTILS_MESSENGER_EXT:      return "debug utils messenger";
        case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR:     return "acceleration structure KHR";
        case VK_OBJECT_TYPE_VALIDATION_CACHE_EXT:           return "validation cache EXT";
        case VK_OBJECT_TYPE_PERFORMANCE_CONFIGURATION_INTEL:return "performance configuration INTEL";
        case VK_OBJECT_TYPE_DEFERRED_OPERATION_KHR:         return "deferred operation KHR";
        case VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_NV:    return "indirect commands layout NV";
        case VK_OBJECT_TYPE_PRIVATE_DATA_SLOT_EXT:          return "private data slot EXT";
        default: return "unknown";
            // clang-format on
    }
}

} // namespace zen::val
