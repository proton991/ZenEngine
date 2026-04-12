#include "AssetLib/TextureLoader.h"
#include "Graphics/RenderCore/TextureManager.h"
#include "Graphics/RenderCore/RenderDevice.h"
#include "Graphics/RenderCore/RenderContext.h"
#include "Utils/Errors.h"
#include "SceneGraph/Scene.h"
#include "Utils/Helpers.h"

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
    val::Image* pImage = m_cache.at(filename).Get();
    if (generateMipmap)
    {
        // TODO: generate mipmap
    }
    else
    {
        auto stagingBuffer = m_renderContext.GetCurrentStagingBuffer();
        auto submitInfo = stagingBuffer->Submit(textureInfo.data.data(), textureInfo.data.size());
        // copy to image
        val::CommandBuffer* pCommandBuffer = m_renderContext.GetCommandBuffer();
        pCommandBuffer->Begin();
        pCommandBuffer->CopyBufferToImage(stagingBuffer, submitInfo.offset, pImage);
        pCommandBuffer->TransferLayout(pImage, val::ImageUsage::TransferDst,
                                      val::ImageUsage::Sampled);
        stagingBuffer->Flush();
        stagingBuffer->ResetOffset();

        pCommandBuffer->End();
        m_renderContext.SubmitImmediate(pCommandBuffer);
    }

    return pImage;
}

void TextureManager::RegisterSceneTextures(sg::Scene* pScene, bool requireMipmap)
{
    std::vector<sg::Texture*> sgTextures = pScene->GetComponents<sg::Texture>();

    StagingBuffer* pStagingBuffer      = m_renderContext.GetCurrentStagingBuffer();
    val::CommandBuffer* pCommandBuffer = m_renderContext.GetCommandBuffer();

    //    commandBuffer->Begin();
    for (auto* pSgTex : sgTextures)
    {
        pCommandBuffer->Begin();
        // first create val::image
        val::ImageCreateInfo imageCI{};
        imageCI.format   = util::ToVkType<VkFormat>(pSgTex->format);
        imageCI.extent3D = {pSgTex->width, pSgTex->height, 1};

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

        m_cache.emplace(pSgTex->GetName(), val::Image::CreateUnique(m_valDevice, imageCI));
        val::Image* pImage = m_cache.at(pSgTex->GetName()).Get();
        pImage->SetObjectDebugName(pSgTex->GetName());
        // copy buffer to image
        auto submitInfo = pStagingBuffer->Submit(pSgTex->bytesData.data(), pSgTex->bytesData.size());
        pCommandBuffer->CopyBufferToImage(pStagingBuffer, submitInfo.offset, pImage);
        pCommandBuffer->TransferLayout(pImage, val::ImageUsage::TransferDst,
                                      val::ImageUsage::Sampled);
        pStagingBuffer->Flush();
        pStagingBuffer->ResetOffset();
        pCommandBuffer->End();
        m_renderContext.SubmitImmediate(pCommandBuffer);
    }
}
} // namespace zen