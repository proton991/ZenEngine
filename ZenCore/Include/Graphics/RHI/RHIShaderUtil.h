#pragma once
#include <algorithm>
#include <bit>
#include <cstring>
#include "RHIResource.h"
#include "spirv_reflect.h"
#include "Utils/Errors.h"

namespace zen
{
class RHIShaderUtil
{
public:
    [[nodiscard]] static bool ReflectShaderGroupInfo(RHIShaderGroupSPIRVPtr shaderGroupSpirv,
                                                     RHIShaderGroupInfo&    shaderGroupInfo);
};

static bool ParseSpvVertexInput(const SpvReflectShaderModule* module, RHIShaderGroupInfo& info)
{
    uint32_t count = 0;

    bool valid     = spvReflectEnumerateInputVariables(module, &count, nullptr) == SPV_REFLECT_RESULT_SUCCESS;

    HeapVector<SpvReflectInterfaceVariable*> inputs(count);

    valid = valid && spvReflectEnumerateInputVariables(module, &count, inputs.data()) == SPV_REFLECT_RESULT_SUCCESS;

    if (valid)
    {
        std::sort(inputs.begin(), inputs.end(),
                  [](const SpvReflectInterfaceVariable* lhs, const SpvReflectInterfaceVariable* rhs) {
                      return lhs->location < rhs->location;
                  });

        for (const SpvReflectInterfaceVariable* input : inputs)
        {
            if ((input->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) == 0)
            {
                valid = input->format != SPV_REFLECT_FORMAT_UNDEFINED && input->numeric.scalar.width != 0;

                if (!valid)
                {
                    break;
                }

                RHIShaderGroupInfo::VertexInputAttribute attribute{};

                attribute.name     = input->name == nullptr ? "" : input->name;

                attribute.location = input->location;

                attribute.binding  = 0;

                attribute.offset   = info.vertexBindingStride;

                attribute.format   = static_cast<DataFormat>(input->format);

                info.vertexBindingStride +=
                    input->numeric.scalar.width / 8 * std::max(1u, input->numeric.vector.component_count);

                info.vertexInputAttributes.push_back(attribute);
            }
        }
    }

    return valid;
}

static bool ParseSpvPushConstants(RHIShaderStage stage, const SpvReflectShaderModule* module, RHIShaderGroupInfo& info)
{
    uint32_t count = 0;

    bool valid     = spvReflectEnumeratePushConstantBlocks(module, &count, nullptr) == SPV_REFLECT_RESULT_SUCCESS && count <= 1;

    if (valid && count == 1)
    {
        SpvReflectBlockVariable* block = nullptr;

        valid = spvReflectEnumeratePushConstantBlocks(module, &count, &block) == SPV_REFLECT_RESULT_SUCCESS && block != nullptr;

        if (valid)
        {
            valid = block->size != 0 && block->offset <= block->size && block->size % 4 == 0 && block->offset % 4 == 0;

            if (valid)
            {
                // Reflect reports the end of the block, including any initial offset.
                // A single range covers the union of the stages' declared members.
                info.pushConstants.size = std::max(info.pushConstants.size, block->size);

                info.pushConstants.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));

                info.pushConstants.name = block->type_description != nullptr && block->type_description->type_name != nullptr
                                            ? block->type_description->type_name
                                            : "";
            }
        }
    }

    if (!valid)
    {
        LOGE("Shader stage {} requires at most one aligned push-constant block", RHIShaderStageToString(stage));
    }

    return valid;
}

