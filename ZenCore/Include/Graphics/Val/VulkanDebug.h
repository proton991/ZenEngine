//#pragma once
//
#include <string>
#include "VulkanHeaders.h"

//
namespace zen::val
{

//// Loads the debug utils functions and initialized the debug callback.
//bool SetupDebugUtils(VkInstance                          instance,
//                     VkDebugUtilsMessageSeverityFlagsEXT messageSeverity,
//                     VkDebugUtilsMessageTypeFlagsEXT     messageType,
//                     void*                               pUserData = nullptr);
//
//// Initializes the debug report callback.
//bool SetupDebugReport(VkInstance               instance,
//                      VkDebugReportFlagBitsEXT flags,
//                      void*                    pUserData = nullptr);
//
//// Clears the debug utils/debug report callback
//void FreeDebug(VkInstance instance);

// Setup and functions for the VK_EXT_debug_marker_extension
// Extension spec can be found at https://github.com/KhronosGroup/Vulkan-Docs/blob/1.0-VK_EXT_debug_marker/doc/specs/vulkan/appendices/VK_EXT_debug_marker.txt
// Note that the extension will only be present if run from an offline debugging application
// The actual check for extension presence and enabling it on the device is done in the example base class
// See VulkanExampleBase::createInstance and VulkanExampleBase::createDevice (base/vulkanexamplebase.cpp)

// Sets the debug name of an object
// All Objects in Vulkan are represented by their 64-bit handles which are passed into this function
// along with the object type
void SetObjectName(VkDevice device, uint64_t object, VkObjectType objectType, const char* pName);



// clang-format off
// Object specific naming functions
void SetDeviceName              (VkDevice device, const char* pName);
void SetCommandPoolName         (VkDevice device, VkCommandPool         cmdPool,             const char * pName);
void SetCommandBufferName       (VkDevice device, VkCommandBuffer       cmdBuffer,           const char * pName);
void SetQueueName               (VkDevice device, VkQueue               queue,               const char * pName);
void SetImageName               (VkDevice device, VkImage               image,               const char * pName);
void SetImageViewName           (VkDevice device, VkImageView           imageView,           const char * pName);
void SetSamplerName             (VkDevice device, VkSampler             sampler,             const char * pName);
void SetBufferName              (VkDevice device, VkBuffer              buffer,              const char * pName);
void SetBufferViewName          (VkDevice device, VkBufferView          bufferView,          const char * pName);
void SetDeviceMemoryName        (VkDevice device, VkDeviceMemory        memory,              const char * pName);
void SetShaderModuleName        (VkDevice device, VkShaderModule        shaderModule,        const char * pName);
void SetPipelineName            (VkDevice device, VkPipeline            pipeline,            const char * pName);
void SetPipelineLayoutName      (VkDevice device, VkPipelineLayout      pipelineLayout,      const char * pName);
void SetRenderPassName          (VkDevice device, VkRenderPass          renderPass,          const char * pName);
void SetFramebufferName         (VkDevice device, VkFramebuffer         framebuffer,         const char * pName);
void SetDescriptorSetLayoutName (VkDevice device, VkDescriptorSetLayout descriptorSetLayout, const char * pName);
void SetDescriptorSetName       (VkDevice device, VkDescriptorSet       descriptorSet,       const char * pName);
void SetDescriptorPoolName      (VkDevice device, VkDescriptorPool      descriptorPool,      const char * pName);
void SetSemaphoreName           (VkDevice device, VkSemaphore           semaphore,           const char * pName);
void SetFenceName               (VkDevice device, VkFence               fence,               const char * pName);
void SetEventName               (VkDevice device, VkEvent               _event,              const char * pName);
void SetQueryPoolName           (VkDevice device, VkQueryPool           queryPool,           const char * pName);

enum class VulkanHandleTypeId : uint32_t;

template<typename VulkanObjectType, VulkanHandleTypeId>
void SetVulkanObjectName(VkDevice device, VulkanObjectType vkObject, const char * pName);

const char* VkResultToString(VkResult errorCode);
const char* VkAccessFlagBitToString(VkAccessFlagBits Bit);
const char* VkImageLayoutToString(VkImageLayout Layout);
std::string VkAccessFlagsToString(VkAccessFlags Flags);
const char* VkObjectTypeToString(VkObjectType ObjectType);
// clang-format on

} // namespace zen::val
