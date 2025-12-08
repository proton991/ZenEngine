#pragma once
#include "Templates/HashMap.h"
#include "Templates/Queue.h"
#include "Graphics/RHI/RHIDebug.h"
#include "RenderCoreDefs.h"

#define TEXTURE_UPLOAD_REGION_SIZE            64
#define STAGING_BLOCK_SIZE_BYTES              (256 * 1024)
#define STAGING_POOL_SIZE_BYTES               (128 * 1024 * 1024)
#define MAX_TEXTURE_STAGING_PENDING_FREE_SIZE (4 * 1024 * 1024 * 10)

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
    explicit GraphicsPassBuilder(RenderDevice* renderDevice) : m_renderDevice(renderDevice) {}

    GraphicsPassBuilder& AddShaderStage(rhi::RHIShaderStage stage, std::string path)
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
    //                                           const rhi::RHITexture* handle,
    //                                           bool clear = true);

    GraphicsPassBuilder& AddViewportColorRT(
        rhi::RHIViewport* viewport,
        rhi::RenderTargetLoadOp loadOp   = rhi::RenderTargetLoadOp::eClear,
        rhi::RenderTargetStoreOp storeOp = rhi::RenderTargetStoreOp::eStore);

    GraphicsPassBuilder& SetViewportDepthStencilRT(
        rhi::RHIViewport* viewport,
        rhi::RenderTargetLoadOp loadOp   = rhi::RenderTargetLoadOp::eClear,
        rhi::RenderTargetStoreOp storeOp = rhi::RenderTargetStoreOp::eStore);

    GraphicsPassBuilder& AddColorRenderTarget(
        rhi::RHITexture* colorRT,
        rhi::RenderTargetLoadOp loadOp   = rhi::RenderTargetLoadOp::eClear,
        rhi::RenderTargetStoreOp storeOp = rhi::RenderTargetStoreOp::eStore);

    GraphicsPassBuilder& SetDepthStencilTarget(
        rhi::RHITexture* depthStencilRT,
        rhi::RenderTargetLoadOp loadOp   = rhi::RenderTargetLoadOp::eClear,
        rhi::RenderTargetStoreOp storeOp = rhi::RenderTargetStoreOp::eStore);

    // GraphicsPassBuilder& SetNumSamples(SampleCount sampleCount)
    // {
    //     m_rpLayout.SetNumSamples(sampleCount);
    //     return *this;
    // }

    // GraphicsPassBuilder& SetDepthStencilTarget(
    //     DataFormat format,
    //     const rhi::RHITexture* handle,
    //     rhi::RenderTargetLoadOp loadOp   = rhi::RenderTargetLoadOp::eClear,
    //     rhi::RenderTargetStoreOp storeOp = rhi::RenderTargetStoreOp::eNone);

    GraphicsPassBuilder& SetShaderResourceBinding(
        uint32_t setIndex,
        const std::vector<rhi::ShaderResourceBinding>& bindings)
    {
        m_dsBindings[setIndex] = bindings;
        return *this;
    }

    GraphicsPassBuilder& SetPipelineState(const rhi::GfxPipelineStates& pso)
    {
        m_PSO = pso;
        return *this;
    }

    GraphicsPassBuilder& SetTag(std::string tag)
    {
        m_tag = std::move(tag);
        return *this;
    }

    GraphicsPassBuilder& SetFramebufferInfo(rhi::RHIViewport* viewport,
                                            uint32_t width,
                                            uint32_t height)
    {
        m_framebufferInfo.width  = width;
        m_framebufferInfo.height = height;
        m_viewport               = viewport;
        return *this;
    }

    GraphicsPassBuilder& SetFramebufferInfo(rhi::RHIViewport* viewport)
    {
        m_framebufferInfo.width  = viewport->GetWidth();
        m_framebufferInfo.height = viewport->GetHeight();
        m_viewport               = viewport;
        return *this;
    }

    GraphicsPassBuilder& SetShaderProgramName(std::string name)
    {
        m_shaderProgramName = std::move(name);
        m_shaderMode        = GfxPassShaderMode::ePreCompiled;
        return *this;
    }

    // todo: allocate GraphicsPass on heap
    GraphicsPass Build();

private:
    RenderDevice* m_renderDevice{nullptr};
    rhi::RHIViewport* m_viewport{nullptr};
    std::string m_tag;
    GfxPassShaderMode m_shaderMode{GfxPassShaderMode::eRuntime};
    HashMap<rhi::RHIShaderStage, std::string> m_shaderStages;
    std::string m_shaderProgramName;
    rhi::GfxPipelineStates m_PSO{};
    rhi::RenderPassLayout m_rpLayout{};
    rhi::FramebufferInfo m_framebufferInfo{};
    HashMap<uint32_t, std::vector<rhi::ShaderResourceBinding>> m_dsBindings;
    HashMap<uint32_t, int> m_specializationConstants;
};

