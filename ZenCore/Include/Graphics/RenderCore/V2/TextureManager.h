#pragma once
#include "Graphics/RenderCore/V2/RenderDevice.h"

namespace zen::rc
{
class SkyboxRenderer;

class TextureManager
{
public:
    TextureManager(RenderDevice* renderDevice, TextureStagingManager* stagingMgr) :
        m_renderDevice(renderDevice), m_stagingMgr(stagingMgr)
    {
        m_RHI = m_renderDevice->GetRHI();
    }

    void Destroy();

    // rhi::TextureHandle CreateTexture(const rhi::TextureInfo& textureInfo);
    //
    // rhi::TextureHandle CreateTextureProxy(const rhi::TextureHandle& baseTexture,
    //                                       const rhi::TextureProxyInfo& proxyInfo);

    TextureRD* LoadTexture2D(const std::string& file, bool requireMipmap = false);

    void LoadSceneTextures(const sg::Scene* scene, std::vector<TextureRD*>& outTextures);

    void LoadTextureEnv(const std::string& file, EnvTexture* outTexture);

    // rhi::TextureHandle GetBaseTextureForProxy(const rhi::TextureHandle& handle) const;
    //
    // bool IsProxyTexture(const rhi::TextureHandle& textureHandle) const;

private:
    void UpdateTexture(const TextureRD* texture, uint32_t dataSize, const uint8_t* pData);

    void UpdateTextureCube(const rhi::TextureHandle& textureHandle,
                           const std::vector<rhi::BufferTextureCopyRegion>& regions,
                           uint32_t dataSize,
                           const uint8_t* pData);

    // void UpdateTexture(const rhi::TextureHandle& textureHandle,
    //                    const Vec3i& textureSize,
    //                    uint32_t dataSize,
    //                    const uint8_t* pData);
    //
    // void UpdateTextureCube(const rhi::TextureHandle& textureHandle,
    //                        const std::vector<rhi::BufferTextureCopyRegion>& regions,
    //                        uint32_t dataSize,
    //                        const uint8_t* pData);

    rhi::DynamicRHI* m_RHI{nullptr};

    RenderDevice* m_renderDevice{nullptr};

    TextureStagingManager* m_stagingMgr{nullptr};

    // HashMap<std::string, rhi::TextureHandle> m_textureCache;
    HashMap<std::string, TextureRD*> m_textureCache;

    HashMap<rhi::TextureHandle, rhi::TextureHandle> m_textureProxyMap; // proxy tex -> base tex
};
} // namespace zen::rc