#pragma once
#include "RHIDefs.h"
#include "Common/BitField.h"
#include "Common/HashMap.h"
#include "Common/Math.h"
#include <string>
#include <vector>

#define MAX_COLOR_ATTACHMENT_COUNT 8
#define MAX_SUBPASS_COUNT          8

#define ZEN_BUFFER_WHOLE_SIZE (~0ULL)

#define ALLOCA(m_size)                (assert((m_size) != 0), alloca(m_size))
#define ALLOCA_ARRAY(m_type, m_count) ((m_type*)ALLOCA(sizeof(m_type) * (m_count)))
#define ALLOCA_SINGLE(m_type)         ALLOCA_ARRAY(m_type, 1)

namespace zen::rhi
{
enum class GraphicsAPIType
{
    eVulkan = 0,
    eMax    = 1
};

struct GPUInfo
{
    bool supportGeometryShader{false};
    size_t uniformBufferAlignment{0};
    size_t storageBufferAlignment{0};
};

template <typename E> constexpr std::underlying_type_t<E> ToUnderlying(E e) noexcept
{
    return static_cast<std::underlying_type_t<E>>(e);
}

struct Rect3f
{
    float minX, maxX;
    float minY, maxY;
    float minZ, maxZ;

    Rect3f() : minX(0.f), maxX(0.f), minY(0.f), maxY(0.f), minZ(0.f), maxZ(1.f) {}

    Rect3f(float width, float height) :
        minX(0.f), maxX(width), minY(0.f), maxY(height), minZ(0.f), maxZ(1.f)
    {}

    Rect3f(float _minX, float _maxX, float _minY, float _maxY, float _minZ, float _maxZ) :
        minX(_minX), maxX(_maxX), minY(_minY), maxY(_maxY), minZ(_minZ), maxZ(_maxZ)
    {}

    bool operator==(const Rect3f& b) const
    {
        return minX == b.minX && minY == b.minY && minZ == b.minZ && maxX == b.maxX &&
            maxY == b.maxY && maxZ == b.maxZ;
    }

    bool operator!=(const Rect3f& b) const
    {
        return !(*this == b);
    }

    [[nodiscard]] float Width() const
    {
        return maxX - minX;
    }

    [[nodiscard]] float Height() const
    {
        return maxY - minY;
    }
};

template <class T = int> struct Rect2
{
    T minX, maxX;
    T minY, maxY;

    Rect2() : minX(0), maxX(0), minY(0), maxY(0) {}

    Rect2(T width, T height) : minX(0), maxX(width), minY(0), maxY(height) {}

    Rect2(T _minX, T _maxX, T _minY, T _maxY) : minX(_minX), maxX(_maxX), minY(_minY), maxY(_maxY)
    {}

    bool operator==(const Rect2& b) const
    {
        return minX == b.minX && minY == b.minY && maxX == b.maxX && maxY == b.maxY;
    }

    bool operator!=(const Rect2& b) const
    {
        return !(*this == b);
    }

    [[nodiscard]] T Width() const
    {
        return maxX - minX;
    }

    [[nodiscard]] T Height() const
    {
        return maxY - minY;
    }
};

struct Color
{
    float r, g, b, a;

    Color() : r(0.f), g(0.f), b(0.f), a(1.f) {}

    explicit Color(float c) : r(c), g(c), b(c), a(c) {}

    Color(float _r, float _g, float _b, float _a) : r(_r), g(_g), b(_b), a(_a) {}

    bool operator==(const Color& _b) const
    {
        return r == _b.r && g == _b.g && b == _b.b && a == _b.a;
    }