class GraphicsPassResourceUpdater
{
public:
    GraphicsPassResourceUpdater(RenderDevice* renderDevice, GraphicsPass* gfxPass) :
        m_renderDevice(renderDevice), m_gfxPass(gfxPass)
    {}

    GraphicsPassResourceUpdater& SetShaderResourceBinding(
        uint32_t setIndex,
        const std::vector<rhi::ShaderResourceBinding>& bindings)
    {
        m_dsBindings[setIndex] = bindings;
        return *this;
    }

    void Update();

private:
    RenderDevice* m_renderDevice{nullptr};
    GraphicsPass* m_gfxPass{nullptr};
    HashMap<uint32_t, std::vector<rhi::ShaderResourceBinding>> m_dsBindings;
};

class ComputePassBuilder
{
public:
    explicit ComputePassBuilder(RenderDevice* renderDevice) : m_renderDevice(renderDevice) {}

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

    // todo: allocate ComputePass on heap
    ComputePass Build();

private:
    RenderDevice* m_renderDevice{nullptr};
    std::string m_tag;
    std::string m_shaderProgramName;
};

class ComputePassResourceUpdater
{
public:
    ComputePassResourceUpdater(RenderDevice* renderDevice, ComputePass* computePass) :
        m_renderDevice(renderDevice), m_computePass(computePass)
    {}

    ComputePassResourceUpdater& SetShaderResourceBinding(
        uint32_t setIndex,
        const std::vector<rhi::ShaderResourceBinding>& bindings)
    {
        m_dsBindings[setIndex] = bindings;
        return *this;
    }

    void Update();

private:
    RenderDevice* m_renderDevice{nullptr};
    ComputePass* m_computePass{nullptr};
    HashMap<uint32_t, std::vector<rhi::ShaderResourceBinding>> m_dsBindings;
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
    rhi::RHIBuffer* buffer;
};

class TextureStagingManager
{
public:
    explicit TextureStagingManager(RenderDevice* renderDevice) :
        m_renderDevice(renderDevice), m_usedMemory(0), m_pendingFreeMemorySize(0)
    {}

    rhi::RHIBuffer* RequireBuffer(uint32_t requiredSize);

    void ReleaseBuffer(const rhi::RHIBuffer* buffer);

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
    RenderDevice* m_renderDevice{nullptr};

    struct Entry
    {
        uint32_t size;
        rhi::RHIBuffer* buffer;
        uint32_t usedFrame;
    };

    std::vector<Entry> m_usedBuffers;
    std::vector<Entry> m_pendingFreeBuffers;
    std::vector<Entry> m_freeBuffers;
    // for memory management
    std::vector<rhi::RHIBuffer*> m_allocatedBuffers;
    uint32_t m_usedMemory;
    uint32_t m_pendingFreeMemorySize;
};

class BufferStagingManager
{
public:
    BufferStagingManager(RenderDevice* renderDevice, uint32_t bufferSize, uint64_t poolSize);

    void Init(uint32_t numFrames);

    void Destroy();

    void BeginSubmit(uint32_t requiredSize,
                     StagingSubmitResult* result,
                     uint32_t requiredAlign = 32,
                     bool canSegment        = false);

    void EndSubmit(const StagingSubmitResult* result);

    void PerformAction(StagingFlushAction action);

private:
    bool FitInBlock(uint32_t blockIndex,
                    uint32_t requiredSize,
                    uint32_t requiredAlign,
                    bool canSegment,
                    StagingSubmitResult* result);

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
    RenderDevice* m_renderDevice{nullptr};
    // rhi::DynamicRHI* GDynamicRHI{nullptr};

    struct StagingBuffer
    {
        rhi::RHIBuffer* buffer;
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
    rhi::RHICommandListContext* cmdListContext{nullptr};
    rhi::RHICommandList* uploadCmdList{nullptr};
    rhi::RHICommandList* drawCmdList{nullptr};
    bool cmdSubmitted{false};
    std::vector<rhi::RHITexture*> texturesPendingFree;
};

struct RenderDeviceFeatures
{
    bool geometryShader{false};
};

// todo: some resources may be freed or recycled, track these and free them before frame begins
class RenderDevice
{
public:
    explicit RenderDevice(rhi::GraphicsAPIType APIType, uint32_t numFrames);

