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

    void EnsureTransferBatch();

    void FlushTransferBatch();
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
    void QueueShaderReadOnlyTransition(RHITexture* pTexture);

    void UpdateTexture(RHITexture* pTexture, uint32_t dataSize, const uint8_t* pData);

    void UpdateTextureCube(RHITexture* pTexture,
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

    RenderDevice* m_pRenderDevice{nullptr};

    TextureStagingManager* m_pStagingMgr{nullptr};

    // HashMap<std::string, RHITexture*> m_textureCache;
    HashMap<std::string, RHITexture*> m_textureCache;

    HashMap<RHITexture*, RHITexture*> m_textureProxyMap; // proxy tex -> base tex

    struct TextureTransferBatch
    {
        RHICommandList* pCmdList{nullptr};
        uint32_t textureCount{0};
        bool recording{false};
    } m_transferBatch;

    HeapVector<RHITexture*> m_pendingShaderReadOnlyTransitions;
};
} // namespace zen::rc