    bool operator!=(const Color& _b) const
    {
        return !(*this == _b);
    }
};

enum class DataFormat : uint32_t
{
    eUndefined          = 0,   // = VK_FORMAT_UNDEFINED
    eR8UNORM            = 9,   // VK_FORMAT_R8_UNORM
    eR8UInt             = 13,  // VK_FORMAT_R8_UINT
    eR8G8B8SRGB         = 29,  // VK_FORMAT_R8G8B8_SRGB
    eR8G8B8UNORM        = 30,  // VK_FORMAT_B8G8R8_UNORM
    eR8G8B8A8UInt       = 41,  // VK_FORMAT_R8G8B8A8_UINT
    eR8G8B8A8SRGB       = 43,  // VK_FORMAT_R8G8B8A8_SRGB
    eR8G8B8A8UNORM      = 37,  // VK_FORMAT_R8G8B8A8_UNORM,
    eR16UInt            = 74,  // = VK_FORMAT_R16_UINT
    eR16SInt            = 75,  // = VK_FORMAT_R16_SINT
    eR16SFloat          = 76,  // = VK_FORMAT_R16_SFLOAT
    eR16G16UInt         = 81,  // = VK_FORMAT_R16G16_UINT
    eR16G16SInt         = 82,  // = VK_FORMAT_R16G16_SINT
    eR16G16SFloat       = 83,  // = VK_FORMAT_R16G16_SFLOAT
    eR16G16B16UInt      = 88,  // = VK_FORMAT_R16G16B16_UINT
    eR16G16B16SInt      = 89,  // = VK_FORMAT_R16G16B16_SINT
    eR16G16B16SFloat    = 90,  // = VK_FORMAT_R16G16B16_SFLOAT
    eR16G16B16A16UInt   = 95,  // = VK_FORMAT_R16G16B16A16_UINT
    eR16G16B16A16SInt   = 96,  // = VK_FORMAT_R16G16B16A16_SINT
    eR16G16B16A16SFloat = 97,  // = VK_FORMAT_R16G16B16A16_SFLOAT
    eR32UInt            = 98,  // = VK_FORMAT_R32_UINT
    eR32SInt            = 99,  // = VK_FORMAT_R32_SINT
    eR32SFloat          = 100, // = VK_FORMAT_R32_SFLOAT
    eR32G32UInt         = 101, // = VK_FORMAT_R32G32_UINT
    eR32G32SInt         = 102, // = VK_FORMAT_R32G32_SINT
    eR32G32SFloat       = 103, // = VK_FORMAT_R32G32_SFLOAT
    eR32G32B32UInt      = 104, // = VK_FORMAT_R32G32B32_UINT
    eR32G32B32SInt      = 105, // = VK_FORMAT_R32G32B32_SINT
    eR32G32B32SFloat    = 106, // = VK_FORMAT_R32G32B32_SFLOAT
    eR32G32B32A32UInt   = 107, // = VK_FORMAT_R32G32B32A32_UINT
    eR32G32B32A32SInt   = 108, // = VK_FORMAT_R32G32B32A32_SINT
    eR32G32B32A32SFloat = 109, // = VK_FORMAT_R32G32B32A32_SFLOAT
    eR64UInt            = 110, // = VK_FORMAT_R64_UINT
    eR64SInt            = 111, // = VK_FORMAT_R64_SINT
    eR64SFloat          = 112, // = VK_FORMAT_R64_SFLOAT
    eR64G64UInt         = 113, // = VK_FORMAT_R64G64_UINT
    eR64G64SInt         = 114, // = VK_FORMAT_R64G64_SINT
    eR64G64SFloat       = 115, // = VK_FORMAT_R64G64_SFLOAT
    eR64G64B64UInt      = 116, // = VK_FORMAT_R64G64B64_UINT
    eR64G64B64SInt      = 117, // = VK_FORMAT_R64G64B64_SINT
    eR64G64B64SFloat    = 118, // = VK_FORMAT_R64G64B64_SFLOAT
    eR64G64B64A64UInt   = 119, // = VK_FORMAT_R64G64B64A64_UINT
    eR64G64B64A64SInt   = 120, // = VK_FORMAT_R64G64B64A64_SINT
    eR64G64B64A64SFloat = 121, // = VK_FORMAT_R64G64B64A64_SFLOAT

