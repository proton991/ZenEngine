#pragma once
#include "Common/BitField.h"


#include <vector>

namespace zen::rhi
{
template <typename E> constexpr std::underlying_type_t<E> ToUnderlying(E e) noexcept
{
    return static_cast<std::underlying_type_t<E>>(e);
}

enum DataFormat : uint32_t
{
    DATA_FORMAT_UNDEFINED           = 0,   // = VK_FORMAT_UNDEFINED
    DATA_FORMAT_R16_UINT            = 74,  // = VK_FORMAT_R16_UINT
    DATA_FORMAT_R16_SINT            = 75,  // = VK_FORMAT_R16_SINT
    DATA_FORMAT_R16_SFLOAT          = 76,  // = VK_FORMAT_R16_SFLOAT
    DATA_FORMAT_R16G16_UINT         = 81,  // = VK_FORMAT_R16G16_UINT
    DATA_FORMAT_R16G16_SINT         = 82,  // = VK_FORMAT_R16G16_SINT
    DATA_FORMAT_R16G16_SFLOAT       = 83,  // = VK_FORMAT_R16G16_SFLOAT
    DATA_FORMAT_R16G16B16_UINT      = 88,  // = VK_FORMAT_R16G16B16_UINT
    DATA_FORMAT_R16G16B16_SINT      = 89,  // = VK_FORMAT_R16G16B16_SINT
    DATA_FORMAT_R16G16B16_SFLOAT    = 90,  // = VK_FORMAT_R16G16B16_SFLOAT
    DATA_FORMAT_R16G16B16A16_UINT   = 95,  // = VK_FORMAT_R16G16B16A16_UINT
    DATA_FORMAT_R16G16B16A16_SINT   = 96,  // = VK_FORMAT_R16G16B16A16_SINT
    DATA_FORMAT_R16G16B16A16_SFLOAT = 97,  // = VK_FORMAT_R16G16B16A16_SFLOAT
    DATA_FORMAT_R32_UINT            = 98,  // = VK_FORMAT_R32_UINT
    DATA_FORMAT_R32_SINT            = 99,  // = VK_FORMAT_R32_SINT
    DATA_FORMAT_R32_SFLOAT          = 100, // = VK_FORMAT_R32_SFLOAT
    DATA_FORMAT_R32G32_UINT         = 101, // = VK_FORMAT_R32G32_UINT
    DATA_FORMAT_R32G32_SINT         = 102, // = VK_FORMAT_R32G32_SINT
    DATA_FORMAT_R32G32_SFLOAT       = 103, // = VK_FORMAT_R32G32_SFLOAT
    DATA_FORMAT_R32G32B32_UINT      = 104, // = VK_FORMAT_R32G32B32_UINT
    DATA_FORMAT_R32G32B32_SINT      = 105, // = VK_FORMAT_R32G32B32_SINT
    DATA_FORMAT_R32G32B32_SFLOAT    = 106, // = VK_FORMAT_R32G32B32_SFLOAT
    DATA_FORMAT_R32G32B32A32_UINT   = 107, // = VK_FORMAT_R32G32B32A32_UINT
    DATA_FORMAT_R32G32B32A32_SINT   = 108, // = VK_FORMAT_R32G32B32A32_SINT
    DATA_FORMAT_R32G32B32A32_SFLOAT = 109, // = VK_FORMAT_R32G32B32A32_SFLOAT
    DATA_FORMAT_R64_UINT            = 110, // = VK_FORMAT_R64_UINT
    DATA_FORMAT_R64_SINT            = 111, // = VK_FORMAT_R64_SINT
    DATA_FORMAT_R64_SFLOAT          = 112, // = VK_FORMAT_R64_SFLOAT
    DATA_FORMAT_R64G64_UINT         = 113, // = VK_FORMAT_R64G64_UINT
    DATA_FORMAT_R64G64_SINT         = 114, // = VK_FORMAT_R64G64_SINT
    DATA_FORMAT_R64G64_SFLOAT       = 115, // = VK_FORMAT_R64G64_SFLOAT
    DATA_FORMAT_R64G64B64_UINT      = 116, // = VK_FORMAT_R64G64B64_UINT
    DATA_FORMAT_R64G64B64_SINT      = 117, // = VK_FORMAT_R64G64B64_SINT
    DATA_FORMAT_R64G64B64_SFLOAT    = 118, // = VK_FORMAT_R64G64B64_SFLOAT
    DATA_FORMAT_R64G64B64A64_UINT   = 119, // = VK_FORMAT_R64G64B64A64_UINT
    DATA_FORMAT_R64G64B64A64_SINT   = 120, // = VK_FORMAT_R64G64B64A64_SINT
    DATA_FORMAT_R64G64B64A64_SFLOAT = 121, // = VK_FORMAT_R64G64B64A64_SFLOAT
};

/*******************/
/**** Shaders ****/
/*******************/
enum class ShaderLanguage : uint32_t
{
    eGLSL = 0,
    eMax  = 1
};
enum class ShaderStage : uint32_t
{
    eVertex                = 0,
    eFragment              = 1,
    eTesselationConrol     = 2,
    eTesselationEvaluation = 3,
    eCompute               = 4,
    eMax                   = 5
};

enum class ShaderStageFlagBits : uint32_t
{
    eVertex                = 1 << 0,
    eFragment              = 1 << 1,
    eTesselationConrol     = 1 << 2,
    eTesselationEvaluation = 1 << 3,
    eCompute               = 1 << 4,
    eMax                   = 1 << 5
};

static std::string ShaderStageToString(ShaderStage stage)
{
    static const char* SHADER_STAGE_NAMES[ToUnderlying(ShaderStage::eMax)] = {
        "Vertex", "Fragment", "TesselationControl", "TesselationEvaluation", "Compute",
    };
    return SHADER_STAGE_NAMES[ToUnderlying(stage)];
}

static std::string ShaderStageFlagToString(BitField<ShaderStageFlagBits> stageFlags)
{
    std::string str;
    if (stageFlags.HasFlag(ShaderStageFlagBits::eVertex)) { str += "Vertex "; }
    if (stageFlags.HasFlag(ShaderStageFlagBits::eFragment)) { str += "Fragment "; }
    if (stageFlags.HasFlag(ShaderStageFlagBits::eTesselationConrol))
    {
        str += "TesselationControl ";
    }
    if (stageFlags.HasFlag(ShaderStageFlagBits::eTesselationEvaluation))
    {
        str += "TesselationEvaluation ";
    }
    if (stageFlags.HasFlag(ShaderStageFlagBits::eCompute)) { str += "Compute "; }
    str.pop_back();
    return str;
}

static ShaderStageFlagBits ShaderStageToFlagBits(ShaderStage stage)
{
    return static_cast<ShaderStageFlagBits>(1 << ToUnderlying(stage));
}

enum class ShaderSpecializationConstantType : uint32_t
{
    eBool  = 0,
    eInt   = 1,
    eFloat = 2,
    eMax   = 3
};

struct ShaderSpecializationConstant
{
    union
    {
        uint32_t intValue = 0;
        float floatValue;
        bool boolValue;
    };
    ShaderSpecializationConstantType type{ShaderSpecializationConstantType::eMax};
    uint32_t constantId{0};
    BitField<ShaderStageFlagBits> stages;
};

enum class ShaderResourceType : uint32_t
{
    // For sampling only (sampler GLSL type).
    eSampler = 0,
    // Only texture, (textureXX GLSL type).
    eTexture = 1,
    // For sampling only, but includes a texture, (samplerXX GLSL type), first a sampler then a texture.
    eSamplerWithTexture = 2,
    // Storage image (imageXX GLSL type), for compute mostly.
    eImage = 3,
    // Buffer texture (or TBO, textureBuffer type).
    eTextureBuffer = 4,
    // Buffer texture with a sampler(or TBO, samplerBuffer type).
    eSamplerWithTextureBuffer = 5,
    // Texel buffer, (imageBuffer type), for compute mostly.
    eImageBuffer = 6,
    // Regular uniform buffer (or UBO).
    eUniformBuffer = 7,
    // Storage buffer ("buffer" qualifier) like UBO, but supports storage, for compute mostly.
    eStorageBuffer = 8,
    // Used for sub-pass read/write, for mobile mostly.
    eInputAttachment = 9,
    eMax             = 10
};

struct ShaderResourceDescriptor
{
    std::string name;
    ShaderResourceType type{ShaderResourceType::eMax};
    BitField<ShaderStageFlagBits> stageFlags;
    bool writable{false};
    // Size of arrays (in total elements), or ubos (in bytes * total elements).
    uint32_t arraySize{1};
    uint32_t blockSize{0};
    uint32_t set{0};
    uint32_t binding{0};
};

struct VertexInputAttribute
{
    std::string name;
    uint32_t location{0};
    uint32_t binding{0};
    uint32_t offset{0};
    DataFormat format{0};
};

struct ShaderPushConstants
{
    std::string name;
    uint32_t size{0};
    BitField<ShaderStageFlagBits> stageFlags;
};

// .vert .frag .compute together
struct ShaderGroupInfo
{
    // TODO: store spirv code using some cache strategy, no-copy
    std::vector<std::vector<uint8_t>> sprivCode;
    ShaderPushConstants pushConstants{};
    // vertex input attribute
    std::vector<VertexInputAttribute> vertexInputAttributes;
    // vertex binding stride
    uint32_t vertexBindingStride{0};
    // per set shader resources
    std::vector<std::vector<ShaderResourceDescriptor>> SRDs;
    // specialization constants
    std::vector<ShaderSpecializationConstant> specializationConstants;
    // shader stages
    std::vector<ShaderStage> shaderStages;
};
} // namespace zen::rhi