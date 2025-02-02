#pragma once
#include "Common/HashMap.h"
#include "Common/Queue.h"
#include "Graphics/RHI/RHIDebug.h"
#include "RenderCoreDefs.h"

#define TEXTURE_UPLOAD_REGION_SIZE            64
#define STAGING_BLOCK_SIZE_BYTES              (256 * 1024)
#define STAGING_POOL_SIZE_BYTES               (128 * 1024 * 1024)
#define MAX_TEXTURE_STAGING_PENDING_FREE_SIZE (4 * 1024 * 1024)

namespace zen::sg
{
class Scene;
}

namespace zen::rc
{
class RenderDevice;
class RenderGraph;

class GraphicsPassBuilder
{
public:
    explicit GraphicsPassBuilder(RenderDevice* renderDevice) : m_renderDevice(renderDevice) {}

    GraphicsPassBuilder& SetVertexShader(const std::string& file)
    {
        m_vsPath = file;
        m_hasVS  = true;
        return *this;
    }

    GraphicsPassBuilder& SetFragmentShader(const std::string& file)
    {
        m_fsPath = file;
        m_hasFS  = true;
        return *this;
    }

    GraphicsPassBuilder& SetShaderSpecializationConstants(uint32_t constantId, int value)
    {
        m_specializationConstants[constantId] = value;
        return *this;
    }

    GraphicsPassBuilder& AddColorRenderTarget(rhi::DataFormat format,
                                              rhi::TextureUsage usage,
                                              const rhi::TextureHandle& handle)
    {
        m_rpLayout.AddColorRenderTarget(format, usage, handle);
        m_rpLayout.SetColorTargetLoadStoreOp(rhi::RenderTargetLoadOp::eClear,
                                             rhi::RenderTargetStoreOp::eStore);
        m_framebufferInfo.numRenderTarget++;
        return *this;
    }

    GraphicsPassBuilder& SetDepthStencilTarget(
        rhi::DataFormat format,
        const rhi::TextureHandle& handle,
        rhi::RenderTargetLoadOp loadOp   = rhi::RenderTargetLoadOp::eClear,
        rhi::RenderTargetStoreOp storeOp = rhi::RenderTargetStoreOp::eNone)
    {
        m_rpLayout.SetDepthStencilRenderTarget(format, handle);
        m_rpLayout.SetDepthStencilTargetLoadStoreOp(loadOp, storeOp);
        m_framebufferInfo.numRenderTarget++;
        return *this;
    }

    GraphicsPassBuilder& SetNumSamples(rhi::SampleCount sampleCount)
    {
        m_rpLayout.SetNumSamples(sampleCount);
        return *this;
    }

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

    GraphicsPass Build();

private:
    RenderDevice* m_renderDevice{nullptr};
    rhi::RHIViewport* m_viewport{nullptr};
    std::string m_tag;
    bool m_hasVS{false};
    bool m_hasFS{false};
    bool m_finished{false};
    std::string m_vsPath;
    std::string m_fsPath;
    rhi::GfxPipelineStates m_PSO{};
    rhi::RenderPassLayout m_rpLayout{};
    rhi::FramebufferInfo m_framebufferInfo{};
    HashMap<uint32_t, std::vector<rhi::ShaderResourceBinding>> m_dsBindings;
    HashMap<uint32_t, int> m_specializationConstants;
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
    rhi::BufferHandle buffer;
};

class TextureStagingManager
{
public:
    explicit TextureStagingManager(RenderDevice* renderDevice) :
        m_renderDevice(renderDevice), m_usedMemory(0), m_pendingFreeMemorySize(0)
    {}

    rhi::BufferHandle RequireBuffer(uint32_t requiredSize);

    void ReleaseBuffer(const rhi::BufferHandle& buffer);

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
        rhi::BufferHandle buffer;
        uint32_t usedFrame;
    };
    std::vector<Entry> m_usedBuffers;
    std::vector<Entry> m_pendingFreeBuffers;
    std::vector<Entry> m_freeBuffers;
    // for memory management
    std::vector<rhi::BufferHandle> m_allocatedBuffers;
    uint32_t m_usedMemory;
    uint32_t m_pendingFreeMemorySize;
};

// todo: fix bugs when loading a large number of textures
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
    rhi::DynamicRHI* m_RHI{nullptr};
    struct StagingBuffer
    {
        rhi::BufferHandle handle;
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
};

class RenderDevice
{
public:
    explicit RenderDevice(rhi::GraphicsAPIType APIType, uint32_t numFrames) :
        m_APIType(APIType), m_numFrames(numFrames)
    {}

    void Init();

    void Destroy();

    void ExecuteFrame(rhi::RHIViewport* viewport, RenderGraph* rdg, bool present = true);

    rhi::TextureHandle CreateTexture(const rhi::TextureInfo& textureInfo, const std::string& tag);

    rhi::BufferHandle CreateVertexBuffer(uint32_t dataSize, const uint8_t* pData);