inline bool ParseSpvSpecializationConstant(RHIShaderStage stage, const SpvReflectShaderModule* module, RHIShaderGroupInfo& info)
{
    uint32_t count = 0;

    bool valid     = spvReflectEnumerateSpecializationConstants(module, &count, nullptr) == SPV_REFLECT_RESULT_SUCCESS;

    HeapVector<SpvReflectSpecializationConstant*> constants(count);

    valid = valid && spvReflectEnumerateSpecializationConstants(module, &count, constants.data()) == SPV_REFLECT_RESULT_SUCCESS;

    for (uint32_t i = 0; valid && i < count; ++i)
    {
        const SpvReflectSpecializationConstant& reflected = *constants[i];

        valid = reflected.type_description != nullptr && reflected.default_value != nullptr
             && reflected.default_value_size == sizeof(uint32_t)
             && (reflected.type_description->op == SpvOpTypeBool
                 || reflected.type_description->traits.numeric.scalar.width == 32);

        if (valid)
        {
            RHIShaderSpecializationConstant value{};

            value.constantId = reflected.constant_id;

            std::memcpy(&value.bits, reflected.default_value, sizeof(value.bits));

            switch (reflected.type_description->op)
            {
                case SpvOpTypeBool:
                    value.type = RHIShaderSpecializationConstantType::eBool;
                    value.bits = value.bits != 0;
                    break;
                case SpvOpTypeInt: value.type = RHIShaderSpecializationConstantType::eInt; break;
                case SpvOpTypeFloat: value.type = RHIShaderSpecializationConstantType::eFloat; break;
                default: valid = false; break;
            }

            value.stages.SetFlag(RHIShaderStageToFlagBits(stage));

            bool existed = false;

            for (RHIShaderSpecializationConstant& previous : info.specializationConstants)
            {
                if (previous.constantId == value.constantId)
                {
                    existed = true;

                    valid   = valid && previous.type == value.type && previous.bits == value.bits;

                    previous.stages.SetFlag(RHIShaderStageToFlagBits(stage));
                }
            }

            if (valid && !existed)
            {
                info.specializationConstants.push_back(value);
            }
        }
    }

    if (!valid)
    {
        LOGE(
            "Shader stage {} has invalid specialization constants; only compatible Boolean and 32-bit scalar constants are supported",
            RHIShaderStageToString(stage));
    }

    return valid;
}

// Member flags describe that member, including a containing struct/array. The bundled
// reflector synthesizes NonWritable on a block when ANY member is readonly, so that
// root block flag cannot classify the whole binding. Union member capabilities instead.
static bool BlockMemberAllowsAccess(const SpvReflectBlockVariable& member, uint32_t forbiddenDecoration, uint32_t depth)
{
    const uint32_t typeFlags = member.type_description ? member.type_description->decoration_flags : 0;
    bool           allowed   = false;

    if (((member.decoration_flags | typeFlags) & forbiddenDecoration) == 0)
    {
        // Unknown/recursive metadata must not hide an access.
        allowed = member.member_count == 0 || member.members == nullptr || depth >= 32;

        for (uint32_t i = 0; !allowed && i < member.member_count; ++i)
        {
            allowed = BlockMemberAllowsAccess(member.members[i], forbiddenDecoration, depth + 1);
        }
    }

    return allowed;
}

static bool DescriptorBindingAllowsAccess(const SpvReflectDescriptorBinding& binding, uint32_t forbiddenDecoration)
{
    const uint32_t typeFlags = binding.type_description ? binding.type_description->decoration_flags : 0;
    bool           allowed   = false;

    if (((binding.decoration_flags | typeFlags) & forbiddenDecoration) == 0)
    {
        if (binding.block.member_count == 0)
        {
            allowed = (binding.block.decoration_flags & forbiddenDecoration) == 0;
        }
        else
        {
            allowed = binding.block.members == nullptr;

            for (uint32_t i = 0; !allowed && i < binding.block.member_count; ++i)
            {
                allowed = BlockMemberAllowsAccess(binding.block.members[i], forbiddenDecoration, 0);
            }
        }
    }

    return allowed;
}

