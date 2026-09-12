#pragma once
#include <algorithm>
#include "RHIResource.h"
#include "spirv_reflect.h"
#include "Utils/Errors.h"

namespace zen
{
class RHIShaderUtil
{
public:
    static RHIShaderGroupSPIRVPtr CompileShaderSourceToSPIRV(
        RHIShaderGroupSourcePtr shaderGroupSource);

    static void ReflectShaderGroupInfo(RHIShaderGroupSPIRVPtr shaderGroupSpirv,
                                       RHIShaderGroupInfo& shaderGroupInfo);

    static void PrintShaderGroupInfo(const RHIShaderGroupInfo& shaderGroupInfo);
};

inline RHIShaderGroupSPIRVPtr RHIShaderUtil::CompileShaderSourceToSPIRV(
    RHIShaderGroupSourcePtr shaderGroupSource)
{
    return MakeRefCountPtr<RHIShaderGroupSPIRV>();
}

static bool StartsWith(std::string_view str, std::string_view prefix)
{
    return str.size() >= prefix.size() && str.substr(0, prefix.size()) == prefix;
}

static void ParseSpvVertexInput(const SpvReflectShaderModule* pModule,
                                RHIShaderGroupInfo& shaderGroupInfo)
{
    uint32_t inputVarCount{0};
    SpvReflectResult result = spvReflectEnumerateInputVariables(pModule, &inputVarCount, nullptr);
    VERIFY_EXPR(result == SPV_REFLECT_RESULT_SUCCESS);
    HeapVector<SpvReflectInterfaceVariable*> inputVars;

    if (inputVarCount > 0)
    {
        inputVars.resize(inputVarCount);
        result = spvReflectEnumerateInputVariables(pModule, &inputVarCount, inputVars.data());

        for (SpvReflectInterfaceVariable* pInputVar : inputVars)
        {
            if (StartsWith(pInputVar->name, "gl_"))
            {
                inputVarCount--;
            }
        }
    }

    if (inputVarCount > 0)
    {
        VERIFY_EXPR(result == SPV_REFLECT_RESULT_SUCCESS);
        std::sort(
            inputVars.begin(), inputVars.end(),
            [](const SpvReflectInterfaceVariable* pLhs, const SpvReflectInterfaceVariable* pRhs) {
                return pLhs->location < pRhs->location;
            });
        shaderGroupInfo.vertexInputAttributes.resize(inputVarCount);
        // packed vertex input data
        //           Location 0     Location 1    Location 0     Location 1
        // binding 0   xyz            uv            xyz            uv
        // struct Vertex {
        //     float   x, y, z;
        //     uint8_t u, v;
        // };
        uint32_t vertexAttributeOffset = 0;

        for (uint32_t i = 0; i < inputVarCount; i++)
        {
            RHIShaderGroupInfo::VertexInputAttribute& vertexAttribute =
                shaderGroupInfo.vertexInputAttributes[i];
            SpvReflectInterfaceVariable* const& inputVar = inputVars[i];
            const uint32_t inputVarSize =
                (inputVar->numeric.scalar.width / 8) * inputVar->numeric.vector.component_count;
            vertexAttribute.name     = inputVar->name;
            vertexAttribute.location = inputVar->location;
            vertexAttribute.binding  = 0;
            vertexAttribute.offset   = vertexAttributeOffset;
            vertexAttribute.format   = static_cast<DataFormat>(inputVar->format);

            vertexAttributeOffset += inputVarSize;
        }

        shaderGroupInfo.vertexBindingStride = vertexAttributeOffset;
    }
}

static void ParseSpvPushConstants(RHIShaderStage stage,
                                  const SpvReflectShaderModule* pModule,
                                  RHIShaderGroupInfo& shaderGroupInfo)
{
    uint32_t pcCount{0};
    SpvReflectResult result = spvReflectEnumeratePushConstantBlocks(pModule, &pcCount, nullptr);
    VERIFY_EXPR(result == SPV_REFLECT_RESULT_SUCCESS);
    HeapVector<SpvReflectBlockVariable*> pconstants;

    if (pcCount > 1)
    {
        LOG_ERROR_AND_THROW(
            "Only one push constant is supported, which should be the same across shader stages.");
    }
    else if (pcCount == 0)
    {
        return;
    }

    pconstants.resize(pcCount);
    result = spvReflectEnumeratePushConstantBlocks(pModule, &pcCount, pconstants.data());
    VERIFY_EXPR(result == SPV_REFLECT_RESULT_SUCCESS);
    shaderGroupInfo.pushConstants.size = pconstants[0]->size;
    shaderGroupInfo.pushConstants.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));
    shaderGroupInfo.pushConstants.name = pconstants[0]->type_description->type_name;
}

