#pragma once
// Small generated environments, so tests never depend on the optional HDR starter set
// under Data/Textures/Environments, which is not stored in the repository.
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace zen::editor
{
// Writes an uncompressed 2:1 Radiance panorama: a sky gradient over a dim ground and a
// 3x3 sun brighter than 1.0. stb_image decodes RGBE as mantissa * 2^(exponent - 136).
// tint varies the sky, giving distinct files; widths are even and at least 8.
inline bool WriteSyntheticPanorama(const std::filesystem::path& file, uint32_t width, uint8_t tint = 0)
{
    const uint32_t height = width / 2;

    std::ofstream stream(file, std::ios::binary | std::ios::trunc);

    stream << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " << height << " +X " << width << "\n";

    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const bool sun = y >= height / 4 && y < height / 4 + 3 && x >= width / 3 && x < width / 3 + 3;

            // The first byte is never 2, so the decoder reads flat rather than run-length data.
            const unsigned char sky[4]    = {static_cast<unsigned char>(64 + tint % 128),
                                             static_cast<unsigned char>(128 + y * 64 / height), 224, 128};

            const unsigned char ground[4] = {80, 64, 48, 127};

            const unsigned char light[4]  = {128, 120, 96, 136};

            const unsigned char* pixel    = sun ? light : y < height / 2 ? sky : ground;

            stream.write(reinterpret_cast<const char*>(pixel), 4);
        }
    }

    return static_cast<bool>(stream);
}
} // namespace zen::editor
