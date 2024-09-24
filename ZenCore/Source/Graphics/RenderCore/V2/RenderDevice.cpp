#include "Graphics/RenderCore/V2/RenderDevice.h"

#include "Graphics/RenderCore/V2/RenderGraph.h"
#include "Graphics/VulkanRHI/VulkanCommands.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"

#define STAGING_BLOCK_SIZE_BYTES (256 * 1024)
#define STAGING_POOL_SIZE_BYTES  (128 * 1024 * 1024)

namespace zen::rc
{
StagingBufferManager::StagingBufferManager(RenderDevice* renderDevice,
                                           uint32_t blockSize,
                                           uint64_t poolSize,
                                           bool supportSegament) :
    SUPPORT_SEGAMENTATION(supportSegament),
    BUFFER_SIZE(blockSize),
    POOL_SIZE(poolSize),
    m_renderDevice(renderDevice),
    m_RHI(renderDevice->GetRHI()),
    m_currentBlockIndex(0)
{
    if (POOL_SIZE < BUFFER_SIZE * 4)
    {
        POOL_SIZE = BUFFER_SIZE * 4;
    }
}

void StagingBufferManager::Init(uint32_t numFrames)
{
    for (uint32_t i = 0; i < numFrames; i++)
    {
        StagingBuffer buffer;
        buffer.handle =
            m_RHI->CreateBuffer(BUFFER_SIZE, BitField(rhi::BufferUsageFlagBits::eTransferSrcBuffer),
                                rhi::BufferAllocateType::eCPU);
        buffer.usedFrame    = 0;
        buffer.occupiedSize = 0;
        m_bufferBlocks.emplace_back(std::move(buffer));
    }
}

void StagingBufferManager::Destroy()
{
    for (auto& buffer : m_bufferBlocks)
    {
        m_RHI->DestroyBuffer(buffer.handle);
    }
}

void StagingBufferManager::InsertNewBlock()
{
    StagingBuffer buffer;
    buffer.handle =
        m_RHI->CreateBuffer(BUFFER_SIZE, BitField(rhi::BufferUsageFlagBits::eTransferSrcBuffer),
                            rhi::BufferAllocateType::eCPU);
    buffer.usedFrame    = 0;
    buffer.occupiedSize = 0;
    m_bufferBlocks.insert(m_bufferBlocks.begin() + m_currentBlockIndex, std::move(buffer));
}

bool StagingBufferManager::FitInBlock(uint32_t blockIndex,
                                      uint32_t requiredSize,
                                      uint32_t requiredAlign,
                                      StagingSubmitResult* result)
{
    uint32_t occupiedSize   = m_bufferBlocks[blockIndex].occupiedSize;
    uint32_t alignRemainder = occupiedSize % requiredAlign;
    occupiedSize += requiredAlign - alignRemainder;
    int32_t availableBytes = static_cast<int32_t>(BUFFER_SIZE) - static_cast<int32_t>(occupiedSize);
    if (static_cast<int32_t>(requiredSize) < availableBytes)
    {
        // enough room, allocate
        result->writeOffset = occupiedSize;
    }
    else if (SUPPORT_SEGAMENTATION && availableBytes <= static_cast<int32_t>(requiredSize))
    {
        result->writeOffset = occupiedSize;
        result->writeSize   = availableBytes - (availableBytes % requiredAlign);
    }
    else
    {
        return false;
    }
    return true;
}

void StagingBufferManager::BeginSubmit(uint32_t requiredSize,
                                       StagingSubmitResult* result,
                                       uint32_t requiredAlign)
{
    result->writeSize = requiredSize;
    while (true)
    {
        result->writeOffset = 0;
        if (m_bufferBlocks[m_currentBlockIndex].usedFrame == m_renderDevice->GetFramesCounter())
        {
            if (!FitInBlock(m_currentBlockIndex, requiredSize, requiredAlign, result))
            {
                m_currentBlockIndex = (m_currentBlockIndex + 1) % m_bufferBlocks.size();
                if (m_bufferBlocks[m_currentBlockIndex].usedFrame ==
                    m_renderDevice->GetFramesCounter())
                {
                    bool secondAttempt =
                        FitInBlock(m_currentBlockIndex, requiredSize, requiredAlign, result);
                    if (!secondAttempt)
                    {
                        if (CanInsertNewBlock())
                        {
                            InsertNewBlock();
                        }
                        else
                        {
                            // not enough space, wait for all frames
                            result->flushAction = StagingFlushAction::eFull;
                        }
                    }
                }
                else
                {
                    continue;
                }
            }
        }
        else if (m_renderDevice->GetFramesCounter() -
                     m_bufferBlocks[m_currentBlockIndex].usedFrame >=
                 m_bufferBlocks.size())
        {
            // reuse old one
            m_bufferBlocks[m_currentBlockIndex].usedFrame    = 0;
            m_bufferBlocks[m_currentBlockIndex].occupiedSize = 0;
        }
        else if (CanInsertNewBlock())
        {
            InsertNewBlock();
        }
        else
        {
            // not enough space, wait for all frames
            result->flushAction = StagingFlushAction::ePartial;
        }
        break;
    }
    result->success = true;
    result->buffer  = m_bufferBlocks[m_currentBlockIndex].handle;
}

void StagingBufferManager::EndSubmit(const StagingSubmitResult* result)
{
    m_bufferBlocks[m_currentBlockIndex].occupiedSize = result->writeOffset + result->writeSize;
    m_bufferBlocks[m_currentBlockIndex].usedFrame    = m_renderDevice->GetFramesCounter();
}

void StagingBufferManager::PerformAction(StagingFlushAction action)
{
    if (action == StagingFlushAction::ePartial)
    {
        for (auto& bufferBlock : m_bufferBlocks)
        {
            if (bufferBlock.usedFrame == m_renderDevice->GetFramesCounter())
            {
                continue;
            }
            bufferBlock.usedFrame    = 0;
            bufferBlock.occupiedSize = 0;
        }
    }
    else if (action == StagingFlushAction::eFull)
    {
        for (auto& bufferBlock : m_bufferBlocks)
        {
            bufferBlock.usedFrame    = 0;
            bufferBlock.occupiedSize = 0;
        }
    }
}

void RenderDevice::Init()
{
    if (m_APIType == rhi::GraphicsAPIType::eVulkan)
    {
        m_RHI = new rhi::VulkanRHI();
        m_RHI->Init();
        m_stagingBufferMgr =
            new StagingBufferManager(this, STAGING_BLOCK_SIZE_BYTES, STAGING_POOL_SIZE_BYTES);
        m_stagingBufferMgr->Init(m_numFrames);
        m_frames.reserve(m_numFrames);
        for (uint32_t i = 0; i < m_numFrames; i++)
        {
            RenderFrame frame{};
            frame.cmdListContext = m_RHI->CreateCmdListContext();
            frame.drawCmdList    = new rhi::VulkanCommandList(
                dynamic_cast<rhi::VulkanCommandListContext*>(frame.cmdListContext));
            frame.uploadCmdList = new rhi::VulkanCommandList(
                dynamic_cast<rhi::VulkanCommandListContext*>(frame.cmdListContext));
            m_frames.emplace_back(frame);
        }
    }
    m_frames[m_currentFrame].uploadCmdList->BeginUpload();
    m_frames[m_currentFrame].drawCmdList->BeginRender();
}

void RenderDevice::Destroy()
{
    m_stagingBufferMgr->Destroy();
    delete m_stagingBufferMgr;
    for (RenderFrame& frame : m_frames)
    {
        delete frame.uploadCmdList;
        delete frame.drawCmdList;
    }
    m_RHI->Destroy();
    delete m_RHI;
}

void RenderDevice::ExecuteFrame(rhi::RHIViewport* viewport, RenderGraph* rdg)
{
    m_RHI->BeginDrawingViewport(viewport);
    rdg->Execute(m_frames[m_currentFrame].drawCmdList);
    m_RHI->EndDrawingViewport(viewport, m_frames[m_currentFrame].cmdListContext, true);
}

rhi::BufferHandle RenderDevice::CreateVertexBuffer(uint32_t dataSize, const uint8_t* pData)
{
    BitField<rhi::BufferUsageFlagBits> usages;
    usages.SetFlag(rhi::BufferUsageFlagBits::eVertexBuffer);
    usages.SetFlag(rhi::BufferUsageFlagBits::eTransferDstBuffer);

    rhi::BufferHandle vertexBuffer =
        m_RHI->CreateBuffer(dataSize, usages, rhi::BufferAllocateType::eGPU);
    UpdateBufferInternal(vertexBuffer, 0, dataSize, pData);
    return vertexBuffer;
}

rhi::BufferHandle RenderDevice::CreateIndexBuffer(uint32_t dataSize, const uint8_t* pData)
{
    BitField<rhi::BufferUsageFlagBits> usages;
    usages.SetFlag(rhi::BufferUsageFlagBits::eIndexBuffer);
    usages.SetFlag(rhi::BufferUsageFlagBits::eTransferDstBuffer);

    rhi::BufferHandle indexBuffer =
        m_RHI->CreateBuffer(dataSize, usages, rhi::BufferAllocateType::eGPU);
    UpdateBufferInternal(indexBuffer, 0, dataSize, pData);
    return indexBuffer;
}

rhi::BufferHandle RenderDevice::CreateUniformBuffer(uint32_t dataSize, const uint8_t* pData)
{
    BitField<rhi::BufferUsageFlagBits> usages;
    usages.SetFlag(rhi::BufferUsageFlagBits::eUniformBuffer);
    usages.SetFlag(rhi::BufferUsageFlagBits::eTransferDstBuffer);

    rhi::BufferHandle uniformBuffer =
        m_RHI->CreateBuffer(dataSize, usages, rhi::BufferAllocateType::eGPU);
    if (pData != nullptr)
    {
        UpdateBufferInternal(uniformBuffer, 0, dataSize, pData);
    }
    return uniformBuffer;
}

void RenderDevice::Updatebuffer(rhi::BufferHandle bufferHandle,
                                uint32_t dataSize,
                                const uint8_t* pData,
                                uint32_t offset)
{
    UpdateBufferInternal(bufferHandle, offset, dataSize, pData);
}

void RenderDevice::DestroyBuffer(rhi::BufferHandle bufferHandle)
{
    m_RHI->DestroyBuffer(bufferHandle);
}

void RenderDevice::UpdateBufferInternal(rhi::BufferHandle bufferHandle,
                                        uint32_t offset,
                                        uint32_t dataSize,
                                        const uint8_t* pData)
{
    uint32_t toSubmit      = dataSize;
    uint32_t writePosition = 0;
    while (toSubmit > 0)
    {
        StagingSubmitResult submitResult;
        m_stagingBufferMgr->BeginSubmit(toSubmit, &submitResult);

        if (submitResult.flushAction == StagingFlushAction::ePartial)
        {
            WaitForPreviousFrames();
        }
        else if (submitResult.flushAction == StagingFlushAction::eFull)
        {
            WaitForAllFrames();
        }
        m_stagingBufferMgr->PerformAction(submitResult.flushAction);

        // map staging buffer
        uint8_t* dataPtr = m_RHI->MapBuffer(submitResult.buffer);
        // copy
        memcpy(dataPtr + submitResult.writeOffset, pData + writePosition, submitResult.writeSize);
        // unmap
        m_RHI->UnmapBuffer(submitResult.buffer);
        // copy to gpu memory
        rhi::BufferCopyRegion copyRegion;
        copyRegion.srcOffset = submitResult.writeOffset;
        copyRegion.dstOffset = writePosition + offset;
        copyRegion.size      = submitResult.writeSize;
        m_frames[m_currentFrame].uploadCmdList->CopyBuffer(submitResult.buffer, bufferHandle,
                                                           copyRegion);

        m_stagingBufferMgr->EndSubmit(&submitResult);
        toSubmit -= submitResult.writeSize;
        writePosition += submitResult.writeSize;
    }
}

void RenderDevice::WaitForPreviousFrames() {}
void RenderDevice::WaitForAllFrames() {}


void RenderDevice::NextFrame()
{
    EndFrame();
    m_currentFrame = (m_currentFrame + 1) % m_frames.size();
    BeginFrame();
}

void RenderDevice::BeginFrame()
{
    m_framesCounter++;
    // m_RHI->WaitForCommandList(m_frames[m_currentFrame].drawCmdList);
    m_frames[m_currentFrame].uploadCmdList->BeginUpload();
    m_frames[m_currentFrame].drawCmdList->BeginRender();
}

void RenderDevice::EndFrame()
{
    m_frames[m_currentFrame].drawCmdList->EndRender();
    m_frames[m_currentFrame].uploadCmdList->EndUpload();
}
} // namespace zen::rc