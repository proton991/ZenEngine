#include "Graphics/RHI/RHICommands.h"
#include "Graphics/RenderCore/V2/TextureManager.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "SceneGraph/Scene.h"
#include "AssetLib/TextureLoader.h"
#include <gli/gli.hpp>

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

rhi::TextureHandle TextureManager::CreateTextureProxy(const rhi::TextureHandle& baseTexture,
                                                      const rhi::TextureProxyInfo& proxyInfo)
{
    if (!m_textureCache.contains(proxyInfo.name))
    {
        m_textureCache[proxyInfo.name] = m_RHI->CreateTextureProxy(baseTexture, proxyInfo);
    }
    return m_textureCache.at(proxyInfo.name);
}

rhi::TextureHandle TextureManager::LoadTexture2D(const std::string& file, bool requireMipmap)
{
    if (m_textureCache.contains(file))
    {
        return m_textureCache[file];
    }
    asset::TextureInfo rawTextureInfo{};
    asset::TextureLoader::LoadTexture2DFromFile(file, &rawTextureInfo);

    rhi::TextureInfo textureInfo{};
    textureInfo.width       = rawTextureInfo.width;
    textureInfo.height      = rawTextureInfo.height;
    textureInfo.format      = rhi::DataFormat::eR8G8B8A8SRGB;
    textureInfo.type        = rhi::TextureType::e2D;
    textureInfo.depth       = 1;
    textureInfo.arrayLayers = 1;
    textureInfo.mipmaps     = requireMipmap ?
            rhi::CalculateTextureMipLevels(rawTextureInfo.width, rawTextureInfo.height) :
            1;
    textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eTransferSrc);
    textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eTransferDst);
    textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eSampled);

    rhi::TextureHandle texture = m_RHI->CreateTexture(textureInfo);

    if (requireMipmap)
    {
        UpdateTexture(texture, {textureInfo.width, textureInfo.height, textureInfo.depth},
                      rawTextureInfo.data.size(), rawTextureInfo.data.data());
        m_renderDevice->GetCurrentUploadCmdList()->GenerateTextureMipmaps(texture);
    }
    else
    {
        UpdateTexture(texture, {textureInfo.width, textureInfo.height, textureInfo.depth},
                      rawTextureInfo.data.size(), rawTextureInfo.data.data());
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
            textureInfo.name        = sgTexture->GetName();
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

void TextureManager::LoadTextureEnv(const std::string& file, EnvTexture* outTexture)
{
    if (m_textureCache.contains(file))
    {
        outTexture->skybox = m_textureCache[file];
    }

    gli::texture_cube texCube(gli::load(file.c_str()));
    uint32_t width     = static_cast<uint32_t>(texCube.extent().x);
    uint32_t height    = static_cast<uint32_t>(texCube.extent().y);
    uint32_t mipLevels = static_cast<uint32_t>(texCube.levels());

    std::vector<uint8_t> vecData;
    rhi::TextureInfo textureInfo{};
    textureInfo.width       = width;
    textureInfo.height      = height;
    textureInfo.format      = rhi::DataFormat::eR16G16B16A16SFloat;
    textureInfo.type        = rhi::TextureType::eCube;
    textureInfo.depth       = 1;
    textureInfo.arrayLayers = 6;
    textureInfo.mipmaps     = mipLevels;
    textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eTransferDst);
    textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eSampled);

    rhi::TextureHandle texture = m_RHI->CreateTexture(textureInfo);
    std::vector<rhi::BufferTextureCopyRegion> regions;
    regions.reserve(mipLevels);
    uint32_t offset = 0;
    for (uint32_t face = 0; face < 6; face++)
    {
        for (uint32_t level = 0; level < mipLevels; level++)
        {
            rhi::BufferTextureCopyRegion region{};
            region.textureSubresources.aspect.SetFlag(rhi::TextureAspectFlagBits::eColor);
            region.textureSubresources.mipmap         = level;
            region.textureSubresources.baseArrayLayer = face;
            region.textureSubresources.layerCount     = 1;
            region.textureSize  = {texCube[face][level].extent().x, texCube[face][level].extent().y,
                                   1};
            region.bufferOffset = offset;

            regions.push_back(region);
            // Increase offset into staging buffer for next level / face
            offset += texCube[face][level].size();
        }
    }

    UpdateTextureCube(texture, regions, texCube.size(),
                      static_cast<const uint8_t*>(texCube.data()));

    m_textureCache[file] = texture;

    outTexture->skybox = texture;

    m_renderDevice->GetRHIDebug()->SetTextureDebugName(outTexture->skybox, "SkyboxTexture");

    const auto irradianceTag  = outTexture->tag + "_irradiance";
    const auto prefilteredTag = outTexture->tag + "_prefiltered";
    const auto lutBRDFTag     = outTexture->tag + "_lutBRDF";

    SkyboxRenderer* skyboxRenderer = m_renderDevice->GetRendererServer()->RequestSkyboxRenderer();
    // note: only generate once
    skyboxRenderer->GenerateEnvCubemaps(outTexture);
    m_textureCache[irradianceTag]  = outTexture->irradiance;
    m_textureCache[prefilteredTag] = outTexture->prefiltered;

    skyboxRenderer->GenerateLutBRDF(outTexture);
    m_textureCache[lutBRDFTag] = outTexture->lutBRDF;
}

void TextureManager::UpdateTexture(const rhi::TextureHandle& textureHandle,
                                   const Vec3i& textureSize,
                                   uint32_t dataSize,
                                   const uint8_t* pData)
{
    rhi::RHICommandList* cmdList = m_renderDevice->GetCurrentUploadCmdList();
    // transfer layout to eTransferDst
    cmdList->ChangeTextureLayout(textureHandle, rhi::TextureLayout::eTransferDst);

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
    cmdList->ChangeTextureLayout(textureHandle, rhi::TextureLayout::eShaderReadOnly);

    if (m_stagingMgr->GetPendingFreeMemorySize() > MAX_TEXTURE_STAGING_PENDING_FREE_SIZE)
    {
        m_renderDevice->WaitForAllFrames();
        m_stagingMgr->ProcessPendingFrees();
    }
}

void TextureManager::UpdateTextureCube(const rhi::TextureHandle& textureHandle,
                                       const std::vector<rhi::BufferTextureCopyRegion>& regions,
                                       uint32_t dataSize,
                                       const uint8_t* pData)
{
    rhi::RHICommandList* cmdList = m_renderDevice->GetCurrentUploadCmdList();
    // transfer layout to eTransferDst
    cmdList->ChangeTextureLayout(textureHandle, rhi::TextureLayout::eTransferDst);

    rhi::BufferHandle stagingBuffer = m_stagingMgr->RequireBuffer(dataSize);
    // map staging buffer
    uint8_t* dataPtr = m_RHI->MapBuffer(stagingBuffer);
    // copy
    memcpy(dataPtr, pData, dataSize);
    // unmap
    m_RHI->UnmapBuffer(stagingBuffer);
    // copy to gpu memory
    cmdList->CopyBufferToTexture(stagingBuffer, textureHandle, regions);
    m_stagingMgr->ReleaseBuffer(stagingBuffer);

    // transfer layout to eShaderReadOnly
    cmdList->ChangeTextureLayout(textureHandle, rhi::TextureLayout::eShaderReadOnly);

    m_renderDevice->WaitForAllFrames();
    m_stagingMgr->ProcessPendingFrees();
}
} // namespace zen::rc