#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/TextureManager.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "SceneGraph/Scene.h"
#include "AssetLib/TextureLoader.h"
#include "Graphics/RenderCore/V2/RenderResource.h"

#include <gli/gli.hpp>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <cctype>

namespace zen::rc
{
namespace
{
uint32_t ReadContainerWord(const HeapVector<char>& bytes, size_t offset)
{
    uint32_t value = 0;

    if (offset <= bytes.size() && bytes.size() - offset >= 4)
    {
        // Both accepted container encodings use little-endian 32-bit words.
        for (uint32_t byte = 0; byte < 4; ++byte)
        {
            value |= uint32_t(static_cast<unsigned char>(bytes[offset + byte])) << (byte * 8);
        }
    }

    return value;
}

bool ValidCubeExtent(uint32_t width, uint32_t height, uint32_t levels)
{
    uint32_t maxLevels = 0;

    for (uint32_t extent = width; extent > 0; extent >>= 1)
    {
        ++maxLevels;
    }

    return width > 0 && width <= 4096 && height == width && levels > 0 && levels <= maxLevels;
}

// GLI assumes valid headers/payloads and asserts or reads past truncated input.
// Admit only the floating-point, non-array cubes this loader actually supports.
bool ValidKTXEnvironment(const HeapVector<char>& bytes)
{
    constexpr unsigned char signature[] = {0xab, 'K', 'T', 'X', ' ', '1', '1', 0xbb, 13, 10, 26, 10};

    bool valid                          = bytes.size() >= 64 && std::memcmp(bytes.data(), signature, sizeof(signature)) == 0;

    if (valid)
    {
        const uint32_t width  = ReadContainerWord(bytes, 36);

        const uint32_t levels = std::max(1u, ReadContainerWord(bytes, 56));

        // Accept GLI's legacy writer too: it stores texel size instead of component size.
        const uint32_t typeSize = ReadContainerWord(bytes, 20);

        const bool half         = ReadContainerWord(bytes, 16) == 0x140b && (typeSize == 2 || typeSize == 8)
                       && ReadContainerWord(bytes, 28) == 0x881a;

        const bool full = ReadContainerWord(bytes, 16) == 0x1406 && (typeSize == 4 || typeSize == 16)
                       && ReadContainerWord(bytes, 28) == 0x8814;

        valid = ReadContainerWord(bytes, 12) == 0x04030201 && (half || full) && ReadContainerWord(bytes, 24) == 0x1908
             && ReadContainerWord(bytes, 32) == 0x1908 && ValidCubeExtent(width, ReadContainerWord(bytes, 40), levels)
             && ReadContainerWord(bytes, 44) == 0 && ReadContainerWord(bytes, 48) == 0 && ReadContainerWord(bytes, 52) == 6;

        size_t offset = size_t(64) + ReadContainerWord(bytes, 60);

        for (uint32_t level = 0; valid && level < levels; ++level)
        {
            const size_t extent       = std::max(1u, width >> level);

            const size_t faceBytes    = extent * extent * (half ? 8 : 16);

            const uint32_t imageBytes = ReadContainerWord(bytes, offset);

            // Standard KTX stores one face size; GLI's writer stores the six-face sum.
            valid = offset <= bytes.size() && bytes.size() - offset >= 4
                 && (imageBytes == faceBytes || imageBytes == faceBytes * 6);

            offset += 4;

            valid   = valid && offset <= bytes.size() && bytes.size() - offset >= faceBytes * 6;

            offset += faceBytes * 6;
        }
    }

    return valid;
}

bool ValidDDSEnvironment(const HeapVector<char>& bytes)
{
    bool valid = bytes.size() >= 128 && std::memcmp(bytes.data(), "DDS ", 4) == 0 && ReadContainerWord(bytes, 4) == 124
              && ReadContainerWord(bytes, 76) == 32 && ReadContainerWord(bytes, 80) == 4
              && (ReadContainerWord(bytes, 112) & 0x200000) == 0;

    if (valid)
    {
        const uint32_t width  = ReadContainerWord(bytes, 16);

        const uint32_t levels = (ReadContainerWord(bytes, 8) & 0x20000) != 0 ? ReadContainerWord(bytes, 28) : 1;

        const uint32_t fourCC = ReadContainerWord(bytes, 84);

        const uint32_t caps   = ReadContainerWord(bytes, 112);

        bool cube             = (caps & 0xfe00) == 0xfe00;

        size_t pixelBytes     = fourCC == 113 ? 8 : fourCC == 116 ? 16 : 0;

        size_t offset         = 128;

        if (fourCC == 0x30315844) // DX10 header
        {
            const uint32_t format = ReadContainerWord(bytes, 128);

            pixelBytes            = format == 10 ? 8 : format == 2 ? 16 : 0;

            // A legacy cube flag takes precedence in GLI; require all its faces.
            cube   = (caps & 0x200) != 0 ? cube : (ReadContainerWord(bytes, 136) & 4) != 0;

            valid  = bytes.size() >= 148 && ReadContainerWord(bytes, 132) == 3 && ReadContainerWord(bytes, 140) == 1;

            offset = 148;
        }

        valid = valid && cube && pixelBytes != 0 && ValidCubeExtent(width, ReadContainerWord(bytes, 12), levels);

        for (uint32_t level = 0; valid && level < levels; ++level)
        {
            const size_t extent  = std::max(1u, width >> level);

            offset              += extent * extent * pixelBytes * 6;
        }

        valid = valid && offset == bytes.size();
    }

    return valid;
}
} // namespace

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

void TextureManager::OwnTexture(RHITexture*& texture)
{
    if (texture != nullptr)
    {
        m_ownedTextures.try_emplace(texture->GetStableId(), texture);
    }
}

bool TextureManager::ReleaseSceneTexture(RHITexture* texture)
{
    bool released = false;

    if (texture != nullptr)
    {
        bool shared = false;

        for (const HashMap<NameID, std::array<RHITexture*, 2>>::value_type& cached : m_textureCache)
        {
            shared |= cached.second[0] == texture || cached.second[1] == texture;
        }

        const HashMap<uint64_t, RHITexture*>::iterator owned = m_ownedTextures.find(texture->GetStableId());

        if (!shared && owned != m_ownedTextures.end())
        {
            m_ownedTextures.erase(owned);

            m_pRenderDevice->DestroyTexture(texture);

            released = true;
        }
    }

    return released;
}

void TextureManager::OwnEnvironmentTextures(EnvTexture* environment)
{
    OwnTexture(environment->pSkybox);

    OwnTexture(environment->pIrradiance);

    OwnTexture(environment->pPrefiltered);

    OwnTexture(environment->pLutBRDF);
}

void TextureManager::ReleaseSceneEnvironment(EnvTexture* environment)
{
    if (environment != nullptr)
    {
        RendererServer* server = m_pRenderDevice->GetRendererServer();

        if (server != nullptr && server->RequestSkyboxRenderer() != nullptr)
        {
            server->RequestSkyboxRenderer()->CancelEnvironmentPreprocessing(environment);
        }

        RHITexture* textures[] = {environment->pSkybox, environment->pIrradiance, environment->pPrefiltered,
                                  environment->pLutBRDF};

        for (size_t index = 0; index < std::size(textures); ++index)
        {
            if (std::find(textures, textures + index, textures[index]) == textures + index)
            {
                ReleaseSceneTexture(textures[index]);
            }
        }

        *environment = {};
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

            texFormat.mipmaps =
                requireMipmap ? RHITexture::CalculateTextureMipLevels(rawTextureInfo.width, rawTextureInfo.height) : 1;

            RHITexture* pTexture = m_pRenderDevice->CreateTextureSampled(texFormat, {.copyUsage = true}, file);

            if (pTexture == nullptr)
            {
                result = nullptr;
            }
            else
            {
                OwnTexture(pTexture);

                UpdateTexture(pTexture, rawTextureInfo.data.size(), rawTextureInfo.data.data(), requireMipmap);

                m_textureCache[key][requireMipmap] = pTexture;

                result                             = pTexture;
            }
        }
    }