static void ParseSpvSpecializationConstant(RHIShaderStage stage,
                                           const SpvReflectShaderModule* pModule,
                                           RHIShaderGroupInfo& shaderGroupInfo)
{
    uint32_t scCount{0};
    SpvReflectResult result =
        spvReflectEnumerateSpecializationConstants(pModule, &scCount, nullptr);

    if (result != SPV_REFLECT_RESULT_SUCCESS)
    {
        LOGE("Reflection of SPIR-V shader stage {} specialization constant failed",
             RHIShaderStageToString(stage));
    }

    if (scCount > 0)
    {
        HeapVector<SpvReflectSpecializationConstant*> specConstants;
        specConstants.resize(scCount);
        spvReflectEnumerateSpecializationConstants(pModule, &scCount, specConstants.data());

        for (uint32_t j = 0; j < scCount; j++)
        {
            int existed = -1;
            RHIShaderSpecializationConstant specConst;
            SpvReflectSpecializationConstant* pSpvSpecConst = specConstants[j];

            specConst.constantId = pSpvSpecConst->constant_id;
            specConst.intValue   = 0;

            switch (pSpvSpecConst->constant_type)
            {
                case SPV_REFLECT_SPECIALIZATION_CONSTANT_BOOL:
                {
                    specConst.type      = RHIShaderSpecializationConstantType::eBool;
                    specConst.boolValue = pSpvSpecConst->default_value.int_bool_value != 0;
                }
                break;

                case SPV_REFLECT_SPECIALIZATION_CONSTANT_INT:
                {
                    specConst.type     = RHIShaderSpecializationConstantType::eInt;
                    specConst.intValue = pSpvSpecConst->default_value.int_bool_value;
                }
                break;

                case SPV_REFLECT_SPECIALIZATION_CONSTANT_FLOAT:
                {
                    specConst.type       = RHIShaderSpecializationConstantType::eFloat;
                    specConst.floatValue = pSpvSpecConst->default_value.float_value;
                    break;
                }
            }

            specConst.stages.SetFlag(RHIShaderStageToFlagBits(stage));

            for (int k = 0; k < shaderGroupInfo.specializationConstants.size(); k++)
            {
                if (shaderGroupInfo.specializationConstants[k].constantId == specConst.constantId)
                {
                    if (shaderGroupInfo.specializationConstants[k].type != specConst.type)
                    {
                        LOGE(
                            "More than one specialization constant used for id={} with different type",
                            specConst.constantId);
                    }

                    if (shaderGroupInfo.specializationConstants[k].intValue != specConst.intValue)
                    {
                        LOGE(
                            "More than one specialization constant used for id={} with different value",
                            specConst.constantId);
                    }

                    existed = k;
                    break;
                }
            }

            if (existed >= 0)
            {
                shaderGroupInfo.specializationConstants[existed].stages.SetFlag(
                    RHIShaderStageToFlagBits(stage));
            }
            else
            {
                shaderGroupInfo.specializationConstants.push_back(specConst);
            }
        }
    }
}

