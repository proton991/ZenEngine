#include "Graphics/RHI/RHICommandList.h"
#include "Graphics/RenderCore/V2/TextureManager.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "SceneGraph/Scene.h"
#include "AssetLib/TextureLoader.h"
#include "Graphics/RenderCore/V2/RenderResource.h"

#include <gli/gli.hpp>

namespace zen::rc
{
void TextureManager::QueueShaderReadOnlyTransition(RHITexture* pTexture)
{
    for (RHITexture* pPendingTexture : m_pendingShaderReadOnlyTransitions)
    {
        if (pPendingTexture == pTexture)
        {
            return;
        }
    }

    m_pendingShaderReadOnlyTransitions.emplace_back(pTexture);
}

void TextureManager::Destroy()
{
    if (m_transferBatch.pCmdList != nullptr)
    {
        m_transferBatch.pCmdList  = nullptr;
        m_transferBatch.recording = false;
    }

    m_pendingShaderReadOnlyTransitions.clear();

    for (auto& kv : m_textureCache)
    {
        kv.second->ReleaseReference();
    }
}
void TextureManager::EnsureTransferBatch()
{
    if (!m_transferBatch.recording)
    {
        m_transferBatch.pCmdList  = m_pRenderDevice->GetImmediateTransferCmdList();
        m_transferBatch.recording = true;
    }
}

void TextureManager::FlushTransferBatch()
{
    if (!m_transferBatch.recording)
    {
        return;
    }

    m_pRenderDevice->SubmitImmediateTransferCmdList();

    if (!m_pendingShaderReadOnlyTransitions.empty())
    {
        // Sampled-image transitions must be recorded on a graphics-capable queue.
        RHICommandList* pGfxCmdList = m_pRenderDevice->GetImmediateGraphicsCmdList();

        for (RHITexture* pTexture : m_pendingShaderReadOnlyTransitions)
        {
            pGfxCmdList->AddTextureTransition(pTexture, RHITextureLayout::eShaderReadOnly);
        }

        RHICommandList* pCmdLists[] = {pGfxCmdList};
        GDynamicRHI->SubmitCommandList(MakeVecView(pCmdLists));
        pGfxCmdList->Reset();

        m_pendingShaderReadOnlyTransitions.clear();
    }

    m_transferBatch.pCmdList  = nullptr;
    m_transferBatch.recording = false;

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

    // TextureHandle texture = m_RHI->CreateTexture(textureInfo);

    if (requireMipmap)
    {
        UpdateTexture(pTexture, rawTextureInfo.data.size(), rawTextureInfo.data.data());
        m_transferBatch.pCmdList->GenerateTextureMipmaps(pTexture);
    }
    else
    {
        UpdateTexture(pTexture, rawTextureInfo.data.size(), rawTextureInfo.data.data());
    }

    FlushTransferBatch();

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

    FlushTransferBatch();
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
    std::vector<RHIBufferTextureCopyRegion> regions;
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

    FlushTransferBatch();

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

void TextureManager::UpdateTexture(RHITexture* pTexture, uint32_t dataSize, const uint8_t* pData)
{
    EnsureTransferBatch();

    RHICommandList* pCmdList = m_transferBatch.pCmdList;
    pCmdList->AddTextureTransition(pTexture, RHITextureLayout::eTransferDst);

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
    pCmdList->CopyBufferToTexture(pStagingBuffer, pTexture, copyRegion);
    m_pStagingMgr->ReleaseBuffer(pStagingBuffer);

    QueueShaderReadOnlyTransition(pTexture);
}

void TextureManager::UpdateTextureCube(RHITexture* pTexture,
                                       const std::vector<RHIBufferTextureCopyRegion>& regions,
                                       uint32_t dataSize,
                                       const uint8_t* pData)
{
    EnsureTransferBatch();
    RHICommandList* pCmdList = m_transferBatch.pCmdList;
    pCmdList->AddTextureTransition(pTexture, RHITextureLayout::eTransferDst);

    RHIBuffer* pStagingBuffer = m_pStagingMgr->RequireBuffer(dataSize);
    // map staging buffer
    uint8_t* pDataPtr = pStagingBuffer->Map();
    // copy
    memcpy(pDataPtr, pData, dataSize);
    // unmap
    pStagingBuffer->Unmap();
    // copy to gpu memory
    pCmdList->CopyBufferToTexture(pStagingBuffer, pTexture, regions);
    m_pStagingMgr->ReleaseBuffer(pStagingBuffer);

    QueueShaderReadOnlyTransition(pTexture);
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
