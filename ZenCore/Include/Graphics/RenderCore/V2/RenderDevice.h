#pragma once
#include "Templates/HashMap.h"
#include "Templates/HeapVector.h"
#include "Templates/Queue.h"
#include "Graphics/RHI/RHIDebug.h"
#include "RenderCoreDefs.h"
#include "Utils/UniquePtr.h"

#define TEXTURE_UPLOAD_REGION_SIZE            64
#define STAGING_BLOCK_SIZE_BYTES              (256 * 1024)
#define STAGING_POOL_SIZE_BYTES               (128 * 1024 * 1024)
#define MAX_TEXTURE_STAGING_PENDING_FREE_SIZE (64 * 1024 * 1024)

namespace zen::sg
{
class Scene;
}

namespace zen::rc
{
class RenderDevice;
class RenderGraph;
class RendererServer;
class TextureManager;
class SkyboxRenderer;

class GraphicsPassBuilder
{
public:
    explicit GraphicsPassBuilder(RenderDevice* pRenderDevice);

    GraphicsPassBuilder& AddShaderStage(RHIShaderStage stage, std::string path)
    {
        m_shaderStages[stage] = std::move(path);
        return *this;
    }

    GraphicsPassBuilder& SetShaderSpecializationConstants(uint32_t constantId, int value)
    {
        m_specializationConstants[constantId] = value;
        return *this;
    }

    // GraphicsPassBuilder& AddColorRenderTarget(DataFormat format,
    //                                           const RHITexture* handle,
    //                                           bool clear = true);

    GraphicsPassBuilder& AddViewportColorRT(
        RHIViewport* pViewport,
        RHIRenderTargetLoadOp loadOp   = RHIRenderTargetLoadOp::eClear,
        RHIRenderTargetStoreOp storeOp = RHIRenderTargetStoreOp::eStore);

    GraphicsPassBuilder& SetViewportDepthStencilRT(
        RHIViewport* pViewport,
        RHIRenderTargetLoadOp loadOp   = RHIRenderTargetLoadOp::eClear,
        RHIRenderTargetStoreOp storeOp = RHIRenderTargetStoreOp::eStore);

    GraphicsPassBuilder& AddColorRenderTarget(
        RHITexture* pColorRT,
        RHIRenderTargetLoadOp loadOp   = RHIRenderTargetLoadOp::eClear,
        RHIRenderTargetStoreOp storeOp = RHIRenderTargetStoreOp::eStore);

    GraphicsPassBuilder& SetDepthStencilTarget(
        RHITexture* pDepthStencilRT,
        RHIRenderTargetLoadOp loadOp   = RHIRenderTargetLoadOp::eClear,
        RHIRenderTargetStoreOp storeOp = RHIRenderTargetStoreOp::eStore);

    // GraphicsPassBuilder& SetNumSamples(SampleCount sampleCount)
    // {
    //     m_rpLayout.SetNumSamples(sampleCount);
    //     return *this;
    // }

    // GraphicsPassBuilder& SetDepthStencilTarget(
    //     DataFormat format,
    //     const RHITexture* handle,
    //     RHIRenderTargetLoadOp loadOp   = RHIRenderTargetLoadOp::eClear,
    //     RHIRenderTargetStoreOp storeOp = RHIRenderTargetStoreOp::eNone);

    GraphicsPassBuilder& SetShaderResourceBinding(uint32_t setIndex,
                                                  HeapVector<RHIShaderResourceBinding>&& bindings)
    {
        m_dsBindings[setIndex] = std::move(bindings);
        return *this;
    }

    GraphicsPassBuilder& SetPipelineState(const RHIGfxPipelineStates& pso)
    {
        m_PSO = pso;
        return *this;
    }

    GraphicsPassBuilder& SetTag(std::string tag)
    {
        m_tag = std::move(tag);
        return *this;
    }

    GraphicsPassBuilder& SetFramebufferInfo(RHIViewport* pViewport, uint32_t width, uint32_t height)
    {
        m_framebufferInfo.width  = width;
        m_framebufferInfo.height = height;
        m_pViewport              = pViewport;
        m_pGfxPass->pRenderingLayout->SetRenderArea(0, 0, width, height);
        return *this;
    }

