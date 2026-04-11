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
void TextureManager::QueueShaderReadOnlyTransition(RHITexture* texture)
{
    for (RHITexture* pendingTexture : m_pendingShaderReadOnlyTransitions)
    {
        if (pendingTexture == texture)
        {
            return;
        }
    }

    m_pendingShaderReadOnlyTransitions.emplace_back(texture);
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
        m_transferBatch.pCmdList  = m_renderDevice->GetTransferCmdList();
        m_transferBatch.recording = true;
    }
}

void TextureManager::FlushTransferBatch()
{
    if (!m_transferBatch.recording)
    {
        return;
    }

    m_renderDevice->SubmitTransferCmdList();

    if (!m_pendingShaderReadOnlyTransitions.empty())
    {
        // Sampled-image transitions must be recorded on a graphics-capable queue.
        RHICommandList* gfxCmdList =
            RHICommandList::Create(GDynamicRHI->GetCommandContext(RHICommandContextType::eGraphics));

        for (RHITexture* texture : m_pendingShaderReadOnlyTransitions)
        {
            gfxCmdList->AddTextureTransition(texture, RHITextureLayout::eShaderReadOnly);
        }

        RHICommandList* cmdLists[] = {gfxCmdList};
        GDynamicRHI->SubmitCommandList(MakeVecView(cmdLists));
        gfxCmdList->WaitUntilCompleted();
        ZEN_DELETE(gfxCmdList);

        m_pendingShaderReadOnlyTransitions.clear();
    }

    m_transferBatch.pCmdList  = nullptr;
    m_transferBatch.recording = false;

    m_stagingMgr->ProcessPendingFrees();
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

    RHITexture* texture =
        m_renderDevice->CreateTextureSampled(texFormat, {.copyUsage = true}, file);

    // TextureHandle texture = m_RHI->CreateTexture(textureInfo);

    if (requireMipmap)
    {
        UpdateTexture(texture, rawTextureInfo.data.size(), rawTextureInfo.data.data());
        m_transferBatch.pCmdList->GenerateTextureMipmaps(texture);
    }
    else
    {
        UpdateTexture(texture, rawTextureInfo.data.size(), rawTextureInfo.data.data());
    }

    FlushTransferBatch();

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
        texFormat.height      = sgTexture->height;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        RHITexture* texture = m_renderDevice->CreateTextureSampled(texFormat, {.copyUsage = true},
                                                                   sgTexture->GetName());
        UpdateTexture(texture, sgTexture->bytesData.size(), sgTexture->bytesData.data());
        m_textureCache[sgTexture->GetName()] = texture;
        outTextures.push_back(texture);
    }

    FlushTransferBatch();
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

    UpdateTextureCube(texture, regions, texCube.size(),
                      static_cast<const uint8_t*>(texCube.data()));

    FlushTransferBatch();

    m_textureCache[file] = texture;

    outTexture->skybox = texture;

    m_renderDevice->GetRHIDebug()->SetTextureDebugName(outTexture->skybox,
                                                       texture->GetBaseInfo().tag);

    SkyboxRenderer* skyboxRenderer = m_renderDevice->GetRendererServer()->RequestSkyboxRenderer();
    // note: only generate once
    skyboxRenderer->PreprocessEnvTexture(outTexture);

    m_textureCache[outTexture->irradiance->GetResourceTag()]  = outTexture->irradiance;
    m_textureCache[outTexture->prefiltered->GetResourceTag()] = outTexture->prefiltered;
    m_textureCache[outTexture->lutBRDF->GetResourceTag()]     = outTexture->lutBRDF;
}

void TextureManager::UpdateTexture(RHITexture* texture, uint32_t dataSize, const uint8_t* pData)
{
    EnsureTransferBatch();

    RHICommandList* cmdList = m_transferBatch.pCmdList;
    cmdList->AddTextureTransition(texture, RHITextureLayout::eTransferDst);

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

    QueueShaderReadOnlyTransition(texture);
}

void TextureManager::UpdateTextureCube(RHITexture* texture,
                                       const std::vector<RHIBufferTextureCopyRegion>& regions,
                                       uint32_t dataSize,
                                       const uint8_t* pData)
{
    EnsureTransferBatch();
    RHICommandList* cmdList = m_transferBatch.pCmdList;
    cmdList->AddTextureTransition(texture, RHITextureLayout::eTransferDst);

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

    QueueShaderReadOnlyTransition(texture);
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
