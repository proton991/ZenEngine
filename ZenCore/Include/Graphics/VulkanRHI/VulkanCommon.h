#pragma once
#include "Utils/Errors.h"
#include "Graphics/RHI/RHIError.h"
#include <string>
#include "VulkanHeaders.h"

namespace zen
{
void ReportVulkanDeviceLoss(VkResult result, const char* operation);

inline RHIError MakeVulkanError(VkResult result, const char* operation, const char* source = nullptr, uint32_t line = 0)
{
    RHIError error{};

    ReportVulkanDeviceLoss(result, operation);

    if (result < VK_SUCCESS)
    {
        error = {RHIErrorCode::eBackendFailure, static_cast<int64_t>(result), operation, source, line};

        switch (result)
        {
            case VK_ERROR_OUT_OF_HOST_MEMORY: error.code = RHIErrorCode::eOutOfHostMemory; break;
            case VK_ERROR_OUT_OF_DEVICE_MEMORY: error.code = RHIErrorCode::eOutOfDeviceMemory; break;
            case VK_ERROR_DEVICE_LOST: error.code = RHIErrorCode::eDeviceLost; break;
            case VK_ERROR_FEATURE_NOT_PRESENT:
            case VK_ERROR_EXTENSION_NOT_PRESENT:
            case VK_ERROR_FORMAT_NOT_SUPPORTED: error.code = RHIErrorCode::eUnsupported; break;
            default: break;
        }
    }

    return error;
}

inline const char* GetResultString(VkResult result)
{
    const char* pResultString = "unknown";

#define STR(a) \
    case a: pResultString = #a; break;

    switch (result)
    {
        STR(VK_SUCCESS);
        STR(VK_NOT_READY);
        STR(VK_TIMEOUT);
        STR(VK_EVENT_SET);
        STR(VK_EVENT_RESET);
        STR(VK_INCOMPLETE);
        STR(VK_ERROR_OUT_OF_HOST_MEMORY);
        STR(VK_ERROR_OUT_OF_DEVICE_MEMORY);
        STR(VK_ERROR_INITIALIZATION_FAILED);
        STR(VK_ERROR_DEVICE_LOST);
        STR(VK_ERROR_MEMORY_MAP_FAILED);
        STR(VK_ERROR_LAYER_NOT_PRESENT);
        STR(VK_ERROR_EXTENSION_NOT_PRESENT);
        STR(VK_ERROR_FEATURE_NOT_PRESENT);
        STR(VK_ERROR_INCOMPATIBLE_DRIVER);
        STR(VK_ERROR_TOO_MANY_OBJECTS);
        STR(VK_ERROR_FORMAT_NOT_SUPPORTED);
        STR(VK_ERROR_FRAGMENTED_POOL);
        STR(VK_ERROR_OUT_OF_POOL_MEMORY);
        STR(VK_ERROR_INVALID_EXTERNAL_HANDLE);
        STR(VK_ERROR_SURFACE_LOST_KHR);
        STR(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR);
        STR(VK_SUBOPTIMAL_KHR);
        STR(VK_ERROR_OUT_OF_DATE_KHR);
        STR(VK_ERROR_INCOMPATIBLE_DISPLAY_KHR);
        STR(VK_ERROR_VALIDATION_FAILED_EXT);
        STR(VK_ERROR_INVALID_SHADER_NV);
        STR(VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT);
        STR(VK_ERROR_FRAGMENTATION_EXT);
        STR(VK_ERROR_NOT_PERMITTED_EXT);
        STR(VK_ERROR_INVALID_DEVICE_ADDRESS_EXT);
        STR(VK_ERROR_UNKNOWN);
        STR(VK_PIPELINE_COMPILE_REQUIRED);
        STR(VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR);
        STR(VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT);
        STR(VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR);
        STR(VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR);
        STR(VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR);
        STR(VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR);
        STR(VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR);
        STR(VK_THREAD_IDLE_KHR);
        STR(VK_THREAD_DONE_KHR);
        STR(VK_OPERATION_DEFERRED_KHR);
        STR(VK_OPERATION_NOT_DEFERRED_KHR);
        STR(VK_ERROR_COMPRESSION_EXHAUSTED_EXT);
        STR(VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT);
        default: break;
    }
#undef STR
    return pResultString;
}

template <typename T> inline std::string VkToString(T value)
{
    return "";
}

template <>
// clang-tidy: disable-next-line
inline std::string VkToString<VkImageUsageFlagBits>(VkImageUsageFlagBits image_usage)
{
    std::string returnValue{};
    switch (image_usage)
    {
        case VK_IMAGE_USAGE_TRANSFER_SRC_BIT: returnValue = "VK_IMAGE_USAGE_TRANSFER_SRC_BIT"; break;
        case VK_IMAGE_USAGE_TRANSFER_DST_BIT: returnValue = "VK_IMAGE_USAGE_TRANSFER_DST_BIT"; break;
        case VK_IMAGE_USAGE_SAMPLED_BIT: returnValue = "VK_IMAGE_USAGE_SAMPLED_BIT"; break;
        case VK_IMAGE_USAGE_STORAGE_BIT: returnValue = "VK_IMAGE_USAGE_STORAGE_BIT"; break;
        case VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT: returnValue = "VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT"; break;
        case VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT: returnValue = "VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT"; break;
        case VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT: returnValue = "VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT"; break;
        case VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT: returnValue = "VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT"; break;
        case VK_IMAGE_USAGE_FLAG_BITS_MAX_ENUM: returnValue = "VK_IMAGE_USAGE_FLAG_BITS_MAX_ENUM"; break;
        default: returnValue = "UNKNOWN IMAGE USAGE FLAG"; break;
    }
    return returnValue;
}

template <>
// clang-tidy: disable-next-line
inline std::string VkToString<VkFormat>(VkFormat format)
{
    std::string returnValue{};
    switch (format)
    {
        case VK_FORMAT_R4G4_UNORM_PACK8: returnValue = "VK_FORMAT_R4G4_UNORM_PACK8"; break;
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16: returnValue = "VK_FORMAT_R4G4B4A4_UNORM_PACK16"; break;
        case VK_FORMAT_B4G4R4A4_UNORM_PACK16: returnValue = "VK_FORMAT_B4G4R4A4_UNORM_PACK16"; break;
        case VK_FORMAT_R5G6B5_UNORM_PACK16: returnValue = "VK_FORMAT_R5G6B5_UNORM_PACK16"; break;
        case VK_FORMAT_B5G6R5_UNORM_PACK16: returnValue = "VK_FORMAT_B5G6R5_UNORM_PACK16"; break;
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16: returnValue = "VK_FORMAT_R5G5B5A1_UNORM_PACK16"; break;
        case VK_FORMAT_B5G5R5A1_UNORM_PACK16: returnValue = "VK_FORMAT_B5G5R5A1_UNORM_PACK16"; break;
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16: returnValue = "VK_FORMAT_A1R5G5B5_UNORM_PACK16"; break;
        case VK_FORMAT_R8_UNORM: returnValue = "VK_FORMAT_R8_UNORM"; break;
        case VK_FORMAT_R8_SNORM: returnValue = "VK_FORMAT_R8_SNORM"; break;
        case VK_FORMAT_R8_USCALED: returnValue = "VK_FORMAT_R8_USCALED"; break;
        case VK_FORMAT_R8_SSCALED: returnValue = "VK_FORMAT_R8_SSCALED"; break;
        case VK_FORMAT_R8_UINT: returnValue = "VK_FORMAT_R8_UINT"; break;
        case VK_FORMAT_R8_SINT: returnValue = "VK_FORMAT_R8_SINT"; break;
        case VK_FORMAT_R8_SRGB: returnValue = "VK_FORMAT_R8_SRGB"; break;
        case VK_FORMAT_R8G8_UNORM: returnValue = "VK_FORMAT_R8G8_UNORM"; break;
        case VK_FORMAT_R8G8_SNORM: returnValue = "VK_FORMAT_R8G8_SNORM"; break;
        case VK_FORMAT_R8G8_USCALED: returnValue = "VK_FORMAT_R8G8_USCALED"; break;
        case VK_FORMAT_R8G8_SSCALED: returnValue = "VK_FORMAT_R8G8_SSCALED"; break;
        case VK_FORMAT_R8G8_UINT: returnValue = "VK_FORMAT_R8G8_UINT"; break;
        case VK_FORMAT_R8G8_SINT: returnValue = "VK_FORMAT_R8G8_SINT"; break;
        case VK_FORMAT_R8G8_SRGB: returnValue = "VK_FORMAT_R8G8_SRGB"; break;
        case VK_FORMAT_R8G8B8_UNORM: returnValue = "VK_FORMAT_R8G8B8_UNORM"; break;
        case VK_FORMAT_R8G8B8_SNORM: returnValue = "VK_FORMAT_R8G8B8_SNORM"; break;
        case VK_FORMAT_R8G8B8_USCALED: returnValue = "VK_FORMAT_R8G8B8_USCALED"; break;
        case VK_FORMAT_R8G8B8_SSCALED: returnValue = "VK_FORMAT_R8G8B8_SSCALED"; break;
        case VK_FORMAT_R8G8B8_UINT: returnValue = "VK_FORMAT_R8G8B8_UINT"; break;
        case VK_FORMAT_R8G8B8_SINT: returnValue = "VK_FORMAT_R8G8B8_SINT"; break;
        case VK_FORMAT_R8G8B8_SRGB: returnValue = "VK_FORMAT_R8G8B8_SRGB"; break;
        case VK_FORMAT_B8G8R8_UNORM: returnValue = "VK_FORMAT_B8G8R8_UNORM"; break;
        case VK_FORMAT_B8G8R8_SNORM: returnValue = "VK_FORMAT_B8G8R8_SNORM"; break;
        case VK_FORMAT_B8G8R8_USCALED: returnValue = "VK_FORMAT_B8G8R8_USCALED"; break;
        case VK_FORMAT_B8G8R8_SSCALED: returnValue = "VK_FORMAT_B8G8R8_SSCALED"; break;
        case VK_FORMAT_B8G8R8_UINT: returnValue = "VK_FORMAT_B8G8R8_UINT"; break;
        case VK_FORMAT_B8G8R8_SINT: returnValue = "VK_FORMAT_B8G8R8_SINT"; break;
        case VK_FORMAT_B8G8R8_SRGB: returnValue = "VK_FORMAT_B8G8R8_SRGB"; break;
        case VK_FORMAT_R8G8B8A8_UNORM: returnValue = "VK_FORMAT_R8G8B8A8_UNORM"; break;
        case VK_FORMAT_R8G8B8A8_SNORM: returnValue = "VK_FORMAT_R8G8B8A8_SNORM"; break;
        case VK_FORMAT_R8G8B8A8_USCALED: returnValue = "VK_FORMAT_R8G8B8A8_USCALED"; break;
        case VK_FORMAT_R8G8B8A8_SSCALED: returnValue = "VK_FORMAT_R8G8B8A8_SSCALED"; break;
        case VK_FORMAT_R8G8B8A8_UINT: returnValue = "VK_FORMAT_R8G8B8A8_UINT"; break;
        case VK_FORMAT_R8G8B8A8_SINT: returnValue = "VK_FORMAT_R8G8B8A8_SINT"; break;
        case VK_FORMAT_R8G8B8A8_SRGB: returnValue = "VK_FORMAT_R8G8B8A8_SRGB"; break;
        case VK_FORMAT_B8G8R8A8_UNORM: returnValue = "VK_FORMAT_B8G8R8A8_UNORM"; break;
        case VK_FORMAT_B8G8R8A8_SNORM: returnValue = "VK_FORMAT_B8G8R8A8_SNORM"; break;
        case VK_FORMAT_B8G8R8A8_USCALED: returnValue = "VK_FORMAT_B8G8R8A8_USCALED"; break;
        case VK_FORMAT_B8G8R8A8_SSCALED: returnValue = "VK_FORMAT_B8G8R8A8_SSCALED"; break;
        case VK_FORMAT_B8G8R8A8_UINT: returnValue = "VK_FORMAT_B8G8R8A8_UINT"; break;
        case VK_FORMAT_B8G8R8A8_SINT: returnValue = "VK_FORMAT_B8G8R8A8_SINT"; break;
        case VK_FORMAT_B8G8R8A8_SRGB: returnValue = "VK_FORMAT_B8G8R8A8_SRGB"; break;
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: returnValue = "VK_FORMAT_A8B8G8R8_UNORM_PACK32"; break;
        case VK_FORMAT_A8B8G8R8_SNORM_PACK32: returnValue = "VK_FORMAT_A8B8G8R8_SNORM_PACK32"; break;
        case VK_FORMAT_A8B8G8R8_USCALED_PACK32: returnValue = "VK_FORMAT_A8B8G8R8_USCALED_PACK32"; break;
        case VK_FORMAT_A8B8G8R8_SSCALED_PACK32: returnValue = "VK_FORMAT_A8B8G8R8_SSCALED_PACK32"; break;
        case VK_FORMAT_A8B8G8R8_UINT_PACK32: returnValue = "VK_FORMAT_A8B8G8R8_UINT_PACK32"; break;
        case VK_FORMAT_A8B8G8R8_SINT_PACK32: returnValue = "VK_FORMAT_A8B8G8R8_SINT_PACK32"; break;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: returnValue = "VK_FORMAT_A8B8G8R8_SRGB_PACK32"; break;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: returnValue = "VK_FORMAT_A2R10G10B10_UNORM_PACK32"; break;
        case VK_FORMAT_A2R10G10B10_SNORM_PACK32: returnValue = "VK_FORMAT_A2R10G10B10_SNORM_PACK32"; break;
        case VK_FORMAT_A2R10G10B10_USCALED_PACK32: returnValue = "VK_FORMAT_A2R10G10B10_USCALED_PACK32"; break;
        case VK_FORMAT_A2R10G10B10_SSCALED_PACK32: returnValue = "VK_FORMAT_A2R10G10B10_SSCALED_PACK32"; break;
        case VK_FORMAT_A2R10G10B10_UINT_PACK32: returnValue = "VK_FORMAT_A2R10G10B10_UINT_PACK32"; break;
        case VK_FORMAT_A2R10G10B10_SINT_PACK32: returnValue = "VK_FORMAT_A2R10G10B10_SINT_PACK32"; break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: returnValue = "VK_FORMAT_A2B10G10R10_UNORM_PACK32"; break;
        case VK_FORMAT_A2B10G10R10_SNORM_PACK32: returnValue = "VK_FORMAT_A2B10G10R10_SNORM_PACK32"; break;
        case VK_FORMAT_A2B10G10R10_USCALED_PACK32: returnValue = "VK_FORMAT_A2B10G10R10_USCALED_PACK32"; break;
        case VK_FORMAT_A2B10G10R10_SSCALED_PACK32: returnValue = "VK_FORMAT_A2B10G10R10_SSCALED_PACK32"; break;
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: returnValue = "VK_FORMAT_A2B10G10R10_UINT_PACK32"; break;
        case VK_FORMAT_A2B10G10R10_SINT_PACK32: returnValue = "VK_FORMAT_A2B10G10R10_SINT_PACK32"; break;
        case VK_FORMAT_R16_UNORM: returnValue = "VK_FORMAT_R16_UNORM"; break;
        case VK_FORMAT_R16_SNORM: returnValue = "VK_FORMAT_R16_SNORM"; break;
        case VK_FORMAT_R16_USCALED: returnValue = "VK_FORMAT_R16_USCALED"; break;
        case VK_FORMAT_R16_SSCALED: returnValue = "VK_FORMAT_R16_SSCALED"; break;
        case VK_FORMAT_R16_UINT: returnValue = "VK_FORMAT_R16_UINT"; break;
        case VK_FORMAT_R16_SINT: returnValue = "VK_FORMAT_R16_SINT"; break;
        case VK_FORMAT_R16_SFLOAT: returnValue = "VK_FORMAT_R16_SFLOAT"; break;
        case VK_FORMAT_R16G16_UNORM: returnValue = "VK_FORMAT_R16G16_UNORM"; break;
        case VK_FORMAT_R16G16_SNORM: returnValue = "VK_FORMAT_R16G16_SNORM"; break;
        case VK_FORMAT_R16G16_USCALED: returnValue = "VK_FORMAT_R16G16_USCALED"; break;
        case VK_FORMAT_R16G16_SSCALED: returnValue = "VK_FORMAT_R16G16_SSCALED"; break;
        case VK_FORMAT_R16G16_UINT: returnValue = "VK_FORMAT_R16G16_UINT"; break;
        case VK_FORMAT_R16G16_SINT: returnValue = "VK_FORMAT_R16G16_SINT"; break;
        case VK_FORMAT_R16G16_SFLOAT: returnValue = "VK_FORMAT_R16G16_SFLOAT"; break;
        case VK_FORMAT_R16G16B16_UNORM: returnValue = "VK_FORMAT_R16G16B16_UNORM"; break;
        case VK_FORMAT_R16G16B16_SNORM: returnValue = "VK_FORMAT_R16G16B16_SNORM"; break;
        case VK_FORMAT_R16G16B16_USCALED: returnValue = "VK_FORMAT_R16G16B16_USCALED"; break;
        case VK_FORMAT_R16G16B16_SSCALED: returnValue = "VK_FORMAT_R16G16B16_SSCALED"; break;
        case VK_FORMAT_R16G16B16_UINT: returnValue = "VK_FORMAT_R16G16B16_UINT"; break;
        case VK_FORMAT_R16G16B16_SINT: returnValue = "VK_FORMAT_R16G16B16_SINT"; break;
        case VK_FORMAT_R16G16B16_SFLOAT: returnValue = "VK_FORMAT_R16G16B16_SFLOAT"; break;
        case VK_FORMAT_R16G16B16A16_UNORM: returnValue = "VK_FORMAT_R16G16B16A16_UNORM"; break;
        case VK_FORMAT_R16G16B16A16_SNORM: returnValue = "VK_FORMAT_R16G16B16A16_SNORM"; break;
        case VK_FORMAT_R16G16B16A16_USCALED: returnValue = "VK_FORMAT_R16G16B16A16_USCALED"; break;
        case VK_FORMAT_R16G16B16A16_SSCALED: returnValue = "VK_FORMAT_R16G16B16A16_SSCALED"; break;
        case VK_FORMAT_R16G16B16A16_UINT: returnValue = "VK_FORMAT_R16G16B16A16_UINT"; break;
        case VK_FORMAT_R16G16B16A16_SINT: returnValue = "VK_FORMAT_R16G16B16A16_SINT"; break;
        case VK_FORMAT_R16G16B16A16_SFLOAT: returnValue = "VK_FORMAT_R16G16B16A16_SFLOAT"; break;
        case VK_FORMAT_R32_UINT: returnValue = "VK_FORMAT_R32_UINT"; break;
        case VK_FORMAT_R32_SINT: returnValue = "VK_FORMAT_R32_SINT"; break;
        case VK_FORMAT_R32_SFLOAT: returnValue = "VK_FORMAT_R32_SFLOAT"; break;
        case VK_FORMAT_R32G32_UINT: returnValue = "VK_FORMAT_R32G32_UINT"; break;
        case VK_FORMAT_R32G32_SINT: returnValue = "VK_FORMAT_R32G32_SINT"; break;
        case VK_FORMAT_R32G32_SFLOAT: returnValue = "VK_FORMAT_R32G32_SFLOAT"; break;
        case VK_FORMAT_R32G32B32_UINT: returnValue = "VK_FORMAT_R32G32B32_UINT"; break;
        case VK_FORMAT_R32G32B32_SINT: returnValue = "VK_FORMAT_R32G32B32_SINT"; break;
        case VK_FORMAT_R32G32B32_SFLOAT: returnValue = "VK_FORMAT_R32G32B32_SFLOAT"; break;
        case VK_FORMAT_R32G32B32A32_UINT: returnValue = "VK_FORMAT_R32G32B32A32_UINT"; break;
        case VK_FORMAT_R32G32B32A32_SINT: returnValue = "VK_FORMAT_R32G32B32A32_SINT"; break;
        case VK_FORMAT_R32G32B32A32_SFLOAT: returnValue = "VK_FORMAT_R32G32B32A32_SFLOAT"; break;
        case VK_FORMAT_R64_UINT: returnValue = "VK_FORMAT_R64_UINT"; break;
        case VK_FORMAT_R64_SINT: returnValue = "VK_FORMAT_R64_SINT"; break;
        case VK_FORMAT_R64_SFLOAT: returnValue = "VK_FORMAT_R64_SFLOAT"; break;
        case VK_FORMAT_R64G64_UINT: returnValue = "VK_FORMAT_R64G64_UINT"; break;
        case VK_FORMAT_R64G64_SINT: returnValue = "VK_FORMAT_R64G64_SINT"; break;
        case VK_FORMAT_R64G64_SFLOAT: returnValue = "VK_FORMAT_R64G64_SFLOAT"; break;
        case VK_FORMAT_R64G64B64_UINT: returnValue = "VK_FORMAT_R64G64B64_UINT"; break;
        case VK_FORMAT_R64G64B64_SINT: returnValue = "VK_FORMAT_R64G64B64_SINT"; break;
        case VK_FORMAT_R64G64B64_SFLOAT: returnValue = "VK_FORMAT_R64G64B64_SFLOAT"; break;
        case VK_FORMAT_R64G64B64A64_UINT: returnValue = "VK_FORMAT_R64G64B64A64_UINT"; break;
        case VK_FORMAT_R64G64B64A64_SINT: returnValue = "VK_FORMAT_R64G64B64A64_SINT"; break;
        case VK_FORMAT_R64G64B64A64_SFLOAT: returnValue = "VK_FORMAT_R64G64B64A64_SFLOAT"; break;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: returnValue = "VK_FORMAT_B10G11R11_UFLOAT_PACK32"; break;
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: returnValue = "VK_FORMAT_E5B9G9R9_UFLOAT_PACK32"; break;
        case VK_FORMAT_D16_UNORM: returnValue = "VK_FORMAT_D16_UNORM"; break;
        case VK_FORMAT_X8_D24_UNORM_PACK32: returnValue = "VK_FORMAT_X8_D24_UNORM_PACK32"; break;
        case VK_FORMAT_D32_SFLOAT: returnValue = "VK_FORMAT_D32_SFLOAT"; break;
        case VK_FORMAT_S8_UINT: returnValue = "VK_FORMAT_S8_UINT"; break;
        case VK_FORMAT_D16_UNORM_S8_UINT: returnValue = "VK_FORMAT_D16_UNORM_S8_UINT"; break;
        case VK_FORMAT_D24_UNORM_S8_UINT: returnValue = "VK_FORMAT_D24_UNORM_S8_UINT"; break;
        case VK_FORMAT_D32_SFLOAT_S8_UINT: returnValue = "VK_FORMAT_D32_SFLOAT_S8_UINT"; break;
        case VK_FORMAT_UNDEFINED: returnValue = "VK_FORMAT_UNDEFINED"; break;
        default: returnValue = "VK_FORMAT_INVALID"; break;
    }
    return returnValue;
}

template <> inline std::string VkToString<VkCompositeAlphaFlagBitsKHR>(VkCompositeAlphaFlagBitsKHR compositeAlpha)
{
    std::string returnValue{};
    switch (compositeAlpha)
    {
        case VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR: returnValue = "VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR"; break;
        case VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR: returnValue = "VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR"; break;
        case VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR: returnValue = "VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR"; break;
        case VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR: returnValue = "VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR"; break;
        case VK_COMPOSITE_ALPHA_FLAG_BITS_MAX_ENUM_KHR: returnValue = "VK_COMPOSITE_ALPHA_FLAG_BITS_MAX_ENUM_KHR"; break;
        default: returnValue = "UNKNOWN COMPOSITE ALPHA FLAG"; break;
    }
    return returnValue;
}

template <> inline std::string VkToString<VkSurfaceFormatKHR>(VkSurfaceFormatKHR surfaceFormat)
{
    std::string str = VkToString(surfaceFormat.format) + ", ";

    switch (surfaceFormat.colorSpace)
    {
        case VK_COLORSPACE_SRGB_NONLINEAR_KHR: str += "VK_COLORSPACE_SRGB_NONLINEAR_KHR"; break;
        default: str += "UNKNOWN COLOR SPACE";
    }
    return str;
}

template <> inline std::string VkToString<VkPresentModeKHR>(VkPresentModeKHR presentMode)
{
    std::string returnValue{};
    switch (presentMode)
    {
        case VK_PRESENT_MODE_MAILBOX_KHR: returnValue = "VK_PRESENT_MODE_MAILBOX_KHR"; break;
        case VK_PRESENT_MODE_IMMEDIATE_KHR: returnValue = "VK_PRESENT_MODE_IMMEDIATE_KHR"; break;
        case VK_PRESENT_MODE_FIFO_KHR: returnValue = "VK_PRESENT_MODE_FIFO_KHR"; break;
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: returnValue = "VK_PRESENT_MODE_FIFO_RELAXED_KHR"; break;
        case VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR:
            returnValue = "VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR";
            break;
        case VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR: returnValue = "VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR"; break;
        default: returnValue = "UNKNOWN_PRESENT_MODE"; break;
    }
    return returnValue;
}

template <> inline std::string VkToString<VkShaderStageFlagBits>(VkShaderStageFlagBits stage)
{
    std::string returnValue{};
    switch (stage)
    {
        case VK_SHADER_STAGE_VERTEX_BIT: returnValue = "VK_SHADER_STAGE_VERTEX_BIT"; break;
        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: returnValue = "VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT"; break;
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: returnValue = "VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT"; break;
        case VK_SHADER_STAGE_GEOMETRY_BIT: returnValue = "VK_SHADER_STAGE_GEOMETRY_BIT"; break;
        case VK_SHADER_STAGE_FRAGMENT_BIT: returnValue = "VK_SHADER_STAGE_FRAGMENT_BIT"; break;
        case VK_SHADER_STAGE_COMPUTE_BIT: returnValue = "VK_SHADER_STAGE_COMPUTE_BIT"; break;
        default: returnValue = "UNKNOWN_SHADER_STAGE"; break;
    }
    return returnValue;
}

template <class T> void InitVkStruct(T& vkStruct, uint32_t vkStructureType)
{
    std::memset((uint8_t*)&vkStruct, 0, sizeof(T));
    (uint32_t&)vkStruct.sType = vkStructureType;
}
} // namespace zen