    return result;
}

void TextureManager::LoadSceneTextures(const sg::Scene* pScene, HeapVector<RHITexture*>& outTextures)
{
    if (pScene == nullptr)
    {
        LOGE("Cannot load textures from a null scene");
    }
    else
    {
        for (const sg::Texture* pSgTexture : pScene->GetComponents<sg::Texture>())
        {
            TextureFormat texFormat{};

            // Both format enums use Vulkan values. Preserve linear material data and sRGB color.
            texFormat.format      = static_cast<DataFormat>(pSgTexture->format);

            texFormat.sampleCount = SampleCount::e1;

            texFormat.dimension   = TextureDimension::e2D;

            texFormat.width       = pSgTexture->width;

            texFormat.height      = pSgTexture->height;

            texFormat.depth       = 1;

            texFormat.arrayLayers = 1;

            texFormat.mipmaps     = pSgTexture->mipBytes.empty()
                                      ? RHITexture::CalculateTextureMipLevels(pSgTexture->width, pSgTexture->height)
                                      : static_cast<uint32_t>(pSgTexture->mipBytes.size());

            RHITexture* pTexture = m_pRenderDevice->CreateTextureSampled(texFormat, {.copyUsage = true}, pSgTexture->GetName());

            if (pTexture != nullptr)
            {
                OwnTexture(pTexture);

                if (pSgTexture->mipBytes.empty())
                {
                    UpdateTexture(pTexture, pSgTexture->bytesData.size(), pSgTexture->bytesData.data(), texFormat.mipmaps > 1);
                }
                else
                {
                    HeapVector<RHIBufferTextureCopyRegion> regions;

                    HeapVector<uint8_t> pixels;

                    for (uint32_t level = 0; level < texFormat.mipmaps; ++level)
                    {
                        RHIBufferTextureCopyRegion region{};

                        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

                        region.textureSubresources.mipmap     = level;

                        region.textureSubresources.layerCount = 1;

                        region.textureSize  = {std::max(1u, texFormat.width >> level), std::max(1u, texFormat.height >> level),
                                               1};

                        region.bufferOffset = static_cast<uint32_t>(pixels.size());

                        regions.push_back(region);

                        for (uint8_t value : pSgTexture->mipBytes[level])
                        {
                            pixels.push_back(value);
                        }
                    }

                    UpdateTextureCube(pTexture, regions, static_cast<uint32_t>(pixels.size()), pixels.data());
                }
            }

            outTextures.push_back(pTexture);
        }
    }
}

