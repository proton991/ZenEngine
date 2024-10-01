#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RHI/ShaderUtil.h"
#include "Graphics/RenderCore/V2/RenderGraph.h"
#include "Graphics/VulkanRHI/VulkanCommands.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include <fstream>

#define STAGING_BLOCK_SIZE_BYTES (256 * 1024)
#define STAGING_POOL_SIZE_BYTES  (128 * 1024 * 1024)

namespace zen::rc
{
std::vector<uint8_t> LoadSpirvCode(const std::string& name)
{
    const auto path = std::string(SPV_SHADER_PATH) + name;
    std::ifstream file(path, std::ios::ate | std::ios::binary);

    VERIFY_EXPR_MSG_F(file.is_open(), "Failed to load shader file {}", path);
    //find what the size of the file is by looking up the location of the cursor
    //because the cursor is at the end, it gives the size directly in bytes
    size_t fileSize = (size_t)file.tellg();

    //spir-v expects the buffer to be on uint32, so make sure to reserve an int vector big enough for the entire file
    std::vector<uint8_t> buffer(fileSize / sizeof(uint8_t));

    //put file cursor at beginning
    file.seekg(0);

    //load the entire file into the buffer
    file.read((char*)buffer.data(), fileSize);

    //now that the file is loaded into the buffer, we can close it
    file.close();

    return buffer;
}

RenderPipeline RenderPipelineBuilder::Build()
{
    using namespace zen::rhi;
    DynamicRHI* RHI       = m_renderDevice->GetRHI();
    auto shaderGroupSpirv = MakeRefCountPtr<ShaderGroupSPIRV>();

    if (m_hasVS)
    {
        shaderGroupSpirv->SetStageSPIRV(ShaderStage::eVertex, LoadSpirvCode(m_vsPath));
    }
    if (m_hasFS)
    {
        shaderGroupSpirv->SetStageSPIRV(ShaderStage::eFragment, LoadSpirvCode(m_fsPath));
    }
    ShaderGroupInfo shaderGroupInfo{};
    ShaderUtil::ReflectShaderGroupInfo(shaderGroupSpirv, shaderGroupInfo);
    ShaderHandle shader = RHI->CreateShader(shaderGroupInfo);

    RenderPipeline renderPipeline;
    renderPipeline.renderPass = m_renderDevice->GetOrCreateRenderPass(m_rpLayout);
    renderPipeline.framebuffer =
        m_renderDevice->GetOrCreateFramebuffer(renderPipeline.renderPass, m_fbInfo);
    renderPipeline.pipeline =
        m_renderDevice->GetOrCreateGfxPipeline(m_PSO, shader, renderPipeline.renderPass);

    renderPipeline.descriptorSets.resize(shaderGroupInfo.SRDs.size());
    for (const auto& kv : m_dsBindings)
    {
        const auto setIndex          = kv.first;
        const auto& bindings         = kv.second;
        DescriptorSetHandle dsHandle = RHI->CreateDescriptorSet(shader, setIndex);
        RHI->UpdateDescriptorSet(dsHandle, bindings);
        renderPipeline.descriptorSets[setIndex] = dsHandle;
    }

    RHI->DestroyShader(shader);
    m_renderDevice->m_renderPipelines.push_back(renderPipeline);
    return renderPipeline;
}

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
    for (auto* viewport : m_viewports)
    {
        m_RHI->DestroyViewport(viewport);
    }
    for (auto& kv : m_framebufferCache)
    {
        m_RHI->DestroyFramebuffer(kv.second);
    }
    for (auto& kv : m_renderPassCache)
    {
        m_RHI->DestroyRenderPass(kv.second);
    }
    for (auto& kv : m_pipelineCache)
    {
        m_RHI->DestroyPipeline(kv.second);
    }

