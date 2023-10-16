#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include "AssetLib/TextureLoader.h"

namespace zen
{
TextureInfo TextureLoader::LoadTexture2DFromFile(const std::string& filename)
{
    // TODO: support more texture formats (e.g.dds) & configurable texture components
    const std::string filepath = ZEN_TEXTURE_PATH + filename;

    int width = 0, height = 0, channels = 0;

    stbi_set_flip_vertically_on_load(true);
    uint8_t* data = stbi_load(filepath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    stbi_set_flip_vertically_on_load(false);

    //    ASSERT(channels == 4 && "Texture format not supported");

    std::vector<uint8_t> vecData;
    vecData.resize(width * height * channels * sizeof(uint8_t));
    std::copy(data, data + vecData.size(), vecData.begin());
    stbi_image_free(data);

    return TextureInfo{(uint32_t)width, (uint32_t)height, Format::R8G8B8A8_UNORM, false,
                       std::move(vecData)};
}
} // namespace zen