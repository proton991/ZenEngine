#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include "AssetLib/TextureLoader.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace zen::asset
{
TextureInfo TextureLoader::LoadTexture2DFromFile(const std::string& filename)
{
    // TODO: support more texture formats (e.g.dds) & configurable texture components
    const std::string filepath = ZEN_TEXTURE_PATH + filename;

    int width = 0, height = 0, channels = 0;

    stbi_set_flip_vertically_on_load_thread(true);

    uint8_t* pData = stbi_load(filepath.c_str(), &width, &height, &channels, STBI_rgb_alpha);

    stbi_set_flip_vertically_on_load_thread(false);

    TextureInfo result;

    if (pData != nullptr && width > 0 && height > 0)
    {
        result.width  = static_cast<uint32_t>(width);

        result.height = static_cast<uint32_t>(height);

        result.format = Format::R8G8B8A8_UNORM;

        result.data.assign(pData, pData + size_t(width) * size_t(height) * STBI_rgb_alpha);
    }

    stbi_image_free(pData);

    return result;
}

void TextureLoader::LoadTexture2DFromFile(const std::string& filename, TextureInfo* pOutTexInfo)
{
    if (pOutTexInfo != nullptr)
    {
        *pOutTexInfo = LoadTexture2DFromFile(filename);
    }
}

namespace
{
Vec3 CubeDirection(uint32_t face, float u, float v)
{
    Vec3 direction;

    switch (face)
    {
        case 0: direction = Vec3(1, -v, -u); break;
        case 1: direction = Vec3(-1, -v, u); break;
        case 2: direction = Vec3(u, 1, v); break;
        case 3: direction = Vec3(u, -1, -v); break;
        case 4: direction = Vec3(u, -v, 1); break;
        default: direction = Vec3(-u, -v, -1); break;
    }

    return glm::normalize(direction);
}

Vec4 PanoramaPixel(const float* source, int width, int height, int x, int y)
{
    x                  = (x % width + width) % width;

    y                  = std::clamp(y, 0, height - 1);

    const float* pixel = source + (size_t(y) * width + x) * 4;

    Vec4 result(0, 0, 0, 1);

    for (int channel = 0; channel < 3; ++channel)
    {
        result[channel] = std::isfinite(pixel[channel]) ? std::max(0.0f, pixel[channel]) : 0.0f;
    }

    return result;
}

void ConvertPanorama(const float* source, int width, int height, uint32_t size, HeapVector<Vec4>& pixels)
{
    pixels.resize(size_t(size) * size * 6);

    for (uint32_t face = 0; face < 6; ++face)
    {
        for (uint32_t y = 0; y < size; ++y)
        {
            for (uint32_t x = 0; x < size; ++x)
            {
                const Vec3 direction = CubeDirection(face, 2.0f * (x + 0.5f) / size - 1.0f, 2.0f * (y + 0.5f) / size - 1.0f);

                const float u        = (std::atan2(direction.z, direction.x) / glm::two_pi<float>() + 0.5f) * width - 0.5f;

                // File environments use inverted source Y; skybox.frag and
                // filtercube.vert convert it back to the world-space convention.
                const float v    = std::acos(std::clamp(-direction.y, -1.0f, 1.0f)) / glm::pi<float>() * height - 0.5f;

                const int left   = static_cast<int>(std::floor(u));

                const int top    = static_cast<int>(std::floor(v));

                const Vec4 upper = glm::mix(PanoramaPixel(source, width, height, left, top),
                                            PanoramaPixel(source, width, height, left + 1, top), u - left);

                const Vec4 lower = glm::mix(PanoramaPixel(source, width, height, left, top + 1),
                                            PanoramaPixel(source, width, height, left + 1, top + 1), u - left);

                pixels[(size_t(face) * size + y) * size + x] = glm::mix(upper, lower, v - top);
            }
        }
    }
}
} // namespace

bool TextureLoader::LoadHDRCubemap(const std::string& path, uint32_t& size, HeapVector<Vec4>& pixels, std::string& error)
{
    bool valid = false;

    size       = 0;

    pixels.clear();

    error = "Expected a readable 2:1 Radiance HDR panorama (maximum 8192 x 4096).";

    std::ifstream stream(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);

    const std::streamoff length = stream ? static_cast<std::streamoff>(stream.tellg()) : 0;

    // Bound both the compressed input and decoded float allocation before decoding.
    if (length > 0 && length <= 256 * 1024 * 1024)
    {
        HeapVector<uint8_t> bytes(static_cast<size_t>(length));

        stream.seekg(0);

        stream.read(reinterpret_cast<char*>(bytes.data()), length);

        int width = 0, height = 0, channels = 0;

        const int byteCount = static_cast<int>(length);

        if (stream && stbi_is_hdr_from_memory(bytes.data(), byteCount)
            && stbi_info_from_memory(bytes.data(), byteCount, &width, &height, &channels) && width >= 4 && width <= 8192
            && height > 0 && width == height * 2)
        {
            stbi_set_flip_vertically_on_load_thread(false);

            float* source = stbi_loadf_from_memory(bytes.data(), byteCount, &width, &height, &channels, STBI_rgb_alpha);

            if (source != nullptr)
            {
                size = std::min(static_cast<uint32_t>(width / 4), 512u);

                ConvertPanorama(source, width, height, size, pixels);

                error.clear();

                valid = true;
            }

            stbi_image_free(source);
        }
    }

    return valid;
}
} // namespace zen::asset
