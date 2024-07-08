#include "Common/Errors.h"
#include "Graphics/Val/VulkanDebug.h"

namespace zen::val
{
void SetObjectName(VkDevice device,
                   uint64_t objectHandle,
                   VkObjectType objectType,
                   const char* name)
{
    // Check for valid function pointer (may not be present if not running in a debug mode)
    VkDebugUtilsObjectNameInfoEXT ObjectNameInfo{};
    ObjectNameInfo.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    ObjectNameInfo.pNext        = nullptr;
    ObjectNameInfo.objectType   = objectType;
    ObjectNameInfo.objectHandle = objectHandle;
    ObjectNameInfo.pObjectName  = name;

    VkResult res = vkSetDebugUtilsObjectNameEXT(device, &ObjectNameInfo);
    VERIFY_EXPR(res == VK_SUCCESS);
    (void)res;
}

void SetDeviceName(VkDevice device, const char* name)
{
    SetObjectName(device, reinterpret_cast<uint64_t>(device), VK_OBJECT_TYPE_DEVICE, name);
}

void SetCommandPoolName(VkDevice device, VkCommandPool cmdPool, const char* name)
{
    SetObjectName(device, (uint64_t)cmdPool, VK_OBJECT_TYPE_COMMAND_POOL, name);
}

void SetCommandBufferName(VkDevice device, VkCommandBuffer cmdBuffer, const char* name)
{
    SetObjectName(device, (uint64_t)cmdBuffer, VK_OBJECT_TYPE_COMMAND_BUFFER, name);
}

void SetQueueName(VkDevice device, VkQueue queue, const char* name)
{
    SetObjectName(device, (uint64_t)queue, VK_OBJECT_TYPE_QUEUE, name);
}

void SetImageName(VkDevice device, VkImage image, const char* name)
{
    SetObjectName(device, (uint64_t)image, VK_OBJECT_TYPE_IMAGE, name);
}

void SetImageViewName(VkDevice device, VkImageView imageView, const char* name)
{
    SetObjectName(device, (uint64_t)imageView, VK_OBJECT_TYPE_IMAGE_VIEW, name);
}

void SetSamplerName(VkDevice device, VkSampler sampler, const char* name)
{
    SetObjectName(device, (uint64_t)sampler, VK_OBJECT_TYPE_SAMPLER, name);
}

void SetBufferName(VkDevice device, VkBuffer buffer, const char* name)
{
    SetObjectName(device, (uint64_t)buffer, VK_OBJECT_TYPE_BUFFER, name);
}

void SetBufferViewName(VkDevice device, VkBufferView bufferView, const char* name)
{
    SetObjectName(device, (uint64_t)bufferView, VK_OBJECT_TYPE_BUFFER_VIEW, name);
}

void SetDeviceMemoryName(VkDevice device, VkDeviceMemory memory, const char* name)
{
    SetObjectName(device, (uint64_t)memory, VK_OBJECT_TYPE_DEVICE_MEMORY, name);
}

void SetShaderModuleName(VkDevice device, VkShaderModule shaderModule, const char* name)
{
    SetObjectName(device, (uint64_t)shaderModule, VK_OBJECT_TYPE_SHADER_MODULE, name);
}

void SetPipelineName(VkDevice device, VkPipeline pipeline, const char* name)
{
    SetObjectName(device, (uint64_t)pipeline, VK_OBJECT_TYPE_PIPELINE, name);
}

void SetPipelineLayoutName(VkDevice device, VkPipelineLayout pipelineLayout, const char* name)
{
    SetObjectName(device, (uint64_t)pipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT, name);
}

void SetRenderPassName(VkDevice device, VkRenderPass renderPass, const char* name)
{
    SetObjectName(device, (uint64_t)renderPass, VK_OBJECT_TYPE_RENDER_PASS, name);
}

void SetFramebufferName(VkDevice device, VkFramebuffer framebuffer, const char* name)
{
    SetObjectName(device, (uint64_t)framebuffer, VK_OBJECT_TYPE_FRAMEBUFFER, name);
}

void SetDescriptorSetLayoutName(VkDevice device,
                                VkDescriptorSetLayout descriptorSetLayout,
                                const char* name)
{
    SetObjectName(device, (uint64_t)descriptorSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                  name);
}

void SetDescriptorSetName(VkDevice device, VkDescriptorSet descriptorSet, const char* name)
{
    SetObjectName(device, (uint64_t)descriptorSet, VK_OBJECT_TYPE_DESCRIPTOR_SET, name);
}

void SetDescriptorPoolName(VkDevice device, VkDescriptorPool descriptorPool, const char* name)
{
    SetObjectName(device, (uint64_t)descriptorPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL, name);
}

void SetSemaphoreName(VkDevice device, VkSemaphore semaphore, const char* name)
{
    SetObjectName(device, (uint64_t)semaphore, VK_OBJECT_TYPE_SEMAPHORE, name);
}

void SetFenceName(VkDevice device, VkFence fence, const char* name)
{
    SetObjectName(device, (uint64_t)fence, VK_OBJECT_TYPE_FENCE, name);
}

void SetEventName(VkDevice device, VkEvent _event, const char* name)
{
    SetObjectName(device, (uint64_t)_event, VK_OBJECT_TYPE_EVENT, name);
}

void SetQueryPoolName(VkDevice device, VkQueryPool queryPool, const char* name)
{
    SetObjectName(device, (uint64_t)queryPool, VK_OBJECT_TYPE_QUERY_POOL, name);
}

void SetAccelStructName(VkDevice device, VkAccelerationStructureKHR accelStruct, const char* name)
{
    SetObjectName(device, (uint64_t)accelStruct, VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, name);
}

void SetPipelineCacheName(VkDevice device, VkPipelineCache pipeCache, const char* name)
{
    SetObjectName(device, (uint64_t)pipeCache, VK_OBJECT_TYPE_PIPELINE_CACHE, name);
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
                                                                         const char* name)
{
    SetCommandPoolName(device, cmdPool, name);
}

template <> void SetVulkanObjectName<VkCommandBuffer, VulkanHandleTypeId::CommandBuffer>(
    VkDevice device,
    VkCommandBuffer cmdBuffer,
    const char* name)
{
    SetCommandBufferName(device, cmdBuffer, name);
}

template <> void SetVulkanObjectName<VkQueue, VulkanHandleTypeId::Queue>(VkDevice device,
                                                                         VkQueue queue,
                                                                         const char* name)
{
    SetQueueName(device, queue, name);
}

template <> void SetVulkanObjectName<VkImage, VulkanHandleTypeId::Image>(VkDevice device,
                                                                         VkImage image,
                                                                         const char* name)
{
    SetImageName(device, image, name);
}

template <>
void SetVulkanObjectName<VkImageView, VulkanHandleTypeId::ImageView>(VkDevice device,
                                                                     VkImageView imageView,
                                                                     const char* name)
{
    SetImageViewName(device, imageView, name);
}

template <> void SetVulkanObjectName<VkSampler, VulkanHandleTypeId::Sampler>(VkDevice device,
                                                                             VkSampler sampler,
                                                                             const char* name)
{
    SetSamplerName(device, sampler, name);
}

template <> void SetVulkanObjectName<VkBuffer, VulkanHandleTypeId::Buffer>(VkDevice device,
                                                                           VkBuffer buffer,
                                                                           const char* name)
{
    SetBufferName(device, buffer, name);
}

template <>
void SetVulkanObjectName<VkBufferView, VulkanHandleTypeId::BufferView>(VkDevice device,
                                                                       VkBufferView bufferView,
                                                                       const char* name)
{
    SetBufferViewName(device, bufferView, name);
}

template <>
void SetVulkanObjectName<VkDeviceMemory, VulkanHandleTypeId::DeviceMemory>(VkDevice device,
                                                                           VkDeviceMemory memory,
                                                                           const char* name)
{
    SetDeviceMemoryName(device, memory, name);
}

template <> void SetVulkanObjectName<VkShaderModule, VulkanHandleTypeId::ShaderModule>(
    VkDevice device,
    VkShaderModule shaderModule,
    const char* name)
{
    SetShaderModuleName(device, shaderModule, name);
}

template <> void SetVulkanObjectName<VkPipeline, VulkanHandleTypeId::Pipeline>(VkDevice device,
                                                                               VkPipeline pipeline,
                                                                               const char* name)
{
    SetPipelineName(device, pipeline, name);
}

template <> void SetVulkanObjectName<VkPipelineLayout, VulkanHandleTypeId::PipelineLayout>(
    VkDevice device,
    VkPipelineLayout pipelineLayout,
    const char* name)
{
    SetPipelineLayoutName(device, pipelineLayout, name);
}

template <>
void SetVulkanObjectName<VkRenderPass, VulkanHandleTypeId::RenderPass>(VkDevice device,
                                                                       VkRenderPass renderPass,
                                                                       const char* name)
{
    SetRenderPassName(device, renderPass, name);
}

template <>
void SetVulkanObjectName<VkFramebuffer, VulkanHandleTypeId::Framebuffer>(VkDevice device,
                                                                         VkFramebuffer framebuffer,
                                                                         const char* name)
{
    SetFramebufferName(device, framebuffer, name);
}

template <>
void SetVulkanObjectName<VkDescriptorSetLayout, VulkanHandleTypeId::DescriptorSetLayout>(
    VkDevice device,
    VkDescriptorSetLayout descriptorSetLayout,
    const char* name)
{
    SetDescriptorSetLayoutName(device, descriptorSetLayout, name);
}

template <> void SetVulkanObjectName<VkDescriptorSet, VulkanHandleTypeId::DescriptorSet>(
    VkDevice device,
    VkDescriptorSet descriptorSet,
    const char* name)
{
    SetDescriptorSetName(device, descriptorSet, name);
}

template <> void SetVulkanObjectName<VkDescriptorPool, VulkanHandleTypeId::DescriptorPool>(
    VkDevice device,
    VkDescriptorPool descriptorPool,
    const char* name)
{
    SetDescriptorPoolName(device, descriptorPool, name);
}

template <>
void SetVulkanObjectName<VkSemaphore, VulkanHandleTypeId::Semaphore>(VkDevice device,
                                                                     VkSemaphore semaphore,
                                                                     const char* name)
{
    SetSemaphoreName(device, semaphore, name);
}

template <> void SetVulkanObjectName<VkFence, VulkanHandleTypeId::Fence>(VkDevice device,
                                                                         VkFence fence,
                                                                         const char* name)
{
    SetFenceName(device, fence, name);
}

template <> void SetVulkanObjectName<VkEvent, VulkanHandleTypeId::Event>(VkDevice device,
                                                                         VkEvent _event,
                                                                         const char* name)
{
    SetEventName(device, _event, name);
}

template <>
void SetVulkanObjectName<VkQueryPool, VulkanHandleTypeId::QueryPool>(VkDevice device,
                                                                     VkQueryPool queryPool,
                                                                     const char* name)
{
    SetQueryPoolName(device, queryPool, name);
}

template <>
void SetVulkanObjectName<VkAccelerationStructureKHR, VulkanHandleTypeId::AccelerationStructureKHR>(
    VkDevice device,
    VkAccelerationStructureKHR accelStruct,
    const char* name)
{
    SetAccelStructName(device, accelStruct, name);
}

template <> void SetVulkanObjectName<VkPipelineCache, VulkanHandleTypeId::PipelineCache>(
    VkDevice device,
    VkPipelineCache pipeCache,
    const char* name)
{
    SetPipelineCacheName(device, pipeCache, name);
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
