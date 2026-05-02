#pragma once
#include "Graphics/RenderCore/V2/RenderDevice.h"

namespace zen::rc
{
class SkyboxRenderer;

class TextureManager
{
public:
    TextureManager(RenderDevice* pRenderDevice, TextureStagingManager* pStagingMgr) :
        m_pRenderDevice(pRenderDevice), m_pStagingMgr(pStagingMgr)
    {
        // m_RHI = m_renderDevice->GetRHI();
    }

    void Destroy();

    void FlushPendingTextureUpdates();
    // RHITexture* CreateTexture(const TextureInfo& textureInfo);
    //
    // RHITexture* CreateTextureProxy(const RHITexture* baseTexture,
    //                                       const TextureProxyInfo& proxyInfo);

    RHITexture* LoadTexture2D(const std::string& file, bool requireMipmap = false);

    void LoadSceneTextures(const sg::Scene* pScene, std::vector<RHITexture*>& outTextures);

    void LoadTextureEnv(const std::string& file, EnvTexture* pOutTexture);

    // RHITexture* GetBaseTextureForProxy(const RHITexture* handle) const;
    //
    // bool IsProxyTexture(const RHITexture* textureHandle) const;

private:
    void UpdateTexture(RHITexture* pTexture,
                       uint32_t dataSize,
                       const uint8_t* pData,
                       bool generateMipmaps = false);

    void UpdateTextureCube(RHITexture* pTexture,
                           const HeapVector<RHIBufferTextureCopyRegion>& regions,
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

    RenderDevice* m_pRenderDevice{nullptr};

    TextureStagingManager* m_pStagingMgr{nullptr};

    // HashMap<std::string, RHITexture*> m_textureCache;
    HashMap<std::string, RHITexture*> m_textureCache;

    HashMap<RHITexture*, RHITexture*> m_textureProxyMap; // proxy tex -> base tex

    struct PendingTextureUpdate
    {
        RHIBuffer* pStagingBuffer{nullptr};
        RHITexture* pTexture{nullptr};
        RHIBufferTextureCopyRegion copyRegion{};
        HeapVector<RHIBufferTextureCopyRegion> copyRegions;
        bool useMultipleRegions{false};
        bool generateMipmaps{false};
    };

    HeapVector<PendingTextureUpdate> m_pendingTextureUpdates;
};
} // namespace zen::rc
