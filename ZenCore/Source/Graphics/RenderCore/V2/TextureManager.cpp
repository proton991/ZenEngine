#include "Graphics/RenderCore/V2/RenderGraph.h"
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
    m_pendingTextureUpdates.clear();

    for (auto& kv : m_textureCache)
    {
        kv.second->ReleaseReference();
    }
}

void TextureManager::FlushPendingTextureUpdates()
{
    if (m_pendingTextureUpdates.empty())
    {
        return;
    }

    RenderGraph uploadGraph("texture_upload");
    uploadGraph.Begin();

    HeapVector<RHIBufferTextureCopySource> copySources;
    for (PendingTextureUpdate& update : m_pendingTextureUpdates)
    {
        if (update.useMultipleRegions)
        {
            copySources.clear();
            copySources.reserve(update.copyRegions.size());
            for (const RHIBufferTextureCopyRegion& region : update.copyRegions)
            {
                copySources.push_back({update.pStagingBuffer, region});
            }
            uploadGraph.AddTextureUpdateNode(update.pTexture, MakeVecView(copySources));
        }
        else
        {
            RHIBufferTextureCopySource copySource{update.pStagingBuffer, update.copyRegion};
            uploadGraph.AddTextureUpdateNode(update.pTexture, MakeVecView(&copySource, 1));
        }

        if (update.generateMipmaps)
        {
            uploadGraph.AddTextureMipmapGenNode(update.pTexture);
        }
    }

    uploadGraph.End();
    uploadGraph.Execute(m_pRenderDevice->GetImmediateTransferCmdList());

    for (const PendingTextureUpdate& update : m_pendingTextureUpdates)
    {
        m_pStagingMgr->ReleaseBuffer(update.pStagingBuffer);
    }
    m_pRenderDevice->SubmitImmediateTransferCmdList();

    m_pendingTextureUpdates.clear();
    m_pStagingMgr->ProcessPendingFrees();
}

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

    RHITexture* pTexture =
        m_pRenderDevice->CreateTextureSampled(texFormat, {.copyUsage = true}, file);

    UpdateTexture(pTexture, rawTextureInfo.data.size(), rawTextureInfo.data.data(), requireMipmap);

    m_textureCache[file] = pTexture;
    return pTexture;
}

void TextureManager::LoadSceneTextures(const sg::Scene* pScene,
                                       std::vector<RHITexture*>& outTextures)
{
    std::vector<sg::Texture*> sgTextures = pScene->GetComponents<sg::Texture>();
    for (sg::Texture* pSgTexture : sgTextures)
    {
        // if (!m_textureCache.contains(pSgTexture->GetName()))
        // {
        //     TextureInfo textureInfo{};
        //     textureInfo.format      = DataFormat::eR8G8B8A8SRGB;
        //     textureInfo.type        = RHITextureType::e2D;
        //     textureInfo.width       = pSgTexture->width;
        //     textureInfo.height      = pSgTexture->height;
        //     textureInfo.depth       = 1;
        //     textureInfo.arrayLayers = 1;
        //     textureInfo.mipmaps     = 1;
        //     textureInfo.name        = pSgTexture->GetName();
        //     textureInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);
        //     textureInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);
        //
        //     TextureHandle texture = m_RHI->CreateTexture(textureInfo);
        //
        //     UpdateTexture(texture, {textureInfo.width, textureInfo.height, textureInfo.depth},
        //                   pSgTexture->bytesData.size(), pSgTexture->bytesData.data());
        //
        //     m_textureCache[pSgTexture->GetName()] = texture;
        // }
        // outTextures.push_back(m_textureCache[pSgTexture->GetName()]);


        TextureFormat texFormat{};
        texFormat.format      = DataFormat::eR8G8B8A8SRGB;
        texFormat.sampleCount = SampleCount::e1;
        texFormat.dimension   = TextureDimension::e2D;
        texFormat.width       = pSgTexture->width;
        texFormat.height      = pSgTexture->height;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        RHITexture* pTexture = m_pRenderDevice->CreateTextureSampled(texFormat, {.copyUsage = true},
                                                                     pSgTexture->GetName());
        UpdateTexture(pTexture, pSgTexture->bytesData.size(), pSgTexture->bytesData.data());
        m_textureCache[pSgTexture->GetName()] = pTexture;
        outTextures.push_back(pTexture);
    }
}