bool TextureManager::LoadTextureEnv(const std::string& file, EnvTexture* pOutTexture, bool fallbackToBlack)
{
    RendererServer* server = m_pRenderDevice->GetRendererServer();

    if (pOutTexture == nullptr || server == nullptr || server->RequestSkyboxRenderer() == nullptr)
    {
        LOGE("Environment loading requires an output and skybox renderer");
    }
    else
    {
        gli::texture_cube texCube;

        std::string extension = std::filesystem::u8path(file).extension().string();

        for (char& character : extension)
        {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }

        if (extension == ".hdr")
        {
            uint32_t size = 0;

            HeapVector<Vec4> pixels;

            std::string error;

            if (asset::TextureLoader::LoadHDRCubemap(file, size, pixels, error))
            {
                texCube = gli::texture_cube(gli::FORMAT_RGBA32_SFLOAT_PACK32, gli::texture_cube::extent_type(size), 1);

                std::memcpy(texCube.data(), pixels.data(), pixels.size() * sizeof(Vec4));
            }
            else
            {
                LOGW("Environment '{}': {}", file, error);
            }
        }
        else if (extension == ".ktx" || extension == ".dds")
        {
            std::ifstream stream(std::filesystem::u8path(file), std::ios::binary | std::ios::ate);

            const std::streamoff length = stream ? static_cast<std::streamoff>(stream.tellg()) : 0;

            if (length > 0 && length <= 256 * 1024 * 1024)
            {
                HeapVector<char> bytes(static_cast<size_t>(length));

                stream.seekg(0);

                stream.read(bytes.data(), length);

                gli::texture loaded;

                if (stream && extension == ".ktx" && ValidKTXEnvironment(bytes))
                {
                    loaded = gli::load_ktx(bytes.data(), bytes.size());
                }
                else if (stream && extension == ".dds" && ValidDDSEnvironment(bytes))
                {
                    loaded = gli::load_dds(bytes.data(), bytes.size());
                }

                if (!loaded.empty() && loaded.target() == gli::TARGET_CUBE
                    && (loaded.format() == gli::FORMAT_RGBA16_SFLOAT_PACK16
                        || loaded.format() == gli::FORMAT_RGBA32_SFLOAT_PACK32))
                {
                    texCube = gli::texture_cube(loaded);
                }
            }
        }

        if (texCube.empty() && fallbackToBlack)
        {
            LOGW("Environment '{}' is missing or unsupported; using black", file);

            texCube = gli::texture_cube(gli::FORMAT_RGBA16_SFLOAT_PACK16, gli::texture_cube::extent_type(1), 1);

            std::memset(texCube.data(), 0, texCube.size());
        }

        if (!texCube.empty())
        {
            uint32_t width     = static_cast<uint32_t>(texCube.extent().x);

            uint32_t height    = static_cast<uint32_t>(texCube.extent().y);

            uint32_t mipLevels = static_cast<uint32_t>(texCube.levels());

            TextureFormat texFormat{};

            texFormat.format      = texCube.format() == gli::FORMAT_RGBA32_SFLOAT_PACK32 ? DataFormat::eR32G32B32A32SFloat
                                                                                         : DataFormat::eR16G16B16A16SFloat;

            texFormat.dimension   = TextureDimension::eCube;

            texFormat.width       = width;

            texFormat.height      = height;

            texFormat.depth       = 1;

            texFormat.arrayLayers = 6;

            texFormat.mipmaps     = mipLevels;

            RHITexture* pTexture  = m_pRenderDevice->CreateTextureSampled(texFormat, {.copyUsage = true}, "env_skybox");

            if (pTexture != nullptr)
            {
                pOutTexture->pSkybox = pTexture;

                OwnTexture(pOutTexture->pSkybox);

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

                        region.textureSize  = {texCube[face][level].extent().x, texCube[face][level].extent().y, 1};

                        region.bufferOffset = offset;

                        regions.push_back(region);

                        offset += texCube[face][level].size();
                    }
                }

                UpdateTextureCube(pOutTexture->pSkybox, regions, texCube.size(), static_cast<const uint8_t*>(texCube.data()));

                if (pOutTexture->pSkybox != nullptr)
                {
                    if (RHIDebug* debug = m_pRenderDevice->GetRHIDebug())
                    {
                        GetRHIThread().Invoke(&RHIDebug::SetTextureDebugName, debug, pOutTexture->pSkybox,
                                              pTexture->GetBaseInfo().tag);
                    }

                    SkyboxRenderer* pSkyboxRenderer = m_pRenderDevice->GetRendererServer()->RequestSkyboxRenderer();

                    pSkyboxRenderer->PreprocessEnvTexture(pOutTexture);

                    OwnEnvironmentTextures(pOutTexture);
                }
            }
        }
    }

    return pOutTexture != nullptr && pOutTexture->IsComplete();
}