    void Init(rhi::RHIViewport* mainViewport);

    void Destroy();

    void ExecuteFrame(rhi::RHIViewport* viewport, RenderGraph* rdg, bool present = true);

    void ExecuteFrame(rhi::RHIViewport* viewport,
                      const std::vector<RenderGraph*>& rdgs,
                      bool present = true);

    void ExecuteImmediate(rhi::RHIViewport* viewport, RenderGraph* rdg);

    rhi::RHITexture* CreateTextureColorRT(const TextureFormat& texFormat,
                                          TextureUsageHint usageHint,
                                          std::string texName);

    rhi::RHITexture* CreateTextureDepthStencilRT(const TextureFormat& texFormat,
                                                 TextureUsageHint usageHint,
                                                 std::string texName);

    rhi::RHITexture* CreateTextureStorage(const TextureFormat& texFormat,
                                          TextureUsageHint usageHint,
                                          std::string texName);

    rhi::RHITexture* CreateTextureSampled(const TextureFormat& texFormat,
                                          TextureUsageHint usageHint,
                                          std::string texName);

    rhi::RHITexture* CreateTextureDummy(const TextureFormat& texFormat,
                                        TextureUsageHint usageHint,
                                        std::string texName);

    rhi::RHITexture* CreateTextureProxy(rhi::RHITexture* baseTexture,
                                        const TextureProxyFormat& proxyFormat,
                                        std::string texName);

    // rhi::RHITexture* GetTextureRDFromHandle(const rhi::RHITexture* handle);

    void DestroyTexture(rhi::RHITexture* texture);

    // rhi::RHITexture* CreateTexture(const rhi::TextureInfo& textureInfo);
    //
    // rhi::RHITexture* CreateTextureProxy(const rhi::RHITexture* baseTexture,
    //                                       const rhi::TextureProxyInfo& proxyInfo);

    // rhi::RHITexture* GetBaseTextureForProxy(const rhi::RHITexture* handle) const;

    // bool IsProxyTexture(const rhi::RHITexture* handle) const;

    void GenerateTextureMipmaps(rhi::RHITexture* textureHandle, rhi::RHICommandList* cmdList);

    rhi::RHIBuffer* CreateVertexBuffer(uint32_t dataSize, const uint8_t* pData);

    rhi::RHIBuffer* CreateIndexBuffer(uint32_t dataSize, const uint8_t* pData);

    rhi::RHIBuffer* CreateUniformBuffer(uint32_t dataSize, const uint8_t* pData);

    rhi::RHIBuffer* CreateStorageBuffer(uint32_t dataSize,
                                        const uint8_t* pData,
                                        std::string bufferName);

    rhi::RHIBuffer* CreateIndirectBuffer(uint32_t dataSize,
                                         const uint8_t* pData,
                                         std::string bufferName);

    void UpdateBuffer(rhi::RHIBuffer* bufferHandle,
                      uint32_t dataSize,
                      const uint8_t* pData,
                      uint32_t offset = 0);

    void DestroyBuffer(rhi::RHIBuffer* bufferHandle);

    rhi::RenderPassHandle GetOrCreateRenderPass(const rhi::RenderPassLayout& layout);

    rhi::RHIPipeline* GetOrCreateGfxPipeline(
        rhi::GfxPipelineStates& PSO,
        rhi::RHIShader* shader,
        const rhi::RenderPassHandle& renderPass,
        const std::vector<rhi::ShaderSpecializationConstant>& specializationConstants = {});

    rhi::RHIPipeline* GetOrCreateGfxPipeline(
        rhi::GfxPipelineStates& PSO,
        rhi::RHIShader* shader,
        const rhi::RenderPassLayout& renderPassLayout,
        const std::vector<rhi::ShaderSpecializationConstant>& specializationConstants = {});

    rhi::RHIPipeline* GetOrCreateComputePipeline(rhi::RHIShader* shader);

    rhi::RHIViewport* CreateViewport(void* pWindow,
                                     uint32_t width,
                                     uint32_t height,
                                     bool enableVSync = true);

    void ResizeViewport(rhi::RHIViewport* viewport, uint32_t width, uint32_t height);

    void UpdateGraphicsPassOnResize(GraphicsPass& gfxPass, rhi::RHIViewport* viewport);

    // rhi::RHITexture* LoadTexture2D(const std::string& file, bool requireMipmap = false);

    rhi::RHITexture* LoadTexture2D(const std::string& file, bool requireMipmap = false);