    GraphicsPassBuilder& SetFramebufferInfo(RHIViewport* pViewport)
    {
        m_framebufferInfo.width  = pViewport->GetWidth();
        m_framebufferInfo.height = pViewport->GetHeight();
        m_pViewport              = pViewport;
        m_pGfxPass->pRenderingLayout->SetRenderArea(0, 0, pViewport->GetWidth(),
                                                    pViewport->GetHeight());
        return *this;
    }

    GraphicsPassBuilder& SetShaderProgramName(std::string name)
    {
        m_shaderProgramName = std::move(name);
        m_shaderMode        = GfxPassShaderMode::ePreCompiled;
        return *this;
    }

    GraphicsPass* Build();

private:
    RenderDevice* m_pRenderDevice{nullptr};
    RHIViewport* m_pViewport{nullptr};
    std::string m_tag;
    GraphicsPass* m_pGfxPass{nullptr};
    GfxPassShaderMode m_shaderMode{GfxPassShaderMode::eNone};
    HashMap<RHIShaderStage, std::string> m_shaderStages;
    std::string m_shaderProgramName;
    RHIGfxPipelineStates m_PSO{};
    // RHIRenderingLayout* m_pRenderingLayout{nullptr};
    // RHIRenderPassLayout m_rpLayout{};
    RHIFramebufferInfo m_framebufferInfo{};
    HashMap<uint32_t, HeapVector<RHIShaderResourceBinding>> m_dsBindings;
    HashMap<uint32_t, int> m_specializationConstants;
};

class GraphicsPassResourceUpdater
{
public:
    GraphicsPassResourceUpdater(RenderDevice* pRenderDevice, GraphicsPass* pGfxPass) :
        m_pRenderDevice(pRenderDevice), m_pGfxPass(pGfxPass)
    {}

    GraphicsPassResourceUpdater& SetShaderResourceBinding(
        uint32_t setIndex,
        HeapVector<RHIShaderResourceBinding>&& bindings)
    {
        m_dsBindings[setIndex] = std::move(bindings);
        return *this;
    }

    void Update();

private:
    RenderDevice* m_pRenderDevice{nullptr};
    GraphicsPass* m_pGfxPass{nullptr};
    HashMap<uint32_t, HeapVector<RHIShaderResourceBinding>> m_dsBindings;
};

class ComputePassBuilder
{
public:
    explicit ComputePassBuilder(RenderDevice* pRenderDevice) : m_pRenderDevice(pRenderDevice) {}

    ComputePassBuilder& SetShaderProgramName(std::string name)
    {
        m_shaderProgramName = std::move(name);
        return *this;
    }
    ComputePassBuilder& SetTag(std::string tag)
    {
        m_tag = std::move(tag);
        return *this;
    }

    ComputePass* Build();

private:
    RenderDevice* m_pRenderDevice{nullptr};
    std::string m_tag;
    std::string m_shaderProgramName;
};

class ComputePassResourceUpdater
{
public:
    ComputePassResourceUpdater(RenderDevice* pRenderDevice, ComputePass* pComputePass) :
        m_pRenderDevice(pRenderDevice), m_pComputePass(pComputePass)
    {}

    ComputePassResourceUpdater& SetShaderResourceBinding(
        uint32_t setIndex,
        HeapVector<RHIShaderResourceBinding>&& bindings)
    {
        m_dsBindings[setIndex] = std::move(bindings);
        return *this;
    }

    void Update();

private:
    RenderDevice* m_pRenderDevice{nullptr};
    ComputePass* m_pComputePass{nullptr};
    HashMap<uint32_t, HeapVector<RHIShaderResourceBinding>> m_dsBindings;
};

enum class StagingFlushAction : uint32_t
{
    eNone    = 0,
    ePartial = 1,
    eFull    = 2
};

struct StagingSubmitResult
{
    bool success{false};
    StagingFlushAction flushAction{StagingFlushAction::eNone};
    uint32_t writeOffset{0};
    uint32_t writeSize{0};
    RHIBuffer* pBuffer;
};

class TextureStagingManager
{
public:
    explicit TextureStagingManager(RenderDevice* pRenderDevice) :
        m_pRenderDevice(pRenderDevice), m_usedMemory(0), m_pendingFreeMemorySize(0)
    {}

