#pragma once
#include "Common/HashMap.h"
#include "Common/Queue.h"
#include "Graphics/RHI/DynamicRHI.h"

namespace zen::rc
{
class RenderDevice;
class RenderGraph;

struct RenderPipeline
{
    rhi::RenderPassHandle renderPass;
    rhi::PipelineHandle pipeline;
    std::vector<rhi::DescriptorSetHandle> descriptorSets;
};

class RenderPipelineBuilder
{
public:
    RenderPipelineBuilder& Begin(RenderDevice* renderDevice)
    {
        m_renderDevice = renderDevice;
        return *this;
    }

    RenderPipelineBuilder& SetVertexShader(const std::string& file)
    {
        m_vsPath = file;
        m_hasVS  = true;
        return *this;
    }

    RenderPipelineBuilder& SetFragmentShader(const std::string& file)
    {
        m_fsPath = file;
        m_hasFS  = true;
        return *this;
    }

    RenderPipelineBuilder& AddColorRenderTarget(rhi::DataFormat format, rhi::TextureUsage usage)
    {
        m_rpLayout.AddColorRenderTarget(format, usage);
        m_rpLayout.SetColorTargetLoadStoreOp(rhi::RenderTargetLoadOp::eClear,
                                             rhi::RenderTargetStoreOp::eStore);
        return *this;
    }

    RenderPipelineBuilder& SetDepthStencilTarget(rhi::DataFormat format)
    {
        m_rpLayout.SetDepthStencilRenderTarget(format);
        return *this;
    }

    RenderPipelineBuilder& SetNumSamples(rhi::SampleCount sampleCount)
    {
        m_rpLayout.SetNumSamples(sampleCount);
        return *this;
    }

    RenderPipelineBuilder& SetFramebufferInfo(const rhi::FramebufferInfo& fbInfo)
    {
        m_fbInfo = fbInfo;
        return *this;
    }

    RenderPipelineBuilder& SetShaderResourceBinding(
        uint32_t setIndex,
        const std::vector<rhi::ShaderResourceBinding>& bindings)
    {
        m_dsBindings[setIndex] = bindings;
        return *this;
    }

    RenderPipelineBuilder& SetPipelineState(const rhi::GfxPipelineStates& pso)
    {
        m_PSO = pso;
        return *this;
    }

    RenderPipeline Build();

private:
    RenderDevice* m_renderDevice{nullptr};
    bool m_hasVS{false};
    bool m_hasFS{false};
    bool m_finished{false};
    std::string m_vsPath;
    std::string m_fsPath;
    rhi::SampleCount m_numSamples{rhi::SampleCount::e1};
    rhi::GfxPipelineStates m_PSO{};
    rhi::RenderPassLayout m_rpLayout{};
    rhi::FramebufferInfo m_fbInfo{};
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
    rhi::BufferHandle buffer;
};

class StagingBufferManager
{
public:
    StagingBufferManager(RenderDevice* renderDevice, uint32_t bufferSize, uint64_t poolSize);

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

    friend class RenderDevice;
};

struct RenderFrame
{
    rhi::RHICommandListContext* cmdListContext{nullptr};
    rhi::RHICommandList* uploadCmdList{nullptr};
    rhi::RHICommandList* drawCmdList{nullptr};
};

class RenderDevice
{
public:
    explicit RenderDevice(rhi::GraphicsAPIType APIType, uint32_t numFrames) :
        m_APIType(APIType), m_numFrames(numFrames)
    {}

    void Init();

    void Destroy();

    void ExecuteFrame(rhi::RHIViewport* viewport, RenderGraph* rdg);

    rhi::BufferHandle CreateVertexBuffer(uint32_t dataSize, const uint8_t* pData);

    rhi::BufferHandle CreateIndexBuffer(uint32_t dataSize, const uint8_t* pData);

    rhi::BufferHandle CreateUniformBuffer(uint32_t dataSize, const uint8_t* pData);

    void UpdateBuffer(rhi::BufferHandle bufferHandle,
                      uint32_t dataSize,
                      const uint8_t* pData,
                      uint32_t offset = 0);

    void DestroyBuffer(rhi::BufferHandle bufferHandle);

    rhi::RenderPassHandle GetOrCreateRenderPass(const rhi::RenderPassLayout& layout);

    rhi::FramebufferHandle GetOrCreateFramebuffer(rhi::RenderPassHandle renderPassHandle,
                                                  const rhi::FramebufferInfo& fbInfo);

    rhi::PipelineHandle GetOrCreateGfxPipeline(rhi::GfxPipelineStates& PSO,
                                               const rhi::ShaderHandle& shader,
                                               const rhi::RenderPassHandle& renderPass);

    rhi::RHIViewport* CreateViewport(void* pWindow,
                                     uint32_t width,
                                     uint32_t height,
                                     bool enableVSync = true);

    void ResizeViewport(rhi::RHIViewport** viewport,
                        void* pWindow,
                        uint32_t width,
                        uint32_t height);

    rhi::TextureHandle RequestTexture2D(const std::string& file, bool requireMipmap = false);

    rhi::SamplerHandle CreateSampler(const rhi::SamplerInfo& samplerInfo);

    auto* GetRHI() const
    {
        return m_RHI;
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
    static size_t CalcGfxPipelineHash(const rhi::GfxPipelineStates& pso,
                                      const rhi::ShaderHandle& shader,
                                      const rhi::RenderPassHandle& renderPass);

    const rhi::GraphicsAPIType m_APIType;
    const uint32_t m_numFrames;
    uint32_t m_currentFrame{0};
    uint64_t m_framesCounter{0};
    std::vector<RenderFrame> m_frames;
    rhi::DynamicRHI* m_RHI{nullptr};
    StagingBufferManager* m_stagingBufferMgr{nullptr};
    DeletionQueue m_deletionQueue;

    HashMap<size_t, rhi::RenderPassHandle> m_renderPassCache;
    HashMap<size_t, rhi::FramebufferHandle> m_framebufferCache;
    HashMap<std::string, rhi::TextureHandle> m_textureCache;
    HashMap<size_t, rhi::PipelineHandle> m_pipelineCache;
    std::vector<rhi::BufferHandle> m_buffers;
    std::vector<RenderPipeline> m_renderPipelines;
    std::vector<rhi::RHIViewport*> m_viewports;

    friend class RenderPipelineBuilder;
};
} // namespace zen::rc