static bool ParseSpvReflectDescriptorBinding(const SpvReflectDescriptorBinding& reflBinding, RHIShaderResourceDescriptor& srd)
{
    bool needArrayDims = false;
    bool needBlockSize = false;
    bool writable      = false;
    bool readable      = true;

    if (reflBinding.type_description != nullptr && reflBinding.type_description->type_name != nullptr)
    {
        srd.name = reflBinding.type_description->type_name;
    }
    else
    {
        srd.name = reflBinding.name != nullptr ? reflBinding.name : "";
    }

    srd.set       = reflBinding.set;
    srd.binding   = reflBinding.binding;
    srd.arraySize = 1;

    switch (reflBinding.descriptor_type)
    {
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
        {
            srd.type      = RHIShaderResourceType::eSampler;
            needArrayDims = true;
        }
        break;

        case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        {
            srd.type      = RHIShaderResourceType::eSamplerWithTexture;
            needArrayDims = true;
        }
        break;

        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        {
            srd.type      = RHIShaderResourceType::eTexture;
            needArrayDims = true;
        }
        break;

        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        {
            srd.type      = RHIShaderResourceType::eImage;
            needArrayDims = true;
            writable      = DescriptorBindingAllowsAccess(reflBinding, SPV_REFLECT_DECORATION_NON_WRITABLE);
            readable      = DescriptorBindingAllowsAccess(reflBinding, SPV_REFLECT_DECORATION_NON_READABLE);
        }
        break;

        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        {
            srd.type      = RHIShaderResourceType::eTextureBuffer;
            needArrayDims = true;
        }
        break;

        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        {
            srd.type      = RHIShaderResourceType::eImageBuffer;
            needArrayDims = true;
            writable      = DescriptorBindingAllowsAccess(reflBinding, SPV_REFLECT_DECORATION_NON_WRITABLE);
            readable      = DescriptorBindingAllowsAccess(reflBinding, SPV_REFLECT_DECORATION_NON_READABLE);
        }
        break;

        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        {
            srd.type      = RHIShaderResourceType::eUniformBuffer;
            needBlockSize = true;
            needArrayDims = true;
        }
        break;

        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        {
            srd.type      = RHIShaderResourceType::eStorageBuffer;
            needBlockSize = true;
            needArrayDims = true;
            writable      = DescriptorBindingAllowsAccess(reflBinding, SPV_REFLECT_DECORATION_NON_WRITABLE);
            readable      = DescriptorBindingAllowsAccess(reflBinding, SPV_REFLECT_DECORATION_NON_READABLE);
        }
        break;

        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        {
            LOGE("Dynamic srd buffer not supported.");
        }
        break;

        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        {
            LOGE("Dynamic storage buffer not supported.");
        }
        break;

        case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        {
            srd.type      = RHIShaderResourceType::eInputAttachment;
            needArrayDims = true;
        }
        break;

        case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
        {
            srd.type      = RHIShaderResourceType::eAccelerationStructure;
            needArrayDims = true;
            readable      = true;
            writable      = false;
        }
        break;
        default: break;
    }

    // Contradictory or incomplete restrictions must not suppress both capabilities.
    if (!readable && !writable)
    {
        readable = writable = true;
    }

    srd.readable = readable;
    srd.writable = writable;

    if (needArrayDims)
    {
        for (uint32_t i = 0; i < reflBinding.array.dims_count; i++)
        {
            const uint32_t arrayDimension = reflBinding.array.dims[i];

            if (arrayDimension == SPV_REFLECT_ARRAY_DIM_RUNTIME)
            {
                srd.bindless = true;
            }
            else
            {
                if (arrayDimension == 0 || srd.arraySize > UINT32_MAX / arrayDimension)
                {
                    srd.type = RHIShaderResourceType::eMax;
                }
                else
                {
                    srd.arraySize *= arrayDimension;
                }
            }
        }
    }

    if (needBlockSize)
    {
        srd.blockSize = reflBinding.block.size;
    }
    return srd.type != RHIShaderResourceType::eMax;
}