    rhi::BufferHandle CreateIndexBuffer(uint32_t dataSize, const uint8_t* pData);

    rhi::BufferHandle CreateUniformBuffer(uint32_t dataSize, const uint8_t* pData);

    rhi::BufferHandle CreateStorageBuffer(uint32_t dataSize, const uint8_t* pData);

    void UpdateBuffer(rhi::BufferHandle bufferHandle,
                      uint32_t dataSize,
                      const uint8_t* pData,
                      uint32_t offset = 0);

    void DestroyBuffer(rhi::BufferHandle bufferHandle);

    rhi::RenderPassHandle GetOrCreateRenderPass(const rhi::RenderPassLayout& layout);

    rhi::FramebufferHandle GetOrCreateFramebuffer(rhi::RenderPassHandle renderPassHandle,
                                                  const rhi::FramebufferInfo& fbInfo);

    rhi::PipelineHandle GetOrCreateGfxPipeline(
        rhi::GfxPipelineStates& PSO,
        const rhi::ShaderHandle& shader,
        const rhi::RenderPassHandle& renderPass,
        const std::vector<rhi::ShaderSpecializationConstant>& specializationConstants = {});

    rhi::RHIViewport* CreateViewport(void* pWindow,
                                     uint32_t width,
                                     uint32_t height,
                                     bool enableVSync = true);

    void ResizeViewport(rhi::RHIViewport** viewport,
                        void* pWindow,
                        uint32_t width,
                        uint32_t height);

    rhi::TextureHandle RequestTexture2D(const std::string& file, bool requireMipmap = false);

    void RegisterSceneTextures(const sg::Scene* scene,
                               std::vector<rhi::TextureHandle>& outTextures);

    rhi::SamplerHandle CreateSampler(const rhi::SamplerInfo& samplerInfo);

    rhi::TextureSubResourceRange GetTextureSubResourceRange(rhi::TextureHandle handle);

    auto* GetRHI() const
    {
        return m_RHI;
    }

    auto* GetRHIDebug() const
    {
        return m_RHIDebug;
    }

    auto GetFramesCounter() const
    {
        return m_framesCounter;
    }

    void WaitForPreviousFrames();

    void WaitForAllFrames();

    void NextFrame();

    void WaitForIdle() const
    {
        m_RHI->WaitDeviceIdle();
    }

private:
    void BeginFrame();

    void EndFrame();

    void UpdateBufferInternal(rhi::BufferHandle bufferHandle,
                              uint32_t offset,
                              uint32_t dataSize,
                              const uint8_t* pData);

    void UpdateTexture(rhi::TextureHandle textureHandle,
                       const Vec3i& textureSize,
                       uint32_t dataSize,
                       const uint8_t* pData);

    void UpdateTextureOneTime(rhi::TextureHandle textureHandle,
                              const Vec3i& textureSize,
                              uint32_t dataSize,
                              const uint8_t* pData);

    void UpdateTextureBatch(rhi::TextureHandle textureHandle,
                            const Vec3i& textureSize,
                            const uint8_t* pData);

    void DestroyViewport(rhi::RHIViewport* viewport);

    static size_t CalcRenderPassLayoutHash(const rhi::RenderPassLayout& layout);
    static size_t CalcFramebufferHash(const rhi::FramebufferInfo& info,
                                      rhi::RenderPassHandle renderPassHandle);
    static size_t CalcGfxPipelineHash(
        const rhi::GfxPipelineStates& pso,
        const rhi::ShaderHandle& shader,
        const rhi::RenderPassHandle& renderPass,
        const std::vector<rhi::ShaderSpecializationConstant>& specializationConstants);

    size_t PadUniformBufferSize(size_t originalSize);

    size_t PadStorageBufferSize(size_t originalSize);

    const rhi::GraphicsAPIType m_APIType;
    const uint32_t m_numFrames;
    uint32_t m_currentFrame{0};
    uint64_t m_framesCounter{0};
    std::vector<RenderFrame> m_frames;
    rhi::DynamicRHI* m_RHI{nullptr};
    rhi::RHIDebug* m_RHIDebug{nullptr};
    BufferStagingManager* m_bufferStagingMgr{nullptr};
    TextureStagingManager* m_textureStagingMgr{nullptr};
    DeletionQueue m_deletionQueue;

    HashMap<size_t, rhi::RenderPassHandle> m_renderPassCache;
    HashMap<size_t, rhi::FramebufferHandle> m_framebufferCache;
    HashMap<std::string, rhi::TextureHandle> m_textureCache;
    HashMap<size_t, rhi::PipelineHandle> m_pipelineCache;
    std::vector<rhi::BufferHandle> m_buffers;
    std::vector<GraphicsPass> m_gfxPasses;
    std::vector<rhi::RHIViewport*> m_viewports;

    friend class GraphicsPassBuilder;
};
} // namespace zen::rc