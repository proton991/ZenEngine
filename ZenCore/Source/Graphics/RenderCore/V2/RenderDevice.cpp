#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/RenderGraph.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Graphics/RenderCore/V2/TextureManager.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "SceneGraph/Scene.h"
#include "Common/Helpers.h"


namespace zen::rc
{
static void CopyRegion(uint8_t const* pSrc,
                       uint8_t* pDst,
                       uint32_t srcX,
                       uint32_t srcY,
                       uint32_t srcW,
                       uint32_t srcH,
                       uint32_t srcFullW,
                       uint32_t dstStride,
                       uint32_t unitSize)
{
    uint32_t srcOffset = (srcY * srcFullW + srcX) * unitSize;
    uint32_t dstOffset = 0;
    for (uint32_t y = srcH; y > 0; y--)
    {
        uint8_t const* src = pSrc + srcOffset;
        uint8_t* dst       = pDst + dstOffset;
        for (uint32_t x = srcW * unitSize; x > 0; x--)
        {
            *dst = *src;
            src++;
            dst++;
        }
        srcOffset += srcFullW * unitSize;
        dstOffset += dstStride;
    }
}

GraphicsPass GraphicsPassBuilder::Build()
{
    using namespace zen::rhi;
    DynamicRHI* RHI = m_renderDevice->GetRHI();

    m_framebufferInfo.renderTargets = m_rpLayout.GetRenderTargetHandles();

    ShaderHandle shader;
    std::vector<ShaderSpecializationConstant> specializationConstants;
    std::vector<std::vector<rhi::ShaderResourceDescriptor>> SRDs;

    GraphicsPass gfxPass;
    if (!RHIOptions::GetInstance().UseDynamicRendering())
    {
        gfxPass.renderPass = m_renderDevice->GetOrCreateRenderPass(m_rpLayout);
        gfxPass.framebuffer =
            m_viewport->GetCompatibleFramebuffer(gfxPass.renderPass, &m_framebufferInfo);
        m_renderDevice->GetRHIDebug()->SetRenderPassDebugName(gfxPass.renderPass,
                                                              m_tag + "_RenderPass");
    }
    gfxPass.renderPassLayout = std::move(m_rpLayout);

    ShaderProgram* shaderProgram;
    if (m_shaderMode == GfxPassShaderMode::ePreCompiled)
    {
        shaderProgram =
            ShaderProgramManager::GetInstance().RequestShaderProgram(m_shaderProgramName);
    }
    else
    {
        shaderProgram =
            ShaderProgramManager::GetInstance().CreateShaderProgram(m_renderDevice, m_tag + "SP");
        for (const auto& kv : m_shaderStages)
        {
            shaderProgram->AddShaderStage(kv.first, kv.second);
        }
        shaderProgram->Init(m_specializationConstants);
    }

    shader = shaderProgram->GetShaderHandle();
    SRDs   = shaderProgram->GetSRDs();

    gfxPass.shaderProgram = shaderProgram;
    if (RHIOptions::GetInstance().UseDynamicRendering())
    {
        gfxPass.pipeline = m_renderDevice->GetOrCreateGfxPipeline(
            m_PSO, shader, gfxPass.renderPassLayout, specializationConstants);
    }
    else
    {
        gfxPass.pipeline = m_renderDevice->GetOrCreateGfxPipeline(m_PSO, shader, gfxPass.renderPass,
                                                                  specializationConstants);
    }

    gfxPass.descriptorSets.resize(SRDs.size());

    for (uint32_t setIndex = 0; setIndex < SRDs.size(); ++setIndex)
    {
        gfxPass.descriptorSets[setIndex] = RHI->CreateDescriptorSet(shader, setIndex);
    }

    // note: support both descriptor set update at build time and late-update using GraphicsPassResourceUpdater
    for (const auto& kv : m_dsBindings)
    {
        const auto setIndex  = kv.first;
        const auto& bindings = kv.second;
        RHI->UpdateDescriptorSet(gfxPass.descriptorSets[setIndex], bindings);
    }

    m_renderDevice->GetRHIDebug()->SetPipelineDebugName(gfxPass.pipeline, m_tag + "_Pipeline");

    m_renderDevice->m_gfxPasses.push_back(gfxPass);
    return gfxPass;
}

void GraphicsPassResourceUpdater::Update()
{
    rhi::DynamicRHI* RHI = m_renderDevice->GetRHI();
    for (const auto& kv : m_dsBindings)
    {
        const auto setIndex               = kv.first;
        const auto& bindings              = kv.second;
        rhi::DescriptorSetHandle dsHandle = m_gfxPass->descriptorSets[setIndex];
        RHI->UpdateDescriptorSet(dsHandle, bindings);
    }
}

ComputePass ComputePassBuilder::Build()
{
    using namespace zen::rhi;

    DynamicRHI* RHI = m_renderDevice->GetRHI();
    ComputePass computePass;

    ShaderProgram* shaderProgram =
        ShaderProgramManager::GetInstance().RequestShaderProgram(m_shaderProgramName);
    ShaderHandle shader = shaderProgram->GetShaderHandle();

    std::vector<std::vector<rhi::ShaderResourceDescriptor>> SRDs = shaderProgram->GetSRDs();

    computePass.shaderProgram = shaderProgram;
    computePass.pipeline      = m_renderDevice->GetOrCreateComputePipeline(shader);
    computePass.descriptorSets.resize(SRDs.size());

    for (uint32_t setIndex = 0; setIndex < SRDs.size(); ++setIndex)
    {
        computePass.descriptorSets[setIndex] = RHI->CreateDescriptorSet(shader, setIndex);
    }

    m_renderDevice->GetRHIDebug()->SetPipelineDebugName(computePass.pipeline, m_tag + "_Pipeline");

    m_renderDevice->m_computePasses.push_back(computePass);
    return computePass;
}


void ComputePassResourceUpdater::Update()
{
    rhi::DynamicRHI* RHI = m_renderDevice->GetRHI();
    for (const auto& kv : m_dsBindings)
    {
        const auto setIndex               = kv.first;
        const auto& bindings              = kv.second;
        rhi::DescriptorSetHandle dsHandle = m_computePass->descriptorSets[setIndex];
        RHI->UpdateDescriptorSet(dsHandle, bindings);
    }
}

rhi::BufferHandle TextureStagingManager::RequireBuffer(uint32_t requiredSize)
{
    for (uint32_t i = 0; i < m_freeBuffers.size(); i++)
    {
        Entry entry = m_freeBuffers[i];
        if (entry.size == requiredSize)
        {
            m_freeBuffers.erase(m_freeBuffers.begin() + i);
            entry.usedFrame = m_renderDevice->GetFramesCounter();
            m_usedBuffers.push_back(entry);
            return entry.buffer;
        }
    }

    Entry newEntry{};
    newEntry.size      = requiredSize;
    newEntry.usedFrame = m_renderDevice->GetFramesCounter();
    newEntry.buffer    = m_renderDevice->GetRHI()->CreateBuffer(
        requiredSize, BitField(rhi::BufferUsageFlagBits::eTransferSrcBuffer),
        rhi::BufferAllocateType::eCPU);

    m_usedBuffers.push_back(newEntry);

    m_allocatedBuffers.push_back(newEntry.buffer);

    m_usedMemory += requiredSize;

    return newEntry.buffer;
}

void TextureStagingManager::ReleaseBuffer(const rhi::BufferHandle& buffer)
{
    for (uint32_t i = 0; i < m_usedBuffers.size(); i++)
    {
        if (m_usedBuffers[i].buffer.value == buffer.value)
        {
            Entry entry = m_usedBuffers[i];
            m_usedBuffers.erase(m_usedBuffers.begin() + i);
            m_pendingFreeBuffers.push_back(entry);
            m_pendingFreeMemorySize += entry.size;
        }
    }
}

void TextureStagingManager::ProcessPendingFrees()
{
    uint32_t currentFrame = m_renderDevice->GetFramesCounter();
    for (uint32_t i = 0; i < m_pendingFreeBuffers.size(); i++)
    {
        Entry entry = m_pendingFreeBuffers[i];
        if (currentFrame - entry.usedFrame >= RenderConfig::GetInstance().numFrames)
        {
            // old buffers, reuse them
            m_pendingFreeBuffers.erase(m_pendingFreeBuffers.begin() + i);
            m_freeBuffers.push_back(entry);
            m_pendingFreeMemorySize -= entry.size;
        }
    }
}

void TextureStagingManager::Destroy()
{
    for (rhi::BufferHandle& buffer : m_allocatedBuffers)
    {
        m_renderDevice->GetRHI()->DestroyBuffer(buffer);
    }
    LOGI("Texture Staging Memory Used: {} bytes.", m_usedMemory);
}

BufferStagingManager::BufferStagingManager(RenderDevice* renderDevice,
                                           uint32_t blockSize,
                                           uint64_t poolSize) :
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

void BufferStagingManager::Init(uint32_t numFrames)
{
    for (uint32_t i = 0; i < numFrames; i++)
    {
        InsertNewBlock();
    }
}

void BufferStagingManager::Destroy()
{
    for (auto& buffer : m_bufferBlocks)
    {
        m_RHI->DestroyBuffer(buffer.handle);
    }
}

void BufferStagingManager::InsertNewBlock()
{
    StagingBuffer buffer;
    buffer.handle =
        m_RHI->CreateBuffer(BUFFER_SIZE, BitField(rhi::BufferUsageFlagBits::eTransferSrcBuffer),
                            rhi::BufferAllocateType::eCPU);
    buffer.usedFrame    = 0;
    buffer.occupiedSize = 0;
    m_bufferBlocks.insert(m_bufferBlocks.begin() + m_currentBlockIndex, std::move(buffer));
}

bool BufferStagingManager::FitInBlock(uint32_t blockIndex,
                                      uint32_t requiredSize,
                                      uint32_t requiredAlign,
                                      bool canSegment,
                                      StagingSubmitResult* result)
{
    uint32_t occupiedSize   = m_bufferBlocks[blockIndex].occupiedSize;
    uint32_t alignRemainder = occupiedSize % requiredAlign;
    if (alignRemainder != 0)
    {
        occupiedSize += requiredAlign - alignRemainder;
    }
    int32_t availableBytes = static_cast<int32_t>(BUFFER_SIZE) - static_cast<int32_t>(occupiedSize);
    if (static_cast<int32_t>(requiredSize) < availableBytes)
    {
        // enough room, allocate
        result->writeOffset = occupiedSize;
    }
    else if (canSegment && availableBytes >= static_cast<int32_t>(requiredAlign))
    {
        // All won't fit but at least we can fit a chunk.
        result->writeOffset = occupiedSize;
        result->writeSize   = availableBytes - (availableBytes % requiredAlign);
    }
    else
    {
        return false;
    }
    return true;
}

void BufferStagingManager::BeginSubmit(uint32_t requiredSize,
                                       StagingSubmitResult* result,
                                       uint32_t requiredAlign,
                                       bool canSegment)
{
    result->writeSize = requiredSize;
    while (true)
    {
        result->writeOffset = 0;
        uint32_t usedFrame  = m_bufferBlocks[m_currentBlockIndex].usedFrame;
        if (m_bufferBlocks[m_currentBlockIndex].usedFrame == m_renderDevice->GetFramesCounter())
        {
            if (!FitInBlock(m_currentBlockIndex, requiredSize, requiredAlign, canSegment, result))
            {
                m_currentBlockIndex = (m_currentBlockIndex + 1) % m_bufferBlocks.size();
                if (m_bufferBlocks[m_currentBlockIndex].usedFrame ==
                    m_renderDevice->GetFramesCounter())
                {
                    if (CanInsertNewBlock())
                    {
                        InsertNewBlock();
                        m_bufferBlocks[m_currentBlockIndex].usedFrame =
                            m_renderDevice->GetFramesCounter();
                    }
                    else
                    {
                        // not enough space, wait for all frames
                        result->flushAction = StagingFlushAction::eFull;
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
            m_bufferBlocks[m_currentBlockIndex].usedFrame = m_renderDevice->GetFramesCounter();
        }
        else
        {
            // not enough space, wait for all frames
            result->flushAction = StagingFlushAction::ePartial;
        }
        break;
    }
    result->success     = true;
    result->buffer      = m_bufferBlocks[m_currentBlockIndex].handle;
    m_stagingBufferUsed = true;
}

void BufferStagingManager::EndSubmit(const StagingSubmitResult* result)
{
    m_bufferBlocks[m_currentBlockIndex].occupiedSize = result->writeOffset + result->writeSize;
    m_bufferBlocks[m_currentBlockIndex].usedFrame    = m_renderDevice->GetFramesCounter();
}

void BufferStagingManager::PerformAction(StagingFlushAction action)
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

void RenderDevice::Init(rhi::RHIViewport* mainViewport)
{
    m_bufferStagingMgr =
        new BufferStagingManager(this, STAGING_BLOCK_SIZE_BYTES, STAGING_POOL_SIZE_BYTES);
    m_bufferStagingMgr->Init(m_numFrames);
    m_textureStagingMgr = new TextureStagingManager(this);
    m_textureManager    = new TextureManager(this, m_textureStagingMgr);
    m_frames.reserve(m_numFrames);
    for (uint32_t i = 0; i < m_numFrames; i++)
    {
        RenderFrame frame{};
        frame.cmdListContext = m_RHI->CreateCmdListContext();
        frame.drawCmdList    = rhi::RHICommandList::Create(m_APIType, frame.cmdListContext);
        frame.uploadCmdList  = rhi::RHICommandList::Create(m_APIType, frame.cmdListContext);
        m_frames.emplace_back(frame);
    }
    m_framesCounter = m_frames.size();
    m_frames[m_currentFrame].uploadCmdList->BeginUpload();
    m_frames[m_currentFrame].drawCmdList->BeginRender();

    m_mainViewport = mainViewport;

    m_rendererServer = new RendererServer(this, m_mainViewport);
    m_rendererServer->Init();
}

void RenderDevice::Destroy()
{
    for (auto* viewport : m_viewports)
    {
        m_RHI->DestroyViewport(viewport);
    }
    for (auto& kv : m_renderPassCache)
    {
        m_RHI->DestroyRenderPass(kv.second);
    }
    for (auto& kv : m_pipelineCache)
    {
        m_RHI->DestroyPipeline(kv.second);
    }
    for (auto& kv : m_samplerCache)
    {
        m_RHI->DestroySampler(kv.second);
    }

    for (auto& rp : m_gfxPasses)
    {
        for (const auto& ds : rp.descriptorSets)
        {
            m_RHI->DestroyDescriptorSet(ds);
        }
    }

    for (auto& rp : m_computePasses)
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

    m_deletionQueue.Flush();

    m_bufferStagingMgr->Destroy();
    delete m_bufferStagingMgr;

    m_textureManager->Destroy();
    delete m_textureManager;

    m_textureStagingMgr->Destroy();
    delete m_textureStagingMgr;

    m_rendererServer->Destroy();

    for (RenderFrame& frame : m_frames)
    {
        delete frame.uploadCmdList;
        delete frame.drawCmdList;
    }
    m_RHI->Destroy();
    delete m_RHI;
}

void RenderDevice::ExecuteFrame(rhi::RHIViewport* viewport, RenderGraph* rdg, bool present)
{
    m_RHI->BeginDrawingViewport(viewport);
    rdg->Execute(m_frames[m_currentFrame].drawCmdList);
    EndFrame();
    m_RHI->EndDrawingViewport(viewport, m_frames[m_currentFrame].cmdListContext, present);
}

void RenderDevice::ExecuteFrame(rhi::RHIViewport* viewport,
                                const std::vector<RenderGraph*>& rdgs,
                                bool present)
{
    m_RHI->BeginDrawingViewport(viewport);
    for (auto* rdg : rdgs)
    {
        rdg->Execute(m_frames[m_currentFrame].drawCmdList);
    }
    EndFrame();
    m_RHI->EndDrawingViewport(viewport, m_frames[m_currentFrame].cmdListContext, present);
}

void RenderDevice::ExecuteImmediate(rhi::RHIViewport* viewport, RenderGraph* rdg)
{
    rhi::RHICommandList* cmdList = m_RHI->GetImmediateCommandList();
    cmdList->BeginRender();
    rdg->Execute(cmdList);
    cmdList->EndRender();
    m_RHI->WaitForCommandList(cmdList);
}

rhi::TextureHandle RenderDevice::CreateTexture(const rhi::TextureInfo& textureInfo,
                                               const std::string& tag)
{
    return m_textureManager->CreateTexture(textureInfo, tag);
}

rhi::TextureHandle RenderDevice::CreateTextureProxy(const rhi::TextureHandle& baseTexture,
                                                    const rhi::TextureProxyInfo& proxyInfo)
{
    return m_textureManager->CreateTextureProxy(baseTexture, proxyInfo);
}

void RenderDevice::GenerateTextureMipmaps(const rhi::TextureHandle& textureHandle,
                                          rhi::RHICommandList* cmdList)
{
    cmdList->GenerateTextureMipmaps(textureHandle);
}

rhi::BufferHandle RenderDevice::CreateVertexBuffer(uint32_t dataSize, const uint8_t* pData)
{
    BitField<rhi::BufferUsageFlagBits> usages;
    usages.SetFlag(rhi::BufferUsageFlagBits::eVertexBuffer);
    usages.SetFlag(rhi::BufferUsageFlagBits::eTransferDstBuffer);
    usages.SetFlag(rhi::BufferUsageFlagBits::eStorageBuffer);

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
    usages.SetFlag(rhi::BufferUsageFlagBits::eStorageBuffer);

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

    uint32_t paddedSize = PadUniformBufferSize(dataSize);
    rhi::BufferHandle uniformBuffer =
        m_RHI->CreateBuffer(paddedSize, usages, rhi::BufferAllocateType::eGPU);
    if (pData != nullptr)
    {
        UpdateBufferInternal(uniformBuffer, 0, paddedSize, pData);
    }
    m_buffers.push_back(uniformBuffer);
    return uniformBuffer;
}

rhi::BufferHandle RenderDevice::CreateStorageBuffer(uint32_t dataSize, const uint8_t* pData)
{
    BitField<rhi::BufferUsageFlagBits> usages;
    usages.SetFlag(rhi::BufferUsageFlagBits::eStorageBuffer);
    usages.SetFlag(rhi::BufferUsageFlagBits::eTransferDstBuffer);

    uint32_t paddedSize = PadStorageBufferSize(dataSize);
    rhi::BufferHandle storageBuffer =
        m_RHI->CreateBuffer(paddedSize, usages, rhi::BufferAllocateType::eGPU);
    if (pData != nullptr)
    {
        UpdateBufferInternal(storageBuffer, 0, paddedSize, pData);
    }
    m_buffers.push_back(storageBuffer);
    return storageBuffer;
}

rhi::BufferHandle RenderDevice::CreateIndirectBuffer(uint32_t dataSize, const uint8_t* pData)
{
    BitField<rhi::BufferUsageFlagBits> usages;
    usages.SetFlag(rhi::BufferUsageFlagBits::eStorageBuffer);
    usages.SetFlag(rhi::BufferUsageFlagBits::eIndirectBuffer);
    usages.SetFlag(rhi::BufferUsageFlagBits::eTransferDstBuffer);

    uint32_t paddedSize = PadStorageBufferSize(dataSize);
    rhi::BufferHandle indirectBuffer =
        m_RHI->CreateBuffer(paddedSize, usages, rhi::BufferAllocateType::eGPU);
    if (pData != nullptr)
    {
        UpdateBufferInternal(indirectBuffer, 0, paddedSize, pData);
    }
    m_buffers.push_back(indirectBuffer);
    return indirectBuffer;
}

size_t RenderDevice::PadUniformBufferSize(size_t originalSize)
{
    auto minUboAlignment = m_RHI->QueryGPUInfo().uniformBufferAlignment;
    size_t alignedSize   = (originalSize + minUboAlignment - 1) & ~(minUboAlignment - 1);
    return alignedSize;
}

size_t RenderDevice::PadStorageBufferSize(size_t originalSize)
{
    auto minAlignment  = m_RHI->QueryGPUInfo().storageBufferAlignment;
    size_t alignedSize = (originalSize + minAlignment - 1) & ~(minAlignment - 1);
    return alignedSize;
}

void RenderDevice::UpdateBuffer(const rhi::BufferHandle& bufferHandle,
                                uint32_t dataSize,
                                const uint8_t* pData,
                                uint32_t offset)
{
    UpdateBufferInternal(bufferHandle, offset, dataSize, pData);
}

void RenderDevice::DestroyBuffer(const rhi::BufferHandle& bufferHandle)
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

rhi::PipelineHandle RenderDevice::GetOrCreateGfxPipeline(
    rhi::GfxPipelineStates& PSO,
    const rhi::ShaderHandle& shader,
    const rhi::RenderPassHandle& renderPass,
    const std::vector<rhi::ShaderSpecializationConstant>& specializationConstants)
{
    auto hash = CalcGfxPipelineHash(PSO, shader, renderPass, specializationConstants);
    if (!m_pipelineCache.contains(hash))
    {
        // create new one
        m_pipelineCache[hash] = m_RHI->CreateGfxPipeline(shader, PSO, renderPass, 0);
    }
    return m_pipelineCache[hash];
}

rhi::PipelineHandle RenderDevice::GetOrCreateGfxPipeline(
    rhi::GfxPipelineStates& PSO,
    const rhi::ShaderHandle& shader,
    const rhi::RenderPassLayout& renderPassLayout,
    const std::vector<rhi::ShaderSpecializationConstant>& specializationConstants)
{
    auto hash = CalcGfxPipelineHash(PSO, shader, renderPassLayout, specializationConstants);
    if (!m_pipelineCache.contains(hash))
    {
        // create new one
        m_pipelineCache[hash] = m_RHI->CreateGfxPipeline(shader, PSO, renderPassLayout, 0);
    }
    return m_pipelineCache[hash];
}

rhi::PipelineHandle RenderDevice::GetOrCreateComputePipeline(const rhi::ShaderHandle& shader)
{
    auto hash = CalcComputePipelineHash(shader);
    if (!m_pipelineCache.contains(hash))
    {
        m_pipelineCache[hash] = m_RHI->CreateComputePipeline(shader);
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

void RenderDevice::ResizeViewport(rhi::RHIViewport* viewport, uint32_t width, uint32_t height)
{
    if (viewport != nullptr && (viewport->GetWidth() != width || viewport->GetHeight() != height))
    {
        m_RHI->SubmitAllGPUCommands();
        viewport->Resize(width, height);
        BeginFrame();
    }
}

void RenderDevice::UpdateGraphicsPassOnResize(GraphicsPass& gfxPass, rhi::RHIViewport* viewport)
{
    if (rhi::RHIOptions::GetInstance().UseDynamicRendering())
    {
        gfxPass.renderPassLayout.ClearRenderTargetInfo();
        gfxPass.renderPassLayout.AddColorRenderTarget(viewport->GetSwapchainFormat(),
                                                      rhi::TextureUsage::eColorAttachment,
                                                      viewport->GetColorBackBuffer());
        gfxPass.renderPassLayout.SetDepthStencilRenderTarget(viewport->GetDepthStencilFormat(),
                                                             viewport->GetDepthStencilBackBuffer());
    }
    else
    {
        gfxPass.framebuffer = viewport->GetCompatibleFramebufferForBackBuffer(gfxPass.renderPass);
    }
}

rhi::SamplerHandle RenderDevice::CreateSampler(const rhi::SamplerInfo& samplerInfo)
{
    const size_t samplerHash = CalcSamplerHash(samplerInfo);
    if (!m_samplerCache.contains(samplerHash))
    {
        m_samplerCache[samplerHash] = m_RHI->CreateSampler(samplerInfo);
    }

    return m_samplerCache[samplerHash];
}

rhi::TextureSubResourceRange RenderDevice::GetTextureSubResourceRange(rhi::TextureHandle handle)
{
    return m_RHI->GetTextureSubResourceRange(handle);
}

rhi::TextureHandle RenderDevice::LoadTexture2D(const std::string& file, bool requireMipmap)
{
    return m_textureManager->LoadTexture2D(file, requireMipmap);
}

void RenderDevice::LoadSceneTextures(const sg::Scene* scene,
                                     std::vector<rhi::TextureHandle>& outTextures)
{
    m_textureManager->LoadSceneTextures(scene, outTextures);
}

void RenderDevice::LoadTextureEnv(const std::string& file, EnvTexture* texture)
{
    auto fullPath = ZEN_TEXTURE_PATH + file;
    m_textureManager->LoadTextureEnv(fullPath, texture);
}

void RenderDevice::UpdateTextureOneTime(rhi::TextureHandle textureHandle,
                                        const Vec3i& textureSize,
                                        uint32_t dataSize,
                                        const uint8_t* pData)
{

    // NOT support mipmap
    // TODO: remove hard code.
    const uint32_t requiredAlign = 4;
    const uint32_t pixelSize     = 4;
    StagingSubmitResult submitResult;
    m_bufferStagingMgr->BeginSubmit(dataSize, &submitResult, requiredAlign);
    if (submitResult.flushAction == StagingFlushAction::ePartial)
    {
        WaitForPreviousFrames();
    }
    else if (submitResult.flushAction == StagingFlushAction::eFull)
    {
        WaitForAllFrames();
    }
    m_bufferStagingMgr->PerformAction(submitResult.flushAction);

    // map staging buffer
    uint8_t* dataPtr = m_RHI->MapBuffer(submitResult.buffer);
    dataPtr += submitResult.writeOffset;

    // copy
    memcpy(dataPtr, pData, submitResult.writeSize);

    // unmap
    m_RHI->UnmapBuffer(submitResult.buffer);
    // copy to gpu memory
    rhi::BufferTextureCopyRegion copyRegion{};
    copyRegion.textureSubresources.aspect.SetFlag(rhi::TextureAspectFlagBits::eColor);
    copyRegion.bufferOffset  = submitResult.writeOffset;
    copyRegion.textureOffset = {0, 0, 0};
    copyRegion.textureSize   = textureSize;
    m_frames[m_currentFrame].uploadCmdList->CopyBufferToTexture(submitResult.buffer, textureHandle,
                                                                copyRegion);

    m_bufferStagingMgr->EndSubmit(&submitResult);
}

void RenderDevice::UpdateTextureBatch(rhi::TextureHandle textureHandle,
                                      const Vec3i& textureSize,
                                      const uint8_t* pData)
{
    // not support mipmap
    uint32_t requiredAlign = 4;
    uint32_t regionSize    = TEXTURE_UPLOAD_REGION_SIZE;

    const uint32_t width     = textureSize.x;
    const uint32_t height    = textureSize.y;
    const uint32_t depth     = textureSize.z;
    const uint32_t pixelSize = 4;
    for (uint32_t z = 0; z < depth; z++)
    {
        for (uint32_t y = 0; y < height; y += regionSize)
        {
            for (uint32_t x = 0; x < width; x += regionSize)
            {
                uint32_t regionWidth  = std::min(regionSize, width - x);
                uint32_t regionHeight = std::min(regionSize, height - y);
                uint32_t imageStride  = regionWidth * pixelSize;
                uint32_t toSubmit     = imageStride * regionHeight;
                StagingSubmitResult submitResult;
                m_bufferStagingMgr->BeginSubmit(toSubmit, &submitResult, requiredAlign);
                if (submitResult.flushAction == StagingFlushAction::ePartial)
                {
                    WaitForPreviousFrames();
                }
                else if (submitResult.flushAction == StagingFlushAction::eFull)
                {
                    WaitForAllFrames();
                }
                m_bufferStagingMgr->PerformAction(submitResult.flushAction);

                // map staging buffer
                uint8_t* dataPtr = m_RHI->MapBuffer(submitResult.buffer);
                // copy
                CopyRegion(pData, dataPtr + submitResult.writeOffset, x, y, regionWidth,
                           regionHeight, width, imageStride, pixelSize);
                // unmap
                m_RHI->UnmapBuffer(submitResult.buffer);
                // copy to gpu memory
                rhi::BufferTextureCopyRegion copyRegion{};
                copyRegion.textureSubresources.aspect.SetFlag(rhi::TextureAspectFlagBits::eColor);
                copyRegion.bufferOffset  = submitResult.writeOffset;
                copyRegion.textureOffset = {x, y, z};
                copyRegion.textureSize   = {regionWidth, regionHeight, 1};
                m_frames[m_currentFrame].uploadCmdList->CopyBufferToTexture(
                    submitResult.buffer, textureHandle, copyRegion);

                m_bufferStagingMgr->EndSubmit(&submitResult);
            }
        }
    }
}

void RenderDevice::UpdateBufferInternal(const rhi::BufferHandle& bufferHandle,
                                        uint32_t offset,
                                        uint32_t dataSize,
                                        const uint8_t* pData)
{
    uint32_t toSubmit      = dataSize;
    uint32_t writePosition = 0;
    while (toSubmit > 0 && pData != nullptr)
    {
        StagingSubmitResult submitResult;
        m_bufferStagingMgr->BeginSubmit(std::min(toSubmit, (uint32_t)STAGING_BLOCK_SIZE_BYTES),
                                        &submitResult, 32, true);

        if (submitResult.flushAction == StagingFlushAction::ePartial)
        {
            WaitForPreviousFrames();
        }
        else if (submitResult.flushAction == StagingFlushAction::eFull)
        {
            WaitForAllFrames();
        }
        m_bufferStagingMgr->PerformAction(submitResult.flushAction);

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

        m_bufferStagingMgr->EndSubmit(&submitResult);
        toSubmit -= submitResult.writeSize;
        writePosition += submitResult.writeSize;
    }
}

void RenderDevice::ProcessViewportResize(uint32_t width, uint32_t height)
{
    ResizeViewport(m_mainViewport, width, height);
    m_rendererServer->ViewportResizeCallback();
}

void RenderDevice::WaitForPreviousFrames()
{
    for (uint32_t i = 0; i < m_frames.size(); i++)
    {
        if (m_frames[i].cmdSubmitted)
        {
            m_RHI->WaitForCommandList(m_frames[i].uploadCmdList);
            m_RHI->WaitForCommandList(m_frames[i].drawCmdList);
            m_frames[i].cmdSubmitted = false;
        }
    }
}

void RenderDevice::WaitForAllFrames()
{
    EndFrame();
    WaitForPreviousFrames();
    BeginFrame();
}


void RenderDevice::NextFrame()
{
    m_currentFrame = (m_currentFrame + 1) % m_frames.size();
    BeginFrame();
}

void RenderDevice::BeginFrame()
{
    m_framesCounter++;
    if (m_bufferStagingMgr->m_stagingBufferUsed)
    {
        m_bufferStagingMgr->UpdateBlockIndex();
        m_bufferStagingMgr->m_stagingBufferUsed = false;
    }
    m_frames[m_currentFrame].uploadCmdList->BeginUpload();
    m_frames[m_currentFrame].drawCmdList->BeginRender();
}

void RenderDevice::EndFrame()
{
    m_frames[m_currentFrame].uploadCmdList->EndUpload();
    m_frames[m_currentFrame].drawCmdList->EndRender();
    m_frames[m_currentFrame].cmdSubmitted = true;
}

// todo: consider attachment size when calculating hash
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

size_t RenderDevice::CalcGfxPipelineHash(
    const rhi::GfxPipelineStates& pso,
    const rhi::ShaderHandle& shader,
    const rhi::RenderPassHandle& renderPass,
    const std::vector<rhi::ShaderSpecializationConstant>& specializationConstants)
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
    for (auto& spc : specializationConstants)
    {
        switch (spc.type)
        {

            case rhi::ShaderSpecializationConstantType::eBool: combineHash(spc.boolValue); break;
            case rhi::ShaderSpecializationConstantType::eInt: combineHash(spc.intValue); break;
            case rhi::ShaderSpecializationConstantType::eFloat: combineHash(spc.floatValue); break;
            default: break;
        }
    }
    return seed;
}

size_t RenderDevice::CalcGfxPipelineHash(
    const rhi::GfxPipelineStates& pso,
    const rhi::ShaderHandle& shader,
    const rhi::RenderPassLayout& renderPassLayout,
    const std::vector<rhi::ShaderSpecializationConstant>& specializationConstants)
{
    std::size_t seed = 0;
    // Hashing utility
    auto combineHash = [&seed](auto&& value) {
        seed ^= std::hash<std::decay_t<decltype(value)>>{}(value) + 0x9e3779b9 + (seed << 6) +
            (seed >> 2);
    };
    combineHash(shader.value);
    for (uint32_t i = 0; i < renderPassLayout.GetNumRenderTargets(); i++)
    {
        combineHash(renderPassLayout.GetRenderTargetHandles()[i].value);
    }
    combineHash(pso.primitiveType);
    for (auto& spc : specializationConstants)
    {
        switch (spc.type)
        {

            case rhi::ShaderSpecializationConstantType::eBool: combineHash(spc.boolValue); break;
            case rhi::ShaderSpecializationConstantType::eInt: combineHash(spc.intValue); break;
            case rhi::ShaderSpecializationConstantType::eFloat: combineHash(spc.floatValue); break;
            default: break;
        }
    }
    return seed;
}

size_t RenderDevice::CalcComputePipelineHash(const rhi::ShaderHandle& shader)
{
    std::size_t seed = 0;
    // Hashing utility
    auto combineHash = [&seed](auto&& value) {
        seed ^= std::hash<std::decay_t<decltype(value)>>{}(value) + 0x9e3779b9 + (seed << 6) +
            (seed >> 2);
    };
    combineHash(shader.value);
    return seed;
}

size_t RenderDevice::CalcSamplerHash(const rhi::SamplerInfo& info)
{
    using namespace zen::util;

    std::size_t seed = 0;

    HashCombine(seed, std::hash<int>()(static_cast<int>(info.magFilter)));
    HashCombine(seed, std::hash<int>()(static_cast<int>(info.minFilter)));
    HashCombine(seed, std::hash<int>()(static_cast<int>(info.mipFilter)));

    HashCombine(seed, std::hash<int>()(static_cast<int>(info.repeatU)));
    HashCombine(seed, std::hash<int>()(static_cast<int>(info.repeatV)));
    HashCombine(seed, std::hash<int>()(static_cast<int>(info.repeatW)));

    HashCombine(seed, std::hash<float>()(info.lodBias));
    HashCombine(seed, std::hash<bool>()(info.useAnisotropy));
    HashCombine(seed, std::hash<float>()(info.maxAnisotropy));
    HashCombine(seed, std::hash<bool>()(info.enableCompare));
    HashCombine(seed, std::hash<int>()(static_cast<int>(info.compareOp)));

    HashCombine(seed, std::hash<float>()(info.minLod));
    HashCombine(seed, std::hash<float>()(info.maxLod));

    HashCombine(seed, std::hash<int>()(static_cast<int>(info.borderColor)));
    HashCombine(seed, std::hash<bool>()(info.unnormalizedUVW));

    return seed;
}
} // namespace zen::rc