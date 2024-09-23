#pragma once
#include "Graphics/RHI/DynamicRHI.h"

namespace zen::rc
{
class RenderDevice;
class RenderGraph;
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
    StagingBufferManager(RenderDevice* renderDevice,
                         uint32_t bufferSize,
                         uint64_t poolSize,
                         bool supportSegament = true);

    void Init(uint32_t numFrames);

    void Destroy();

    void BeginSubmit(uint32_t requiredSize,
                     StagingSubmitResult* result,
                     uint32_t requiredAlign = 32);

    void EndSubmit(const StagingSubmitResult* result);

    void PerformAction(StagingFlushAction action);

private:
    bool FitInBlock(uint32_t blockIndex,
                    uint32_t requiredSize,
                    uint32_t requiredAlign,
                    StagingSubmitResult* result);

    bool CanInsertNewBlock() const
    {
        return BUFFER_SIZE * m_bufferBlocks.size() < POOL_SIZE;
    }

    void InsertNewBlock();

    bool SUPPORT_SEGAMENTATION{true};
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

    void DestroyBuffer(rhi::BufferHandle bufferHandle);

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

private:
    void BeginFrame();

    void EndFrame();

    void UpdateBuffer(rhi::BufferHandle bufferHandle,
                      uint32_t offset,
                      uint32_t dataSize,
                      const uint8_t* pData);

    const rhi::GraphicsAPIType m_APIType;
    const uint32_t m_numFrames;
    uint32_t m_currentFrame{0};
    uint64_t m_framesCounter{0};
    std::vector<RenderFrame> m_frames;
    rhi::DynamicRHI* m_RHI{nullptr};
    StagingBufferManager* m_stagingBufferMgr{nullptr};
};
} // namespace zen::rc