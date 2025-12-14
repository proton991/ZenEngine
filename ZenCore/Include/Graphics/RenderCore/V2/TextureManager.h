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
        // m_RHI = m_renderDevice->GetRHI();
    }

    void Destroy();

    // RHITexture* CreateTexture(const TextureInfo& textureInfo);
    //
    // RHITexture* CreateTextureProxy(const RHITexture* baseTexture,
    //                                       const TextureProxyInfo& proxyInfo);

    RHITexture* LoadTexture2D(const std::string& file, bool requireMipmap = false);

    void LoadSceneTextures(const sg::Scene* scene, std::vector<RHITexture*>& outTextures);

    void LoadTextureEnv(const std::string& file, EnvTexture* outTexture);

    // RHITexture* GetBaseTextureForProxy(const RHITexture* handle) const;
    //
    // bool IsProxyTexture(const RHITexture* textureHandle) const;

private:
    void UpdateTexture(RHITexture* texture, uint32_t dataSize, const uint8_t* pData);

    void UpdateTextureCube(RHITexture* texture,
                           const std::vector<RHIBufferTextureCopyRegion>& regions,
                           uint32_t dataSize,
                           const uint8_t* pData);

    // void UpdateTexture(const RHITexture* textureHandle,
    //                    const Vec3i& textureSize,
    //                    uint32_t dataSize,
    //                    const uint8_t* pData);
    //
    // void UpdateTextureCube(const RHITexture* textureHandle,
    //                        const std::vector<RHIBufferTextureCopyRegion>& regions,
    //                        uint32_t dataSize,
    //                        const uint8_t* pData);

    // DynamicRHI* m_RHI{nullptr};

    RenderDevice* m_renderDevice{nullptr};

    TextureStagingManager* m_stagingMgr{nullptr};

    // HashMap<std::string, RHITexture*> m_textureCache;
    HashMap<std::string, RHITexture*> m_textureCache;

    HashMap<RHITexture*, RHITexture*> m_textureProxyMap; // proxy tex -> base tex
};
} // namespace zen::rc