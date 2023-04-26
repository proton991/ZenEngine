#include "Graphics/Vulkan/DeviceVK.h"
#include "Common/Error.h"

namespace zen::vulkan {

Device::Device(const Context& context) {
  m_instance  = context.GetInstance();
  m_loader    = context.m_loader;
  m_gpu       = context.GetGPU();
  m_handle    = context.GetLogicalDevice();
  m_queueInfo = context.m_queueInfo;

  InitVma();
}

uint32_t Device::GetQueueFamliyIndex(QueueIndices index) const {
  return m_queueInfo.familyIndices[index];
}

void Device::InitVma() {

  VmaVulkanFunctions vmaVukanFunc{};
  vmaVukanFunc.vkAllocateMemory =
      reinterpret_cast<PFN_vkAllocateMemory>(GetHandle().getProcAddr("vkAllocateMemory"));
  vmaVukanFunc.vkBindBufferMemory =
      reinterpret_cast<PFN_vkBindBufferMemory>(GetHandle().getProcAddr("vkBindBufferMemory"));
  vmaVukanFunc.vkBindImageMemory =
      reinterpret_cast<PFN_vkBindImageMemory>(GetHandle().getProcAddr("vkBindImageMemory"));
  vmaVukanFunc.vkCreateBuffer =
      reinterpret_cast<PFN_vkCreateBuffer>(GetHandle().getProcAddr("vkCreateBuffer"));
  vmaVukanFunc.vkCreateImage =
      reinterpret_cast<PFN_vkCreateImage>(GetHandle().getProcAddr("vkCreateImage"));
  vmaVukanFunc.vkDestroyBuffer =
      reinterpret_cast<PFN_vkDestroyBuffer>(GetHandle().getProcAddr("vkDestroyBuffer"));
  vmaVukanFunc.vkDestroyImage =
      reinterpret_cast<PFN_vkDestroyImage>(GetHandle().getProcAddr("vkDestroyImage"));
  vmaVukanFunc.vkFlushMappedMemoryRanges = reinterpret_cast<PFN_vkFlushMappedMemoryRanges>(
      GetHandle().getProcAddr("vkFlushMappedMemoryRanges"));
  vmaVukanFunc.vkFreeMemory =
      reinterpret_cast<PFN_vkFreeMemory>(GetHandle().getProcAddr("vkFreeMemory"));
  vmaVukanFunc.vkGetBufferMemoryRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
      GetHandle().getProcAddr("vkGetBufferMemoryRequirements"));
  vmaVukanFunc.vkGetImageMemoryRequirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
      GetHandle().getProcAddr("vkGetImageMemoryRequirements"));
  vmaVukanFunc.vkGetPhysicalDeviceMemoryProperties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
          GetHandle().getProcAddr("vkGetPhysicalDeviceMemoryProperties"));
  vmaVukanFunc.vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      GetHandle().getProcAddr("vkGetPhysicalDeviceProperties"));
  vmaVukanFunc.vkInvalidateMappedMemoryRanges =
      reinterpret_cast<PFN_vkInvalidateMappedMemoryRanges>(
          GetHandle().getProcAddr("vkInvalidateMappedMemoryRanges"));
  vmaVukanFunc.vkMapMemory =
      reinterpret_cast<PFN_vkMapMemory>(GetHandle().getProcAddr("vkMapMemory"));
  vmaVukanFunc.vkUnmapMemory =
      reinterpret_cast<PFN_vkUnmapMemory>(GetHandle().getProcAddr("vkUnmapMemory"));
  vmaVukanFunc.vkCmdCopyBuffer =
      reinterpret_cast<PFN_vkCmdCopyBuffer>(GetHandle().getProcAddr("vkCmdCopyBuffer"));

  VmaAllocatorCreateInfo allocatorCI{};
  allocatorCI.physicalDevice   = static_cast<VkPhysicalDevice>(m_gpu);
  allocatorCI.device           = static_cast<VkDevice>(GetHandle());
  allocatorCI.instance         = static_cast<VkInstance>(m_instance);
  allocatorCI.pVulkanFunctions = &vmaVukanFunc;

  VK_CHECK(vmaCreateAllocator(&allocatorCI, &m_allocator), "create vma");
}

Device::~Device() {
  if (m_allocator != nullptr) {
    vmaDestroyAllocator(m_allocator);
  }
}

}  // namespace zen::vulkan