void TextureManager::UploadEnvironmentCube(uint32_t                            size,
                                           uint32_t                            mipLevels,
                                           const HeapVector<HeapVector<Vec4>>& faces,
                                           const char*                         name,
                                           RHITexture*&                        texture)
{
    TextureFormat format{};

    format.format      = DataFormat::eR32G32B32A32SFloat;

    format.dimension   = TextureDimension::eCube;

    format.width       = size;

    format.height      = size;

    format.depth       = 1;

    format.arrayLayers = 6;

    format.mipmaps     = mipLevels;

    texture            = m_pRenderDevice->CreateTextureSampled(format, {.copyUsage = true}, name);

    if (texture != nullptr)
    {
        OwnTexture(texture);

        HeapVector<RHIBufferTextureCopyRegion> regions;

        HeapVector<Vec4> pixels;

        size_t total = 0;

        for (const HeapVector<Vec4>& face : faces)
        {
            total += face.size();
        }

        pixels.reserve(total);

        for (uint32_t level = 0; level < mipLevels; ++level)
        {
            for (uint32_t face = 0; face < 6; ++face)
            {
                RHIBufferTextureCopyRegion region{};

                region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

                region.textureSubresources.mipmap         = level;

                region.textureSubresources.baseArrayLayer = face;

                region.textureSubresources.layerCount     = 1;

                const uint32_t extent                     = std::max(1u, size >> level);

                region.textureSize                        = {extent, extent, 1};

                region.bufferOffset                       = static_cast<uint32_t>(pixels.size() * sizeof(Vec4));

                regions.push_back(region);

                for (const Vec4& value : faces[level * 6 + face])
                {
                    pixels.push_back(value);
                }
            }
        }

        UpdateTextureCube(texture, regions, static_cast<uint32_t>(pixels.size() * sizeof(Vec4)),
                          reinterpret_cast<const uint8_t*>(pixels.data()));
    }
}

static Vec3 CubeDirection(uint32_t face, float u, float v)
{
    Vec3 direction(0);

    switch (face)
    {
        case 0: direction = Vec3(1, -v, -u); break;
        case 1: direction = Vec3(-1, -v, u); break;
        case 2: direction = Vec3(u, 1, v); break;
        case 3: direction = Vec3(u, -1, -v); break;
        case 4: direction = Vec3(u, -v, 1); break;
        case 5: direction = Vec3(-u, -v, -1); break;
        default: break;
    }

    return glm::normalize(direction);
}

