#include "Graphics/Utils/SpirvReflection.h"
#include "Common/Logging.h"
namespace zen {
namespace {
template <ShaderResourceType T>
inline void ReadShaderResource(const spirv_cross::Compiler& compiler, vk::ShaderStageFlagBits stage,
                               std::vector<ShaderResource>& resources,
                               const ShaderVariant& variant) {
  LOGE("Not implemented! Read shader resources of type.");
}

template <spv::Decoration T>
inline void ReadResourceDecoration(const spirv_cross::Compiler& /*compiler*/,
                                   const spirv_cross::Resource& /*resource*/,
                                   ShaderResource& /*shaderResource*/,
                                   const ShaderVariant& /* variant */) {
  LOGE("Not implemented! Read resources decoration of type.");
}

template <>
inline void ReadResourceDecoration<spv::DecorationLocation>(const spirv_cross::Compiler& compiler,
                                                            const spirv_cross::Resource& resource,
                                                            ShaderResource& shaderResource,
                                                            const ShaderVariant& variant) {
  shaderResource.location = compiler.get_decoration(resource.id, spv::DecorationLocation);
}

template <>
inline void ReadResourceDecoration<spv::DecorationDescriptorSet>(
    const spirv_cross::Compiler& compiler, const spirv_cross::Resource& resource,
    ShaderResource& shaderResource, const ShaderVariant& variant) {
  shaderResource.set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
}

template <>
inline void ReadResourceDecoration<spv::DecorationBinding>(const spirv_cross::Compiler& compiler,
                                                           const spirv_cross::Resource& resource,
                                                           ShaderResource& shaderResource,
                                                           const ShaderVariant& variant) {
  shaderResource.binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
}

template <>
inline void ReadResourceDecoration<spv::DecorationInputAttachmentIndex>(
    const spirv_cross::Compiler& compiler, const spirv_cross::Resource& resource,
    ShaderResource& shaderResource, const ShaderVariant& variant) {
  shaderResource.inputAttachmentIndex =
      compiler.get_decoration(resource.id, spv::DecorationInputAttachmentIndex);
}

template <>
inline void ReadResourceDecoration<spv::DecorationNonWritable>(
    const spirv_cross::Compiler& compiler, const spirv_cross::Resource& resource,
    ShaderResource& shaderResource, const ShaderVariant& variant) {
  shaderResource.qualifiers |= ShaderResourceQualifiers::NonWritable;
}

template <>
inline void ReadResourceDecoration<spv::DecorationNonReadable>(
    const spirv_cross::Compiler& compiler, const spirv_cross::Resource& resource,
    ShaderResource& shaderResource, const ShaderVariant& variant) {
  shaderResource.qualifiers |= ShaderResourceQualifiers::NonReadable;
}

inline void ReadResourceVecSize(const spirv_cross::Compiler& compiler,
                                const spirv_cross::Resource& resource,
                                ShaderResource& shaderResource, const ShaderVariant& variant) {
  const auto& spirvType = compiler.get_type_from_variable(resource.id);

  shaderResource.vecSize = spirvType.vecsize;
  shaderResource.columns = spirvType.columns;
}

inline void ReadResourceArraySize(const spirv_cross::Compiler& compiler,
                                  const spirv_cross::Resource& resource,
                                  ShaderResource& shaderResource, const ShaderVariant& variant) {
  const auto& spirv_type = compiler.get_type_from_variable(resource.id);

  shaderResource.arraySize = spirv_type.array.size() ? spirv_type.array[0] : 1;
}

inline void ReadResourceSize(const spirv_cross::Compiler& compiler,
                             const spirv_cross::Resource& resource, ShaderResource& shaderResource,
                             const ShaderVariant& variant) {
  const auto& spirvType = compiler.get_type_from_variable(resource.id);

  size_t arraySize = 0;
  if (variant.GetRuntimeArraySizes().count(resource.name) != 0) {
    arraySize = variant.GetRuntimeArraySizes().at(resource.name);
  }

  shaderResource.size =
      static_cast<uint32_t>(compiler.get_declared_struct_size_runtime_array(spirvType, arraySize));
}

inline void ReadResourceSize(const spirv_cross::Compiler& compiler,
                             const spirv_cross::SPIRConstant& constant,
                             ShaderResource& shaderResource, const ShaderVariant& variant) {
  auto spirv_type = compiler.get_type(constant.constant_type);

  switch (spirv_type.basetype) {
    case spirv_cross::SPIRType::BaseType::Boolean:
    case spirv_cross::SPIRType::BaseType::Char:
    case spirv_cross::SPIRType::BaseType::Int:
    case spirv_cross::SPIRType::BaseType::UInt:
    case spirv_cross::SPIRType::BaseType::Float:
      shaderResource.size = 4;
      break;
    case spirv_cross::SPIRType::BaseType::Int64:
    case spirv_cross::SPIRType::BaseType::UInt64:
    case spirv_cross::SPIRType::BaseType::Double:
      shaderResource.size = 8;
      break;
    default:
      shaderResource.size = 0;
      break;
  }
}

template <>
inline void ReadShaderResource<ShaderResourceType::Input>(const spirv_cross::Compiler& compiler,
                                                          vk::ShaderStageFlagBits stage,
                                                          std::vector<ShaderResource>& resources,
                                                          const ShaderVariant& variant) {
  auto input_resources = compiler.get_shader_resources().stage_inputs;

  for (auto& resource : input_resources) {
    ShaderResource shaderResource{};
    shaderResource.type   = ShaderResourceType::Input;
    shaderResource.stages = stage;
    shaderResource.name   = resource.name;

    ReadResourceVecSize(compiler, resource, shaderResource, variant);
    ReadResourceArraySize(compiler, resource, shaderResource, variant);
    ReadResourceDecoration<spv::DecorationLocation>(compiler, resource, shaderResource, variant);

    resources.push_back(shaderResource);
  }
}

template <>
inline void ReadShaderResource<ShaderResourceType::InputAttachment>(
    const spirv_cross::Compiler& compiler, vk::ShaderStageFlagBits /*stage*/,
    std::vector<ShaderResource>& resources, const ShaderVariant& variant) {
  auto subpass_resources = compiler.get_shader_resources().subpass_inputs;

  for (auto& resource : subpass_resources) {
    ShaderResource shaderResource{};
    shaderResource.type   = ShaderResourceType::InputAttachment;
    shaderResource.stages = vk::ShaderStageFlagBits::eFragment;
    shaderResource.name   = resource.name;

    ReadResourceArraySize(compiler, resource, shaderResource, variant);
    ReadResourceDecoration<spv::DecorationInputAttachmentIndex>(compiler, resource, shaderResource,
                                                                variant);
    ReadResourceDecoration<spv::DecorationDescriptorSet>(compiler, resource, shaderResource,
                                                         variant);
    ReadResourceDecoration<spv::DecorationBinding>(compiler, resource, shaderResource, variant);

    resources.push_back(shaderResource);
  }
}

template <>
inline void ReadShaderResource<ShaderResourceType::Output>(const spirv_cross::Compiler& compiler,
                                                           vk::ShaderStageFlagBits stage,
                                                           std::vector<ShaderResource>& resources,
                                                           const ShaderVariant& variant) {
  auto output_resources = compiler.get_shader_resources().stage_outputs;

  for (auto& resource : output_resources) {
    ShaderResource shaderResource{};
    shaderResource.type   = ShaderResourceType::Output;
    shaderResource.stages = stage;
    shaderResource.name   = resource.name;

    ReadResourceArraySize(compiler, resource, shaderResource, variant);
    ReadResourceVecSize(compiler, resource, shaderResource, variant);
    ReadResourceDecoration<spv::DecorationLocation>(compiler, resource, shaderResource, variant);

    resources.push_back(shaderResource);
  }
}

template <>
inline void ReadShaderResource<ShaderResourceType::Image>(const spirv_cross::Compiler& compiler,
                                                          vk::ShaderStageFlagBits stage,
                                                          std::vector<ShaderResource>& resources,
                                                          const ShaderVariant& variant) {
  auto imageResources = compiler.get_shader_resources().separate_images;

  for (auto& resource : imageResources) {
    ShaderResource shaderResource{};
    shaderResource.type   = ShaderResourceType::Image;
    shaderResource.stages = stage;
    shaderResource.name   = resource.name;

    ReadResourceArraySize(compiler, resource, shaderResource, variant);
    ReadResourceDecoration<spv::DecorationDescriptorSet>(compiler, resource, shaderResource,
                                                         variant);
    ReadResourceDecoration<spv::DecorationBinding>(compiler, resource, shaderResource, variant);

    resources.push_back(shaderResource);
  }
}

template <>
inline void ReadShaderResource<ShaderResourceType::ImageSampler>(
    const spirv_cross::Compiler& compiler, vk::ShaderStageFlagBits stage,
    std::vector<ShaderResource>& resources, const ShaderVariant& variant) {
  auto imageResources = compiler.get_shader_resources().sampled_images;

  for (auto& resource : imageResources) {
    ShaderResource shaderResource{};
    shaderResource.type   = ShaderResourceType::ImageSampler;
    shaderResource.stages = stage;
    shaderResource.name   = resource.name;

    ReadResourceArraySize(compiler, resource, shaderResource, variant);
    ReadResourceDecoration<spv::DecorationDescriptorSet>(compiler, resource, shaderResource,
                                                         variant);
    ReadResourceDecoration<spv::DecorationBinding>(compiler, resource, shaderResource, variant);

    resources.push_back(shaderResource);
  }
}

template <>
inline void ReadShaderResource<ShaderResourceType::ImageStorage>(
    const spirv_cross::Compiler& compiler, vk::ShaderStageFlagBits stage,
    std::vector<ShaderResource>& resources, const ShaderVariant& variant) {
  auto storageResources = compiler.get_shader_resources().storage_images;

  for (auto& resource : storageResources) {
    ShaderResource shaderResource{};
    shaderResource.type   = ShaderResourceType::ImageStorage;
    shaderResource.stages = stage;
    shaderResource.name   = resource.name;

    ReadResourceArraySize(compiler, resource, shaderResource, variant);
    ReadResourceDecoration<spv::DecorationNonReadable>(compiler, resource, shaderResource, variant);
    ReadResourceDecoration<spv::DecorationNonWritable>(compiler, resource, shaderResource, variant);
    ReadResourceDecoration<spv::DecorationDescriptorSet>(compiler, resource, shaderResource,
                                                         variant);
    ReadResourceDecoration<spv::DecorationBinding>(compiler, resource, shaderResource, variant);

    resources.push_back(shaderResource);
  }
}

template <>
inline void ReadShaderResource<ShaderResourceType::Sampler>(const spirv_cross::Compiler& compiler,
                                                            vk::ShaderStageFlagBits stage,
                                                            std::vector<ShaderResource>& resources,
                                                            const ShaderVariant& variant) {
  auto samplerResources = compiler.get_shader_resources().separate_samplers;

  for (auto& resource : samplerResources) {
    ShaderResource shaderResource{};
    shaderResource.type   = ShaderResourceType::Sampler;
    shaderResource.stages = stage;
    shaderResource.name   = resource.name;

    ReadResourceArraySize(compiler, resource, shaderResource, variant);
    ReadResourceDecoration<spv::DecorationDescriptorSet>(compiler, resource, shaderResource,
                                                         variant);
    ReadResourceDecoration<spv::DecorationBinding>(compiler, resource, shaderResource, variant);

    resources.push_back(shaderResource);
  }
}

template <>
inline void ReadShaderResource<ShaderResourceType::BufferUniform>(
    const spirv_cross::Compiler& compiler, vk::ShaderStageFlagBits stage,
    std::vector<ShaderResource>& resources, const ShaderVariant& variant) {
  auto uniformResources = compiler.get_shader_resources().uniform_buffers;

  for (auto& resource : uniformResources) {
    ShaderResource shaderResource{};
    shaderResource.type   = ShaderResourceType::BufferUniform;
    shaderResource.stages = stage;
    shaderResource.name   = resource.name;

    ReadResourceSize(compiler, resource, shaderResource, variant);
    ReadResourceArraySize(compiler, resource, shaderResource, variant);
    ReadResourceDecoration<spv::DecorationDescriptorSet>(compiler, resource, shaderResource,
                                                         variant);
    ReadResourceDecoration<spv::DecorationBinding>(compiler, resource, shaderResource, variant);

    resources.push_back(shaderResource);
  }
}

template <>
inline void ReadShaderResource<ShaderResourceType::BufferStorage>(
    const spirv_cross::Compiler& compiler, vk::ShaderStageFlagBits stage,
    std::vector<ShaderResource>& resources, const ShaderVariant& variant) {
  auto storageResources = compiler.get_shader_resources().storage_buffers;

  for (auto& resource : storageResources) {
    ShaderResource shaderResource;
    shaderResource.type   = ShaderResourceType::BufferStorage;
    shaderResource.stages = stage;
    shaderResource.name   = resource.name;

    ReadResourceSize(compiler, resource, shaderResource, variant);
    ReadResourceArraySize(compiler, resource, shaderResource, variant);
    ReadResourceDecoration<spv::DecorationNonReadable>(compiler, resource, shaderResource, variant);
    ReadResourceDecoration<spv::DecorationNonWritable>(compiler, resource, shaderResource, variant);
    ReadResourceDecoration<spv::DecorationDescriptorSet>(compiler, resource, shaderResource,
                                                         variant);
    ReadResourceDecoration<spv::DecorationBinding>(compiler, resource, shaderResource, variant);

    resources.push_back(shaderResource);
  }
}
}  // namespace

bool SPIRVReflection::ReflectShaderResources(vk::ShaderStageFlagBits stage,
                                             const std::vector<uint32_t>& spirv,
                                             std::vector<ShaderResource>& resources,
                                             const ShaderVariant& variant) {
  spirv_cross::CompilerGLSL compiler{spirv};

  auto opts                     = compiler.get_common_options();
  opts.enable_420pack_extension = true;

  compiler.set_common_options(opts);

  ParseShaderResources(compiler, stage, resources, variant);
  ParsePushConstants(compiler, stage, resources, variant);
  ParseSpecializationConstants(compiler, stage, resources, variant);

  return true;
}

void SPIRVReflection::ParseShaderResources(const spirv_cross::Compiler& compiler,
                                           vk::ShaderStageFlagBits stage,
                                           std::vector<ShaderResource>& resources,
                                           const ShaderVariant& variant) {
  ReadShaderResource<ShaderResourceType::Input>(compiler, stage, resources, variant);
  ReadShaderResource<ShaderResourceType::InputAttachment>(compiler, stage, resources, variant);
  ReadShaderResource<ShaderResourceType::Output>(compiler, stage, resources, variant);
  ReadShaderResource<ShaderResourceType::Image>(compiler, stage, resources, variant);
  ReadShaderResource<ShaderResourceType::ImageSampler>(compiler, stage, resources, variant);
  ReadShaderResource<ShaderResourceType::ImageStorage>(compiler, stage, resources, variant);
  ReadShaderResource<ShaderResourceType::Sampler>(compiler, stage, resources, variant);
  ReadShaderResource<ShaderResourceType::BufferUniform>(compiler, stage, resources, variant);
  ReadShaderResource<ShaderResourceType::BufferStorage>(compiler, stage, resources, variant);
}

void SPIRVReflection::ParsePushConstants(const spirv_cross::Compiler& compiler,
                                         vk::ShaderStageFlagBits stage,
                                         std::vector<ShaderResource>& resources,
                                         const ShaderVariant& variant) {
  auto shaderResources = compiler.get_shader_resources();

  for (auto& resource : shaderResources.push_constant_buffers) {
    const auto& spivr_type = compiler.get_type_from_variable(resource.id);

    std::uint32_t offset = std::numeric_limits<std::uint32_t>::max();

    for (auto i = 0U; i < spivr_type.member_types.size(); ++i) {
      auto mem_offset = compiler.get_member_decoration(spivr_type.self, i, spv::DecorationOffset);

      offset = std::min(offset, mem_offset);
    }

    ShaderResource shaderResource{};
    shaderResource.type   = ShaderResourceType::PushConstant;
    shaderResource.stages = stage;
    shaderResource.name   = resource.name;
    shaderResource.offset = offset;

    ReadResourceSize(compiler, resource, shaderResource, variant);

    shaderResource.size -= shaderResource.offset;

    resources.push_back(shaderResource);
  }
}

void SPIRVReflection::ParseSpecializationConstants(const spirv_cross::Compiler& compiler,
                                                   vk::ShaderStageFlagBits stage,
                                                   std::vector<ShaderResource>& resources,
                                                   const ShaderVariant& variant) {
  auto specializationConstants = compiler.get_specialization_constants();

  for (auto& resource : specializationConstants) {
    auto& spirv_value = compiler.get_constant(resource.id);

    ShaderResource shaderResource{};
    shaderResource.type       = ShaderResourceType::SpecializationConstant;
    shaderResource.stages     = stage;
    shaderResource.name       = compiler.get_name(resource.id);
    shaderResource.offset     = 0;
    shaderResource.constantId = resource.constant_id;

    ReadResourceSize(compiler, spirv_value, shaderResource, variant);

    resources.push_back(shaderResource);
  }
}
}  // namespace zen