static bool MergeOrAddSRDs(RHIShaderStage stage, RHIShaderResourceDescriptor& descriptor, RHIShaderGroupInfo& info)
{
    bool valid = descriptor.set < 32;

    if (valid)
    {
        if (info.SRDTable.size() <= descriptor.set)
        {
            info.SRDTable.resize(descriptor.set + 1);
        }

        bool existed = false;

        for (RHIShaderResourceDescriptor& previous : info.SRDTable[descriptor.set])
        {
            if (previous.binding == descriptor.binding)
            {
                existed = true;

                valid   = previous.type == descriptor.type && previous.arraySize == descriptor.arraySize
                     && previous.blockSize == descriptor.blockSize && previous.bindless == descriptor.bindless;

                previous.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));

                previous.readable |= descriptor.readable;

                previous.writable |= descriptor.writable;
            }
        }

        if (!existed)
        {
            descriptor.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));

            info.SRDTable[descriptor.set].push_back(descriptor);
        }
    }

    return valid;
}

static bool HasValidSpirvStructure(const HeapVector<uint8_t>& code)
{
    bool valid = code.size() >= 5 * sizeof(uint32_t) && code.size() % sizeof(uint32_t) == 0;

    if (valid)
    {
        uint32_t header[5]{};

        std::memcpy(header, code.data(), sizeof(header));

        valid         = header[0] == 0x07230203u && header[3] != 0 && header[4] == 0;

        size_t offset = sizeof(header);

        while (valid && offset < code.size())
        {
            uint32_t instruction = 0;

            std::memcpy(&instruction, code.data() + offset, sizeof(instruction));

            const size_t size  = size_t(instruction >> 16) * sizeof(uint32_t);

            valid              = size != 0 && size <= code.size() - offset;

            offset            += size;
        }
    }

    return valid;
}

inline bool RHIShaderUtil::ReflectShaderGroupInfo(RHIShaderGroupSPIRVPtr spirv, RHIShaderGroupInfo& output)
{
    bool valid    = spirv != nullptr;

    bool hasStage = false;

    RHIShaderGroupInfo info{};

    for (uint32_t i = 0; valid && i < ToUnderlying(RHIShaderStage::eMax); ++i)
    {
        const RHIShaderStage stage = static_cast<RHIShaderStage>(i);

        if (spirv->HasShaderStage(stage))
        {
            hasStage = true;

            SpvReflectShaderModule module{};

            const HeapVector<uint8_t>& code = spirv->GetStageSPIRV(stage);

            const SpvReflectResult result   = HasValidSpirvStructure(code)
                                                ? spvReflectCreateShaderModule(code.size(), code.data(), &module)
                                                : SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_CODE_SIZE;

            valid                           = result == SPV_REFLECT_RESULT_SUCCESS;

            if (valid)
            {
                uint32_t count = 0;

                valid          = spvReflectEnumerateDescriptorSets(&module, &count, nullptr) == SPV_REFLECT_RESULT_SUCCESS;

                HeapVector<SpvReflectDescriptorSet*> sets(count);

                valid = valid && spvReflectEnumerateDescriptorSets(&module, &count, sets.data()) == SPV_REFLECT_RESULT_SUCCESS;

                for (uint32_t set = 0; valid && set < count; ++set)
                {
                    for (uint32_t binding = 0; valid && binding < sets[set]->binding_count; ++binding)
                    {
                        RHIShaderResourceDescriptor descriptor{};

                        valid = ParseSpvReflectDescriptorBinding(*sets[set]->bindings[binding], descriptor)
                             && MergeOrAddSRDs(stage, descriptor, info);
                    }
                }

                valid = valid && ParseSpvSpecializationConstant(stage, &module, info)
                     && (stage != RHIShaderStage::eVertex || ParseSpvVertexInput(&module, info))
                     && ParseSpvPushConstants(stage, &module, info);

                spvReflectDestroyShaderModule(&module);
            }

            if (!valid)
            {
                LOGE("SPIR-V reflection rejected stage {} (result {}): corrupt or unsupported shader input",
                     RHIShaderStageToString(stage), static_cast<int32_t>(result));
            }
        }
    }

    valid = valid && hasStage;

    if (valid)
    {
        output = std::move(info);
    }

    return valid;
}
} // namespace zen