    RHIBuffer* RequireBuffer(uint32_t requiredSize);

    void ReleaseBuffer(const RHIBuffer* pBuffer);

    void ProcessPendingFrees();

    uint32_t GetUsedMemorySize() const
    {
        return m_usedMemory;
    }

    uint32_t GetPendingFreeMemorySize() const
    {
        return m_pendingFreeMemorySize;
    }

    void Destroy();

private:
    RenderDevice* m_pRenderDevice{nullptr};

    struct Entry
    {
        uint32_t size;
        RHIBuffer* pBuffer;
        uint32_t usedFrame;
    };

    std::vector<Entry> m_usedBuffers;
    std::vector<Entry> m_pendingFreeBuffers;
    std::vector<Entry> m_freeBuffers;
    // for memory management
    std::vector<RHIBuffer*> m_allocatedBuffers;
    uint32_t m_usedMemory;
    uint32_t m_pendingFreeMemorySize;
};

class BufferStagingManager
{
public:
    BufferStagingManager(RenderDevice* pRenderDevice, uint32_t bufferSize, uint64_t poolSize);

    void Init(uint32_t numFrames);

    void Destroy();

    void BeginSubmit(uint32_t requiredSize,
                     StagingSubmitResult* pResult,
                     uint32_t requiredAlign = 32,
                     bool canSegment        = false);

    void EndSubmit(const StagingSubmitResult* pResult);

    void PerformAction(StagingFlushAction action);

private:
    bool FitInBlock(uint32_t blockIndex,
                    uint32_t requiredSize,
                    uint32_t requiredAlign,
                    bool canSegment,
                    StagingSubmitResult* pResult);

    bool CanInsertNewBlock() const
    {
        return BUFFER_SIZE * m_bufferBlocks.size() < POOL_SIZE;
    }

    void InsertNewBlock();

    void UpdateBlockIndex()
    {
        m_currentBlockIndex = (m_currentBlockIndex + 1) % m_bufferBlocks.size();
    }

    uint32_t BUFFER_SIZE;
    uint64_t POOL_SIZE;
    RenderDevice* m_pRenderDevice{nullptr};
    // DynamicRHI* GDynamicRHI{nullptr};

    struct StagingBuffer
    {
        RHIBuffer* pBuffer;
        uint64_t usedFrame;
        uint32_t occupiedSize;
    };

    std::vector<StagingBuffer> m_bufferBlocks;
    uint32_t m_currentBlockIndex;
    bool m_stagingBufferUsed{false};

    friend class RenderDevice;
};

struct RenderFrame
{
    std::vector<RHITexture*> texturesPendingFree;
};

struct RenderDeviceFeatures
{
    bool geometryShader{false};
};

// todo: some resources may be freed or recycled, track these and free them before frame begins
class RenderDevice
{
public:
    explicit RenderDevice(RHIAPIType APIType, uint32_t numFrames);

    void Init(RHIViewport* pMainViewport);

    void Destroy();

    void ExecuteRenderGraphs(RHIViewport* pViewport, VectorView<RenderGraph*> rdgs);

    void ExecuteRenderGraphs(VectorView<UniquePtr<RenderGraph>> rdgs);

    // todo: implement pool based recycle mechanism
    RHIRenderingLayout* AcquireRenderingLayout();

    void DestroyRenderingLayout(RHIRenderingLayout* pLayout);

    RHITexture* CreateTextureColorRT(const TextureFormat& texFormat,
                                     TextureUsageHint usageHint,
                                     std::string texName);

    RHITexture* CreateTextureDepthStencilRT(const TextureFormat& texFormat,
                                            TextureUsageHint usageHint,
                                            std::string texName);

    RHITexture* CreateTextureStorage(const TextureFormat& texFormat,
                                     TextureUsageHint usageHint,
                                     std::string texName);

