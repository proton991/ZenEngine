#include "AssetLib/ImageAsset.h"

namespace zen::asset
{
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
