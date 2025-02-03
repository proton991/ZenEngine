#include "Graphics/RHI/RHICommands.h"
#include "Graphics/RenderCore/V2/TextureManager.h"
#include "SceneGraph/Scene.h"
#include "stb_image.h"

namespace zen::rc
{
void TextureManager::Destroy()
{
    for (auto& kv : m_textureCache)
    {
        m_RHI->DestroyTexture(kv.second);
    }
}

rhi::TextureHandle TextureManager::CreateTexture(const rhi::TextureInfo& textureInfo,
                                                 const std::string& tag)
{
    if (!m_textureCache.contains(tag))
    {
        m_textureCache[tag] = m_RHI->CreateTexture(textureInfo);
    }
    return m_textureCache.at(tag);
}

rhi::TextureHandle TextureManager::LoadTexture2D(const std::string& file, bool requireMipmap)
{
    if (m_textureCache.contains(file))
    {
        return m_textureCache[file];
    }
    // TODO: Refactor AssetLib
    const std::string filepath = ZEN_TEXTURE_PATH + file;
    int width = 0, height = 0, channels = 0;
    stbi_set_flip_vertically_on_load(true);
    uint8_t* data = stbi_load(filepath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    stbi_set_flip_vertically_on_load(false);

    std::vector<uint8_t> vecData;
    // ONLY support RGBA format
    vecData.resize(width * height * 4 * sizeof(uint8_t));
    std::copy(data, data + vecData.size(), vecData.begin());
    stbi_image_free(data);

    rhi::TextureInfo textureInfo{};
    textureInfo.format      = rhi::DataFormat::eR8G8B8A8SRGB;
    textureInfo.type        = rhi::TextureType::e2D;
    textureInfo.width       = width;
    textureInfo.height      = height;
    textureInfo.depth       = 1;
    textureInfo.arrayLayers = 1;
    textureInfo.mipmaps     = 1;
    textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eTransferDst);
    textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eSampled);

    rhi::TextureHandle texture = m_RHI->CreateTexture(textureInfo);

    if (requireMipmap)
    {
        // TODO: generate mipmaps
    }
    else
    {
        UpdateTexture(texture, {textureInfo.width, textureInfo.height, textureInfo.depth},
                      vecData.size(), vecData.data());
    }

    m_textureCache[file] = texture;
    return texture;
}

void TextureManager::LoadSceneTextures(const sg::Scene* scene,
                                       std::vector<rhi::TextureHandle>& outTextures)
{
    std::vector<sg::Texture*> sgTextures = scene->GetComponents<sg::Texture>();
    for (sg::Texture* sgTexture : sgTextures)
    {
        if (!m_textureCache.contains(sgTexture->GetName()))
        {
            rhi::TextureInfo textureInfo{};
            textureInfo.format      = rhi::DataFormat::eR8G8B8A8SRGB;
            textureInfo.type        = rhi::TextureType::e2D;
            textureInfo.width       = sgTexture->width;
            textureInfo.height      = sgTexture->height;
            textureInfo.depth       = 1;
            textureInfo.arrayLayers = 1;
            textureInfo.mipmaps     = 1;
            textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eTransferDst);
            textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eSampled);

            rhi::TextureHandle texture = m_RHI->CreateTexture(textureInfo);

            UpdateTexture(texture, {textureInfo.width, textureInfo.height, textureInfo.depth},
                          sgTexture->bytesData.size(), sgTexture->bytesData.data());

            m_textureCache[sgTexture->GetName()] = texture;
        }
        outTextures.push_back(m_textureCache[sgTexture->GetName()]);
    }
}

void TextureManager::UpdateTexture(const rhi::TextureHandle& textureHandle,
                                   const Vec3i& textureSize,
                                   uint32_t dataSize,
                                   const uint8_t* pData)
{
    rhi::RHICommandList* cmdList = m_RHI->GetImmediateCommandList();
    cmdList->BeginUpload();
    // transfer layout to eTransferDst
    m_RHI->ChangeTextureLayout(cmdList, textureHandle, rhi::TextureLayout::eUndefined,
                               rhi::TextureLayout::eTransferDst);

    rhi::BufferHandle stagingBuffer = m_stagingMgr->RequireBuffer(dataSize);
    // map staging buffer
    uint8_t* dataPtr = m_RHI->MapBuffer(stagingBuffer);
    // copy
    memcpy(dataPtr, pData, dataSize);
    // unmap
    m_RHI->UnmapBuffer(stagingBuffer);
    // copy to gpu memory
    rhi::BufferTextureCopyRegion copyRegion{};
    copyRegion.textureSubresources.aspect.SetFlag(rhi::TextureAspectFlagBits::eColor);
    copyRegion.bufferOffset  = 0;
    copyRegion.textureOffset = {0, 0, 0};
    copyRegion.textureSize   = textureSize;
    cmdList->CopyBufferToTexture(stagingBuffer, textureHandle, copyRegion);
    m_stagingMgr->ReleaseBuffer(stagingBuffer);

    // transfer layout to eShaderReadOnly
    m_RHI->ChangeTextureLayout(cmdList, textureHandle, rhi::TextureLayout::eTransferDst,
                               rhi::TextureLayout::eShaderReadOnly);

    cmdList->EndUpload();

    if (m_stagingMgr->GetPendingFreeMemorySize() > MAX_TEXTURE_STAGING_PENDING_FREE_SIZE)
    {
        m_renderDevice->WaitForAllFrames();
    }
}
} // namespace zen::rc