    RHITexture* CreateTextureSampled(const TextureFormat& texFormat,
                                     TextureUsageHint usageHint,
                                     std::string texName);

    RHITexture* CreateTextureDummy(const TextureFormat& texFormat,
                                   TextureUsageHint usageHint,
                                   std::string texName);

    RHITexture* CreateTextureProxy(RHITexture* pBaseTexture,
                                   const TextureProxyFormat& proxyFormat,
                                   std::string texName);

    // RHITexture* GetTextureRDFromHandle(const RHITexture* handle);

    void DestroyTexture(RHITexture* pTexture);

    // RHITexture* CreateTexture(const TextureInfo& textureInfo);
    //
    // RHITexture* CreateTextureProxy(const RHITexture* baseTexture,
    //                                       const TextureProxyInfo& proxyInfo);

    // RHITexture* GetBaseTextureForProxy(const RHITexture* handle) const;

    // bool IsProxyTexture(const RHITexture* handle) const;

    void GenerateTextureMipmaps(RHITexture* pTextureHandle, RHICommandList* pCmdList);

    RHIBuffer* CreateVertexBuffer(uint32_t dataSize, const uint8_t* pData);

    RHIBuffer* CreateIndexBuffer(uint32_t dataSize, const uint8_t* pData);

    RHIBuffer* CreateUniformBuffer(uint32_t dataSize, const uint8_t* pData, std::string bufferName);

    RHIBuffer* CreateStorageBuffer(uint32_t dataSize, const uint8_t* pData, std::string bufferName);

    RHIBuffer* CreateIndirectBuffer(uint32_t dataSize,
                                    const uint8_t* pData,
                                    std::string bufferName);

    void UpdateBuffer(RHIBuffer* pBufferHandle,
                      uint32_t dataSize,
                      const uint8_t* pData,
                      uint32_t offset = 0);

    void DestroyBuffer(RHIBuffer* pBufferHandle);

    // RenderPassHandle GetOrCreateRenderPass(const RHIRenderPassLayout& layout);

    // RHIPipeline* GetOrCreateGfxPipeline(RHIGfxPipelineStates& PSO,
    //                                     RHIShader* shader,
    //                                     const RenderPassHandle& renderPass,
    //                                     const HashMap<uint32_t, int>& specializationConstants);

    RHIPipeline* GetOrCreateGfxPipeline(RHIGfxPipelineStates& PSO,
                                        RHIShader* pShader,
                                        const RHIRenderingLayout* pRenderingLayout,
                                        const HashMap<uint32_t, int>& specializationConstants);

    RHIPipeline* GetOrCreateComputePipeline(RHIShader* pShader);

    RHIViewport* CreateViewport(void* pWindow,
                                uint32_t width,
                                uint32_t height,
                                bool enableVSync = true);

    void ResizeViewport(RHIViewport* pViewport, uint32_t width, uint32_t height);

    void UpdateGraphicsPassOnResize(GraphicsPass* pGfxPass, RHIViewport* pViewport);

    // RHITexture* LoadTexture2D(const std::string& file, bool requireMipmap = false);

    RHITexture* LoadTexture2D(const std::string& file, bool requireMipmap = false);

    void LoadSceneTextures(const sg::Scene* pScene, std::vector<RHITexture*>& outTextures);

    void LoadTextureEnv(const std::string& file, EnvTexture* pTexture);

    RHISampler* CreateSampler(const RHISamplerCreateInfo& samplerInfo);

    // RHITextureSubResourceRange GetTextureSubResourceRange(RHITexture* handle);

    // auto* GetRHI() const
    // {
    //     return GDynamicRHI;
    // }

    auto* GetRHIDebug() const
    {
        return m_pRHIDebug;
    }

    auto GetFramesCounter() const
    {
        return m_framesCounter;
    }

    RHICommandList* GetImmediateTransferCmdList() const
    {
        return m_pImmediateTransferCmdList;
    }

    RHICommandList* GetImmediateGraphicsCmdList() const
    {
        return m_pImmediateGraphicsCmdList;
    }

    void SubmitImmediateTransferCmdList();

