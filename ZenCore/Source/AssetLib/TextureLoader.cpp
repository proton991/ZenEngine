#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include "AssetLib/TextureLoader.h"

namespace zen::asset
{
TextureInfo TextureLoader::LoadTexture2DFromFile(const std::string& filename)
{
    // TODO: support more texture formats (e.g.dds) & configurable texture components
    const std::string filepath = ZEN_TEXTURE_PATH + filename;

    int width = 0, height = 0, channels = 0;

    stbi_set_flip_vertically_on_load(true);
    uint8_t* pData = stbi_load(filepath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    stbi_set_flip_vertically_on_load(false);

    //    ASSERT(channels == 4 && "Texture format not supported");

    std::vector<uint8_t> vecData;
    vecData.resize(width * height * 4 * sizeof(uint8_t));
    std::copy(pData, pData + vecData.size(), vecData.begin());
    stbi_image_free(pData);

    return TextureInfo{(uint32_t)width, (uint32_t)height, Format::R8G8B8A8_UNORM,
                       std::move(vecData)};
}

void TextureLoader::LoadTexture2DFromFile(const std::string& filename, TextureInfo* pOutTexInfo)
{
    const std::string filepath = ZEN_TEXTURE_PATH + filename;

    int width = 0, height = 0, channels = 0;

    stbi_set_flip_vertically_on_load(true);
    uint8_t* pData = stbi_load(filepath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    stbi_set_flip_vertically_on_load(false);

    std::vector<uint8_t> vecData;
    vecData.resize(width * height * 4 * sizeof(uint8_t));
    std::copy(pData, pData + vecData.size(), vecData.begin());
    stbi_image_free(pData);

    pOutTexInfo->width  = width;
    pOutTexInfo->height = height;
    pOutTexInfo->data   = std::move(vecData);
    pOutTexInfo->format = Format::R8G8B8A8_UNORM;
}
} // namespace zen::asset