// Member flags describe that member, including a containing struct/array. The bundled
// reflector synthesizes NonWritable on a block when ANY member is readonly, so that
// root block flag cannot classify the whole binding. Union member capabilities instead.
static bool BlockMemberAllowsAccess(const SpvReflectBlockVariable& member,
                                    uint32_t forbiddenDecoration,
                                    uint32_t depth)
{
    const uint32_t typeFlags =
        member.type_description ? member.type_description->decoration_flags : 0;
    bool allowed = false;

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

static bool DescriptorBindingAllowsAccess(const SpvReflectDescriptorBinding& binding,
                                          uint32_t forbiddenDecoration)
{
    const uint32_t typeFlags =
        binding.type_description ? binding.type_description->decoration_flags : 0;
    bool allowed = false;

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

static void ParseSpvReflectDescriptorBinding(const SpvReflectDescriptorBinding& reflBinding,
                                             RHIShaderResourceDescriptor& srd)
{
    bool needArrayDims = false;
    bool needBlockSize = false;
    bool writable      = false;
    bool readable      = true;

    if (reflBinding.type_description != nullptr &&
        reflBinding.type_description->type_name != nullptr)
    {
        srd.name = reflBinding.type_description->type_name;
    }
    else
    {
        srd.name = reflBinding.name;
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
            writable =
                DescriptorBindingAllowsAccess(reflBinding, SPV_REFLECT_DECORATION_NON_WRITABLE);
            readable =
                DescriptorBindingAllowsAccess(reflBinding, SPV_REFLECT_DECORATION_NON_READABLE);
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
            writable =
                DescriptorBindingAllowsAccess(reflBinding, SPV_REFLECT_DECORATION_NON_WRITABLE);
            readable =
                DescriptorBindingAllowsAccess(reflBinding, SPV_REFLECT_DECORATION_NON_READABLE);
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
            writable =
                DescriptorBindingAllowsAccess(reflBinding, SPV_REFLECT_DECORATION_NON_WRITABLE);
            readable =
                DescriptorBindingAllowsAccess(reflBinding, SPV_REFLECT_DECORATION_NON_READABLE);
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
            LOGE("Acceleration structure not supported.");
        }
        break;
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
                srd.arraySize *= arrayDimension;
            }
        }
    }

    if (needBlockSize)
    {
        srd.blockSize = reflBinding.block.size;
    }
}

static void MergeOrAddSRDs(RHIShaderStage stage,
                           RHIShaderResourceDescriptor& srd,
                           RHIShaderGroupInfo& shaderGroupInfo)
{
    const RHIShaderStageFlagBits stageFlag = RHIShaderStageToFlagBits(stage);
    const uint32_t setIndex                = srd.set;
    bool existed                           = false;

    RHIShaderResourceDescriptorTable& allSRDs = shaderGroupInfo.SRDTable;

    if (setIndex < allSRDs.size())
    {
        for (uint32_t k = 0; k < allSRDs[setIndex].size(); k++)
        {
            RHIShaderResourceDescriptor const& existSRD = allSRDs[setIndex][k];

            if (existSRD.binding == srd.binding)
            {
                if (existSRD.type != srd.type)
                {
                    LOGE(
                        "On shader stage {} , srd {} trying to reuse location for set={}, binding={} with different srd type",
                        RHIShaderStageToString(stage), srd.name.CStr(), setIndex, srd.binding);
                }

                if (existSRD.arraySize != srd.arraySize)
                {
                    LOGE(
                        "On shader stage {} , srd {} trying to reuse location for set={}, binding={} with different srd arraySize",
                        RHIShaderStageToString(stage), srd.name.CStr(), setIndex, srd.binding);
                }

                if (existSRD.blockSize != srd.blockSize)
                {
                    LOGE(
                        "On shader stage {} , srd {} trying to reuse location for set={}, binding={} with different srd blockSize",
                        RHIShaderStageToString(stage), srd.name.CStr(), setIndex, srd.binding);
                }

                if (existSRD.bindless != srd.bindless)
                {
                    LOGE(
                        "On shader stage {} , srd {} trying to reuse location for set={}, binding={} with different bindless state",
                        RHIShaderStageToString(stage), srd.name.CStr(), setIndex, srd.binding);
                }

                existed = true;
            }

            if (existed)
            {
                allSRDs[setIndex][k].stageFlags.SetFlag(stageFlag);
                allSRDs[setIndex][k].readable |= srd.readable;
                allSRDs[setIndex][k].writable |= srd.writable;
                break;
            }
        }
    }
    else
    {
        allSRDs.resize(setIndex + 1);
    }

    if (!existed)
    {
        srd.stageFlags.SetFlag(stageFlag);
        allSRDs[setIndex].push_back(srd);
    }
}

