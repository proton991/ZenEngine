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

    // rhi::RHITexture* CreateTexture(const rhi::TextureInfo& textureInfo);
    //
    // rhi::RHITexture* CreateTextureProxy(const rhi::RHITexture* baseTexture,
    //                                       const rhi::TextureProxyInfo& proxyInfo);

    rhi::RHITexture* LoadTexture2D(const std::string& file, bool requireMipmap = false);

    void LoadSceneTextures(const sg::Scene* scene, std::vector<rhi::RHITexture*>& outTextures);

    void LoadTextureEnv(const std::string& file, EnvTexture* outTexture);

    // rhi::RHITexture* GetBaseTextureForProxy(const rhi::RHITexture* handle) const;
    //
    // bool IsProxyTexture(const rhi::RHITexture* textureHandle) const;

private:
    void UpdateTexture(rhi::RHITexture* texture, uint32_t dataSize, const uint8_t* pData);

    void UpdateTextureCube(rhi::RHITexture* texture,
                           const std::vector<rhi::BufferTextureCopyRegion>& regions,
                           uint32_t dataSize,
                           const uint8_t* pData);

    // void UpdateTexture(const rhi::RHITexture* textureHandle,
    //                    const Vec3i& textureSize,
    //                    uint32_t dataSize,
    //                    const uint8_t* pData);
    //
    // void UpdateTextureCube(const rhi::RHITexture* textureHandle,
    //                        const std::vector<rhi::BufferTextureCopyRegion>& regions,
    //                        uint32_t dataSize,
    //                        const uint8_t* pData);

    rhi::DynamicRHI* m_RHI{nullptr};

    RenderDevice* m_renderDevice{nullptr};

    TextureStagingManager* m_stagingMgr{nullptr};

    // HashMap<std::string, rhi::RHITexture*> m_textureCache;
    HashMap<std::string, rhi::RHITexture*> m_textureCache;

    HashMap<rhi::RHITexture*, rhi::RHITexture*> m_textureProxyMap; // proxy tex -> base tex
};
} // namespace zen::rc