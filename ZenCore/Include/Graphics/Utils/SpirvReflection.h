#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "Common/Base.h"

ZEN_DISABLE_WARNINGS()
#include "SpirvCross/spirv_glsl.hpp"
ZEN_ENABLE_WARNINGS()
#include "Graphics/Vulkan/ShaderModuleVK.h"
namespace zen {
/// Generate a list of shader resource based on SPIRV reflection code, and provided ShaderVariant
using namespace zen::vulkan;

class SPIRVReflection {
public:
  /// @brief Reflects shader resources from SPIRV code
  /// @param stage The Vulkan shader stage flag
  /// @param spirv The SPIRV code of shader
  /// @param[out] resources The list of reflected shader resources
  /// @param variant ShaderVariant used for reflection to specify the size of the runtime arrays in Storage Buffers
  bool ReflectShaderResources(vk::ShaderStageFlagBits stage, const std::vector<uint32_t>& spirv,
                              std::vector<ShaderResource>& resources, const ShaderVariant& variant);

private:
  void ParseShaderResources(const spirv_cross::Compiler& compiler, vk::ShaderStageFlagBits stage,
                            std::vector<ShaderResource>& resources, const ShaderVariant& variant);

  void ParsePushConstants(const spirv_cross::Compiler& compiler, vk::ShaderStageFlagBits stage,
                          std::vector<ShaderResource>& resources, const ShaderVariant& variant);

  void ParseSpecializationConstants(const spirv_cross::Compiler& compiler,
                                    vk::ShaderStageFlagBits stage,
                                    std::vector<ShaderResource>& resources,
                                    const ShaderVariant& variant);
};
}  // namespace zen