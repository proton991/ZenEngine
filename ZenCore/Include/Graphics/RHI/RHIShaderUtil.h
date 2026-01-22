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
    if (str.size() < prefix.size())
    {
        return false;
    }
    return str.substr(0, prefix.size()) == prefix;
}

static void ParseSpvVertexInput(const SpvReflectShaderModule* module,
                                RHIShaderGroupInfo& shaderGroupInfo)
{
    uint32_t inputVarCount{0};
    SpvReflectResult result = spvReflectEnumerateInputVariables(module, &inputVarCount, nullptr);
    VERIFY_EXPR(result == SPV_REFLECT_RESULT_SUCCESS);
    std::vector<SpvReflectInterfaceVariable*> inputVars;
    if (inputVarCount > 0)
    {

        inputVars.resize(inputVarCount);
        result = spvReflectEnumerateInputVariables(module, &inputVarCount, inputVars.data());
        for (auto* inputVar : inputVars)
        {
            if (StartsWith(inputVar->name, "gl_"))
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
            [](const SpvReflectInterfaceVariable* lhs, const SpvReflectInterfaceVariable* rhs) {
                return lhs->location < rhs->location;
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
            auto& vertexAttribute = shaderGroupInfo.vertexInputAttributes[i];
            const auto& inputVar  = inputVars[i];
            const auto inputVarSize =
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
                                  const SpvReflectShaderModule* module,
                                  RHIShaderGroupInfo& shaderGroupInfo)
{
    uint32_t pcCount{0};
    SpvReflectResult result = spvReflectEnumeratePushConstantBlocks(module, &pcCount, nullptr);
    VERIFY_EXPR(result == SPV_REFLECT_RESULT_SUCCESS);
    std::vector<SpvReflectBlockVariable*> pconstants;

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
    result = spvReflectEnumeratePushConstantBlocks(module, &pcCount, pconstants.data());
    VERIFY_EXPR(result == SPV_REFLECT_RESULT_SUCCESS);
    shaderGroupInfo.pushConstants.size = pconstants[0]->size;
    shaderGroupInfo.pushConstants.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));
    shaderGroupInfo.pushConstants.name = pconstants[0]->type_description->type_name;
}

static void ParseSpvSpecializationConstant(RHIShaderStage stage,
                                           const SpvReflectShaderModule* module,
                                           RHIShaderGroupInfo& shaderGroupInfo)
{
    uint32_t scCount{0};
    SpvReflectResult result = spvReflectEnumerateSpecializationConstants(module, &scCount, nullptr);
    if (result != SPV_REFLECT_RESULT_SUCCESS)
    {
        LOGE("Reflection of SPIR-V shader stage {} specialization constant failed",
             RHIShaderStageToString(stage));
    }
    if (scCount > 0)
    {
        std::vector<SpvReflectSpecializationConstant*> specConstants;
        specConstants.resize(scCount);
        spvReflectEnumerateSpecializationConstants(module, &scCount, specConstants.data());
        int existed = -1;
        for (uint32_t j = 0; j < scCount; j++)
        {
            RHIShaderSpecializationConstant specConst;
            SpvReflectSpecializationConstant* spvSpecConst = specConstants[j];

            specConst.constantId = spvSpecConst->constant_id;
            specConst.intValue   = 0;
            switch (spvSpecConst->constant_type)
            {
                case SPV_REFLECT_SPECIALIZATION_CONSTANT_BOOL:
                {
                    specConst.type      = RHIShaderSpecializationConstantType::eBool;
                    specConst.boolValue = spvSpecConst->default_value.int_bool_value != 0;
                }
                break;
                case SPV_REFLECT_SPECIALIZATION_CONSTANT_INT:
                {
                    specConst.type     = RHIShaderSpecializationConstantType::eInt;
                    specConst.intValue = spvSpecConst->default_value.int_bool_value;
                }
                break;
                case SPV_REFLECT_SPECIALIZATION_CONSTANT_FLOAT:
                {
                    specConst.type       = RHIShaderSpecializationConstantType::eFloat;
                    specConst.floatValue = spvSpecConst->default_value.float_value;
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
            if (existed > 0)
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

static bool IsDescriptorBindingWritable(const SpvReflectDescriptorBinding& reflBinding)
{
    bool writable = true;
    if ((reflBinding.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) != 0 ||
        (reflBinding.block.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) != 0)
    {
        writable = false;
    }
    if ((reflBinding.type_description != nullptr) &&
        (reflBinding.type_description->decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) != 0)
    {
        writable = false;
    }
    return writable;
}

static void ParseSpvReflectDescriptorBinding(const SpvReflectDescriptorBinding& reflBinding,
                                             RHIShaderResourceDescriptor& srd)
{
    bool needArrayDims = false;
    bool needBlockSize = false;
    bool writable      = false;

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
            writable      = IsDescriptorBindingWritable(reflBinding);
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
            writable      = IsDescriptorBindingWritable(reflBinding);
        }
        break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        {
            srd.type      = RHIShaderResourceType::eUniformBuffer;
            needBlockSize = true;
        }
        break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        {
            srd.type      = RHIShaderResourceType::eStorageBuffer;
            needBlockSize = true;
            writable      = IsDescriptorBindingWritable(reflBinding);
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

    srd.writable = writable;
    if (needArrayDims)
    {
        for (uint32_t i = 0; i < reflBinding.array.dims_count; i++)
        {
            srd.arraySize *= reflBinding.array.dims[i];
        }
    }
    else if (needBlockSize)
    {
        srd.blockSize = reflBinding.block.size;
    }
}

static void MergeOrAddSRDs(RHIShaderStage stage,
                           RHIShaderResourceDescriptor& srd,
                           RHIShaderGroupInfo& shaderGroupInfo)
{
    const auto stageFlag = RHIShaderStageToFlagBits(stage);
    const auto setIndex  = srd.set;
    bool existed         = false;

    RHIShaderResourceDescriptorTable& allSRDs = shaderGroupInfo.SRDTable;
    if (setIndex < allSRDs.size())
    {
        for (uint32_t k = 0; k < allSRDs[setIndex].size(); k++)
        {
            const auto& existSRD = allSRDs[setIndex][k];
            if (existSRD.binding == srd.binding)
            {
                if (existSRD.type != srd.type)
                {
                    LOGE(
                        "On shader stage {} , srd {} trying to reuse location for set={}, binding={} with different srd type",
                        RHIShaderStageToString(stage), srd.name, setIndex, srd.binding);
                }
                if (existSRD.arraySize != srd.arraySize)
                {
                    LOGE(
                        "On shader stage {} , srd {} trying to reuse location for set={}, binding={} with different srd arraySize",
                        RHIShaderStageToString(stage), srd.name, setIndex, srd.binding);
                }
                if (existSRD.blockSize != srd.blockSize)
                {
                    LOGE(
                        "On shader stage {} , srd {} trying to reuse location for set={}, binding={} with different srd blockSize",
                        RHIShaderStageToString(stage), srd.name, setIndex, srd.binding);
                }
                existed = true;
            }
            if (existed)
            {
                allSRDs[setIndex][k].stageFlags.SetFlag(stageFlag);
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
            const std::vector<uint8_t>& spirvCode = shaderGroupSpirv->GetStageSPIRV(stage);
            SpvReflectResult result =
                spvReflectCreateShaderModule(spirvCode.size(), spirvCode.data(), &module);
            if (result != SPV_REFLECT_RESULT_SUCCESS)
            {
                LOGE("Reflection of SPIR-V shader stage {} failed", RHIShaderStageToString(stage));
            }
            uint32_t setCount{0};
            result = spvReflectEnumerateDescriptorSets(&module, &setCount, nullptr);
            VERIFY_EXPR(result == SPV_REFLECT_RESULT_SUCCESS);
            std::vector<SpvReflectDescriptorSet*> sets(setCount);
            result = spvReflectEnumerateDescriptorSets(&module, &setCount, sets.data());
            VERIFY_EXPR(result == SPV_REFLECT_RESULT_SUCCESS);

            // if (shaderGroupInfo.SRDs.size() < setCount) { shaderGroupInfo.SRDs.resize(setCount); }

            for (uint32_t setIndex = 0; setIndex < setCount; setIndex++)
            {
                const SpvReflectDescriptorSet& reflSet = *(sets[setIndex]);
                // std::vector<RHIShaderResourceDescriptor>& setResources =
                //     shaderGroupInfo.SRDs[setIndex];
                // setResources.resize(reflSet.binding_count);
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
    // for (const auto& kv : sgInfo.sprivCode)
    // {
    //     stagesStr += RHIShaderStageToString(kv.first) + " ";
    // }
    LOGI("Shader Stages: {}", stagesStr);
    LOGI("PushConstant: name={} size={}", sgInfo.pushConstants.name, sgInfo.pushConstants.size);
    LOGI("SRD Set Count={}", sgInfo.SRDTable.size());
    for (const auto& setSRD : sgInfo.SRDTable)
    {
        for (const auto& srd : setSRD)
        {
            LOGI("SRD stage={} name={} set={} binding={} arraySize={}",
                 RHIShaderStageFlagToString(srd.stageFlags), srd.name, srd.set, srd.binding,
                 srd.arraySize);
        }
    }
    for (const auto& va : sgInfo.vertexInputAttributes)
    {
        LOGI("Vertex Input Attr name={} binding={} location={} offset={}", va.name, va.binding,
             va.location, va.offset);
    }
    LOGI("Vertex Binding Stride={}", sgInfo.vertexBindingStride);
    LOGI("======= End Printing RHIShaderGroupInfo =======")
}
} // namespace zen