#pragma once
#include <vector>

namespace zen::rhi
{
template <typename E> constexpr std::underlying_type_t<E> ToUnderlying(E e) noexcept
{
    return static_cast<std::underlying_type_t<E>>(e);
}

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
        float    floatValue;
        bool     boolValue;
    };
    ShaderSpecializationConstantType type{ShaderSpecializationConstantType::eMax};
    uint32_t                         constantId{0};
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

struct ShaderResource
{
    ShaderResourceType type{ShaderResourceType::eMax};
    ShaderStage        stage{ShaderStage::eMax};
    // Size of arrays (in total elements), or ubos (in bytes * total elements).
    uint32_t length{0};
    uint32_t binding{0};
};

// .vert .frag .compute together
struct ShaderGroupInfo
{
    // per set shader resources
    std::vector<std::vector<ShaderResource>> shaderResources;
    // specialization constants
    std::vector<ShaderSpecializationConstant> specializationConstants;
    // shader stages
    std::vector<ShaderStage> shaderStages;
};
} // namespace zen::rhi