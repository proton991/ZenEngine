#include "Graphics/Vulkan/DeviceContext.h"
#include "Common/Logging.h"

namespace zen::vulkan {
vk::PhysicalDevice DeviceContext::SelectPhysicalDevice(vk::Instance instance) {
  std::vector<vk::PhysicalDevice> physicalDevices = instance.enumeratePhysicalDevices();
  LOGI("Found {} physical device(s)", physicalDevices.size());
  vk::PhysicalDevice physicalDevice = nullptr;
  for (const auto& device : physicalDevices) {
    vk::PhysicalDeviceProperties deviceProperties = device.getProperties();
    LOGI("\tPhysical device found: {}", deviceProperties.deviceName);
    vk::PhysicalDeviceFeatures deviceFeatures = device.getFeatures();

    if (deviceProperties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
      physicalDevice = device;
    }
  }
  if (!physicalDevice)
    throw std::runtime_error("Failed to find physical device");
  auto gpuProps = physicalDevice.getProperties();
  LOGI("Using: {}", gpuProps.deviceName);
  LOGI("\tMax DescriptorSetSamplers: {}", gpuProps.limits.maxDescriptorSetSamplers);
  LOGI("\tMax DescriptorSetSampledImages: {}", gpuProps.limits.maxDescriptorSetSampledImages);
  LOGI("\tMax DescriptorSetStorageImages: {}", gpuProps.limits.maxDescriptorSetStorageImages);
  LOGI("\tMax DescriptorSetUniformBuffers: {}", gpuProps.limits.maxDescriptorSetUniformBuffers);
  LOGI("\tMax DescriptorSetStorageBuffers: {}", gpuProps.limits.maxDescriptorSetStorageBuffers);
  LOGI("\tMax DescriptorSetUniformBuffersDynamic: {}",
       gpuProps.limits.maxDescriptorSetUniformBuffersDynamic);
  LOGI("\tMax DescriptorSetStorageBuffersDynamic: {}",
       gpuProps.limits.maxDescriptorSetStorageBuffersDynamic);
  LOGI("\tMax DescriptorSetInputAttachments: {}", gpuProps.limits.maxDescriptorSetInputAttachments);
  return physicalDevice;
}

DeviceQueueInfo DeviceContext::GetDeviceQueueInfo(vk::PhysicalDevice gpu, vk::SurfaceKHR surface) {

  DeviceQueueInfo info{};
  std::vector<vk::QueueFamilyProperties> queueFamilyProps = gpu.getQueueFamilyProperties();
  std::vector<uint32_t> queueOffsets(queueFamilyProps.size());
  std::vector<std::vector<float>> queuePriorites(queueFamilyProps.size());

  const auto findProperQueue = [&](uint32_t& family, uint32_t& index, vk::QueueFlags required,
                                   vk::QueueFlags ignored, float priority) -> bool {
    for (unsigned familyIndex = 0; familyIndex < queueFamilyProps.size(); familyIndex++) {
      if (queueFamilyProps[familyIndex].queueFlags & ignored)
        continue;

      // A graphics queue candidate must support present for us to select it.
      if ((required & vk::QueueFlagBits::eGraphics) && surface) {
        if (!gpu.getSurfaceSupportKHR(familyIndex, surface))
          continue;
      }

      if (queueFamilyProps[familyIndex].queueCount > 0 &&
          (queueFamilyProps[familyIndex].queueFlags & required)) {
        family = familyIndex;
        queueFamilyProps[familyIndex].queueCount--;
        index = queueOffsets[familyIndex]++;
        queuePriorites[familyIndex].push_back(priority);
        return true;
      }
    }
    return false;
  };
  // find graphics queue
  if (!findProperQueue(info.familyIndices[QUEUE_INDEX_GRAPHICS], info.indices[QUEUE_INDEX_GRAPHICS],
                       vk::QueueFlagBits::eGraphics, vk::QueueFlagBits(0), 1.0f)) {
    throw std::runtime_error("Failed to find a graphics queue");
  }
  // Prefer another graphics queue since we can do async graphics that way.
  // The compute queue is to be treated as high priority since we also do async graphics on it.
  if (!findProperQueue(info.familyIndices[QUEUE_INDEX_COMPUTE], info.indices[QUEUE_INDEX_COMPUTE],
                       vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute,
                       vk::QueueFlagBits(0), 1.0f)) {
    if (!findProperQueue(info.familyIndices[QUEUE_INDEX_COMPUTE], info.indices[QUEUE_INDEX_COMPUTE],
                         vk::QueueFlagBits::eCompute, vk::QueueFlagBits(0), 1.0f)) {
      // fall back to the graphics queue if we must.
      info.familyIndices[QUEUE_INDEX_COMPUTE] = info.familyIndices[QUEUE_INDEX_GRAPHICS];
      info.indices[QUEUE_INDEX_COMPUTE]       = info.indices[QUEUE_INDEX_GRAPHICS];
    }
  }
  // For transfer, try to find a queue which only supports transfer, e.g. DMA queue.
  // If not, fall back to a dedicated compute queue.
  // Finally, fall back to same queue as compute.
  if (!findProperQueue(info.familyIndices[QUEUE_INDEX_TRANSFER], info.indices[QUEUE_INDEX_TRANSFER],
                       vk::QueueFlagBits::eTransfer,
                       vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute, 0.5f) &&
      !findProperQueue(info.familyIndices[QUEUE_INDEX_TRANSFER],
                       info.familyIndices[QUEUE_INDEX_TRANSFER], vk::QueueFlagBits::eCompute,
                       vk::QueueFlagBits::eGraphics, 0.5f)) {
    info.familyIndices[QUEUE_INDEX_TRANSFER] = info.familyIndices[QUEUE_INDEX_COMPUTE];
    info.indices[QUEUE_INDEX_TRANSFER]       = info.indices[QUEUE_INDEX_COMPUTE];
  }

  info.queuePriorites = std::move(queuePriorites);
  info.queueOffsets   = std::move(queueOffsets);

  return info;
}

vk::UniqueDevice DeviceContext::CreateDevice(vk::PhysicalDevice gpu,
                                             const DeviceQueueInfo& queueInfo,
                                             const std::vector<const char*>& deviceExtensions) {
  std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
  for (uint32_t familyIndex = 0; familyIndex < QUEUE_INDEX_COUNT; familyIndex++) {
    if (queueInfo.queueOffsets[familyIndex] != 0) {
      auto queueCreateInfo = vk::DeviceQueueCreateInfo()
                                 .setQueueFamilyIndex(familyIndex)
                                 .setQueueCount(queueInfo.queueOffsets[familyIndex])
                                 .setPQueuePriorities(queueInfo.queuePriorites[familyIndex].data());
      queueCreateInfos.push_back(queueCreateInfo);
    }
  }

  auto deviceFeatures = vk::PhysicalDeviceFeatures()
                            .setFragmentStoresAndAtomics(true)
                            .setVertexPipelineStoresAndAtomics(true);

  auto deviceCreateInfo = vk::DeviceCreateInfo()
                              .setQueueCreateInfoCount(uint32_t(queueCreateInfos.size()))
                              .setPQueueCreateInfos(queueCreateInfos.data())
                              .setPEnabledFeatures(&deviceFeatures)
                              .setEnabledExtensionCount(uint32_t(deviceExtensions.size()))
                              .setPpEnabledExtensionNames(deviceExtensions.data());

  auto deviceFeatures12 = vk::PhysicalDeviceVulkan12Features().setScalarBlockLayout(true);

  vk::StructureChain<vk::DeviceCreateInfo, vk::PhysicalDeviceVulkan12Features> chain = {
      deviceCreateInfo, deviceFeatures12};

  return gpu.createDeviceUnique(chain.get<vk::DeviceCreateInfo>());
}

void DeviceContext::SetupDevice(const char** extensions, uint32_t extensionsCount,
                                vk::SurfaceKHR surface) {
  m_gpu       = SelectPhysicalDevice(m_instance.GetHandle());
  m_queueInfo = GetDeviceQueueInfo(m_gpu, surface);
  std::vector<const char*> deviceExtensions(extensions, extensions + extensionsCount);

  m_device = CreateDevice(m_gpu, m_queueInfo, deviceExtensions);
  // get queues
  m_graphicsQueue = m_device.get().getQueue(m_queueInfo.familyIndices[QUEUE_INDEX_GRAPHICS],
                                            m_queueInfo.indices[QUEUE_INDEX_GRAPHICS]);
  m_presentQueue  = m_device.get().getQueue(m_queueInfo.familyIndices[QUEUE_INDEX_GRAPHICS],
                                            m_queueInfo.indices[QUEUE_INDEX_GRAPHICS]);
  m_transferQueue = m_device.get().getQueue(m_queueInfo.familyIndices[QUEUE_INDEX_TRANSFER],
                                            m_queueInfo.indices[QUEUE_INDEX_TRANSFER]);
}

}  // namespace zen::vulkan