    for (auto& rp : m_renderPipelines)
    {
        for (const auto& ds : rp.descriptorSets)
        {
            m_RHI->DestroyDescriptorSet(ds);
        }
    }
    for (auto& buffer : m_buffers)
    {
        m_RHI->DestroyBuffer(buffer);
    }

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
    m_buffers.push_back(vertexBuffer);
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
    m_buffers.push_back(indexBuffer);
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
    m_buffers.push_back(uniformBuffer);
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

rhi::RenderPassHandle RenderDevice::GetOrCreateRenderPass(const rhi::RenderPassLayout& layout)
{
    auto hash = CalcRenderPassLayoutHash(layout);
    if (!m_renderPassCache.contains(hash))
    {
        // create new one
        m_renderPassCache[hash] = m_RHI->CreateRenderPass(layout);
    }
    return m_renderPassCache[hash];
}

rhi::FramebufferHandle RenderDevice::GetOrCreateFramebuffer(rhi::RenderPassHandle renderPassHandle,
                                                            const rhi::FramebufferInfo& fbInfo)
{
    auto hash = CalcFramebufferHash(fbInfo, renderPassHandle);
    if (!m_framebufferCache.contains(hash))
    {
        // create new one
        m_framebufferCache[hash] = m_RHI->CreateFramebuffer(renderPassHandle, fbInfo);
    }
    return m_framebufferCache[hash];
}

rhi::PipelineHandle RenderDevice::GetOrCreateGfxPipeline(rhi::GfxPipelineStates& PSO,
                                                         const rhi::ShaderHandle& shader,
                                                         const rhi::RenderPassHandle& renderPass)
{
    auto hash = CalcGfxPipelineHash(PSO, shader, renderPass);
    if (!m_pipelineCache.contains(hash))
    {
        // create new one
        m_pipelineCache[hash] = m_RHI->CreateGfxPipeline(shader, PSO, renderPass, 0);
    }
    return m_pipelineCache[hash];
}

rhi::RHIViewport* RenderDevice::CreateViewport(void* pWindow,
                                               uint32_t width,
                                               uint32_t height,
                                               bool enableVSync)
{
    auto* viewport = m_RHI->CreateViewport(pWindow, width, height, enableVSync);
    m_viewports.push_back(viewport);
    return viewport;
}

void RenderDevice::DestroyViewport(rhi::RHIViewport* viewport)
{
    auto it = m_viewports.begin();
    while (it != m_viewports.end())
    {
        if (viewport == *it)
        {
            m_viewports.erase(it);
            break;
        }
    }
    m_RHI->DestroyViewport(viewport);
}

void RenderDevice::ResizeViewport(rhi::RHIViewport** viewport,
                                  void* window,
                                  uint32_t width,
                                  uint32_t height)
{
    if (viewport != nullptr &&
        ((*viewport)->GetWidth() != width || (*viewport)->GetHeight() != height))
    {
        DestroyViewport(*viewport);
        *viewport = CreateViewport(window, width, height);
    }
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

size_t RenderDevice::CalcRenderPassLayoutHash(const rhi::RenderPassLayout& layout)
{
    std::size_t seed = 0;

    // Hashing utility
    auto combineHash = [&seed](auto&& value) {
        seed ^= std::hash<std::decay_t<decltype(value)>>{}(value) + 0x9e3779b9 + (seed << 6) +
            (seed >> 2);
    };

    // Hash basic types
    combineHash(layout.GetNumColorRenderTargets());
    combineHash(layout.GetNumSamples());
    combineHash(layout.GetColorRenderTargetLoadOp());
    combineHash(layout.GetColorRenderTargetStoreOp());
    combineHash(layout.GetDepthStencilRenderTargetLoadOp());
    combineHash(layout.GetDepthStencilRenderTargetStoreOp());
    combineHash(layout.HasDepthStencilRenderTarget());

    // Hash color render targets
    for (const auto& rt : layout.GetColorRenderTargets())
    {
        // Assuming RenderTarget is hashable
        combineHash(rt.format);
        combineHash(rt.usage);
    }
    //
    // // Hash depth stencil render target (assuming RenderTarget is hashable)
    combineHash(layout.GetDepthStencilRenderTarget().format);
    combineHash(layout.GetDepthStencilRenderTarget().usage);

    return seed;
}

size_t RenderDevice::CalcFramebufferHash(const rhi::FramebufferInfo& info,
                                         rhi::RenderPassHandle renderPassHandle)
{
    std::size_t seed = 0;

    // Hash each member and combine the result
    std::hash<uint32_t> uint32Hasher;
    std::hash<uint64_t> uint64Hasher;

    // Hash the individual members
    seed ^= uint32Hasher(info.numRenderTarget) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    seed ^= uint32Hasher(info.width) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    seed ^= uint32Hasher(info.height) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    seed ^= uint32Hasher(info.depth) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    seed ^= uint64Hasher(renderPassHandle.value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    for (uint32_t i = 0; i < info.numRenderTarget; i++)
    {
        seed ^= uint64Hasher(info.renderTargets[i].value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
}

size_t RenderDevice::CalcGfxPipelineHash(const rhi::GfxPipelineStates& pso,
                                         const rhi::ShaderHandle& shader,
                                         const rhi::RenderPassHandle& renderPass)
{
    std::size_t seed = 0;
    // Hashing utility
    auto combineHash = [&seed](auto&& value) {
        seed ^= std::hash<std::decay_t<decltype(value)>>{}(value) + 0x9e3779b9 + (seed << 6) +
            (seed >> 2);
    };
    combineHash(shader.value);
    combineHash(renderPass.value);
    combineHash(pso.primitiveType);

    return seed;
}
} // namespace zen::rc