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
class ImagePayloadSource;
struct ImagePixels
{
    uint32_t width{0};
    uint32_t height{0};
    Format   format{Format::R8G8B8A8_UNORM};
    // Base-only storage when mips is empty. Explicit chains own level zero in
    // mips[0]; an identical legacy duplicate in bytes is accepted on input.
    HeapVector<uint8_t>             bytes;
    HeapVector<HeapVector<uint8_t>> mips;
    // Each explicit mip stores complete consecutive layers. Arrays require mips.
    uint32_t arrayLayers{1};
    // Native packages retain descriptors, not decoded CPU pixels. Acquire a
    // temporary resident lease for uploads, export or editor previews.
    SharedPtr<const ImagePayloadSource, MultiThreadCounter> payloadSource;
};

class ImagePayloadSource
{
public:
    virtual ~ImagePayloadSource()                                                                         = default;
    virtual bool Read(SharedPtr<const ImagePixels, MultiThreadCounter>& pixels, std::string& error) const = 0;
};

bool ResolveImagePixels(const SharedPtr<const ImagePixels, MultiThreadCounter>& pixels,
                        SharedPtr<const ImagePixels, MultiThreadCounter>&       resolved,
                        std::string*                                            error = nullptr);

inline const HeapVector<uint8_t>& GetImageBaseBytes(const ImagePixels& pixels)
{
    return pixels.mips.empty() ? pixels.bytes : pixels.mips[0];
}

struct ImageAsset
{
    std::string                                      name;
    SharedPtr<const ImagePixels, MultiThreadCounter> pixels;
};

enum class TextureInterpretation : uint32_t
{
    Default           = 0,
    NormalXYPositiveZ = 1
};

// Logical interior in an immutable spatial page; all zero selects the full image.
// Kept separate from the authored material UV transform.
struct ImageRegion
{
    uint32_t x{0};
    uint32_t y{0};
    uint32_t width{0};
    uint32_t height{0};
    bool     operator==(const ImageRegion&) const = default;
};

struct AlphaMipStatistics
{
    uint64_t texels{0};
    uint64_t zeroTexels{0};
    uint64_t oneTexels{0};
    uint64_t zeroCells{0};
    uint64_t oneCells{0};
    bool     operator==(const AlphaMipStatistics&) const = default;
};

// Analyze a resident logical mip without retaining decoded image storage.
bool AnalyzeImageAlpha(Format                    format,
                       uint32_t                  width,
                       uint32_t                  height,
                       VectorView<const uint8_t> bytes,
                       const ImageRegion&        region,
                       AlphaMipStatistics&       statistics,
                       std::string&              error);

struct TextureAsset
{
    std::string                    name;
    ImageId                        image;
    SamplerId                      sampler;
    TextureId                      linearSource;
    Format                         format{Format::R8G8B8A8_UNORM};
    bool                           flatNormal{false};
    TextureInterpretation          interpretation{TextureInterpretation::Default};
    uint32_t                       arrayLayer{0};
    ImageRegion                    atlasRegion;
    HeapVector<AlphaMipStatistics> alphaMips;
};
} // namespace zen::asset
