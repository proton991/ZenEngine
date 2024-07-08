#pragma once
#include "Shader.h"
#include "spirv_glsl.hpp"
#include "Common/Errors.h"
#include <unordered_map>

namespace zen::val
{
template <ShaderResourceType T>
inline void ReadShaderResource(const spirv_cross::Compiler& /* compiler */,
                               VkShaderStageFlagBits /* stage */,
                               std::vector<ShaderResource>& /* resources */,
                               const RuntimeArraySizes& /* runtime array size */)
{
    LOGE("Not implemented! Read shader resources of type.");
}

template <spv::Decoration T>
inline void ReadResourceDecoration(const spirv_cross::Compiler& /*compiler*/,
                                   const spirv_cross::Resource& /*resource*/,
                                   ShaderResource& /*shader_resource*/)
{
    LOGE("Not implemented! Read resources decoration of type.");
}

void ParseShaderResources(const spirv_cross::Compiler& compiler,
                          VkShaderStageFlagBits stage,
                          std::vector<ShaderResource>& resources,
                          const RuntimeArraySizes& arraySizes);

void ParsePushConstants(const spirv_cross::Compiler& compiler,
                        VkShaderStageFlagBits stage,
                        std::vector<ShaderResource>& resources,
                        const RuntimeArraySizes& arraySizes);

void ParseSpecializationConstants(const spirv_cross::Compiler& compiler,
                                  VkShaderStageFlagBits stage,
                                  std::vector<ShaderResource>& resources,
                                  const RuntimeArraySizes& arraySizes);


template <>
inline void ReadResourceDecoration<spv::DecorationLocation>(const spirv_cross::Compiler& compiler,
                                                            const spirv_cross::Resource& resource,
                                                            ShaderResource& shaderResource)
{
    shaderResource.location = compiler.get_decoration(resource.id, spv::DecorationLocation);
}

template <> inline void ReadResourceDecoration<spv::DecorationDescriptorSet>(
    const spirv_cross::Compiler& compiler,
    const spirv_cross::Resource& resource,
    ShaderResource& shaderResource)
{
    shaderResource.set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
}

template <>
inline void ReadResourceDecoration<spv::DecorationBinding>(const spirv_cross::Compiler& compiler,
                                                           const spirv_cross::Resource& resource,
                                                           ShaderResource& shaderResource)
{
    shaderResource.binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
}

template <> inline void ReadResourceDecoration<spv::DecorationInputAttachmentIndex>(
    const spirv_cross::Compiler& compiler,
    const spirv_cross::Resource& resource,
    ShaderResource& shaderResource)
{
    shaderResource.inputAttachmentIndex =
        compiler.get_decoration(resource.id, spv::DecorationInputAttachmentIndex);
}

template <> inline void ReadResourceDecoration<spv::DecorationNonWritable>(
    const spirv_cross::Compiler& compiler,
    const spirv_cross::Resource& resource,
    ShaderResource& shaderResource)
{
    shaderResource.qualifiers |= ShaderResourceQualifiers::NonWritable;
}

template <> inline void ReadResourceDecoration<spv::DecorationNonReadable>(
    const spirv_cross::Compiler& compiler,
    const spirv_cross::Resource& resource,
    ShaderResource& shaderResource)
{
    shaderResource.qualifiers |= ShaderResourceQualifiers::NonReadable;
}

inline void ReadResourceFormat(const spirv_cross::SPIRType& spirType,
                               ShaderResource& shaderResource)
{
    if (spirType.basetype == spirv_cross::SPIRType::BaseType::Float)
    {
        switch (shaderResource.vecSize)
        {
            case 1: shaderResource.format = VK_FORMAT_R32_SFLOAT; break;
            case 2: shaderResource.format = VK_FORMAT_R32G32_SFLOAT; break;
            case 3: shaderResource.format = VK_FORMAT_R32G32B32_SFLOAT; break;
            case 4: shaderResource.format = VK_FORMAT_R32G32B32A32_SFLOAT; break;
            default: shaderResource.format = VK_FORMAT_UNDEFINED; break;
        }
    }
    else if (spirType.basetype == spirv_cross::SPIRType::BaseType::Int)
    {
        switch (shaderResource.vecSize)
        {
            case 1: shaderResource.format = VK_FORMAT_R32_SINT; break;
            case 2: shaderResource.format = VK_FORMAT_R32G32_SINT; break;
            case 3: shaderResource.format = VK_FORMAT_R32G32B32_SINT; break;
            case 4: shaderResource.format = VK_FORMAT_R32G32B32A32_SINT; break;
            default: shaderResource.format = VK_FORMAT_UNDEFINED; break;
        }
    }
    else if (spirType.basetype == spirv_cross::SPIRType::BaseType::Double)
    {
        switch (shaderResource.vecSize)
        {
            case 1: shaderResource.format = VK_FORMAT_R64_SFLOAT; break;
            case 2: shaderResource.format = VK_FORMAT_R64G64_SFLOAT; break;
            case 3: shaderResource.format = VK_FORMAT_R64G64B64_SFLOAT; break;
            case 4: shaderResource.format = VK_FORMAT_R64G64B64A64_SFLOAT; break;
            default: shaderResource.format = VK_FORMAT_UNDEFINED; break;
        }
    }
}

inline void ReadResourceVecSize(const spirv_cross::Compiler& compiler,
                                const spirv_cross::Resource& resource,
                                ShaderResource& shaderResource)
{
    const auto& spirType = compiler.get_type_from_variable(resource.id);

    shaderResource.vecSize = spirType.vecsize;
    shaderResource.columns = spirType.columns;
    shaderResource.size    = (spirType.width / 8) * spirType.vecsize;
    ReadResourceFormat(spirType, shaderResource);
}

inline void ReadResourceArraySize(const spirv_cross::Compiler& compiler,
                                  const spirv_cross::Resource& resource,
                                  ShaderResource& shaderResource,
                                  const RuntimeArraySizes& arraySizes)
{
    if (arraySizes.count(resource.name) != 0)
    {
        shaderResource.arraySize = arraySizes.at(resource.name);
    }
    else
    {
        shaderResource.arraySize = compiler.get_type(resource.type_id).array.empty() ?
            1 :
            compiler.get_type(resource.type_id).array[0];
    }
}

inline void ReadResourceSize(const spirv_cross::Compiler& compiler,
                             const spirv_cross::Resource& resource,
                             ShaderResource& shaderResource,
                             const RuntimeArraySizes& arraySizes)
{
    const auto& spirvType = compiler.get_type_from_variable(resource.id);

    size_t arraySize = 0;
    if (arraySizes.count(resource.name) != 0)
    {
        arraySize = arraySizes.at(resource.name);
    }

    shaderResource.size = static_cast<uint32_t>(
        compiler.get_declared_struct_size_runtime_array(spirvType, arraySize));
}

inline void ReadResourceSize(const spirv_cross::Compiler& compiler,
                             const spirv_cross::SPIRConstant& constant,
                             ShaderResource& shaderResource,
                             const RuntimeArraySizes& arraySizes)
{
    auto spirvType = compiler.get_type(constant.constant_type);

    switch (spirvType.basetype)
    {
        case spirv_cross::SPIRType::BaseType::Boolean:
        case spirv_cross::SPIRType::BaseType::Char:
        case spirv_cross::SPIRType::BaseType::Int:
        case spirv_cross::SPIRType::BaseType::UInt:
        case spirv_cross::SPIRType::BaseType::Float: shaderResource.size = 4; break;
        case spirv_cross::SPIRType::BaseType::Int64:
        case spirv_cross::SPIRType::BaseType::UInt64:
        case spirv_cross::SPIRType::BaseType::Double: shaderResource.size = 8; break;
        default: shaderResource.size = 0; break;
    }
}

template <>
inline void ReadShaderResource<ShaderResourceType::Input>(const spirv_cross::Compiler& compiler,
                                                          VkShaderStageFlagBits stage,
                                                          std::vector<ShaderResource>& resources,
                                                          const RuntimeArraySizes& arraySizes)
{
    auto input_resources = compiler.get_shader_resources().stage_inputs;

    for (auto& resource : input_resources)
    {
        ShaderResource shaderResource{};
        shaderResource.type   = ShaderResourceType::Input;
        shaderResource.stages = stage;
        shaderResource.name   = resource.name;

        ReadResourceVecSize(compiler, resource, shaderResource);
        ReadResourceArraySize(compiler, resource, shaderResource, arraySizes);
        ReadResourceDecoration<spv::DecorationLocation>(compiler, resource, shaderResource);

        resources.push_back(shaderResource);
    }
}

template <> inline void ReadShaderResource<ShaderResourceType::InputAttachment>(
    const spirv_cross::Compiler& compiler,
    VkShaderStageFlagBits /*stage*/,
    std::vector<ShaderResource>& resources,
    const RuntimeArraySizes& arraySizes)
{
    auto subpass_resources = compiler.get_shader_resources().subpass_inputs;

    for (auto& resource : subpass_resources)
    {
        ShaderResource shaderResource{};
        shaderResource.type   = ShaderResourceType::InputAttachment;
        shaderResource.stages = VK_SHADER_STAGE_FRAGMENT_BIT;
        shaderResource.name   = resource.name;

        ReadResourceArraySize(compiler, resource, shaderResource, arraySizes);
        ReadResourceDecoration<spv::DecorationInputAttachmentIndex>(compiler, resource,
                                                                    shaderResource);
        ReadResourceDecoration<spv::DecorationDescriptorSet>(compiler, resource, shaderResource);
        ReadResourceDecoration<spv::DecorationBinding>(compiler, resource, shaderResource);

        resources.push_back(shaderResource);
    }
}

template <>
inline void ReadShaderResource<ShaderResourceType::Output>(const spirv_cross::Compiler& compiler,
                                                           VkShaderStageFlagBits stage,
                                                           std::vector<ShaderResource>& resources,
                                                           const RuntimeArraySizes& arraySizes)
{
    auto output_resources = compiler.get_shader_resources().stage_outputs;

    for (auto& resource : output_resources)
    {
        ShaderResource shaderResource{};
        shaderResource.type   = ShaderResourceType::Output;
        shaderResource.stages = stage;
        shaderResource.name   = resource.name;

        ReadResourceArraySize(compiler, resource, shaderResource, arraySizes);
        ReadResourceVecSize(compiler, resource, shaderResource);
        ReadResourceDecoration<spv::DecorationLocation>(compiler, resource, shaderResource);

        resources.push_back(shaderResource);
    }
}

template <>
inline void ReadShaderResource<ShaderResourceType::Image>(const spirv_cross::Compiler& compiler,
                                                          VkShaderStageFlagBits stage,
                                                          std::vector<ShaderResource>& resources,
                                                          const RuntimeArraySizes& arraySizes)
{
    auto imageResources = compiler.get_shader_resources().separate_images;

    for (auto& resource : imageResources)
    {
        ShaderResource shaderResource{};
        shaderResource.type   = ShaderResourceType::Image;
        shaderResource.stages = stage;
        shaderResource.name   = resource.name;

        ReadResourceArraySize(compiler, resource, shaderResource, arraySizes);
        ReadResourceDecoration<spv::DecorationDescriptorSet>(compiler, resource, shaderResource);
        ReadResourceDecoration<spv::DecorationBinding>(compiler, resource, shaderResource);

        resources.push_back(shaderResource);
    }
}

template <> inline void ReadShaderResource<ShaderResourceType::ImageSampler>(
    const spirv_cross::Compiler& compiler,
    VkShaderStageFlagBits stage,
    std::vector<ShaderResource>& resources,
    const RuntimeArraySizes& arraySizes)
{
    auto imageResources = compiler.get_shader_resources().sampled_images;

    for (auto& resource : imageResources)
    {
        ShaderResource shaderResource{};
        shaderResource.type   = ShaderResourceType::ImageSampler;
        shaderResource.stages = stage;
        shaderResource.name   = resource.name;

        ReadResourceArraySize(compiler, resource, shaderResource, arraySizes);
        ReadResourceDecoration<spv::DecorationDescriptorSet>(compiler, resource, shaderResource);
        ReadResourceDecoration<spv::DecorationBinding>(compiler, resource, shaderResource);

        resources.push_back(shaderResource);
    }
}

template <> inline void ReadShaderResource<ShaderResourceType::ImageStorage>(
    const spirv_cross::Compiler& compiler,
    VkShaderStageFlagBits stage,
    std::vector<ShaderResource>& resources,
    const RuntimeArraySizes& arraySizes)
{
    auto storageResources = compiler.get_shader_resources().storage_images;

    for (auto& resource : storageResources)
    {
        ShaderResource shaderResource{};
        shaderResource.type   = ShaderResourceType::ImageStorage;
        shaderResource.stages = stage;
        shaderResource.name   = resource.name;

        ReadResourceArraySize(compiler, resource, shaderResource, arraySizes);
        ReadResourceDecoration<spv::DecorationNonReadable>(compiler, resource, shaderResource);
        ReadResourceDecoration<spv::DecorationNonWritable>(compiler, resource, shaderResource);
        ReadResourceDecoration<spv::DecorationDescriptorSet>(compiler, resource, shaderResource);
        ReadResourceDecoration<spv::DecorationBinding>(compiler, resource, shaderResource);

        resources.push_back(shaderResource);
    }
}

template <>
inline void ReadShaderResource<ShaderResourceType::Sampler>(const spirv_cross::Compiler& compiler,
                                                            VkShaderStageFlagBits stage,
                                                            std::vector<ShaderResource>& resources,
                                                            const RuntimeArraySizes& arraySizes)
{
    auto samplerResources = compiler.get_shader_resources().separate_samplers;

    for (auto& resource : samplerResources)
    {
        ShaderResource shaderResource{};
        shaderResource.type   = ShaderResourceType::Sampler;
        shaderResource.stages = stage;
        shaderResource.name   = resource.name;

        ReadResourceArraySize(compiler, resource, shaderResource, arraySizes);
        ReadResourceDecoration<spv::DecorationDescriptorSet>(compiler, resource, shaderResource);
        ReadResourceDecoration<spv::DecorationBinding>(compiler, resource, shaderResource);

        resources.push_back(shaderResource);
    }
}

template <> inline void ReadShaderResource<ShaderResourceType::BufferUniform>(
    const spirv_cross::Compiler& compiler,
    VkShaderStageFlagBits stage,
    std::vector<ShaderResource>& resources,
    const RuntimeArraySizes& arraySizes)
{
    auto uniformResources = compiler.get_shader_resources().uniform_buffers;

    for (auto& resource : uniformResources)
    {
        ShaderResource shaderResource{};
        shaderResource.type   = ShaderResourceType::BufferUniform;
        shaderResource.stages = stage;
        shaderResource.name   = resource.name;

        ReadResourceSize(compiler, resource, shaderResource, arraySizes);
        ReadResourceArraySize(compiler, resource, shaderResource, arraySizes);
        ReadResourceDecoration<spv::DecorationDescriptorSet>(compiler, resource, shaderResource);
        ReadResourceDecoration<spv::DecorationBinding>(compiler, resource, shaderResource);

        resources.push_back(shaderResource);
    }
}

template <> inline void ReadShaderResource<ShaderResourceType::BufferStorage>(
    const spirv_cross::Compiler& compiler,
    VkShaderStageFlagBits stage,
    std::vector<ShaderResource>& resources,
    const RuntimeArraySizes& arraySizes)
{
    auto storageResources = compiler.get_shader_resources().storage_buffers;

    for (auto& resource : storageResources)
    {
        ShaderResource shaderResource;
        shaderResource.type   = ShaderResourceType::BufferStorage;
        shaderResource.stages = stage;
        shaderResource.name   = resource.name;

        ReadResourceSize(compiler, resource, shaderResource, arraySizes);
        ReadResourceArraySize(compiler, resource, shaderResource, arraySizes);
        ReadResourceDecoration<spv::DecorationNonReadable>(compiler, resource, shaderResource);
        ReadResourceDecoration<spv::DecorationNonWritable>(compiler, resource, shaderResource);
        ReadResourceDecoration<spv::DecorationDescriptorSet>(compiler, resource, shaderResource);
        ReadResourceDecoration<spv::DecorationBinding>(compiler, resource, shaderResource);

        resources.push_back(shaderResource);
    }
}

class SpirvReflection
{
public:
    static void ReflectEntryPoint(const std::vector<uint32_t>& spirv, std::string& entryPoint)
    {
        spirv_cross::CompilerGLSL compiler{spirv};
        entryPoint = compiler.get_entry_points_and_stages()[0].name;
    }

