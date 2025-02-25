#include "AssetLib/TextureLoader.h"
#include "Graphics/RenderCore/TextureManager.h"
#include "Graphics/RenderCore/RenderDevice.h"
#include "Graphics/RenderCore/RenderContext.h"
#include "Common/Errors.h"
#include "SceneGraph/Scene.h"
#include "Common/Helpers.h"

namespace zen
{
val::Image* TextureManager::RequestTexture2D(const std::string& filename, bool requireMipmap)
{
    if (m_cache.count(filename))
    {
        return m_cache[filename].Get();
    }
    asset::TextureInfo textureInfo = asset::TextureLoader::LoadTexture2DFromFile(filename);
    // Create Image
    val::ImageCreateInfo imageCI{};
    imageCI.format      = util::ToVkType<VkFormat>(textureInfo.format);
    imageCI.extent3D    = {textureInfo.width, textureInfo.height, 1};
    bool generateMipmap = false;
    if (requireMipmap && (!textureInfo.hasMipmap))
    {
        // generate mipmap
        generateMipmap = true;
        imageCI.usage =
            val::ImageUsage::TransferDst | val::ImageUsage::TransferSrc | val::ImageUsage::Sampled;
    }
    else
    {
        imageCI.usage = val::ImageUsage::TransferDst | val::ImageUsage::Sampled;
    }
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
        auto submitInfo = stagingBuffer->Submit(textureInfo.data.data(), textureInfo.data.size());
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

void TextureManager::RegisterSceneTextures(sg::Scene* scene, bool requireMipmap)
{
    std::vector<sg::Texture*> sgTextures = scene->GetComponents<sg::Texture>();

    StagingBuffer* stagingBuffer      = m_renderContext.GetCurrentStagingBuffer();
    val::CommandBuffer* commandBuffer = m_renderContext.GetCommandBuffer();

    //    commandBuffer->Begin();
    for (auto* sgTex : sgTextures)
    {
        commandBuffer->Begin();
        // first create val::image
        val::ImageCreateInfo imageCI{};
        imageCI.format   = util::ToVkType<VkFormat>(sgTex->format);
        imageCI.extent3D = {sgTex->width, sgTex->height, 1};

        bool generateMipmap = false;
        if (requireMipmap)
        {
            generateMipmap = true;
            imageCI.usage  = val::ImageUsage::TransferDst | val::ImageUsage::TransferSrc |
                val::ImageUsage::Sampled;
        }
        else
        {
            imageCI.usage = val::ImageUsage::TransferDst | val::ImageUsage::Sampled;
        }

        if (generateMipmap)
        {
            // TODO: generate mipmap
        }

        m_cache.emplace(sgTex->GetName(), val::Image::CreateUnique(m_valDevice, imageCI));
        val::Image* image = m_cache.at(sgTex->GetName()).Get();
        image->SetObjectDebugName(sgTex->GetName());
        // copy buffer to image
        auto submitInfo = stagingBuffer->Submit(sgTex->bytesData.data(), sgTex->bytesData.size());
        commandBuffer->CopyBufferToImage(stagingBuffer, submitInfo.offset, image);
        commandBuffer->TransferLayout(image, val::ImageUsage::TransferDst,
                                      val::ImageUsage::Sampled);
        stagingBuffer->Flush();
        stagingBuffer->ResetOffset();
        commandBuffer->End();
        m_renderContext.SubmitImmediate(commandBuffer);
    }
}
} // namespace zen