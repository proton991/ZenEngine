#pragma once
#include "AssetLib/AssetId.h"
#include "AssetLib/Types.h"
#include "Templates/HeapVector.h"
#include "Utils/SharedPtr.h"
#include <string>

namespace zen::asset
{
enum class TextureFilter : uint32_t
{
    Nearest = 0,
    Linear  = 1,
    MaxEnum = 0x7FFFFFFF
};

enum class SamplerAddressMode : uint32_t
{
    Repeat            = 0,
    MirroredRepeat    = 1,
    ClampToEdge       = 2,
    ClampToBorder     = 3,
    MirrorClampToEdge = 4,
    MaxEnum           = 0x7FFFFFFF
};

struct SamplerParameters
{
    TextureFilter minFilter{TextureFilter::MaxEnum};
    TextureFilter magFilter{TextureFilter::MaxEnum};
    TextureFilter mipFilter{TextureFilter::Linear};
    bool          useMipmaps{true};

    SamplerAddressMode wrapS{SamplerAddressMode::MaxEnum};
    SamplerAddressMode wrapT{SamplerAddressMode::MaxEnum};
};

struct SamplerAsset : SamplerParameters
{
    std::string name;
};

// Pixel storage is immutable after processing and shared by runtime instances.
// View format and sampling are separate, so one image can serve color/data roles.
struct ImagePixels
{
    uint32_t                        width{0};
    uint32_t                        height{0};
    Format                          format{Format::R8G8B8A8_UNORM};
    HeapVector<uint8_t>             bytes;
    HeapVector<HeapVector<uint8_t>> mips;
};

struct ImageAsset
{
    std::string                                      name;
    SharedPtr<const ImagePixels, MultiThreadCounter> pixels;
};

struct TextureAsset
{
    std::string name;
    ImageId     image;
    SamplerId   sampler;
    TextureId   linearSource;
    Format      format{Format::R8G8B8A8_UNORM};
    bool        flatNormal{false};
};
} // namespace zen::asset
