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

    rhi::TextureHandle CreateTexture(const rhi::TextureInfo& textureInfo, const std::string& tag);

    rhi::TextureHandle CreateTextureProxy(const rhi::TextureHandle& baseTexture,
                                          const rhi::TextureProxyInfo& proxyInfo);

    rhi::TextureHandle LoadTexture2D(const std::string& file, bool requireMipmap = false);

    void LoadSceneTextures(const sg::Scene* scene, std::vector<rhi::TextureHandle>& outTextures);

    void LoadTextureEnv(const std::string& file, EnvTexture* outTexture);

private:
    void UpdateTexture(const rhi::TextureHandle& textureHandle,
                       const Vec3i& textureSize,
                       uint32_t dataSize,
                       const uint8_t* pData);

    void UpdateTextureCube(const rhi::TextureHandle& textureHandle,
                           const std::vector<rhi::BufferTextureCopyRegion>& regions,
                           uint32_t dataSize,
                           const uint8_t* pData);

    rhi::DynamicRHI* m_RHI{nullptr};

    RenderDevice* m_renderDevice{nullptr};

    TextureStagingManager* m_stagingMgr{nullptr};

    HashMap<std::string, rhi::TextureHandle> m_textureCache;
};
} // namespace zen::rc