void TextureManager::LoadTextureEnv(const std::string& file, EnvTexture* pOutTexture)
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

    RHITexture* pTexture =
        m_pRenderDevice->CreateTextureSampled(texFormat, {.copyUsage = true}, "env_skybox");

    // TextureHandle texture = m_RHI->CreateTexture(textureInfo);
    HeapVector<RHIBufferTextureCopyRegion> regions;
    regions.reserve(6 * mipLevels);
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

    UpdateTextureCube(pTexture, regions, texCube.size(),
                      static_cast<const uint8_t*>(texCube.data()));

    m_textureCache[file] = pTexture;

    pOutTexture->pSkybox = pTexture;

    m_pRenderDevice->GetRHIDebug()->SetTextureDebugName(pOutTexture->pSkybox,
                                                        pTexture->GetBaseInfo().tag);

    SkyboxRenderer* pSkyboxRenderer = m_pRenderDevice->GetRendererServer()->RequestSkyboxRenderer();
    // note: only generate once
    pSkyboxRenderer->PreprocessEnvTexture(pOutTexture);

    m_textureCache[pOutTexture->pIrradiance->GetResourceTag()]  = pOutTexture->pIrradiance;
    m_textureCache[pOutTexture->pPrefiltered->GetResourceTag()] = pOutTexture->pPrefiltered;
    m_textureCache[pOutTexture->pLutBRDF->GetResourceTag()]     = pOutTexture->pLutBRDF;
}

void TextureManager::UpdateTexture(RHITexture* pTexture,
                                   uint32_t dataSize,
                                   const uint8_t* pData,
                                   bool generateMipmaps)
{
    RHIBuffer* pStagingBuffer = m_pStagingMgr->RequireBuffer(dataSize);
    // map staging buffer
    uint8_t* pDataPtr = pStagingBuffer->Map();
    // copy
    memcpy(pDataPtr, pData, dataSize);
    // unmap
    pStagingBuffer->Unmap();
    // copy to gpu memory
    RHIBufferTextureCopyRegion copyRegion{};
    copyRegion.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    copyRegion.bufferOffset  = 0;
    copyRegion.textureOffset = {0, 0, 0};
    copyRegion.textureSize   = {pTexture->GetWidth(), pTexture->GetHeight(), pTexture->GetDepth()};

    m_pendingTextureUpdates.push_back(PendingTextureUpdate{
        pStagingBuffer,
        pTexture,
        copyRegion,
        {},
        false,
        generateMipmaps,
    });
}

void TextureManager::UpdateTextureCube(RHITexture* pTexture,
                                       const HeapVector<RHIBufferTextureCopyRegion>& regions,
                                       uint32_t dataSize,
                                       const uint8_t* pData)
{
    RHIBuffer* pStagingBuffer = m_pStagingMgr->RequireBuffer(dataSize);
    // map staging buffer
    uint8_t* pDataPtr = pStagingBuffer->Map();
    // copy
    memcpy(pDataPtr, pData, dataSize);
    // unmap
    pStagingBuffer->Unmap();

    PendingTextureUpdate update{};
    update.pStagingBuffer = pStagingBuffer;
    update.pTexture       = pTexture;
    update.copyRegions.reserve(regions.size());
    for (const RHIBufferTextureCopyRegion& region : regions)
    {
        update.copyRegions.push_back(region);
    }
    update.useMultipleRegions = true;
    m_pendingTextureUpdates.push_back(std::move(update));
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
