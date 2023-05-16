#pragma once
#include <stdexcept>
#include <string>
#include <vulkan/vulkan.hpp>

namespace zen {
class VulkanException : public std::runtime_error {
public:
  VulkanException(vk::Result result, const std::string& msg = "Vulkan Exception")
      : std::runtime_error(msg) {
    m_errorMsg = std::string(std::runtime_error::what()) + std::string{" : "} + to_string(result);
  }

  const char* what() const noexcept override { return m_errorMsg.c_str(); }

private:
  std::string m_errorMsg;
};
}  // namespace zen