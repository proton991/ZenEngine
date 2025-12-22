#include "Graphics/RHI/RHICommands.h"
#include "Graphics/RenderCore/V2/TextureManager.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "SceneGraph/Scene.h"
#include "AssetLib/TextureLoader.h"
#include "Graphics/RenderCore/V2/RenderResource.h"

#include <gli/gli.hpp>

namespace zen::rc
{
void TextureManager::Destroy()
{
    for (auto& kv : m_textureCache)
    {
        kv.second->ReleaseReference();
    }
}

// TextureHandle TextureManager::CreateTexture(const TextureInfo& textureInfo)
// {
//     if (!m_textureCache.contains(textureInfo.name))
//     {
//         m_textureCache[textureInfo.name] = m_RHI->CreateTexture(textureInfo);
//     }
//     return m_textureCache.at(textureInfo.name);
// }

// TextureHandle TextureManager::CreateTextureProxy(const TextureHandle& baseTexture,
//                                                       const TextureProxyInfo& proxyInfo)
// {
//     if (!m_textureCache.contains(proxyInfo.name))
//     {
//         m_textureCache[proxyInfo.name] = m_RHI->CreateTextureProxy(baseTexture, proxyInfo);
//         m_textureProxyMap[m_textureCache[proxyInfo.name]] = baseTexture;
//     }
//     return m_textureCache.at(proxyInfo.name);
// }

RHITexture* TextureManager::LoadTexture2D(const std::string& file, bool requireMipmap)
{
    // if (m_textureCache.contains(file))
    // {
    //     return m_textureCache[file];
    // }
    asset::TextureInfo rawTextureInfo{};
    asset::TextureLoader::LoadTexture2DFromFile(file, &rawTextureInfo);

    // TextureInfo textureInfo{};
    // textureInfo.width       = rawTextureInfo.width;
    // textureInfo.height      = rawTextureInfo.height;
    // textureInfo.format      = DataFormat::eR8G8B8A8SRGB;
    // textureInfo.type        = RHITextureType::e2D;
    // textureInfo.depth       = 1;
    // textureInfo.arrayLayers = 1;
    // textureInfo.mipmaps     = requireMipmap ?
    //         CalculateTextureMipLevels(rawTextureInfo.width, rawTextureInfo.height) :
    //         1;
    // textureInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferSrc);
    // textureInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);
    // textureInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);

    TextureFormat texFormat{};
    texFormat.format      = DataFormat::eR8G8B8A8SRGB;
    texFormat.sampleCount = SampleCount::e1;
    texFormat.dimension   = TextureDimension::e2D;
    texFormat.width       = rawTextureInfo.width;
    texFormat.height      = rawTextureInfo.height;
    texFormat.depth       = 1;
    texFormat.arrayLayers = 1;
    texFormat.mipmaps     = requireMipmap ?
            RHITexture::CalculateTextureMipLevels(rawTextureInfo.width, rawTextureInfo.height) :
            1;

    RHITexture* texture =
        m_renderDevice->CreateTextureSampled(texFormat, {.copyUsage = true}, file);

    // TextureHandle texture = m_RHI->CreateTexture(textureInfo);

    if (requireMipmap)
    {
        UpdateTexture(texture, rawTextureInfo.data.size(), rawTextureInfo.data.data());
        m_renderDevice->GetCurrentTransferCmdList()->GenerateTextureMipmaps(texture);
    }
    else
    {
        UpdateTexture(texture, rawTextureInfo.data.size(), rawTextureInfo.data.data());
    }

    m_textureCache[file] = texture;
    return texture;
}

void TextureManager::LoadSceneTextures(const sg::Scene* scene,
                                       std::vector<RHITexture*>& outTextures)
{
    std::vector<sg::Texture*> sgTextures = scene->GetComponents<sg::Texture>();
    for (sg::Texture* sgTexture : sgTextures)
    {
        // if (!m_textureCache.contains(sgTexture->GetName()))
        // {
        //     TextureInfo textureInfo{};
        //     textureInfo.format      = DataFormat::eR8G8B8A8SRGB;
        //     textureInfo.type        = RHITextureType::e2D;
        //     textureInfo.width       = sgTexture->width;
        //     textureInfo.height      = sgTexture->height;
        //     textureInfo.depth       = 1;
        //     textureInfo.arrayLayers = 1;
        //     textureInfo.mipmaps     = 1;
        //     textureInfo.name        = sgTexture->GetName();
        //     textureInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);
        //     textureInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);
        //
        //     TextureHandle texture = m_RHI->CreateTexture(textureInfo);
        //
        //     UpdateTexture(texture, {textureInfo.width, textureInfo.height, textureInfo.depth},
        //                   sgTexture->bytesData.size(), sgTexture->bytesData.data());
        //
        //     m_textureCache[sgTexture->GetName()] = texture;
        // }
        // outTextures.push_back(m_textureCache[sgTexture->GetName()]);


        TextureFormat texFormat{};
        texFormat.format      = DataFormat::eR8G8B8A8SRGB;
        texFormat.sampleCount = SampleCount::e1;
        texFormat.dimension   = TextureDimension::e2D;
        texFormat.width       = sgTexture->width;
        texFormat.height      = sgTexture->width;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        RHITexture* texture = m_renderDevice->CreateTextureSampled(texFormat, {.copyUsage = true},
                                                                   sgTexture->GetName());
        UpdateTexture(texture, sgTexture->bytesData.size(), sgTexture->bytesData.data());
        m_textureCache[sgTexture->GetName()] = texture;
        outTextures.push_back(texture);
    }
}

void TextureManager::LoadTextureEnv(const std::string& file, EnvTexture* outTexture)
{
    // if (m_textureCache.contains(file))
    // {
    //     outTexture->skybox = m_textureCache[file];
    // }

    gli::texture_cube texCube(gli::load(file.c_str()));
    uint32_t width     = static_cast<uint32_t>(texCube.extent().x);
    uint32_t height    = static_cast<uint32_t>(texCube.extent().y);
    uint32_t mipLevels = static_cast<uint32_t>(texCube.levels());

    // std::vector<uint8_t> vecData;
    // TextureInfo textureInfo{};
    // textureInfo.width       = width;
    // textureInfo.height      = height;
    // textureInfo.format      = DataFormat::eR16G16B16A16SFloat;
    // textureInfo.type        = RHITextureType::eCube;
    // textureInfo.depth       = 1;
    // textureInfo.arrayLayers = 6;
    // textureInfo.mipmaps     = mipLevels;
    // textureInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);
    // textureInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);

    TextureFormat texFormat{};
    texFormat.format      = DataFormat::eR16G16B16A16SFloat;
    texFormat.dimension   = TextureDimension::eCube;
    texFormat.width       = width;
    texFormat.height      = height;
    texFormat.depth       = 1;
    texFormat.arrayLayers = 6;
    texFormat.mipmaps     = mipLevels;

    RHITexture* texture =
        m_renderDevice->CreateTextureSampled(texFormat, {.copyUsage = true}, "env_skybox");

    // TextureHandle texture = m_RHI->CreateTexture(textureInfo);
    std::vector<RHIBufferTextureCopyRegion> regions;
    regions.reserve(mipLevels);
    uint32_t offset = 0;
    for (uint32_t face = 0; face < 6; face++)
    {
        for (uint32_t level = 0; level < mipLevels; level++)
        {
            RHIBufferTextureCopyRegion region{};
            region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
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
    m_textureCache[outTexture->irradiance->GetResourceTag()]  = outTexture->irradiance;
    m_textureCache[outTexture->prefiltered->GetResourceTag()] = outTexture->prefiltered;

    skyboxRenderer->GenerateLutBRDF(outTexture);
    m_textureCache[outTexture->lutBRDF->GetResourceTag()] = outTexture->lutBRDF;
}

void TextureManager::UpdateTexture(RHITexture* texture, uint32_t dataSize, const uint8_t* pData)
{
    RHICommandList* cmdList = m_renderDevice->GetCurrentTransferCmdList();
    // transfer layout to eTransferDst
    cmdList->ChangeTextureLayout(texture, RHITextureLayout::eTransferDst);

    RHIBuffer* stagingBuffer = m_stagingMgr->RequireBuffer(dataSize);
    // map staging buffer
    uint8_t* dataPtr = stagingBuffer->Map();
    // copy
    memcpy(dataPtr, pData, dataSize);
    // unmap
    stagingBuffer->Unmap();
    // copy to gpu memory
    RHIBufferTextureCopyRegion copyRegion{};
    copyRegion.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    copyRegion.bufferOffset  = 0;
    copyRegion.textureOffset = {0, 0, 0};
    copyRegion.textureSize   = {texture->GetWidth(), texture->GetHeight(), texture->GetDepth()};
    cmdList->CopyBufferToTexture(stagingBuffer, texture, copyRegion);
    m_stagingMgr->ReleaseBuffer(stagingBuffer);

    // transfer layout to eShaderReadOnly
    cmdList->ChangeTextureLayout(texture, RHITextureLayout::eShaderReadOnly);

    if (m_stagingMgr->GetPendingFreeMemorySize() > MAX_TEXTURE_STAGING_PENDING_FREE_SIZE)
    {
        m_renderDevice->WaitForAllFrames();
        m_stagingMgr->ProcessPendingFrees();
    }
}

void TextureManager::UpdateTextureCube(RHITexture* texture,
                                       const std::vector<RHIBufferTextureCopyRegion>& regions,
                                       uint32_t dataSize,
                                       const uint8_t* pData)
{
    RHICommandList* cmdList = m_renderDevice->GetCurrentTransferCmdList();
    // transfer layout to eTransferDst
    cmdList->ChangeTextureLayout(texture, RHITextureLayout::eTransferDst);

    RHIBuffer* stagingBuffer = m_stagingMgr->RequireBuffer(dataSize);
    // map staging buffer
    uint8_t* dataPtr = stagingBuffer->Map();
    // copy
    memcpy(dataPtr, pData, dataSize);
    // unmap
    stagingBuffer->Unmap();
    // copy to gpu memory
    cmdList->CopyBufferToTexture(stagingBuffer, texture, regions);
    m_stagingMgr->ReleaseBuffer(stagingBuffer);

    // transfer layout to eShaderReadOnly
    cmdList->ChangeTextureLayout(texture, RHITextureLayout::eShaderReadOnly);

    m_renderDevice->WaitForAllFrames();
    m_stagingMgr->ProcessPendingFrees();
}

// TextureHandle TextureManager::GetBaseTextureForProxy(const TextureHandle& handle) const
// {
//     // assume the handle is a proxy texture handle
//     return m_textureProxyMap.at(handle);
// }

// bool TextureManager::IsProxyTexture(const TextureHandle& textureHandle) const
// {
//     return m_textureProxyMap.contains(textureHandle);
// }
} // namespace zen::rc