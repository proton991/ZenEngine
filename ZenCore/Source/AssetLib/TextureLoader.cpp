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

} // namespace zen::asset