    // RHICommandList* GetCurrentCmdList() const
    // {
    //     return m_frames[m_currentFrame].pGfxCmdList;
    // }

    // RHICommandList* GetCurrentDrawCmdList() const
    // {
    //     return m_frames[m_currentFrame].drawCmdList;
    // }

    RendererServer* GetRendererServer() const
    {
        return m_pRendererServer;
    }

    void ProcessViewportResize(uint32_t width, uint32_t height);

    void WaitForPreviousFrames();

    void WaitForAllFrames();

    void NextFrame();

    void WaitForIdle() const
    {
        GDynamicRHI->WaitDeviceIdle();
    }

    const RHIGPUInfo& GetGPUInfo() const
    {
        return GDynamicRHI->QueryGPUInfo();
    }

private:
    void BeginFrame();

    void EndFrame();
    
    void AcquireGraphicsCmdLists(size_t numCmdLists, HeapVector<RHICommandList*>& outCmdLists);

    void ProcessPendingFreeResources(uint32_t frameIndex);

    void UpdateBufferInternal(RHIBuffer* pBufferHandle,
                              uint32_t offset,
                              uint32_t dataSize,
                              const uint8_t* pData);

    void UpdateTextureOneTime(RHITexture* pTextureHandle,
                              const Vec3i& textureSize,
                              uint32_t dataSize,
                              const uint8_t* pData);

    void UpdateTextureBatch(RHITexture* pTextureHandle,
                            const Vec3i& textureSize,
                            const uint8_t* pData);

    void DestroyViewport(RHIViewport* pViewport);

    static size_t CalcRenderPassLayoutHash(const RHIRenderPassLayout& layout);
    // static size_t CalcFramebufferHash(const RHIFramebufferInfo& info,
    //                                   RenderPassHandle renderPassHandle);

    // static size_t CalcGfxPipelineHash(const RHIGfxPipelineStates& pso,
    //                                   RHIShader* shader,
    //                                   const RenderPassHandle& renderPass,
    //                                   const HashMap<uint32_t, int>& specializationConstants);

    static size_t CalcGfxPipelineHash(const RHIGfxPipelineStates& pso,
                                      RHIShader* pShader,
                                      // const RHIRenderPassLayout& renderPassLayout,
                                      const HashMap<uint32_t, int>& specializationConstants);

    static size_t CalcComputePipelineHash(RHIShader* pShader);

    static size_t CalcSamplerHash(const RHISamplerCreateInfo& info);

    size_t PadUniformBufferSize(size_t originalSize);

    size_t PadStorageBufferSize(size_t originalSize);

    const RHIAPIType m_APIType;
    const uint32_t m_numFrames;
    uint32_t m_currentFrame{0};
    uint64_t m_framesCounter{0};
    std::vector<RenderFrame> m_frames;

    // DynamicRHI* GDynamicRHI{nullptr};
    RHIDebug* m_pRHIDebug{nullptr};

    RHICommandList* m_pImmediateGraphicsCmdList{nullptr};
    RHICommandList* m_pImmediateTransferCmdList{nullptr};
    HeapVector<RHICommandList*> m_graphicsCmdListPool;
    BufferStagingManager* m_pBufferStagingMgr{nullptr};
    TextureStagingManager* m_pTextureStagingMgr{nullptr};

    RendererServer* m_pRendererServer{nullptr};
    TextureManager* m_pTextureManager{nullptr};

    DeletionQueue m_deletionQueue;

    // HashMap<size_t, RenderPassHandle> m_renderPassCache;
    HashMap<size_t, RHIPipeline*> m_pipelineCache;
    HashMap<size_t, RHISampler*> m_samplerCache;
    // HashMap<RHITexture*, RHITexture*> m_textureMap;
    std::vector<RHIBuffer*> m_buffers;
    std::vector<GraphicsPass*> m_gfxPasses;
    std::vector<ComputePass*> m_computePasses;
    std::vector<RHIViewport*> m_viewports;
    RHIViewport* m_pMainViewport{nullptr};

    friend class GraphicsPassBuilder;
    friend class ComputePassBuilder;
};
} // namespace zen::rc
