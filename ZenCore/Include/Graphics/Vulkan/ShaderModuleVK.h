#pragma once
#include <string>
#include <unordered_map>
#include "DeviceResource.h"

namespace zen::vulkan {
class ShaderSource {
public:
  ShaderSource(const std::string& name);

  const std::string& GetName() const { return m_name; }
  const std::string& GetSourceCode() const { return m_srcCode; }

private:
  std::string m_name;
  std::string m_srcCode;
};

class ShaderVariant {
public:
  ShaderVariant() = default;
  ShaderVariant(std::string&& preamble, std::vector<std::string>&& processes);
  ShaderVariant& AddDefine(const std::string& def);
  ShaderVariant& AddUnDefine(const std::string& undef);
  ShaderVariant& AddRuntimeArraySize(const std::string& name, size_t size);

  void Clear();

  auto GetId() const { return m_id; }
  const auto& GetPreamble() const { return m_preamble; }
  const auto& GetProcesses() const { return m_processes; }
  const auto& GetRuntimeArraySizes() const { return m_runtimeArraySizes; }

private:
  void UpdateId();
  size_t m_id;
  std::string m_preamble;
  std::vector<std::string> m_processes;
  std::unordered_map<std::string, size_t> m_runtimeArraySizes;
};

/// Types of shader resources
enum class ShaderResourceType {
  Input,
  InputAttachment,
  Output,
  Image,
  ImageSampler,
  ImageStorage,
  Sampler,
  BufferUniform,
  BufferStorage,
  PushConstant,
  SpecializationConstant,
  All
};

/// This determines the type and method of how descriptor set should be created and bound
enum class ShaderResourceMode { Static, Dynamic, UpdateAfterBind };

/// A bitmask of qualifiers applied to a resource
struct ShaderResourceQualifiers {
  enum : uint32_t {
    None        = 0,
    NonReadable = 1,
    NonWritable = 2,
  };
};

/// Store shader resource data.
/// Used by the shader module.
struct ShaderResource {
  vk::ShaderStageFlagBits stages;

  ShaderResourceType type;

  ShaderResourceMode mode;

  uint32_t set;

  uint32_t binding;

  uint32_t location;

  uint32_t inputAttachmentIndex;

  uint32_t vecSize;

  uint32_t columns;

  uint32_t arraySize;

  uint32_t offset;

  uint32_t size;

  uint32_t constantId;

  uint32_t qualifiers;

  std::string name;
};

class ShaderModule : public DeviceResource<vk::ShaderModule> {
public:
  ZEN_MOVE_CONSTRUCTOR_ONLY(ShaderModule)
  ShaderModule(const Device& device, vk::ShaderStageFlagBits stage, const ShaderSource& source,
               const ShaderVariant& shaderVariant, std::string entryPoint = "main");
  ShaderModule(ShaderModule&& other);
  ~ShaderModule();

  size_t GetId() const { return m_id; }
  auto GetStage() const { return m_stage; }
  const std::string& GetEntryPoint() const { return m_entryPoint; }
  const std::vector<ShaderResource>& GetResources() const { return m_resources; }

private:
  size_t m_id;
  vk::ShaderStageFlagBits m_stage;
  std::string m_entryPoint;
  // Compiled source
  std::vector<uint32_t> m_spirv;

  std::vector<ShaderResource> m_resources;
  std::string m_infoLog;
};
}  // namespace zen::vulkan