    eD16UNORM        = 124, // VK_FORMAT_D16_UNORM
    eD32SFloat       = 126, // VK_FORMAT_D32_SFLOAT
    eS8UInt          = 127, // VK_FORMAT_S8_UINT
    eD16UNORMS8UInt  = 128, // VK_FORMAT_D16_UNORM_S8_UINT
    eD24UNORMS8UInt  = 129, //VK_FORMAT_D24_UNORM_S8_UINT
    eD32SFloatS8UInt = 130, //VK_FORMAT_D32_SFLOAT_S8_UINT
};

inline uint32_t GetTextureFormatPixelSize(DataFormat format)
{
    switch (format)
    {
        case DataFormat::eR16UInt:
        case DataFormat::eR16SInt:
        case DataFormat::eR16SFloat: return 2;

        case DataFormat::eR16G16UInt:
        case DataFormat::eR16G16SInt:
        case DataFormat::eR16G16SFloat:
        case DataFormat::eR32UInt:
        case DataFormat::eR32SInt:
        case DataFormat::eR32SFloat: return 4;

        case DataFormat::eR16G16B16UInt:
        case DataFormat::eR16G16B16SInt:
        case DataFormat::eR16G16B16SFloat: return 6;

        case DataFormat::eR16G16B16A16UInt:
        case DataFormat::eR16G16B16A16SInt:
        case DataFormat::eR16G16B16A16SFloat:
        case DataFormat::eR32G32UInt:
        case DataFormat::eR32G32SInt:
        case DataFormat::eR32G32SFloat:
        case DataFormat::eR64UInt:
        case DataFormat::eR64SInt:
        case DataFormat::eR64SFloat: return 8;

        case DataFormat::eR32G32B32UInt:
        case DataFormat::eR32G32B32SInt:
        case DataFormat::eR32G32B32SFloat: return 12;

        case DataFormat::eR32G32B32A32UInt:
        case DataFormat::eR32G32B32A32SInt:
        case DataFormat::eR32G32B32A32SFloat:
        case DataFormat::eR64G64UInt:
        case DataFormat::eR64G64SInt:
        case DataFormat::eR64G64SFloat: return 16;

        case DataFormat::eR64G64B64UInt:
        case DataFormat::eR64G64B64SInt:
        case DataFormat::eR64G64B64SFloat: return 24;

        case DataFormat::eR64G64B64A64UInt:
        case DataFormat::eR64G64B64A64SInt:
        case DataFormat::eR64G64B64A64SFloat: return 32;

        default: return 0x7fffffff;
    }
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
    eTesselationControl    = 1,
    eTesselationEvaluation = 2,
    eGeometry              = 3,
    eFragment              = 4,
    eCompute               = 5,
    eMax                   = 6
};

enum class ShaderStageFlagBits : uint32_t
{
    eVertex                = 1 << 0,
    eTesselationControl    = 1 << 1,
    eTesselationEvaluation = 1 << 2,
    eGeometry              = 1 << 3,
    eFragment              = 1 << 4,
    eCompute               = 1 << 5,
    eMax                   = 1 << 6
};

static std::string ShaderStageToString(ShaderStage stage)
{
    static const char* SHADER_STAGE_NAMES[ToUnderlying(ShaderStage::eMax)] = {
        "Vertex", "Fragment", "TesselationControl", "TesselationEvaluation", "Geometry", "Compute",
    };
    return SHADER_STAGE_NAMES[ToUnderlying(stage)];
}

static std::string ShaderStageFlagToString(BitField<ShaderStageFlagBits> stageFlags)
{
    std::string str;
    if (stageFlags.HasFlag(ShaderStageFlagBits::eVertex))
    {
        str += "Vertex ";
    }
    if (stageFlags.HasFlag(ShaderStageFlagBits::eFragment))
    {
        str += "Fragment ";
    }
    if (stageFlags.HasFlag(ShaderStageFlagBits::eTesselationControl))
    {
        str += "TesselationControl ";
    }
    if (stageFlags.HasFlag(ShaderStageFlagBits::eTesselationEvaluation))
    {
        str += "TesselationEvaluation ";
    }
    if (stageFlags.HasFlag(ShaderStageFlagBits::eCompute))
    {
        str += "Compute ";
    }
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

#define ADD_SHADER_BINDING_SINGLE(bindings_, index_, type_, ...)                           \
    {                                                                                      \
        ShaderResourceBinding binding{};                                                   \
        binding.binding = index_;                                                          \
        binding.type    = type_;                                                           \
        {                                                                                  \
            std::initializer_list<Handle> handles = {__VA_ARGS__};                         \
            binding.handles.insert(binding.handles.end(), handles.begin(), handles.end()); \
        }                                                                                  \
        bindings_.emplace_back(std::move(binding));                                        \
    }

#define ADD_SHADER_BINDING_TEXTURE_ARRAY(bindings_, index_, type_, sampler_, textures_) \
    {                                                                                   \
        ShaderResourceBinding binding{};                                                \
        binding.binding = index_;                                                       \
        binding.type    = type_;                                                        \
        for (const TextureHandle& textureHandle : textures_)                            \
        {                                                                               \
            binding.handles.push_back(sampler_);                                        \
            binding.handles.push_back(textureHandle);                                   \
        }                                                                               \
        bindings_.emplace_back(std::move(binding));                                     \
    }

struct ShaderResourceBinding
{
    ShaderResourceType type{ShaderResourceType::eMax};
    uint32_t binding{0};
    std::vector<Handle> handles;
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
    HashMap<ShaderStage, std::vector<uint8_t>> sprivCode;
    ShaderPushConstants pushConstants{};
    // vertex input attribute
    std::vector<VertexInputAttribute> vertexInputAttributes;
    // vertex binding stride
    uint32_t vertexBindingStride{0};
    // per set shader resources
    std::vector<std::vector<ShaderResourceDescriptor>> SRDs;
    // specialization constants
    std::vector<ShaderSpecializationConstant> specializationConstants;
    std::string name;
};

/*****************************/
/**** Gfx Pipeline States ****/
/*****************************/
enum class DrawPrimitiveType : uint32_t
{
    ePointList                  = 0,
    eLineList                   = 1,
    eLineStrip                  = 2,
    eTriangleList               = 3,
    eTriangleStrip              = 4,
    eTriangleFan                = 5,
    eLineListWithAdjacency      = 6,
    eLineStripWithAdjacency     = 7,
    eTriangleListWithAdjacency  = 8,
    eTriangleStripWithAdjacency = 9,
    ePatchList                  = 10,
    eMax                        = 11
};

enum class PolygonCullMode : uint32_t
{
    eDisabled     = 0,
    eFront        = 1,
    eBack         = 2,
    eFrontAndBack = 3,
    eMax          = 4
};

enum class PolygonFrontFace : uint32_t
{
    eCounterClockWise = 0,
    eClockWise        = 1,
    eMax              = 2,
};

enum class SampleCount : uint32_t
{
    e1   = 0,
    e2   = 1,
    e4   = 2,
    e8   = 3,
    e16  = 4,
    e32  = 5,
    e64  = 6,
    eMax = 7
};

enum class CompareOperator : uint32_t
{
    eNever          = 0,
    eLess           = 1,
    eEqual          = 2,
    eLessOrEqual    = 3,
    eGreater        = 4,
    eNotEqual       = 5,
    eGreaterOrEqual = 6,
    eAlways         = 7,
    eMax            = 8
};

enum class LogicOperation : uint32_t
{
    eClear        = 0,
    eAnd          = 1,
    eAndReverse   = 2,
    eCopy         = 3,
    eAndInverted  = 4,
    eNoOp         = 5,
    eXor          = 6,
    eOr           = 7,
    eNor          = 8,
    eEquivalent   = 9,
    eInvert       = 10,
    eOrReverse    = 11,
    eCopyInverted = 12,
    eOrInverted   = 13,
    eNand         = 14,
    eSet          = 15,
    eMax          = 16
};

struct GfxPipelineRasterizationState
{
    bool enableDepthClamp{false};
    bool discardPrimitives{false};
    bool wireframe{false};
    PolygonCullMode cullMode{PolygonCullMode::eDisabled};
    PolygonFrontFace frontFace{PolygonFrontFace::eCounterClockWise};
    bool enableDepthBias{false};
    float depthBiasConstantFactor{0.0f};
    float depthBiasClamp{0.0f};
    float depthBiasSlopeFactor{0.0f};
    float lineWidth{1.0f};
};

struct GfxPipelineMultiSampleState
{
    SampleCount sampleCount{SampleCount::e1};
    bool enableSampleShading{false};
    float minSampleShading{0.0f};
    bool enableAlphaToCoverage{false};
    bool enbaleAlphaToOne{false};
    std::vector<uint32_t> sampleMasks;
};

enum class StencilOperation : uint32_t
{
    eKeep              = 0,
    eZero              = 1,
    eReplace           = 2,
    eIncrementAndClamp = 3,
    eDecrementAndClamp = 4,
    eInvert            = 5,
    eIncrementAndWrap  = 6,
    eDecrementAndWrap  = 7,
    eMax               = 8
};

struct StencilOperationState
{
    StencilOperation fail{StencilOperation::eReplace};
    StencilOperation pass{StencilOperation::eReplace};
    StencilOperation depthFail{StencilOperation::eReplace};
    CompareOperator compare{CompareOperator::eAlways};
    uint32_t compareMask{0};
    uint32_t writeMask{0};
    uint32_t reference{0};
};


struct GfxPipelineDepthStencilState
{
    bool enableDepthTest{false};
    bool enableDepthWrite{false};
    CompareOperator depthCompareOp{CompareOperator::eNever};
    bool enableDepthBoundsTest{false};
    bool enableStencilTest{false};
    StencilOperationState frontOp{};
    StencilOperationState backOp{};
    float minDepthBounds{0.0f};
    float maxDepthBounds{1.0f};

    static GfxPipelineDepthStencilState Create(bool enableDepthTest,
                                               bool enableDepthWrite,
                                               CompareOperator depthCompareOp)
    {
        GfxPipelineDepthStencilState state{};
        state.enableDepthTest  = enableDepthTest;
        state.enableDepthWrite = enableDepthWrite;
        state.depthCompareOp   = depthCompareOp;
        return state;
    }
};

enum class BlendFactor : uint32_t
{
    eZero                  = 0,
    eOne                   = 1,
    eSrcColor              = 2,
    eOneMinusSrcColor      = 3,
    eDstColor              = 4,
    eOneMinusDstColor      = 5,
    eSrcAlpha              = 6,
    eOneMinusSrcAlpha      = 7,
    eDstAlpha              = 8,
    eOneMinusDstAlpha      = 9,
    eConstantColor         = 10,
    eOneMinusConstantColor = 11,
    eConstantAlpha         = 12,
    eOneMinusConstantAlpha = 13,
    eSrcAlphaSatuate       = 14,
    eSrc1Color             = 15,
    eOneMinusSrc1Color     = 16,
    eSrc1Alpha             = 17,
    eOneMinusSrc1Alpha     = 18,
    eMax                   = 19
};

enum class BlendOperation : uint32_t
{
    eAdd              = 0,
    eSubstract        = 1,
    eReverseSubstract = 2,
    eMinimum          = 3,
    eMaximum          = 4,
    eMax              = 5
};

struct GfxPipelineColorBlendState
{
    bool enableLogicOp{false};
    LogicOperation logicOp{LogicOperation::eClear};

    struct Attachment
    {
        bool enableBlend{false};
        BlendFactor srcColorBlendFactor{BlendFactor::eZero};
        BlendFactor dstColorBlendFactor{BlendFactor::eZero};
        BlendOperation colorBlendOp{BlendOperation::eAdd};
        BlendFactor srcAlphaBlendFactor{BlendFactor::eZero};
        BlendFactor dstAlphaBlendFactor{BlendFactor::eZero};
        BlendOperation alphaBlendOp{BlendOperation::eAdd};
        bool writeR{true};
        bool writeG{true};
        bool writeB{true};
        bool writeA{true};
    };

    static GfxPipelineColorBlendState CreateColorWriteDisabled(int count = 1)
    {
        GfxPipelineColorBlendState bs;
        for (int i = 0; i < count; i++)
        {
            Attachment attachment{};
            attachment.writeR = false;
            attachment.writeG = false;
            attachment.writeB = false;
            attachment.writeA = false;
            bs.attachments.emplace_back(attachment);
        }
        return bs;
    }

    static GfxPipelineColorBlendState CreateDisabled(int count = 1)
    {
        GfxPipelineColorBlendState bs;
        for (int i = 0; i < count; i++)
        {
            bs.attachments.emplace_back();
        }
        return bs;
    }

    static GfxPipelineColorBlendState CreateBlend(int count = 1)
    {
        GfxPipelineColorBlendState bs;
        for (int i = 0; i < count; i++)
        {
            Attachment ba;
            ba.enableBlend         = true;
            ba.srcColorBlendFactor = BlendFactor::eSrcAlpha;
            ba.dstColorBlendFactor = BlendFactor::eOneMinusSrcAlpha;
            ba.srcAlphaBlendFactor = BlendFactor::eSrcAlpha;
            ba.dstAlphaBlendFactor = BlendFactor::eOneMinusSrcAlpha;

            bs.attachments.emplace_back(ba);
        }
        return bs;
    }

    std::vector<Attachment> attachments; // One per render target texture.
    Color blendConstants;
};

enum class DynamicState : uint32_t
{
    eViewPort  = 0,
    eScissor   = 1,
    eLineWidth = 2,
    eDepthBias = 3,
    eMax       = 4
};

struct GfxPipelineStates
{
    DrawPrimitiveType primitiveType{DrawPrimitiveType::eTriangleList};
    GfxPipelineRasterizationState rasterizationState;
    GfxPipelineMultiSampleState multiSampleState;
    GfxPipelineDepthStencilState depthStencilState;
    GfxPipelineColorBlendState colorBlendState;
    std::vector<DynamicState> dynamicStates;
};

/*****************************/
/********* Buffers ***********/
/*****************************/
enum class BufferUsageFlagBits : uint32_t
{
    eTransferSrcBuffer = 1 << 0,
    eTransferDstBuffer = 1 << 1,
    eTextureBuffer     = 1 << 2,
    eImageBuffer       = 1 << 3,
    eUniformBuffer     = 1 << 4,
    eStorageBuffer     = 1 << 5,
    eIndexBuffer       = 1 << 6,
    eVertexBuffer      = 1 << 7,
    eIndirectBuffer    = 1 << 8,
    eMax               = 0x7FFFFFFF
};

enum class BufferUsage : uint32_t
{
    eNone           = 0,
    eTransferSrc    = 1,
    eTransferDst    = 2,
    eTextureBuffer  = 3,
    eImageBuffer    = 4,
    eUniformBuffer  = 5,
    eStorageBuffer  = 6,
    eIndexBuffer    = 7,
    eVertexBuffer   = 8,
    eIndirectBuffer = 9,
    eMax            = 10
};

struct BufferCopyRegion
{
    uint64_t srcOffset{0};
    uint64_t dstOffset{0};
    uint64_t size{0};
};

struct BufferCopySource
{
    BufferHandle buffer;
    BufferCopyRegion region;
};

enum class BufferAllocateType
{
    eCPU = 0,
    eGPU = 1,
    eMax = 2
};

/*****************************/
/********** Sampler **********/
/*****************************/
enum class SamplerFilter : uint32_t
{
    eNearest = 0,
    eLinear  = 1,
};

enum class SamplerRepeatMode : uint32_t
{
    eRepeat            = 0,
    eMirroredRepeat    = 1,
    eClampToEdge       = 2,
    eClampToBorder     = 3,
    eMirrorClampToEdge = 4,
    eMax               = 0x7FFFFFFF
};

enum class SamplerBorderColor : uint32_t
{
    eFloatTransparentBlack = 0,
    eIntTransparentBlack   = 1,
    eFloatOpaqueBlack      = 2,
    eIntOpaqueBlack        = 3,
    eFloatOpaqueWhite      = 4,
    eIntOpaqueWhite        = 5,
    eMax                   = 0x7FFFFFFF
};

/*****************************/
/********* Textures **********/
/*****************************/
enum class TextureUsageFlagBits : uint32_t
{
    eTransferSrc            = 1 << 0,
    eTransferDst            = 1 << 1,
    eSampled                = 1 << 2,
    eStorage                = 1 << 3,
    eColorAttachment        = 1 << 4,
    eDepthStencilAttachment = 1 << 5,
    eTransientAttachment    = 1 << 6,
    eInputAttachment        = 1 << 7,
    eMax                    = 0x7FFFFFFF
};

enum class TextureUsage : uint32_t
{
    eNone                   = 0,
    eTransferSrc            = 1,
    eTransferDst            = 2,
    eSampled                = 3,
    eStorage                = 4,
    eColorAttachment        = 5,
    eDepthStencilAttachment = 6,
    eTransientAttachment    = 7,
    eInputAttachment        = 8,
    eMax                    = 9
};

enum class TextureAspectFlagBits : uint32_t
{
    eColor   = 1 << 0,
    eDepth   = 1 << 1,
    eStencil = 1 << 2,
    eMax     = 0x7FFFFFFF
};

enum class TextureLayout : uint32_t
{
    eUndefined,
    eGeneral,
    eColorTarget,
    eDepthStencilTarget,
    eShaderReadOnly,
    eTransferSrc,
    eTransferDst,
    eMax
};

enum class TextureType : uint32_t
{
    e1D   = 0,
    e2D   = 1,
    e3D   = 2,
    eCube = 3,
    eMax  = 4
};

struct TextureSubResourceRange
{
    BitField<TextureAspectFlagBits> aspect;
    uint32_t baseMipLevel{0};
    uint32_t levelCount{0};
    uint32_t baseArrayLayer{0};
    uint32_t layerCount{0};

    static TextureSubResourceRange Color(uint32_t baseMiplevel   = 0,
                                         uint32_t baseArrayLayer = 0,
                                         uint32_t levelCount     = 1,
                                         uint32_t layerCount     = 1)
    {
        TextureSubResourceRange range{};
        range.aspect.SetFlag(TextureAspectFlagBits::eColor);
        range.layerCount     = layerCount;
        range.levelCount     = levelCount;
        range.baseMipLevel   = baseMiplevel;
        range.baseArrayLayer = baseArrayLayer;

        return range;
    }

    static TextureSubResourceRange Depth(uint32_t baseMipLevel   = 0,
                                         uint32_t baseArrayLayer = 0,
                                         uint32_t levelCount     = 1,
                                         uint32_t layerCount     = 1)
    {
        TextureSubResourceRange range{};
        range.aspect.SetFlag(TextureAspectFlagBits::eDepth);
        range.layerCount     = layerCount;
        range.levelCount     = levelCount;
        range.baseMipLevel   = baseMipLevel;
        range.baseArrayLayer = baseArrayLayer;

        return range;
    }

    static TextureSubResourceRange Stencil(uint32_t baseMipLevel   = 0,
                                           uint32_t baseArrayLayer = 0,
                                           uint32_t levelCount     = 1,
                                           uint32_t layerCount     = 1)
    {
        TextureSubResourceRange range{};
        range.aspect.SetFlag(TextureAspectFlagBits::eStencil);
        range.layerCount     = layerCount;
        range.levelCount     = levelCount;
        range.baseMipLevel   = baseMipLevel;
        range.baseArrayLayer = baseArrayLayer;

        return range;
    }

    static TextureSubResourceRange DepthStencil(uint32_t baseMipLevel   = 0,
                                                uint32_t baseArrayLayer = 0,
                                                uint32_t levelCount     = 1,
                                                uint32_t layerCount     = 1)
    {
        TextureSubResourceRange range{};
        range.aspect.SetFlag(TextureAspectFlagBits::eDepth);
        range.aspect.SetFlag(TextureAspectFlagBits::eStencil);
        range.layerCount     = layerCount;
        range.levelCount     = levelCount;
        range.baseMipLevel   = baseMipLevel;
        range.baseArrayLayer = baseArrayLayer;

        return range;
    }
};

struct TextureSubresourceLayers
{
    BitField<TextureAspectFlagBits> aspect;
    uint32_t mipmap{0};
    uint32_t baseArrayLayer{0};
    uint32_t layerCount{1};
};

struct TextureCopyRegion
{
    Vec3i srcOffset;
    Vec3i dstOffset;
    Vec3i size;
    TextureSubresourceLayers srcSubresources;
    TextureSubresourceLayers dstSubresources;
};

struct BufferTextureCopyRegion
{
    uint64_t bufferOffset{0};
    TextureSubresourceLayers textureSubresources;
    Vec3i textureOffset;
    Vec3i textureSize;
};

struct BufferTextureCopySource
{
    BufferHandle buffer;
    BufferTextureCopyRegion region;
};

inline TextureLayout TextureUsageToLayout(TextureUsage usage)
{
    switch (usage)
    {
        case TextureUsage::eNone: return TextureLayout::eUndefined;
        case TextureUsage::eTransferSrc: return TextureLayout::eTransferSrc;
        case TextureUsage::eTransferDst: return TextureLayout::eTransferDst;
        case TextureUsage::eSampled:
        case TextureUsage::eInputAttachment: return TextureLayout::eShaderReadOnly;
        case TextureUsage::eStorage: return TextureLayout::eGeneral;
        case TextureUsage::eColorAttachment: return TextureLayout::eColorTarget;
        case TextureUsage::eDepthStencilAttachment: return TextureLayout::eDepthStencilTarget;
        default: return TextureLayout::eUndefined;
    }
}

/*****************************/
/** Renderpass and subpass ***/
/*****************************/
enum class RenderTargetLoadOp : uint32_t
{
    eLoad  = 0,
    eClear = 1,
    eNone  = 2,
    eMax   = 3
};

enum class RenderTargetStoreOp : uint32_t
{
    eStore = 0,
    eNone  = 1,
    eMax   = 2
};

struct RenderTarget
{
    RenderTarget() = default;

    RenderTarget(DataFormat format_, TextureUsage usage_) : format(format_), usage(usage_) {}

    DataFormat format{DataFormat::eUndefined};
    TextureUsage usage{TextureUsage::eMax};
};

union RenderPassClearValue
{
    Color color = {};

    struct
    {
        float depth;
        uint32_t stencil;
    };

    RenderPassClearValue() {}
};

class RenderPassLayout
{
public:
    RenderPassLayout() = default;

    void AddColorRenderTarget(DataFormat format, TextureUsage usage, const TextureHandle& handle)
    {
        m_colorRTs.emplace_back(format, usage);
        m_numColorRT++;
        m_rtHandles.push_back(handle);
    }

    void SetDepthStencilRenderTarget(DataFormat format, const TextureHandle& handle)
    {
        if (!m_hasDepthStencilRT)
        {
            m_depthStencilRT.format = format;
            m_depthStencilRT.usage  = TextureUsage::eDepthStencilAttachment;
            m_hasDepthStencilRT     = true;
            m_rtHandles.push_back(handle);
        }
    }

    void SetColorTargetLoadStoreOp(RenderTargetLoadOp loadOp, RenderTargetStoreOp storeOp)
    {
        m_colorRToadOp   = loadOp;
        m_colorRTStoreOp = storeOp;
    }

    void SetDepthStencilTargetLoadStoreOp(RenderTargetLoadOp loadOp, RenderTargetStoreOp storeOp)
    {
        m_depthStencilRTLoadOp  = loadOp;
        m_depthStencilRTStoreOp = storeOp;
    }

    void SetNumSamples(SampleCount sampleCount)
    {
        m_numSamples = sampleCount;
    }

    const auto GetNumColorRenderTargets() const
    {
        return m_numColorRT;
    }

    const auto GetNumSamples() const
    {
        return m_numSamples;
    }

    const auto HasDepthStencilRenderTarget() const
    {
        return m_hasDepthStencilRT;
    }

    const auto GetColorRenderTargetLoadOp() const
    {
        return m_colorRToadOp;
    }

    const auto GetDepthStencilRenderTargetLoadOp() const
    {
        return m_depthStencilRTLoadOp;
    }

    const auto GetColorRenderTargetStoreOp() const
    {
        return m_colorRTStoreOp;
    }

    const auto GetDepthStencilRenderTargetStoreOp() const
    {
        return m_depthStencilRTStoreOp;
    }

    const auto& GetColorRenderTargets() const
    {
        return m_colorRTs;
    }

    const auto& GetDepthStencilRenderTarget() const
    {
        return m_depthStencilRT;
    }

    const TextureHandle& GetDepthStencilRenderTargetHandle() const
    {
        return m_rtHandles.back();
    }

    const TextureHandle* GetRenderTargetHandles() const
    {
        return m_rtHandles.data();
    }

    TextureHandle* GetRenderTargetHandles()
    {
        return m_rtHandles.data();
    }

    auto GetNumRenderTargets() const
    {
        return static_cast<uint32_t>(m_rtHandles.size());
    }

    void ClearRenderTargetInfo()
    {
        m_rtHandles.clear();
        m_colorRTs.clear();
        m_hasDepthStencilRT = false;
        m_numColorRT        = 0;
    }

private:
    uint32_t m_numColorRT{0};
    std::vector<RenderTarget> m_colorRTs;
    RenderTarget m_depthStencilRT;
    SampleCount m_numSamples{SampleCount::e1};
    RenderTargetLoadOp m_colorRToadOp{RenderTargetLoadOp::eNone};
    RenderTargetStoreOp m_colorRTStoreOp{RenderTargetStoreOp::eNone};
    RenderTargetLoadOp m_depthStencilRTLoadOp{RenderTargetLoadOp::eNone};
    RenderTargetStoreOp m_depthStencilRTStoreOp{RenderTargetStoreOp::eNone};
    std::vector<TextureHandle> m_rtHandles;
    bool m_hasDepthStencilRT{false};
};

struct FramebufferInfo
{
    uint32_t numRenderTarget{0};
    TextureHandle* renderTargets{nullptr};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t depth{1};
};

enum class PipelineType : uint32_t
{
    eNone     = 0,
    eGraphics = 1,
    eCompute  = 2,
    eMax      = 3
};

/**********************************************/
/********** Context for RHICommandList ********/
/*********************************************/
struct VertexBufferBinding
{
    BufferHandle buffer{nullptr};
    uint64_t offset{0};
    uint32_t slot;

    bool operator==(const VertexBufferBinding& b) const
    {
        return buffer == b.buffer && slot == b.slot && offset == b.offset;
    }

    bool operator!=(const VertexBufferBinding& b) const
    {
        return !(*this == b);
    }
};

struct IndexBufferBinding
{
    BufferHandle buffer{nullptr};
    uint64_t offset{0};
    DataFormat format{DataFormat::eUndefined};


    bool operator==(const IndexBufferBinding& b) const
    {
        return buffer == b.buffer && format == b.format && offset == b.offset;
    }

    bool operator!=(const IndexBufferBinding& b) const
    {
        return !(*this == b);
    }
};


/*****************************/
/********* Barriers **********/
/*****************************/
enum class AccessMode
{
    eNone      = 0,
    eRead      = 1,
    eReadWrite = 2
};

enum class AccessFlagBits : uint32_t
{
    eNone                        = 0,
    eIndirectCommandRead         = 1 << 0,
    eIndexRead                   = 1 << 1,
    eVertexAttributeRead         = 1 << 2,
    eUniformRead                 = 1 << 3,
    eInputAttachmentRead         = 1 << 4,
    eShaderRead                  = 1 << 5,
    eShaderWrite                 = 1 << 6,
    eColorAttachmentRead         = 1 << 7,
    eColorAttachmentWrite        = 1 << 8,
    eDepthStencilAttachmentRead  = 1 << 9,
    eDepthStencilAttachmentWrite = 1 << 10,
    eTransferRead                = 1 << 11,
    eTransferWrite               = 1 << 12,
    eHostRead                    = 1 << 13,
    eHostWrite                   = 1 << 14,
    eMemoryRead                  = 1 << 15,
    eMemoryWrite                 = 1 << 16,
    eMax                         = 0x7FFFFFFF
};

enum class PipelineStageBits : uint32_t
{
    eNone                         = 0,
    eTopOfPipe                    = 1 << 0,
    eDrawIndirect                 = 1 << 1,
    eVertexInput                  = 1 << 2,
    eVertexShader                 = 1 << 3,
    eTessellationControlShader    = 1 << 4,
    eTessellationEvaluationShader = 1 << 5,
    eGeometryShader               = 1 << 6,
    eFragmentShader               = 1 << 7,
    eEarlyFragmentTests           = 1 << 8,
    eLateFragmentTests            = 1 << 9,
    eColorAttachmentOutput        = 1 << 10,
    eComputeShader                = 1 << 11,
    eTransfer                     = 1 << 12,
    eBottomOfPipe                 = 1 << 13,
    eHost                         = 1 << 14,
    eAllGraphics                  = 1 << 15,
    eAllCommands                  = 1 << 16,
    eMax                          = 0x7FFFFFFF
};

inline BitField<AccessFlagBits> TextureUsageToAccessFlagBits(TextureUsage usage, AccessMode mode)
{
    BitField<AccessFlagBits> result;
    switch (usage)
    {
        case TextureUsage::eNone: break;
        case TextureUsage::eTransferSrc: result.SetFlag(AccessFlagBits::eTransferRead); break;
        case TextureUsage::eTransferDst: result.SetFlag(AccessFlagBits::eTransferWrite); break;
        case TextureUsage::eSampled: result.SetFlag(AccessFlagBits::eShaderRead); break;
        case TextureUsage::eStorage:
        {
            result.SetFlag(AccessFlagBits::eShaderRead);
            if (mode == AccessMode::eReadWrite)
            {
                result.SetFlag(AccessFlagBits::eShaderWrite);
            }
        }
        break;
        case TextureUsage::eColorAttachment:
            result.SetFlag(mode == AccessMode::eRead ? AccessFlagBits::eColorAttachmentRead :
                                                       AccessFlagBits::eColorAttachmentWrite);
            break;
        case TextureUsage::eDepthStencilAttachment:
            result.SetFlag(mode == AccessMode::eRead ?
                               AccessFlagBits::eDepthStencilAttachmentRead :
                               AccessFlagBits::eDepthStencilAttachmentWrite);
            break;
        case TextureUsage::eInputAttachment:
            result.SetFlag(AccessFlagBits::eInputAttachmentRead);
            break;
        default: break;
    }
    return result;
}

inline BitField<PipelineStageBits> TextureUsageToPipelineStage(TextureUsage usage)
{
    BitField<PipelineStageBits> result;
    switch (usage)
    {
        case TextureUsage::eNone: result.SetFlag(PipelineStageBits::eTopOfPipe); break;

        case TextureUsage::eTransferSrc:
        case TextureUsage::eTransferDst: result.SetFlag(PipelineStageBits::eTransfer); break;

        case TextureUsage::eSampled:
        case TextureUsage::eInputAttachment:
        case TextureUsage::eStorage: result.SetFlag(PipelineStageBits::eFragmentShader); break;

        case TextureUsage::eColorAttachment:
            result.SetFlag(PipelineStageBits::eColorAttachmentOutput);
            break;

        case TextureUsage::eDepthStencilAttachment:
            result.SetFlag(PipelineStageBits::eEarlyFragmentTests);
            result.SetFlag(PipelineStageBits::eLateFragmentTests);
        default: break;
    }
    return result;
}

inline BitField<PipelineStageBits> BufferUsageToPipelineStage(BufferUsage usage)
{
    BitField<PipelineStageBits> result;
    switch (usage)
    {
        case BufferUsage::eNone: result.SetFlag(PipelineStageBits::eTopOfPipe); break;

        case BufferUsage::eTransferSrc:
        case BufferUsage::eTransferDst: result.SetFlag(PipelineStageBits::eTransfer); break;

        case BufferUsage::eTextureBuffer:
        case BufferUsage::eImageBuffer:
        case BufferUsage::eUniformBuffer:
        case BufferUsage::eStorageBuffer:
        case BufferUsage::eIndirectBuffer:
            result.SetFlags(PipelineStageBits::eAllGraphics, PipelineStageBits::eAllCommands);
            break;

        case BufferUsage::eIndexBuffer:
        case BufferUsage::eVertexBuffer: result.SetFlag(PipelineStageBits::eVertexShader); break;

        default: break;
    }
    return result;
}

inline BitField<AccessFlagBits> BufferUsageToAccessFlagBits(BufferUsage usage, AccessMode mode)
{
    BitField<AccessFlagBits> result;
    switch (usage)
    {
        case BufferUsage::eTransferSrc: result.SetFlag(AccessFlagBits::eTransferRead); break;
        case BufferUsage::eTransferDst: result.SetFlag(AccessFlagBits::eTransferWrite); break;

        case BufferUsage::eUniformBuffer:
        case BufferUsage::eTextureBuffer: result.SetFlag(AccessFlagBits::eShaderRead); break;

        case BufferUsage::eStorageBuffer:
        case BufferUsage::eImageBuffer:
        {
            result.SetFlag(AccessFlagBits::eShaderRead);
            if (mode == AccessMode::eReadWrite)
            {
                result.SetFlag(AccessFlagBits::eShaderWrite);
            }
        }
        break;

        case BufferUsage::eIndexBuffer: result.SetFlag(AccessFlagBits::eIndexRead); break;
        case BufferUsage::eVertexBuffer:
            result.SetFlag(AccessFlagBits::eVertexAttributeRead);
            break;
        case BufferUsage::eIndirectBuffer:
            result.SetFlag(AccessFlagBits::eIndirectCommandRead);
            break;
        default: break;
    }
    return result;
}

inline bool FormatIsDepthStencil(DataFormat format)
{
    bool result = false;
    if (format == DataFormat::eD24UNORMS8UInt || format == DataFormat::eD32SFloatS8UInt)
    {
        result = true;
    }
    return result;
}

inline bool FormatIsStencilOnly(DataFormat format)
{
    bool result = false;
    if (format == DataFormat::eS8UInt)
    {
        result = true;
    }
    return result;
}

inline bool FormatIsDepthOnly(DataFormat format)
{
    bool result = false;
    if (format == DataFormat::eD32SFloat || format == DataFormat::eD16UNORM)
    {
        result = true;
    }
    return result;
}


struct MemoryTransition
{
    BitField<AccessFlagBits> srcAccess;
    BitField<AccessFlagBits> dstAccess;
};

struct TextureTransition
{
    AccessMode oldAccessMode;
    AccessMode newAccessMode;
    TextureHandle textureHandle;
    TextureUsage oldUsage;
    TextureUsage newUsage;
    TextureSubResourceRange subResourceRange;
};

struct BufferTransition
{
    AccessMode oldAccessMode;
    AccessMode newAccessMode;
    BufferHandle bufferHandle;
    BufferUsage oldUsage;
    BufferUsage newUsage;
    uint64_t offset{0};
    uint64_t size{ZEN_BUFFER_WHOLE_SIZE};
};
} // namespace zen::rhi