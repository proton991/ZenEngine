#include "AssetLib/ImageAsset.h"
#include "ZenAsset/Reader/Image.h"

namespace zen::asset
{
bool AnalyzeImageAlpha(Format                    format,
                       uint32_t                  width,
                       uint32_t                  height,
                       VectorView<const uint8_t> bytes,
                       const ImageRegion&        region,
                       AlphaMipStatistics&       statistics,
                       std::string&              error)
{
    zenasset::PixelFormat native = zenasset::PixelFormat::Undefined;
    switch (format)
    {
        case Format::R8G8B8A8_UNORM: native = zenasset::PixelFormat::RGBA8Unorm; break;
        case Format::R8G8B8A8_SRGB: native = zenasset::PixelFormat::RGBA8Srgb; break;
        case Format::BC7_UNORM_BLOCK: native = zenasset::PixelFormat::BC7Unorm; break;
        case Format::BC7_SRGB_BLOCK: native = zenasset::PixelFormat::BC7Srgb; break;
        default: break;
    }
    const zenasset::ImageMipView               mip{native,
                                                   width,
                                                   height,
                                                   std::as_bytes(std::span(bytes.data(), bytes.size())),
                                                   zenasset::TextureInterpretation::Default,
                                                   {region.x, region.y, region.width, region.height}};
    const zenasset::ImageAlphaStatisticsResult result = zenasset::AnalyzeImageAlpha(mip);
    const zenasset::AlphaMipStatistics&        value  = result.statistics;
    statistics = {value.texels, value.zeroTexels, value.oneTexels, value.zeroCells, value.oneCells};
    error      = result.status.message;
    return result.status.Succeeded();
}

bool ResolveImagePixels(const SharedPtr<const ImagePixels, MultiThreadCounter>& pixels,
                        SharedPtr<const ImagePixels, MultiThreadCounter>&       resolved,
                        std::string*                                            error)
{
    SharedPtr<const ImagePixels, MultiThreadCounter> candidate;
    std::string                                      failure;
    bool                                             valid = bool(pixels);
    if (!valid)
    {
        failure = "Image has no pixel descriptor";
    }
    else if (pixels->payloadSource)
    {
        valid = pixels->payloadSource->Read(candidate, failure);
        if (valid)
        {
            valid = candidate && !candidate->payloadSource && candidate->width == pixels->width
                 && candidate->height == pixels->height && candidate->format == pixels->format
                 && candidate->arrayLayers == pixels->arrayLayers && candidate->mips.size() == pixels->mips.size()
                 && pixels->bytes.empty();
            for (const HeapVector<uint8_t>& mip : pixels->mips)
            {
                valid &= mip.empty();
            }
            if (!valid)
            {
                failure = "Image payload disagrees with its descriptor";
            }
        }
    }
    else
    {
        candidate = pixels;
    }
    resolved = valid ? std::move(candidate) : SharedPtr<const ImagePixels, MultiThreadCounter>{};
    if (error != nullptr)
    {
        *error = std::move(failure);
    }
    return valid;
}
} // namespace zen::asset
