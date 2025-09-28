#pragma once
#include <cstdint>

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

struct TextureFormat
{
    DataFormat internalFormat{DataFormat::eUndefined};
    SampleCount samples{SampleCount::e1};
    uint32_t width{1};
    uint32_t height{1};
    uint32_t depth{1};
    uint32_t arrayLayers{1};
    uint32_t mipmaps{1};
};
