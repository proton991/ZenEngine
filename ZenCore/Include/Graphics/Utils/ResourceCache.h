#pragma once
#include <mutex>
#include "Common/Logging.h"
#include "ResourceHashing.h"
namespace zen::vulkan {
class Device;
}
namespace {
using namespace zen::vulkan;
template <typename T>
inline void HashParam(size_t& seed, const T& value) {
  HashCombine(seed, value);
}

template <typename T, typename... Args>
inline void HashParam(size_t& seed, const T& firstArg, const Args&... args) {
  HashParam(seed, firstArg);

  HashParam(seed, args...);
}

template <>
inline void HashParam<std::vector<ShaderModule*>>(size_t& seed,
                                                  const std::vector<ShaderModule*>& value) {
  for (auto& shaderModule : value) {
    HashCombine(seed, shaderModule->GetId());
  }
}

template <>
inline void HashParam<std::vector<ShaderResource>>(size_t& seed,
                                                   const std::vector<ShaderResource>& value) {
  for (auto& resource : value) {
    HashCombine(seed, resource);
  }
}

template <class T, class... A>
T& RequestResourceNoLock(const Device& device, std::unordered_map<std::size_t, T>& resources,
                         A&... args) {
  std::size_t hash{0U};
  HashParam(hash, args...);

  auto resIt = resources.find(hash);

  if (resIt != resources.end()) {
    return resIt->second;
  }

  // If we do not have it already, create and cache it
  const char* resType = typeid(T).name();
  size_t resId        = resources.size();

  LOGD("Building #{} cache object ({})", resId, resType);

// Only error handle in release
#ifndef ZEN_DEBUG
  try {
#endif
    T resource(device, args...);

    auto [insertIt, inserted] = resources.emplace(hash, std::move(resource));

    if (!inserted) {
      throw std::runtime_error{std::string{"Insertion error for #"} + std::to_string(resId) +
                               "cache object (" + resType + ")"};
    }

#ifndef ZEN_DEBUG
  } catch (const std::exception& e) {
    LOGE("Creation error for #{} cache object ({})", res_id, res_type);
    throw e;
  }
#endif

  return insertIt->second;
}

template <class T, class... A>
T& RequestResource(const Device& device, std::mutex& resourceMutex,
                   std::unordered_map<std::size_t, T>& resources, A&... args) {
  std::lock_guard<std::mutex> guard(resourceMutex);

  auto& res = RequestResourceNoLock(device, resources, args...);

  return res;
}
}  // namespace

namespace zen::vulkan {
class ResourceCache {
public:
  ResourceCache(const Device& device) : m_device(device) {}
  DescriptorSetLayout& RequestDescriptorSetLayout(
      uint32_t setIndex, const std::vector<ShaderModule*>& shaderModules,
      const std::vector<ShaderResource>& shaderResources);

private:
  const Device& m_device;
  struct MutexTable {
    std::mutex descriptorSetLayout;
  } m_mutexTable;
  struct ResourceTable {
    std::unordered_map<std::size_t, DescriptorSetLayout> descriptorSetLayouts;
  } m_resourceTable;
};

}  // namespace zen::vulkan