    void LoadSceneTextures(const sg::Scene* scene, std::vector<rhi::RHITexture*>& outTextures);

    void LoadTextureEnv(const std::string& file, EnvTexture* texture);

    rhi::RHISampler* CreateSampler(const rhi::RHISamplerCreateInfo& samplerInfo);

    // rhi::TextureSubResourceRange GetTextureSubResourceRange(rhi::RHITexture* handle);

    // auto* GetRHI() const
    // {
    //     return GDynamicRHI;
    // }

    auto* GetRHIDebug() const
    {
        return m_RHIDebug;
    }

    auto GetFramesCounter() const
    {
        return m_framesCounter;
    }

    rhi::RHICommandList* GetCurrentUploadCmdList() const
    {
        return m_frames[m_currentFrame].uploadCmdList;
    }

    rhi::RHICommandList* GetCurrentDrawCmdList() const
    {
        return m_frames[m_currentFrame].drawCmdList;
    }

    RendererServer* GetRendererServer() const
    {
        return m_rendererServer;
    }

    void ProcessViewportResize(uint32_t width, uint32_t height);

    void WaitForPreviousFrames();

    void WaitForAllFrames();

    void NextFrame();

    void WaitForIdle() const
    {
        GDynamicRHI->WaitDeviceIdle();
    }

    const rhi::GPUInfo& GetGPUInfo() const
    {
        return GDynamicRHI->QueryGPUInfo();
    }

private:
    void BeginFrame();

    void EndFrame();

    void ProcessPendingFreeResources(uint32_t frameIndex);

    void UpdateBufferInternal(rhi::RHIBuffer* bufferHandle,
                              uint32_t offset,
                              uint32_t dataSize,
                              const uint8_t* pData);

    void UpdateTextureOneTime(rhi::RHITexture* textureHandle,
                              const Vec3i& textureSize,
                              uint32_t dataSize,
                              const uint8_t* pData);

    void UpdateTextureBatch(rhi::RHITexture* textureHandle,
                            const Vec3i& textureSize,
                            const uint8_t* pData);

    void DestroyViewport(rhi::RHIViewport* viewport);

    static size_t CalcRenderPassLayoutHash(const rhi::RenderPassLayout& layout);
    static size_t CalcFramebufferHash(const rhi::FramebufferInfo& info,
                                      rhi::RenderPassHandle renderPassHandle);

    static size_t CalcGfxPipelineHash(
        const rhi::GfxPipelineStates& pso,
        rhi::RHIShader* shader,
        const rhi::RenderPassHandle& renderPass,
        const std::vector<rhi::ShaderSpecializationConstant>& specializationConstants);

    static size_t CalcGfxPipelineHash(
        const rhi::GfxPipelineStates& pso,
        rhi::RHIShader* shader,
        const rhi::RenderPassLayout& renderPassLayout,
        const std::vector<rhi::ShaderSpecializationConstant>& specializationConstants);

    static size_t CalcComputePipelineHash(rhi::RHIShader* shader);

    static size_t CalcSamplerHash(const rhi::RHISamplerCreateInfo& info);

    size_t PadUniformBufferSize(size_t originalSize);

    size_t PadStorageBufferSize(size_t originalSize);

    const rhi::GraphicsAPIType m_APIType;
    const uint32_t m_numFrames;
    uint32_t m_currentFrame{0};
    uint64_t m_framesCounter{0};
    std::vector<RenderFrame> m_frames;

    // rhi::DynamicRHI* GDynamicRHI{nullptr};
    rhi::RHIDebug* m_RHIDebug{nullptr};


    BufferStagingManager* m_bufferStagingMgr{nullptr};
    TextureStagingManager* m_textureStagingMgr{nullptr};

    RendererServer* m_rendererServer{nullptr};
    TextureManager* m_textureManager{nullptr};

    DeletionQueue m_deletionQueue;

    HashMap<size_t, rhi::RenderPassHandle> m_renderPassCache;
    HashMap<size_t, rhi::RHIPipeline*> m_pipelineCache;
    HashMap<size_t, rhi::RHISampler*> m_samplerCache;
    // HashMap<rhi::RHITexture*, rhi::RHITexture*> m_textureMap;
    std::vector<rhi::RHIBuffer*> m_buffers;
    std::vector<GraphicsPass> m_gfxPasses;
    std::vector<ComputePass> m_computePasses;
    std::vector<rhi::RHIViewport*> m_viewports;
    rhi::RHIViewport* m_mainViewport{nullptr};

    friend class GraphicsPassBuilder;
    friend class ComputePassBuilder;
};
} // namespace zen::rc