#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "Graphics/Rendering/TextureManager.h"
#include "Graphics/Rendering/RenderDevice.h"
#include "Graphics/Rendering/RenderContext.h"
#include "Common/Errors.h"

namespace zen
{

val::Image* TextureManager::RequestTexture2D(const std::string& filename, bool requireMipmap)
{
    TextureInfo          textureInfo = LoadTexture2DFromFile(filename);
    val::ImageCreateInfo imageCI{};
    imageCI.format      = textureInfo.format;
    imageCI.extent3D    = {textureInfo.width, textureInfo.height, 1};
    bool generateMipmap = false;
    if (requireMipmap && (!textureInfo.hasMipMap))
    {
        // generate mipmap
        generateMipmap = true;
        imageCI.usage  = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    imageCI.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageCI.mipLevels =
        textureInfo.otherLeveData.empty() ? 1 : textureInfo.otherLeveData.size() + 1;
    m_cache.emplace(filename, val::Image::CreateUnique(m_valDevice, imageCI));
    val::Image* image = m_cache.at(filename).Get();
    if (generateMipmap)
    {
        // TODO: generate mipmap
    }
    else
    {
        auto stagingBuffer = m_renderContext.GetCurrentStagingBuffer();
        auto submitInfo    = stagingBuffer->Submit(textureInfo.baseLevelData.data(),
                                                   textureInfo.baseLevelData.size());
        // copy to image
        val::CommandBuffer* commandBuffer = m_renderContext.GetCommandBuffer();
        commandBuffer->Begin();
        commandBuffer->CopyBufferToImage(stagingBuffer, submitInfo.offset, image);
        commandBuffer->TransferLayout(image, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                      VK_IMAGE_USAGE_SAMPLED_BIT);
        stagingBuffer->Flush();
        stagingBuffer->ResetOffset();

        commandBuffer->End();
        m_renderContext.SubmitImmediate(commandBuffer);
    }

    return image;
}

TextureInfo TextureManager::LoadTexture2DFromFile(const std::string& filename)
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

    return TextureInfo{(uint32_t)width, (uint32_t)height, VK_FORMAT_R8G8B8A8_UNORM, false,
                       std::move(vecData)};
}
} // namespace zen