    static void ReflectShaderResources(VkShaderStageFlagBits stage,
                                       const std::vector<uint32_t>& spirv,
                                       std::vector<ShaderResource>& resources,
                                       const RuntimeArraySizes& arraySizes)
    {
        spirv_cross::CompilerGLSL compiler{spirv};

        auto opts                     = compiler.get_common_options();
        opts.enable_420pack_extension = true;

        compiler.set_common_options(opts);

        ParseShaderResources(compiler, stage, resources, arraySizes);
        ParsePushConstants(compiler, stage, resources, arraySizes);
        ParseSpecializationConstants(compiler, stage, resources, arraySizes);
    }
};

void ParseShaderResources(const spirv_cross::Compiler& compiler,
                          VkShaderStageFlagBits stage,
                          std::vector<ShaderResource>& resources,
                          const RuntimeArraySizes& arraySizes)
{
    ReadShaderResource<ShaderResourceType::Input>(compiler, stage, resources, arraySizes);
    ReadShaderResource<ShaderResourceType::InputAttachment>(compiler, stage, resources, arraySizes);
    ReadShaderResource<ShaderResourceType::Output>(compiler, stage, resources, arraySizes);
    ReadShaderResource<ShaderResourceType::Image>(compiler, stage, resources, arraySizes);
    ReadShaderResource<ShaderResourceType::ImageSampler>(compiler, stage, resources, arraySizes);
    ReadShaderResource<ShaderResourceType::ImageStorage>(compiler, stage, resources, arraySizes);
    ReadShaderResource<ShaderResourceType::Sampler>(compiler, stage, resources, arraySizes);
    ReadShaderResource<ShaderResourceType::BufferUniform>(compiler, stage, resources, arraySizes);
    ReadShaderResource<ShaderResourceType::BufferStorage>(compiler, stage, resources, arraySizes);
}

void ParsePushConstants(const spirv_cross::Compiler& compiler,
                        VkShaderStageFlagBits stage,
                        std::vector<ShaderResource>& resources,
                        const RuntimeArraySizes& arraySizes)
{
    auto shaderResources = compiler.get_shader_resources();

    for (auto& resource : shaderResources.push_constant_buffers)
    {
        const auto& spirvType = compiler.get_type_from_variable(resource.id);

        uint32_t offset = (std::numeric_limits<uint32_t>::max)();

        for (auto i = 0U; i < spirvType.member_types.size(); ++i)
        {
            auto memOffset =
                compiler.get_member_decoration(spirvType.self, i, spv::DecorationOffset);

            offset = std::min(offset, memOffset);
        }

        ShaderResource shaderResource{};
        shaderResource.type   = ShaderResourceType::PushConstant;
        shaderResource.stages = stage;
        shaderResource.name   = resource.name;
        shaderResource.offset = offset;

        ReadResourceSize(compiler, resource, shaderResource, arraySizes);

        shaderResource.size -= shaderResource.offset;

        resources.push_back(shaderResource);
    }
}

void ParseSpecializationConstants(const spirv_cross::Compiler& compiler,
                                  VkShaderStageFlagBits stage,
                                  std::vector<ShaderResource>& resources,
                                  const RuntimeArraySizes& arraySizes)
{
    auto specializationConstants = compiler.get_specialization_constants();

    for (auto& resource : specializationConstants)
    {
        auto& spirvValue = compiler.get_constant(resource.id);

        ShaderResource shaderResource{};
        shaderResource.type       = ShaderResourceType::SpecializationConstant;
        shaderResource.stages     = stage;
        shaderResource.name       = compiler.get_name(resource.id);
        shaderResource.offset     = 0;
        shaderResource.constantId = resource.constant_id;

        ReadResourceSize(compiler, spirvValue, shaderResource, arraySizes);

        resources.push_back(shaderResource);
    }
}

} // namespace zen::val