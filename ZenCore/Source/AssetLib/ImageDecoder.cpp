#include "AssetLib/ImageDecoder.h"
#include <webp/decode.h>
#include <basisu_transcoder.h>
#include <stb_image.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>

namespace zen::asset
{
// A KTX2 or WebP failure records its cause. Any failure returns no pixels, or leaves an error
// recorded when a later mip level fails.
HeapVector<uint8_t> DecodeImagePixels(const uint8_t*                   data,
                                      size_t                           size,
                                      uint32_t&                        width,
                                      uint32_t&                        height,
                                      ImportErrors&                    errors,
                                      HeapVector<HeapVector<uint8_t>>* mipBytes)
{
    HeapVector<uint8_t> pixels;

    static const uint8_t ktx2Identifier[]{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

    if (size >= sizeof(ktx2Identifier) && std::memcmp(data, ktx2Identifier, sizeof(ktx2Identifier)) == 0)
    {
        static std::once_flag initializeBasis;

        std::call_once(initializeBasis, basist::basisu_transcoder_init);

        basist::ktx2_transcoder transcoder;

        if (size > UINT32_MAX || !transcoder.init(data, static_cast<uint32_t>(size)) || !transcoder.start_transcoding())
        {
            errors.Fail("Failed to initialize KTX2 Basis Universal transcoder");
        }
        else
        {
            width  = transcoder.get_width();

            height = transcoder.get_height();

            if (width == 0 || height == 0 || size_t(width) * height > UINT32_MAX / 4)
            {
                errors.Fail("Invalid KTX2 image dimensions");
            }
            else
            {
                pixels.resize(size_t(width) * height * 4);

                if (!transcoder.transcode_image_level(0, 0, 0, pixels.data(), width * height,
                                                      basist::transcoder_texture_format::cTFRGBA32))
                {
                    errors.Fail("Failed to transcode KTX2 Basis Universal image");

                    pixels.clear();
                }
                else if (mipBytes != nullptr)
                {
                    mipBytes->push_back(pixels);

                    for (uint32_t level = 1; level < transcoder.get_levels() && !errors.Failed(); ++level)
                    {
                        const uint32_t levelWidth  = std::max(1u, width >> level);

                        const uint32_t levelHeight = std::max(1u, height >> level);

                        HeapVector<uint8_t> mip(size_t(levelWidth) * levelHeight * 4);

                        if (!transcoder.transcode_image_level(level, 0, 0, mip.data(), levelWidth * levelHeight,
                                                              basist::transcoder_texture_format::cTFRGBA32))
                        {
                            errors.Fail("Failed to transcode KTX2 Basis Universal mip level");
                        }
                        else
                        {
                            mipBytes->push_back(std::move(mip));
                        }
                    }
                }
            }
        }
    }
    else
    {
        int imageWidth  = 0;

        int imageHeight = 0;

        if (WebPGetInfo(data, size, &imageWidth, &imageHeight))
        {
            width  = static_cast<uint32_t>(imageWidth);

            height = static_cast<uint32_t>(imageHeight);

            pixels.resize(size_t(width) * height * 4);

            if (WebPDecodeRGBAInto(data, size, pixels.data(), pixels.size(), imageWidth * 4) == nullptr)
            {
                errors.Fail("Failed to decode WebP image");

                pixels.clear();
            }
        }
        else if (size <= static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            stbi_set_flip_vertically_on_load_thread(0);
            int channels = 0;

            stbi_uc* decoded =
                stbi_load_from_memory(data, static_cast<int>(size), &imageWidth, &imageHeight, &channels, STBI_rgb_alpha);

            if (decoded != nullptr && imageWidth > 0 && imageHeight > 0)
            {
                width  = static_cast<uint32_t>(imageWidth);

                height = static_cast<uint32_t>(imageHeight);

                pixels = HeapVector<uint8_t>(decoded, size_t(width) * height * 4);
            }
            else
            {
                // Unsupported containers such as TIFF or DDS land here; callers must
                // never see an empty image as a successful decode.
                const char* reason = stbi_failure_reason();
                errors.Fail(std::string("Unsupported or corrupt image (") + (reason != nullptr ? reason : "unknown format")
                            + ")");
            }

            stbi_image_free(decoded);
        }
        else
        {
            errors.Fail("Image exceeds the decoder size limit", SceneImportStatus::ResourceLimit);
        }
    }

    return pixels;
}

} // namespace zen::asset