inline void RHIShaderUtil::ReflectShaderGroupInfo(RHIShaderGroupSPIRVPtr shaderGroupSpirv,
                                                  RHIShaderGroupInfo& shaderGroupInfo)
{
    for (uint32_t i = 0; i < ToUnderlying(RHIShaderStage::eMax); i++)
    {
        RHIShaderStage stage = static_cast<RHIShaderStage>(i);

        if (shaderGroupSpirv->HasShaderStage(stage))
        {
            // shaderGroupInfo.sprivCode[stage] = std::move(shaderGroupSpirv->GetStageSPIRV(stage));
            SpvReflectShaderModule module;
            const HeapVector<uint8_t>& spirvCode = shaderGroupSpirv->GetStageSPIRV(stage);
            SpvReflectResult result =
                spvReflectCreateShaderModule(spirvCode.size(), spirvCode.data(), &module);

            if (result != SPV_REFLECT_RESULT_SUCCESS)
            {
                LOGE("Reflection of SPIR-V shader stage {} failed", RHIShaderStageToString(stage));
            }

            uint32_t setCount{0};
            result = spvReflectEnumerateDescriptorSets(&module, &setCount, nullptr);
            VERIFY_EXPR(result == SPV_REFLECT_RESULT_SUCCESS);
            HeapVector<SpvReflectDescriptorSet*> sets(setCount);
            result = spvReflectEnumerateDescriptorSets(&module, &setCount, sets.data());
            VERIFY_EXPR(result == SPV_REFLECT_RESULT_SUCCESS);

            // if (shaderGroupInfo.SRDs.size() < setCount) { shaderGroupInfo.SRDs.resize(setCount); }

            for (uint32_t setIndex = 0; setIndex < setCount; setIndex++)
            {
                const SpvReflectDescriptorSet& reflSet = *(sets[setIndex]);

                // std::vector<RHIShaderResourceDescriptor>& setResources =
                //     shaderGroupInfo.SRDs[setIndex];
                // setResources.resize(reflSet.binding_count);
                if (shaderGroupInfo.SRDTable.size() <= reflSet.set)
                {
                    shaderGroupInfo.SRDTable.resize(reflSet.set + 1);
                }

                shaderGroupInfo.SRDTable[reflSet.set].reserve(reflSet.binding_count);

                for (uint32_t binding = 0; binding < reflSet.binding_count; binding++)
                {
                    const SpvReflectDescriptorBinding& reflBinding = *(reflSet.bindings[binding]);
                    RHIShaderResourceDescriptor srd{};
                    ParseSpvReflectDescriptorBinding(reflBinding, srd);
                    MergeOrAddSRDs(stage, srd, shaderGroupInfo);
                }
            }

            // Specialization Constants
            ParseSpvSpecializationConstant(stage, &module, shaderGroupInfo);

            // Parse vertex input
            if (stage == RHIShaderStage::eVertex)
            {
                ParseSpvVertexInput(&module, shaderGroupInfo);
            }

            // Parse push constants
            ParseSpvPushConstants(stage, &module, shaderGroupInfo);
        }
    }
}

inline void RHIShaderUtil::PrintShaderGroupInfo(const RHIShaderGroupInfo& sgInfo)
{
    LOGI("======= Begin Printing RHIShaderGroupInfo =======")
    std::string stagesStr;
    LOGI("Shader Stages: {}", stagesStr);
    LOGI("PushConstant: name={} size={}", sgInfo.pushConstants.name.CStr(),
         sgInfo.pushConstants.size);
    LOGI("SRD Set Count={}", sgInfo.SRDTable.size());

    for (SmallVector<RHIShaderResourceDescriptor> const& setSRD : sgInfo.SRDTable)
    {
        for (RHIShaderResourceDescriptor const& srd : setSRD)
        {
            LOGI("SRD stage={} name={} set={} binding={} arraySize={}",
                 RHIShaderStageFlagToString(srd.stageFlags), srd.name.CStr(), srd.set, srd.binding,
                 srd.arraySize);
        }
    }

    for (RHIShaderGroupInfo::VertexInputAttribute const& va : sgInfo.vertexInputAttributes)
    {
        LOGI("Vertex Input Attr name={} binding={} location={} offset={}", va.name.CStr(),
             va.binding, va.location, va.offset);
    }

    LOGI("Vertex Binding Stride={}", sgInfo.vertexBindingStride);
    LOGI("======= End Printing RHIShaderGroupInfo =======")
}
} // namespace zen
