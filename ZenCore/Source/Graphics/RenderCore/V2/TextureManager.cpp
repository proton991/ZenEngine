#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/TextureManager.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "SceneGraph/Scene.h"
#include "AssetLib/TextureLoader.h"
#include "Graphics/RenderCore/V2/RenderResource.h"

#include <gli/gli.hpp>
#include <filesystem>

namespace zen::rc
{
void TextureManager::Destroy()
{
    m_pUploadQueue->Flush();

    for (HashMap<uint64_t, RHITexture*>::value_type& owned : m_ownedTextures)
    {
        m_pRenderDevice->DestroyTexture(owned.second);
    }

    m_textureCache.clear();
    m_ownedTextures.clear();
}

void TextureManager::OwnTexture(RHITexture* texture)
{
    if (texture != nullptr)
    {
        m_ownedTextures.try_emplace(texture->GetStableId(), texture);
    }
}

void TextureManager::FlushPendingTextureUpdates()
{
    m_pUploadQueue->Flush();
}

RHITexture* TextureManager::LoadTexture2D(const std::string& file, bool requireMipmap)
{
    RHITexture* result{};

    const NameID key(std::filesystem::path(file).lexically_normal().generic_string());

    if (HashMap<NameID, std::array<RHITexture*, 2>>::iterator it = m_textureCache.find(key);
        it != m_textureCache.end() && it->second[requireMipmap] != nullptr)
    {
        result = it->second[requireMipmap];
    }
    else
    {
        asset::TextureInfo rawTextureInfo{};
        asset::TextureLoader::LoadTexture2DFromFile(file, &rawTextureInfo);

        if (rawTextureInfo.width == 0 || rawTextureInfo.height == 0 || rawTextureInfo.data.empty())
        {
            LOGE("Failed to load texture '{}'", file);

            result = nullptr;
        }
        else
        {
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

            if (pTexture == nullptr)
            {
                result = nullptr;
            }
            else
            {
                OwnTexture(pTexture);
                UpdateTexture(pTexture, rawTextureInfo.data.size(), rawTextureInfo.data.data(),
                              requireMipmap);

                m_textureCache[key][requireMipmap] = pTexture;
                result                             = pTexture;
            }
        }
    }

    return result;
}

void TextureManager::LoadSceneTextures(const sg::Scene* pScene,
                                       std::vector<RHITexture*>& outTextures)
{
    if (pScene == nullptr)
    {
        LOGE("Cannot load textures from a null scene");
        return;
    }

    std::vector<sg::Texture*> sgTextures = pScene->GetComponents<sg::Texture>();

    for (sg::Texture* pSgTexture : sgTextures)
    {
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

        if (pTexture == nullptr)
        {
            outTextures.push_back(nullptr);
            continue;
        }

        OwnTexture(pTexture);
        UpdateTexture(pTexture, pSgTexture->bytesData.size(), pSgTexture->bytesData.data());
        outTextures.push_back(pTexture);
    }
}

void TextureManager::LoadTextureEnv(const std::string& file, EnvTexture* pOutTexture)
{
    RendererServer* server = m_pRenderDevice->GetRendererServer();

    if (pOutTexture == nullptr || server == nullptr || server->RequestSkyboxRenderer() == nullptr)
    {
        LOGE("Environment loading requires an output and skybox renderer");
    }
    else
    {
        gli::texture_cube texCube(gli::load(file.c_str()));

        if (texCube.empty())
        {
            LOGE("Failed to load environment '{}'", file);
        }
        else
        {
            uint32_t width     = static_cast<uint32_t>(texCube.extent().x);
            uint32_t height    = static_cast<uint32_t>(texCube.extent().y);
            uint32_t mipLevels = static_cast<uint32_t>(texCube.levels());

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

            if (pTexture != nullptr)
            {
                OwnTexture(pTexture);
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
                        region.textureSize  = {texCube[face][level].extent().x,
                                               texCube[face][level].extent().y, 1};
                        region.bufferOffset = offset;

                        regions.push_back(region);
                        offset += texCube[face][level].size();
                    }
                }

                UpdateTextureCube(pTexture, regions, texCube.size(),
                                  static_cast<const uint8_t*>(texCube.data()));

                pOutTexture->pSkybox = pTexture;

                if (RHIDebug* debug = m_pRenderDevice->GetRHIDebug())
                {
                    GetRHIThread().Invoke(&RHIDebug::SetTextureDebugName, debug,
                                          pOutTexture->pSkybox, pTexture->GetBaseInfo().tag);
                }

                SkyboxRenderer* pSkyboxRenderer =
                    m_pRenderDevice->GetRendererServer()->RequestSkyboxRenderer();
                pSkyboxRenderer->PreprocessEnvTexture(pOutTexture);

                OwnTexture(pOutTexture->pIrradiance);
                OwnTexture(pOutTexture->pPrefiltered);
                OwnTexture(pOutTexture->pLutBRDF);
            }
        }
    }
}

void TextureManager::UpdateTexture(RHITexture* texture,
                                   uint32_t dataSize,
                                   const uint8_t* data,
                                   bool generateMipmaps)
{
    RHIBufferTextureCopyRegion region{};
    region.textureSubresources.aspect     = texture->GetSubResourceRange().aspect;
    region.textureSubresources.layerCount = texture->GetArrayLayers();
    region.textureSize = {texture->GetWidth(), texture->GetHeight(), texture->GetDepth()};
    m_pUploadQueue->EnqueueTexture(texture, MakeVecView(&region, 1), dataSize, data,
                                   generateMipmaps);
}

void TextureManager::UpdateTextureCube(RHITexture* texture,
                                       const HeapVector<RHIBufferTextureCopyRegion>& regions,
                                       uint32_t dataSize,
                                       const uint8_t* data)
{
    HeapVector<RHIBufferTextureCopyRegion> copy = regions;
    m_pUploadQueue->EnqueueTexture(texture, copy, dataSize, data);
}
} // namespace zen::rc