static Vec3 EvaluateIrradiance(const HeapVector<Vec3>& coefficients, const Vec3& direction)
{
    const float x       = direction.x;

    const float y       = direction.y;

    const float z       = direction.z;

    const float basis[] = {0.2820947918f,
                           -0.4886025119f * y,
                           0.4886025119f * z,
                           -0.4886025119f * x,
                           1.0925484306f * x * y,
                           -1.0925484306f * y * z,
                           0.3153915653f * (3 * z * z - 1),
                           -1.0925484306f * x * z,
                           0.5462742153f * (x * x - y * y)};

    Vec3 value(0);

    for (uint32_t index = 0; index < 9 && index < coefficients.size(); ++index)
    {
        value += coefficients[index] * basis[index];
    }

    // The shading texture stores irradiance divided by pi (unit Lambertian response).
    return glm::max(value / glm::pi<float>(), Vec3(0));
}

void TextureManager::LoadSceneEnvironment(const sg::Scene* scene, EnvTexture* environment)
{
    const sg::SceneAssetData& data        = scene->GetAssetData();

    const sg::ImageBasedLightAsset& light = data.imageBasedLights[data.imageBasedLight];

    environment->authoredCubemaps         = true;

    UploadEnvironmentCube(light.size, light.mipLevels, light.specularMipFaces, "gltf_environment_specular",
                          environment->pSkybox);

    environment->pPrefiltered         = environment->pSkybox;

    constexpr uint32_t irradianceSize = 64;

    HeapVector<HeapVector<Vec4>> irradiance(6);

    for (uint32_t face = 0; face < 6; ++face)
    {
        irradiance[face].reserve(irradianceSize * irradianceSize);

        for (uint32_t y = 0; y < irradianceSize; ++y)
        {
            for (uint32_t x = 0; x < irradianceSize; ++x)
            {
                const Vec3 direction =
                    CubeDirection(face, 2 * (x + 0.5f) / irradianceSize - 1, 2 * (y + 0.5f) / irradianceSize - 1);

                irradiance[face].push_back(Vec4(EvaluateIrradiance(light.irradianceCoefficients, direction), 1));
            }
        }
    }

    UploadEnvironmentCube(irradianceSize, 1, irradiance, "gltf_environment_irradiance", environment->pIrradiance);

    RHISamplerCreateInfo sampler     = RHISamplerCreateInfo::CreateLinearRepeat();

    sampler.repeatU                  = RHISamplerRepeatMode::eClampToEdge;

    sampler.repeatV                  = RHISamplerRepeatMode::eClampToEdge;

    sampler.repeatW                  = RHISamplerRepeatMode::eClampToEdge;

    sampler.maxLod                   = static_cast<float>(light.mipLevels - 1);

    environment->pPrefilteredSampler = m_pRenderDevice->CreateSampler(sampler);

    sampler.maxLod                   = 0;

    environment->pIrradianceSampler  = m_pRenderDevice->CreateSampler(sampler);

    m_pRenderDevice->GetRendererServer()->RequestSkyboxRenderer()->PreprocessEnvTexture(environment);

    OwnEnvironmentTextures(environment);
}

void TextureManager::UpdateTexture(RHITexture*& texture, uint32_t dataSize, const uint8_t* data, bool generateMipmaps)
{
    RHIBufferTextureCopyRegion region{};

    region.textureSubresources.aspect     = texture->GetSubResourceRange().aspect;

    region.textureSubresources.layerCount = texture->GetArrayLayers();

    region.textureSize                    = {texture->GetWidth(), texture->GetHeight(), texture->GetDepth()};

    if (!m_pUploadQueue->EnqueueTexture(texture, MakeVecView(&region, 1), dataSize, data, generateMipmaps))
    {
        ReleaseSceneTexture(texture);

        texture = nullptr;
    }
}

void TextureManager::UpdateTextureCube(RHITexture*&                                  texture,
                                       const HeapVector<RHIBufferTextureCopyRegion>& regions,
                                       uint32_t                                      dataSize,
                                       const uint8_t*                                data)
{
    HeapVector<RHIBufferTextureCopyRegion> copy = regions;

    if (!m_pUploadQueue->EnqueueTexture(texture, copy, dataSize, data))
    {
        ReleaseSceneTexture(texture);

        texture = nullptr;
    }
}
} // namespace zen::rc
