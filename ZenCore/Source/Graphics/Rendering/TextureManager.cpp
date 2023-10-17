#include "AssetLib/TextureLoader.h"
#include "Graphics/Rendering/TextureManager.h"
#include "Graphics/Rendering/RenderDevice.h"
#include "Graphics/Rendering/RenderContext.h"
#include "Common/Errors.h"
#include "Common/Helpers.h"

namespace zen
{

val::Image* TextureManager::RequestTexture2D(const std::string& filename, bool requireMipmap)
{
    TextureInfo textureInfo = TextureLoader::LoadTexture2DFromFile(filename);
    // Create Image
    val::ImageCreateInfo imageCI{};
    imageCI.format      = util::ToVkType<VkFormat>(textureInfo.format);
    imageCI.extent3D    = {textureInfo.width, textureInfo.height, 1};
    bool generateMipmap = false;
    if (requireMipmap && (!textureInfo.hasMipMap))
    {
        // generate mipmap
        generateMipmap = true;
        imageCI.usage =
            val::ImageUsage::TransferDst | val::ImageUsage::TransferSrc | val::ImageUsage::Sampled;
    }
    imageCI.usage = val::ImageUsage::TransferDst | val::ImageUsage::Sampled;
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
        commandBuffer->TransferLayout(image, val::ImageUsage::TransferDst,
                                      val::ImageUsage::Sampled);
        stagingBuffer->Flush();
        stagingBuffer->ResetOffset();

        commandBuffer->End();
        m_renderContext.SubmitImmediate(commandBuffer);
    }

    return image;
}
} // namespace zen