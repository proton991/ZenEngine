#include <utility>

#include "Graphics/RenderCore/V2/RenderDevice.h"

#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/RenderGraph.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Graphics/RenderCore/V2/RenderResource.h"
#include "Graphics/RenderCore/V2/TextureManager.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "SceneGraph/Scene.h"
#include "Utils/Helpers.h"


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

// GraphicsPassBuilder& GraphicsPassBuilder::AddColorRenderTarget(DataFormat format,
//                                                                const rhi::RHITexture* handle,
//                                                                bool clear)
// {
//     m_rpLayout.AddColorRenderTarget(format, rhi::TextureUsage::eColorAttachment, handle,
//                                     m_renderDevice->GetTextureSubResourceRange(handle));
//     m_rpLayout.SetColorTargetLoadStoreOp(clear ? rhi::RenderTargetLoadOp::eClear :
//                                                  rhi::RenderTargetLoadOp::eLoad,
//                                          rhi::RenderTargetStoreOp::eStore);
//     m_framebufferInfo.numRenderTarget++;
//     return *this;
// }

GraphicsPassBuilder& GraphicsPassBuilder::AddViewportColorRT(rhi::RHIViewport* viewport,
                                                             rhi::RenderTargetLoadOp loadOp,
                                                             rhi::RenderTargetStoreOp storeOp)
{
    m_rpLayout.AddColorRenderTarget(viewport->GetSwapchainFormat(), viewport->GetColorBackBuffer(),
                                    viewport->GetColorBackBufferRange(), loadOp, storeOp);
    // m_rpLayout.SetColorTargetLoadStoreOp(clear ? rhi::RenderTargetLoadOp::eClear :
    //                                              rhi::RenderTargetLoadOp::eLoad,
    //                                      rhi::RenderTargetStoreOp::eStore);
    m_framebufferInfo.numRenderTarget++;
    return *this;
}

GraphicsPassBuilder& GraphicsPassBuilder::SetViewportDepthStencilRT(
    rhi::RHIViewport* viewport,
    rhi::RenderTargetLoadOp loadOp,
    rhi::RenderTargetStoreOp storeOp)
{
    m_rpLayout.SetDepthStencilRenderTarget(
        viewport->GetDepthStencilFormat(), viewport->GetDepthStencilBackBuffer(),
        viewport->GetDepthStencilBackBufferRange(), loadOp, storeOp);
    // m_rpLayout.SetDepthStencilTargetLoadStoreOp(loadOp, storeOp);
    m_framebufferInfo.numRenderTarget++;
    return *this;
}

GraphicsPassBuilder& GraphicsPassBuilder::AddColorRenderTarget(rhi::RHITexture* colorRT,
                                                               rhi::RenderTargetLoadOp loadOp,
                                                               rhi::RenderTargetStoreOp storeOp)
{
    m_rpLayout.AddColorRenderTarget(colorRT->GetFormat(), colorRT, colorRT->GetSubResourceRange(),
                                    loadOp, storeOp);
    // m_rpLayout.SetColorTargetLoadStoreOp(clear ? rhi::RenderTargetLoadOp::eClear :
    //                                              rhi::RenderTargetLoadOp::eLoad,
    // rhi::RenderTargetStoreOp::eStore);
    m_framebufferInfo.numRenderTarget++;
    return *this;
}

GraphicsPassBuilder& GraphicsPassBuilder::SetDepthStencilTarget(rhi::RHITexture* depthStencilRT,
                                                                rhi::RenderTargetLoadOp loadOp,
                                                                rhi::RenderTargetStoreOp storeOp)
{
    m_rpLayout.SetDepthStencilRenderTarget(depthStencilRT->GetFormat(), depthStencilRT,
                                           depthStencilRT->GetSubResourceRange(), loadOp, storeOp);
    // m_rpLayout.SetDepthStencilTargetLoadStoreOp(loadOp, storeOp);
    m_framebufferInfo.numRenderTarget++;
    return *this;
}

// GraphicsPassBuilder& GraphicsPassBuilder::SetDepthStencilTarget(DataFormat format,
//                                                                 const rhi::RHITexture* handle,
//                                                                 rhi::RenderTargetLoadOp loadOp,
//                                                                 rhi::RenderTargetStoreOp storeOp)
// {
//     m_rpLayout.SetDepthStencilRenderTarget(format, handle,
//                                            m_renderDevice->GetTextureSubResourceRange(handle));
//     m_rpLayout.SetDepthStencilTargetLoadStoreOp(loadOp, storeOp);
//     m_framebufferInfo.numRenderTarget++;
//     return *this;
// }

GraphicsPass GraphicsPassBuilder::Build()
{
    using namespace zen::rhi;
    // DynamicRHI* GDynamicRHI = m_renderDevice->GetRHI();

    std::vector<RHITexture*> rtHandles;
    rtHandles.resize(m_rpLayout.GetNumColorRenderTargets());

    for (uint32_t i = 0; i < m_rpLayout.GetNumColorRenderTargets(); i++)
    {
        rtHandles[i] = m_rpLayout.GetColorRenderTargets()[i].texture;
    }

    // m_framebufferInfo.renderTargets = m_rpLayout.GetRenderTargetHandles();
    m_framebufferInfo.renderTargets = rtHandles.data();

    // ShaderHandle shader;
    std::vector<ShaderSpecializationConstant> specializationConstants;
    // ShaderResourceDescriptorTable SRDs;

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

    RHIShader* shader                         = shaderProgram->GetShader();
    const ShaderResourceDescriptorTable& SRDs = shaderProgram->GetSRDTable();

    gfxPass.shaderProgram = shaderProgram;

    // set up resource trackers
    gfxPass.resourceTrackers.resize(SRDs.size());
    // build PassTextureTracker and PassBufferTracker
    // PassTextureTracker's handle is not available until UpdatePassResource is called
    for (auto& srd : shaderProgram->GetSampledTextureSRDs())
    {
        PassResourceTracker tracker;
        tracker.name = srd.name;
        // for combined image samplers,
        tracker.textureUsage = TextureUsage::eSampled;
        tracker.resourceType = PassResourceType::eTexture;
        tracker.accessMode   = AccessMode::eRead;
        tracker.accessFlags.SetFlag(AccessFlagBits::eShaderRead);
        gfxPass.resourceTrackers[srd.set][srd.binding] = tracker;
    }
    for (auto& srd : shaderProgram->GetStorageImageSRDs())
    {
        PassResourceTracker tracker;
        tracker.name         = srd.name;
        tracker.resourceType = PassResourceType::eTexture;
        tracker.textureUsage = TextureUsage::eStorage;
        if (!srd.writable)
        {
            tracker.accessFlags.SetFlag(AccessFlagBits::eShaderRead);
            tracker.accessMode = AccessMode::eRead;
        }
        else
        {
            tracker.accessFlags.SetFlags(AccessFlagBits::eShaderRead, AccessFlagBits::eShaderWrite);
            tracker.accessMode = AccessMode::eReadWrite;
        }
        gfxPass.resourceTrackers[srd.set][srd.binding] = tracker;
    }
    for (auto& srd : shaderProgram->GetStorageBufferSRDs())
    {
        PassResourceTracker tracker;
        tracker.name         = srd.name;
        tracker.resourceType = PassResourceType::eBuffer;
        tracker.bufferUsage  = BufferUsage::eStorageBuffer;
        if (!srd.writable)
        {
            tracker.accessFlags.SetFlag(AccessFlagBits::eShaderRead);
            tracker.accessMode = AccessMode::eRead;
        }
        else
        {
            tracker.accessFlags.SetFlags(AccessFlagBits::eShaderRead, AccessFlagBits::eShaderWrite);
            tracker.accessMode = AccessMode::eReadWrite;
        }
        gfxPass.resourceTrackers[srd.set][srd.binding] = tracker;
    }

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
        gfxPass.descriptorSets[setIndex] = shader->CreateDescriptorSet(setIndex);
    }

    // note: support both descriptor set update at build time and late-update using GraphicsPassResourceUpdater
    for (const auto& kv : m_dsBindings)
    {
        const auto setIndex  = kv.first;
        const auto& bindings = kv.second;
        gfxPass.descriptorSets[setIndex]->Update(bindings);
    }

    m_renderDevice->GetRHIDebug()->SetPipelineDebugName(gfxPass.pipeline, m_tag + "_Pipeline");

    m_renderDevice->m_gfxPasses.push_back(gfxPass);
    return gfxPass;
}

void GraphicsPassResourceUpdater::Update()
{
    // rhi::DynamicRHI* GDynamicRHI = m_renderDevice->GetRHI();
    for (const auto& kv : m_dsBindings)
    {
        const auto setIndex             = kv.first;
        const auto& bindings            = kv.second;
        rhi::RHIDescriptorSet* dsHandle = m_gfxPass->descriptorSets[setIndex];
        dsHandle->Update(bindings);
        // set pass tracker handle values here
        for (auto& srb : bindings)
        {
            PassResourceTracker& tracker = m_gfxPass->resourceTrackers[setIndex][srb.binding];
            bool hasSampler = srb.type == rhi::ShaderResourceType::eSamplerWithTexture ||
                srb.type == rhi::ShaderResourceType::eSamplerWithTextureBuffer;
            uint32_t index = 0;
            while (index < srb.resources.size())
            {
                rhi::RHIResource* resource = srb.resources[hasSampler ? index + 1 : index];
                if (tracker.resourceType == PassResourceType::eTexture)
                {
                    tracker.textures.emplace_back(dynamic_cast<rhi::RHITexture*>(resource));
                    // tracker.textureHandle = TO_TEX_HANDLE(handle);
                    // tracker.textureSubResRange =
                    //     m_renderDevice->GetTextureSubResourceRange(tracker.textureHandle);
                }
                else if (tracker.resourceType == PassResourceType::eBuffer)
                {
                    tracker.buffer = dynamic_cast<rhi::RHIBuffer*>(resource);
                }
                index = hasSampler ? index + 2 : index + 1;
            }
            // if (srb.type == rhi::ShaderResourceType::eSamplerWithTexture ||
            //     srb.type == rhi::ShaderResourceType::eSamplerWithTextureBuffer)
            // {
            //     handle = srb.handles[1];
            // }
            // else
            // {
            //     handle = srb.handles[0];
            // }
            // if (tracker.resourceType == PassResourceType::eTexture)
            // {
            //     // check if a texture is a proxy
            //     // rhi::RHITexture* textureHandle = TO_TEX_HANDLE(handle);
            //     // if (m_renderDevice->IsProxyTexture(textureHandle))
            //     // {
            //     //     textureHandle = m_renderDevice->GetBaseTextureForProxy(textureHandle);
            //     // }
            //     // else
            //     // {
            //     //     textureHandle = TO_TEX_HANDLE(handle);
            //     // }
            //     tracker.textureHandle = TO_TEX_HANDLE(handle);
            //     tracker.textureSubResRange =
            //         m_renderDevice->GetTextureSubResourceRange(tracker.textureHandle);
            // }
            // else if (tracker.resourceType == PassResourceType::eBuffer)
            // {
            //     tracker.bufferHandle = TO_BUF_HANDLE(handle);
            // }
        }
    }
}

ComputePass ComputePassBuilder::Build()
{
    using namespace zen::rhi;

    // DynamicRHI* GDynamicRHI = m_renderDevice->GetRHI();
    ComputePass computePass;

    ShaderProgram* shaderProgram =
        ShaderProgramManager::GetInstance().RequestShaderProgram(m_shaderProgramName);
    RHIShader* shader = shaderProgram->GetShader();

    const ShaderResourceDescriptorTable& SRDs = shaderProgram->GetSRDTable();

    computePass.shaderProgram = shaderProgram;
    computePass.pipeline      = m_renderDevice->GetOrCreateComputePipeline(shader);
    computePass.descriptorSets.resize(SRDs.size());

    // set up resource trackers
    computePass.resourceTrackers.resize(SRDs.size());
    // build PassTextureTracker and PassBufferTracker
    // PassTextureTracker's handle is not available until UpdatePassResource is called
    for (auto& srd : shaderProgram->GetSampledTextureSRDs())
    {
        PassResourceTracker tracker;
        tracker.name = srd.name;
        // for combined image samplers,
        tracker.resourceType = PassResourceType::eTexture;
        tracker.textureUsage = TextureUsage::eSampled;
        tracker.accessMode   = AccessMode::eRead;
        tracker.accessFlags.SetFlag(AccessFlagBits::eShaderRead);
        computePass.resourceTrackers[srd.set][srd.binding] = tracker;
    }
    for (auto& srd : shaderProgram->GetStorageImageSRDs())
    {
        PassResourceTracker tracker;
        tracker.name         = srd.name;
        tracker.resourceType = PassResourceType::eTexture;
        tracker.textureUsage = TextureUsage::eStorage;
        if (!srd.writable)
        {
            tracker.accessFlags.SetFlag(AccessFlagBits::eShaderRead);
            tracker.accessMode = AccessMode::eRead;
        }
        else
        {
            tracker.accessFlags.SetFlags(AccessFlagBits::eShaderRead, AccessFlagBits::eShaderWrite);
            tracker.accessMode = AccessMode::eReadWrite;
        }
        computePass.resourceTrackers[srd.set][srd.binding] = tracker;
    }
    for (auto& srd : shaderProgram->GetStorageBufferSRDs())
    {
        PassResourceTracker tracker;
        tracker.name         = srd.name;
        tracker.resourceType = PassResourceType::eBuffer;
        tracker.bufferUsage  = BufferUsage::eStorageBuffer;
        if (!srd.writable)
        {
            tracker.accessFlags.SetFlag(AccessFlagBits::eShaderRead);
            tracker.accessMode = AccessMode::eRead;
        }
        else
        {
            tracker.accessFlags.SetFlags(AccessFlagBits::eShaderRead, AccessFlagBits::eShaderWrite);
            tracker.accessMode = AccessMode::eReadWrite;
        }
        computePass.resourceTrackers[srd.set][srd.binding] = tracker;
    }

    for (uint32_t setIndex = 0; setIndex < SRDs.size(); ++setIndex)
    {
        computePass.descriptorSets[setIndex] = shader->CreateDescriptorSet(setIndex);
    }

    m_renderDevice->GetRHIDebug()->SetPipelineDebugName(computePass.pipeline, m_tag + "_Pipeline");

    m_renderDevice->m_computePasses.push_back(computePass);
    return computePass;
}


void ComputePassResourceUpdater::Update()
{
    // rhi::DynamicRHI* GDynamicRHI = m_renderDevice->GetRHI();
    for (const auto& kv : m_dsBindings)
    {
        const auto setIndex             = kv.first;
        const auto& bindings            = kv.second;
        rhi::RHIDescriptorSet* dsHandle = m_computePass->descriptorSets[setIndex];
        dsHandle->Update(bindings);
        // set pass tracker handle values here
        rhi::Handle handle;
        for (auto& srb : bindings)
        {
            PassResourceTracker& tracker = m_computePass->resourceTrackers[setIndex][srb.binding];
            bool hasSampler = srb.type == rhi::ShaderResourceType::eSamplerWithTexture ||
                srb.type == rhi::ShaderResourceType::eSamplerWithTextureBuffer;
            uint32_t index = 0;
            while (index < srb.resources.size())
            {
                rhi::RHIResource* resource = srb.resources[hasSampler ? index + 1 : index];
                if (tracker.resourceType == PassResourceType::eTexture)
                {
                    tracker.textures.emplace_back(dynamic_cast<rhi::RHITexture*>(resource));
                    // tracker.textureHandle = TO_TEX_HANDLE(handle);
                    // tracker.textureSubResRange =
                    //     m_renderDevice->GetTextureSubResourceRange(tracker.textureHandle);
                }
                else if (tracker.resourceType == PassResourceType::eBuffer)
                {
                    tracker.buffer = dynamic_cast<rhi::RHIBuffer*>(resource);
                }
                index = hasSampler ? index + 2 : index + 1;
            }
            // PassResourceTracker& tracker = m_computePass->resourceTrackers[setIndex][srb.binding];
            // if (srb.type == rhi::ShaderResourceType::eSamplerWithTexture ||
            //     srb.type == rhi::ShaderResourceType::eSamplerWithTextureBuffer)
            // {
            //     handle = srb.handles[1];
            // }
            // else
            // {
            //     handle = srb.handles[0];
            // }
            // if (tracker.resourceType == PassResourceType::eTexture)
            // {
            //     tracker.textureHandle = TO_TEX_HANDLE(handle);
            //     tracker.textureSubResRange =
            //         m_renderDevice->GetTextureSubResourceRange(tracker.textureHandle);
            // }
            // else
            // {
            //     tracker.bufferHandle = TO_BUF_HANDLE(handle);
            // }
        }
    }
}

rhi::RHIBuffer* TextureStagingManager::RequireBuffer(uint32_t requiredSize)
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

    rhi::RHIBufferCreateInfo createInfo{};
    createInfo.size = requiredSize;
    createInfo.usageFlags.SetFlag(rhi::BufferUsageFlagBits::eTransferSrcBuffer);
    createInfo.allocateType = rhi::BufferAllocateType::eCPU;

    Entry newEntry{};
    newEntry.size      = requiredSize;
    newEntry.usedFrame = m_renderDevice->GetFramesCounter();
    newEntry.buffer    = GDynamicRHI->CreateBuffer(createInfo);

    m_usedBuffers.push_back(newEntry);

    m_allocatedBuffers.push_back(newEntry.buffer);

    m_usedMemory += requiredSize;

    return newEntry.buffer;
}

void TextureStagingManager::ReleaseBuffer(const rhi::RHIBuffer* buffer)
{
    for (uint32_t i = 0; i < m_usedBuffers.size(); i++)
    {
        if (m_usedBuffers[i].buffer == buffer)
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
    for (rhi::RHIBuffer* buffer : m_allocatedBuffers)
    {
        GDynamicRHI->DestroyBuffer(buffer);
    }
    LOGI("Texture Staging Memory Used: {} bytes.", m_usedMemory);
}

BufferStagingManager::BufferStagingManager(RenderDevice* renderDevice,
                                           uint32_t blockSize,
                                           uint64_t poolSize) :
    BUFFER_SIZE(blockSize),
    POOL_SIZE(poolSize),
    m_renderDevice(renderDevice),
    // GDynamicRHI(renderDevice->GetRHI()),
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
        GDynamicRHI->DestroyBuffer(buffer.buffer);
    }
}

void BufferStagingManager::InsertNewBlock()
{
    rhi::RHIBufferCreateInfo createInfo{};
    createInfo.size         = BUFFER_SIZE;
    createInfo.allocateType = rhi::BufferAllocateType::eCPU;
    createInfo.usageFlags.SetFlag(rhi::BufferUsageFlagBits::eTransferSrcBuffer);

    StagingBuffer buffer{};
    buffer.buffer       = GDynamicRHI->CreateBuffer(createInfo);
    buffer.usedFrame    = 0;
    buffer.occupiedSize = 0;
    m_bufferBlocks.insert(m_bufferBlocks.begin() + m_currentBlockIndex, buffer);
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
    result->buffer      = m_bufferBlocks[m_currentBlockIndex].buffer;
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

RenderDevice::RenderDevice(rhi::GraphicsAPIType APIType, uint32_t numFrames) :
    m_APIType(APIType), m_numFrames(numFrames)
{
    rhi::DynamicRHI::Create(m_APIType);
    m_RHIDebug = rhi::RHIDebug::Create();
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
        frame.cmdListContext = GDynamicRHI->CreateCmdListContext();
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
        GDynamicRHI->DestroyViewport(viewport);
        // delete viewport;
    }
    for (auto& kv : m_renderPassCache)
    {
        GDynamicRHI->DestroyRenderPass(kv.second);
    }
    for (auto& kv : m_pipelineCache)
    {
        GDynamicRHI->DestroyPipeline(kv.second);
    }
    for (auto& kv : m_samplerCache)
    {
        GDynamicRHI->DestroySampler(kv.second);
    }

    for (auto& rp : m_gfxPasses)
    {
        for (const auto& ds : rp.descriptorSets)
        {
            GDynamicRHI->DestroyDescriptorSet(ds);
        }
    }

    for (auto& rp : m_computePasses)
    {
        for (const auto& ds : rp.descriptorSets)
        {
            GDynamicRHI->DestroyDescriptorSet(ds);
        }
    }

    for (auto& buffer : m_buffers)
    {
        GDynamicRHI->DestroyBuffer(buffer);
    }

    m_deletionQueue.Flush();

    m_bufferStagingMgr->Destroy();
    delete m_bufferStagingMgr;

    m_textureManager->Destroy();
    delete m_textureManager;

    m_textureStagingMgr->Destroy();
    delete m_textureStagingMgr;

    m_rendererServer->Destroy();
    delete m_rendererServer;

    for (uint32_t i = 0; i < m_numFrames; i++)
    {
        ProcessPendingFreeResources(i);
    }

    for (RenderFrame& frame : m_frames)
    {
        delete frame.uploadCmdList;
        delete frame.drawCmdList;
    }

    delete m_RHIDebug;

    GDynamicRHI->Destroy();
    delete GDynamicRHI;
}

void RenderDevice::ExecuteFrame(rhi::RHIViewport* viewport, RenderGraph* rdg, bool present)
{
    GDynamicRHI->BeginDrawingViewport(viewport);
    rdg->Execute(m_frames[m_currentFrame].drawCmdList);
    EndFrame();
    GDynamicRHI->EndDrawingViewport(viewport, m_frames[m_currentFrame].cmdListContext, present);
}

void RenderDevice::ExecuteFrame(rhi::RHIViewport* viewport,
                                const std::vector<RenderGraph*>& rdgs,
                                bool present)
{
    GDynamicRHI->BeginDrawingViewport(viewport);
    for (auto* rdg : rdgs)
    {
        rdg->Execute(m_frames[m_currentFrame].drawCmdList);
    }
    EndFrame();
    GDynamicRHI->EndDrawingViewport(viewport, m_frames[m_currentFrame].cmdListContext, present);
}

void RenderDevice::ExecuteImmediate(rhi::RHIViewport* viewport, RenderGraph* rdg)
{
    rhi::RHICommandList* cmdList = GDynamicRHI->GetImmediateCommandList();
    cmdList->BeginRender();
    rdg->Execute(cmdList);
    cmdList->EndRender();
    GDynamicRHI->WaitForCommandList(cmdList);
}

rhi::RHITexture* RenderDevice::CreateTextureColorRT(const TextureFormat& texFormat,
                                                    TextureUsageHint usageHint,
                                                    std::string texName)
{
    rhi::RHITextureCreateInfo texInfo{};
    texInfo.type          = static_cast<rhi::TextureType>(texFormat.dimension);
    texInfo.format        = texFormat.format;
    texInfo.width         = texFormat.width;
    texInfo.height        = texFormat.height;
    texInfo.depth         = texFormat.depth;
    texInfo.mipmaps       = texFormat.mipmaps;
    texInfo.arrayLayers   = texFormat.arrayLayers;
    texInfo.samples       = texFormat.sampleCount;
    texInfo.mutableFormat = texFormat.mutableFormat;
    texInfo.tag           = std::move(texName);
    texInfo.usageFlags.SetFlags(rhi::TextureUsageFlagBits::eColorAttachment,
                                rhi::TextureUsageFlagBits::eSampled);

    if (usageHint.copyUsage)
    {
        texInfo.usageFlags.SetFlags(rhi::TextureUsageFlagBits::eTransferSrc,
                                    rhi::TextureUsageFlagBits::eTransferDst);
    }

    rhi::RHITexture* textureRD = GDynamicRHI->CreateTexture(texInfo);
    // rhi::RHITexture* handle    = GDynamicRHI->CreateTexture(texInfo);
    // textureRD->Init(this, handle);

    // m_textureMap[handle] = textureRD;

    return textureRD;
}

rhi::RHITexture* RenderDevice::CreateTextureDepthStencilRT(const TextureFormat& texFormat,
                                                           TextureUsageHint usageHint,
                                                           std::string texName)
{
    rhi::RHITextureCreateInfo texInfo{};
    texInfo.type          = static_cast<rhi::TextureType>(texFormat.dimension);
    texInfo.format        = texFormat.format;
    texInfo.width         = texFormat.width;
    texInfo.height        = texFormat.height;
    texInfo.depth         = texFormat.depth;
    texInfo.mipmaps       = texFormat.mipmaps;
    texInfo.arrayLayers   = texFormat.arrayLayers;
    texInfo.samples       = texFormat.sampleCount;
    texInfo.mutableFormat = texFormat.mutableFormat;
    texInfo.tag           = std::move(texName);
    texInfo.usageFlags.SetFlags(rhi::TextureUsageFlagBits::eDepthStencilAttachment,
                                rhi::TextureUsageFlagBits::eSampled);

    if (usageHint.copyUsage)
    {
        texInfo.usageFlags.SetFlags(rhi::TextureUsageFlagBits::eTransferSrc,
                                    rhi::TextureUsageFlagBits::eTransferDst);
    }

    rhi::RHITexture* texture = GDynamicRHI->CreateTexture(texInfo);

    return texture;
}

rhi::RHITexture* RenderDevice::CreateTextureStorage(const TextureFormat& texFormat,
                                                    TextureUsageHint usageHint,
                                                    std::string texName)
{
    rhi::RHITextureCreateInfo texInfo{};
    texInfo.type          = static_cast<rhi::TextureType>(texFormat.dimension);
    texInfo.format        = texFormat.format;
    texInfo.width         = texFormat.width;
    texInfo.height        = texFormat.height;
    texInfo.depth         = texFormat.depth;
    texInfo.mipmaps       = texFormat.mipmaps;
    texInfo.arrayLayers   = texFormat.arrayLayers;
    texInfo.samples       = texFormat.sampleCount;
    texInfo.mutableFormat = texFormat.mutableFormat;
    texInfo.tag           = std::move(texName);

    texInfo.usageFlags.SetFlags(rhi::TextureUsageFlagBits::eStorage,
                                rhi::TextureUsageFlagBits::eSampled);

    if (usageHint.copyUsage)
    {
        texInfo.usageFlags.SetFlags(rhi::TextureUsageFlagBits::eTransferSrc,
                                    rhi::TextureUsageFlagBits::eTransferDst);
    }

    rhi::RHITexture* texture = GDynamicRHI->CreateTexture(texInfo);

    return texture;
}

rhi::RHITexture* RenderDevice::CreateTextureSampled(const TextureFormat& texFormat,
                                                    TextureUsageHint usageHint,
                                                    std::string texName)
{
    rhi::RHITextureCreateInfo texInfo{};
    texInfo.type          = static_cast<rhi::TextureType>(texFormat.dimension);
    texInfo.format        = texFormat.format;
    texInfo.width         = texFormat.width;
    texInfo.height        = texFormat.height;
    texInfo.depth         = texFormat.depth;
    texInfo.mipmaps       = texFormat.mipmaps;
    texInfo.arrayLayers   = texFormat.arrayLayers;
    texInfo.samples       = texFormat.sampleCount;
    texInfo.mutableFormat = texFormat.mutableFormat;
    texInfo.tag           = std::move(texName);
    texInfo.usageFlags.SetFlags(rhi::TextureUsageFlagBits::eSampled);

    if (usageHint.copyUsage)
    {
        texInfo.usageFlags.SetFlags(rhi::TextureUsageFlagBits::eTransferSrc,
                                    rhi::TextureUsageFlagBits::eTransferDst);
    }

    rhi::RHITexture* texture = GDynamicRHI->CreateTexture(texInfo);

    return texture;
}

rhi::RHITexture* RenderDevice::CreateTextureDummy(const TextureFormat& texFormat,
                                                  TextureUsageHint usageHint,
                                                  std::string texName)
{
    rhi::RHITextureCreateInfo texInfo{};
    texInfo.type          = static_cast<rhi::TextureType>(texFormat.dimension);
    texInfo.format        = texFormat.format;
    texInfo.width         = texFormat.width;
    texInfo.height        = texFormat.height;
    texInfo.depth         = texFormat.depth;
    texInfo.mipmaps       = texFormat.mipmaps;
    texInfo.arrayLayers   = texFormat.arrayLayers;
    texInfo.samples       = texFormat.sampleCount;
    texInfo.mutableFormat = texFormat.mutableFormat;
    texInfo.tag           = std::move(texName);
    texInfo.usageFlags.SetFlags(rhi::TextureUsageFlagBits::eSampled);

    if (usageHint.copyUsage)
    {
        texInfo.usageFlags.SetFlags(rhi::TextureUsageFlagBits::eTransferSrc,
                                    rhi::TextureUsageFlagBits::eTransferDst);
    }

    rhi::RHITexture* texture = GDynamicRHI->CreateTexture(texInfo);

    return texture;
}

rhi::RHITexture* RenderDevice::CreateTextureProxy(rhi::RHITexture* baseTexture,
                                                  const TextureProxyFormat& proxyFormat,
                                                  std::string texName)
{
    rhi::RHITextureProxyCreateInfo textureProxyInfo{};
    textureProxyInfo.type        = static_cast<rhi::TextureType>(proxyFormat.dimension);
    textureProxyInfo.arrayLayers = proxyFormat.arrayLayers;
    textureProxyInfo.mipmaps     = proxyFormat.mipmaps;
    textureProxyInfo.format      = proxyFormat.format;
    textureProxyInfo.tag         = std::move(texName);

    rhi::RHITexture* texture = baseTexture->CreateProxy(textureProxyInfo);

    return texture;
}

// rhi::RHITexture* RenderDevice::GetTextureRDFromHandle(const rhi::RHITexture* handle)
// {
//     return m_textureMap[handle];
// }

void RenderDevice::DestroyTexture(rhi::RHITexture* texture)
{
    // if (textureRD->IsProxy())
    // {
    //     textureRD->GetBaseTexture()->DecreaseRefCount();
    // }
    // textureRD->DecreaseRefCount();
    // m_textureMap.erase(textureRD->GetHandle());
    m_frames[m_currentFrame].texturesPendingFree.emplace_back(texture);
}

// rhi::RHITexture* RenderDevice::CreateTexture(const rhi::TextureInfo& textureInfo)
// {
//     ASSERT(!textureInfo.name.empty());
//     return m_textureManager->CreateTexture(textureInfo);
// }

// rhi::RHITexture* RenderDevice::CreateTextureProxy(const rhi::RHITexture* baseTexture,
//                                                     const rhi::TextureProxyInfo& proxyInfo)
// {
//     return m_textureManager->CreateTextureProxy(baseTexture, proxyInfo);
// }

// rhi::RHITexture* RenderDevice::GetBaseTextureForProxy(const rhi::RHITexture* handle) const
// {
//     return m_textureManager->GetBaseTextureForProxy(handle);
// }
//
// bool RenderDevice::IsProxyTexture(const rhi::RHITexture* handle) const
// {
//     return m_textureManager->IsProxyTexture(handle);
// }

void RenderDevice::GenerateTextureMipmaps(rhi::RHITexture* textureHandle,
                                          rhi::RHICommandList* cmdList)
{
    cmdList->GenerateTextureMipmaps(textureHandle);
}

rhi::RHIBuffer* RenderDevice::CreateVertexBuffer(uint32_t dataSize, const uint8_t* pData)
{
    BitField<rhi::BufferUsageFlagBits> usages;
    usages.SetFlag(rhi::BufferUsageFlagBits::eVertexBuffer);
    usages.SetFlag(rhi::BufferUsageFlagBits::eTransferDstBuffer);
    usages.SetFlag(rhi::BufferUsageFlagBits::eStorageBuffer);

    rhi::RHIBufferCreateInfo createInfo{};
    createInfo.size         = dataSize;
    createInfo.usageFlags   = usages;
    createInfo.allocateType = rhi::BufferAllocateType::eGPU;

    rhi::RHIBuffer* vertexBuffer = GDynamicRHI->CreateBuffer(createInfo);
    UpdateBufferInternal(vertexBuffer, 0, dataSize, pData);
    m_buffers.push_back(vertexBuffer);
    return vertexBuffer;
}

rhi::RHIBuffer* RenderDevice::CreateIndexBuffer(uint32_t dataSize, const uint8_t* pData)
{
    BitField<rhi::BufferUsageFlagBits> usages;
    usages.SetFlag(rhi::BufferUsageFlagBits::eIndexBuffer);
    usages.SetFlag(rhi::BufferUsageFlagBits::eTransferDstBuffer);
    usages.SetFlag(rhi::BufferUsageFlagBits::eStorageBuffer);

    rhi::RHIBufferCreateInfo createInfo{};
    createInfo.size         = dataSize;
    createInfo.usageFlags   = usages;
    createInfo.allocateType = rhi::BufferAllocateType::eGPU;

    rhi::RHIBuffer* indexBuffer = GDynamicRHI->CreateBuffer(createInfo);
    UpdateBufferInternal(indexBuffer, 0, dataSize, pData);
    m_buffers.push_back(indexBuffer);
    return indexBuffer;
}

rhi::RHIBuffer* RenderDevice::CreateUniformBuffer(uint32_t dataSize, const uint8_t* pData)
{
    BitField<rhi::BufferUsageFlagBits> usages;
    usages.SetFlag(rhi::BufferUsageFlagBits::eUniformBuffer);
    usages.SetFlag(rhi::BufferUsageFlagBits::eTransferDstBuffer);

    uint32_t paddedSize = PadUniformBufferSize(dataSize);

    rhi::RHIBufferCreateInfo createInfo{};
    createInfo.size         = paddedSize;
    createInfo.usageFlags   = usages;
    createInfo.allocateType = rhi::BufferAllocateType::eGPU;


    rhi::RHIBuffer* uniformBuffer = GDynamicRHI->CreateBuffer(createInfo);
    if (pData != nullptr)
    {
        UpdateBufferInternal(uniformBuffer, 0, paddedSize, pData);
    }
    m_buffers.push_back(uniformBuffer);
    return uniformBuffer;
}

rhi::RHIBuffer* RenderDevice::CreateStorageBuffer(uint32_t dataSize,
                                                  const uint8_t* pData,
                                                  std::string bufferName)
{
    BitField<rhi::BufferUsageFlagBits> usages;
    usages.SetFlag(rhi::BufferUsageFlagBits::eStorageBuffer);
    usages.SetFlag(rhi::BufferUsageFlagBits::eTransferDstBuffer);

    uint32_t paddedSize = PadStorageBufferSize(dataSize);

    rhi::RHIBufferCreateInfo createInfo{};
    createInfo.size         = paddedSize;
    createInfo.usageFlags   = usages;
    createInfo.allocateType = rhi::BufferAllocateType::eGPU;
    createInfo.tag          = std::move(bufferName);

    rhi::RHIBuffer* storageBuffer = GDynamicRHI->CreateBuffer(createInfo);

    if (pData != nullptr)
    {
        UpdateBufferInternal(storageBuffer, 0, paddedSize, pData);
    }
    m_buffers.push_back(storageBuffer);
    return storageBuffer;
}

rhi::RHIBuffer* RenderDevice::CreateIndirectBuffer(uint32_t dataSize,
                                                   const uint8_t* pData,
                                                   std::string bufferName)
{
    BitField<rhi::BufferUsageFlagBits> usages;
    usages.SetFlag(rhi::BufferUsageFlagBits::eStorageBuffer);
    usages.SetFlag(rhi::BufferUsageFlagBits::eIndirectBuffer);
    usages.SetFlag(rhi::BufferUsageFlagBits::eTransferDstBuffer);

    uint32_t paddedSize = PadStorageBufferSize(dataSize);

    rhi::RHIBufferCreateInfo createInfo{};
    createInfo.size         = paddedSize;
    createInfo.usageFlags   = usages;
    createInfo.allocateType = rhi::BufferAllocateType::eGPU;
    createInfo.tag          = std::move(bufferName);

    rhi::RHIBuffer* indirectBuffer = GDynamicRHI->CreateBuffer(createInfo);

    if (pData != nullptr)
    {
        UpdateBufferInternal(indirectBuffer, 0, paddedSize, pData);
    }
    m_buffers.push_back(indirectBuffer);
    return indirectBuffer;
}

size_t RenderDevice::PadUniformBufferSize(size_t originalSize)
{
    auto minUboAlignment = GDynamicRHI->QueryGPUInfo().uniformBufferAlignment;
    size_t alignedSize   = (originalSize + minUboAlignment - 1) & ~(minUboAlignment - 1);
    return alignedSize;
}

size_t RenderDevice::PadStorageBufferSize(size_t originalSize)
{
    auto minAlignment  = GDynamicRHI->QueryGPUInfo().storageBufferAlignment;
    size_t alignedSize = (originalSize + minAlignment - 1) & ~(minAlignment - 1);
    return alignedSize;
}

void RenderDevice::UpdateBuffer(rhi::RHIBuffer* buffer,
                                uint32_t dataSize,
                                const uint8_t* pData,
                                uint32_t offset)
{
    UpdateBufferInternal(buffer, offset, dataSize, pData);
}

void RenderDevice::DestroyBuffer(rhi::RHIBuffer* bufferHandle)
{
    GDynamicRHI->DestroyBuffer(bufferHandle);
}

rhi::RenderPassHandle RenderDevice::GetOrCreateRenderPass(const rhi::RenderPassLayout& layout)
{
    auto hash = CalcRenderPassLayoutHash(layout);
    if (!m_renderPassCache.contains(hash))
    {
        // create new one
        m_renderPassCache[hash] = GDynamicRHI->CreateRenderPass(layout);
    }
    return m_renderPassCache[hash];
}

rhi::RHIPipeline* RenderDevice::GetOrCreateGfxPipeline(
    rhi::GfxPipelineStates& PSO,
    rhi::RHIShader* shader,
    const rhi::RenderPassHandle& renderPass,
    const std::vector<rhi::ShaderSpecializationConstant>& specializationConstants)
{
    rhi::RHIGfxPipelineCreateInfo createInfo{};
    createInfo.shader           = shader;
    createInfo.states           = PSO;
    createInfo.renderPassHandle = renderPass;
    createInfo.subpassIdx       = 0;

    auto hash = CalcGfxPipelineHash(PSO, shader, renderPass, specializationConstants);
    if (!m_pipelineCache.contains(hash))
    {
        // create new one
        m_pipelineCache[hash] = GDynamicRHI->CreatePipeline(createInfo);
    }
    return m_pipelineCache[hash];
}

rhi::RHIPipeline* RenderDevice::GetOrCreateGfxPipeline(
    rhi::GfxPipelineStates& PSO,
    rhi::RHIShader* shader,
    const rhi::RenderPassLayout& renderPassLayout,
    const std::vector<rhi::ShaderSpecializationConstant>& specializationConstants)
{
    rhi::RHIGfxPipelineCreateInfo createInfo{};
    createInfo.shader           = shader;
    createInfo.states           = PSO;
    createInfo.renderPassLayout = renderPassLayout;
    createInfo.subpassIdx       = 0;

    auto hash = CalcGfxPipelineHash(PSO, shader, renderPassLayout, specializationConstants);
    if (!m_pipelineCache.contains(hash))
    {
        // create new one
        m_pipelineCache[hash] = GDynamicRHI->CreatePipeline(createInfo);
    }
    return m_pipelineCache[hash];
}

rhi::RHIPipeline* RenderDevice::GetOrCreateComputePipeline(rhi::RHIShader* shader)
{
    rhi::RHIComputePipelineCreateInfo createInfo{};
    createInfo.shader = shader;

    auto hash = CalcComputePipelineHash(shader);
    if (!m_pipelineCache.contains(hash))
    {
        m_pipelineCache[hash] = GDynamicRHI->CreatePipeline(createInfo);
    }
    return m_pipelineCache[hash];
}

rhi::RHIViewport* RenderDevice::CreateViewport(void* pWindow,
                                               uint32_t width,
                                               uint32_t height,
                                               bool enableVSync)
{
    // auto* viewport = GDynamicRHI->CreateViewport(pWindow, width, height, enableVSync);
    auto* viewport = GDynamicRHI->CreateViewport(pWindow, width, height, enableVSync);
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
    GDynamicRHI->DestroyViewport(viewport);
}

void RenderDevice::ResizeViewport(rhi::RHIViewport* viewport, uint32_t width, uint32_t height)
{
    if (viewport != nullptr && (viewport->GetWidth() != width || viewport->GetHeight() != height))
    {
        GDynamicRHI->SubmitAllGPUCommands();
        viewport->Resize(width, height);
        BeginFrame();
    }
}

void RenderDevice::UpdateGraphicsPassOnResize(GraphicsPass& gfxPass, rhi::RHIViewport* viewport)
{
    if (rhi::RHIOptions::GetInstance().UseDynamicRendering())
    {
        rhi::RenderTargetLoadOp oldColorRTLoadOp =
            gfxPass.renderPassLayout.GetColorRenderTargets()[0].loadOp;
        rhi::RenderTargetStoreOp oldColorRTStoreOp =
            gfxPass.renderPassLayout.GetColorRenderTargets()[0].storeOp;
        gfxPass.renderPassLayout.ClearRenderTargetInfo();
        gfxPass.renderPassLayout.AddColorRenderTarget(
            viewport->GetSwapchainFormat(), viewport->GetColorBackBuffer(),
            viewport->GetColorBackBufferRange(), oldColorRTLoadOp, oldColorRTStoreOp);


        gfxPass.renderPassLayout.SetDepthStencilRenderTarget(
            viewport->GetDepthStencilFormat(), viewport->GetDepthStencilBackBuffer(),
            viewport->GetDepthStencilBackBufferRange(), rhi::RenderTargetLoadOp::eClear,
            rhi::RenderTargetStoreOp::eStore);
    }
    else
    {
        gfxPass.framebuffer = viewport->GetCompatibleFramebufferForBackBuffer(gfxPass.renderPass);
    }
}

rhi::RHISampler* RenderDevice::CreateSampler(const rhi::RHISamplerCreateInfo& samplerInfo)
{
    const size_t samplerHash = CalcSamplerHash(samplerInfo);
    if (!m_samplerCache.contains(samplerHash))
    {
        m_samplerCache[samplerHash] = GDynamicRHI->CreateSampler(samplerInfo);
    }

    return m_samplerCache[samplerHash];
}

// rhi::TextureSubResourceRange RenderDevice::GetTextureSubResourceRange(rhi::RHITexture* handle)
// {
//     return GDynamicRHI->GetTextureSubResourceRange(handle);
// }

rhi::RHITexture* RenderDevice::LoadTexture2D(const std::string& file, bool requireMipmap)
{
    return m_textureManager->LoadTexture2D(file, requireMipmap);
}

void RenderDevice::LoadSceneTextures(const sg::Scene* scene,
                                     std::vector<rhi::RHITexture*>& outTextures)
{
    m_textureManager->LoadSceneTextures(scene, outTextures);
}

void RenderDevice::LoadTextureEnv(const std::string& file, EnvTexture* texture)
{
    auto fullPath = ZEN_TEXTURE_PATH + file;
    m_textureManager->LoadTextureEnv(fullPath, texture);
}

void RenderDevice::UpdateTextureOneTime(rhi::RHITexture* textureHandle,
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
    uint8_t* dataPtr = submitResult.buffer->Map();
    dataPtr += submitResult.writeOffset;

    // copy
    memcpy(dataPtr, pData, submitResult.writeSize);

    // unmap
    submitResult.buffer->Unmap();
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

void RenderDevice::UpdateTextureBatch(rhi::RHITexture* textureHandle,
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
                uint8_t* dataPtr = submitResult.buffer->Map();
                // copy
                CopyRegion(pData, dataPtr + submitResult.writeOffset, x, y, regionWidth,
                           regionHeight, width, imageStride, pixelSize);
                // unmap
                submitResult.buffer->Unmap();
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

void RenderDevice::UpdateBufferInternal(rhi::RHIBuffer* bufferHandle,
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
        uint8_t* dataPtr = submitResult.buffer->Map();
        // copy
        memcpy(dataPtr + submitResult.writeOffset, pData + writePosition, submitResult.writeSize);
        // unmap
        submitResult.buffer->Unmap();
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
            GDynamicRHI->WaitForCommandList(m_frames[i].uploadCmdList);
            GDynamicRHI->WaitForCommandList(m_frames[i].drawCmdList);
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
    // for (rhi::RHITexture* texture : GetCurrentFrame()->texturesPendingFree)
    // {
    //     texture->DecreaseRefCount();
    // }
    // GetCurrentFrame()->texturesPendingFree.clear();
    ProcessPendingFreeResources(m_currentFrame);
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

void RenderDevice::ProcessPendingFreeResources(uint32_t frameIndex)
{
    for (rhi::RHITexture* texture : m_frames[frameIndex].texturesPendingFree)
    {
        texture->ReleaseReference();
    }
    m_frames[frameIndex].texturesPendingFree.clear();
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
    // combineHash(layout.GetNumSamples());
    // combineHash(layout.GetColorRenderTargetLoadOp());
    // combineHash(layout.GetColorRenderTargetStoreOp());
    // combineHash(layout.GetDepthStencilRenderTargetLoadOp());
    // combineHash(layout.GetDepthStencilRenderTargetStoreOp());
    combineHash(layout.HasDepthStencilRenderTarget());

    // Hash color render targets
    for (const auto& rt : layout.GetColorRenderTargets())
    {
        // Assuming RenderTarget is hashable
        combineHash(rt.format);
        combineHash(rt.numSamples);
        combineHash(rt.loadOp);
        combineHash(rt.storeOp);
        // combineHash(rt.usage);
    }
    if (layout.HasDepthStencilRenderTarget())
    {
        // Hash depth stencil render target (assuming RenderTarget is hashable)
        combineHash(layout.GetDepthStencilRenderTarget().format);
    }

    // combineHash(layout.GetDepthStencilRenderTarget().usage);

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
        seed ^= uint64Hasher(info.renderTargets[i]->GetHash32()) + 0x9e3779b9 + (seed << 6) +
            (seed >> 2);
    }
    return seed;
}

size_t RenderDevice::CalcGfxPipelineHash(
    const rhi::GfxPipelineStates& pso,
    rhi::RHIShader* shader,
    const rhi::RenderPassHandle& renderPass,
    const std::vector<rhi::ShaderSpecializationConstant>& specializationConstants)
{
    std::size_t seed = 0;
    // Hashing utility
    auto combineHash = [&seed](auto&& value) {
        seed ^= std::hash<std::decay_t<decltype(value)>>{}(value) + 0x9e3779b9 + (seed << 6) +
            (seed >> 2);
    };
    combineHash(shader);
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
    rhi::RHIShader* shader,
    const rhi::RenderPassLayout& renderPassLayout,
    const std::vector<rhi::ShaderSpecializationConstant>& specializationConstants)
{
    std::size_t seed = 0;
    // Hashing utility
    auto combineHash = [&seed](auto&& value) {
        seed ^= std::hash<std::decay_t<decltype(value)>>{}(value) + 0x9e3779b9 + (seed << 6) +
            (seed >> 2);
    };
    combineHash(shader);
    // for (uint32_t i = 0; i < renderPassLayout.GetNumRenderTargets(); i++)
    for (auto& colorRT : renderPassLayout.GetColorRenderTargets())
    {
        combineHash(colorRT.texture);
    }
    if (renderPassLayout.HasDepthStencilRenderTarget())
    {
        combineHash(renderPassLayout.GetDepthStencilRenderTarget().texture);
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

size_t RenderDevice::CalcComputePipelineHash(rhi::RHIShader* shader)
{
    std::size_t seed = 0;
    // Hashing utility
    auto combineHash = [&seed](auto&& value) {
        seed ^= std::hash<std::decay_t<decltype(value)>>{}(value) + 0x9e3779b9 + (seed << 6) +
            (seed >> 2);
    };
    // todo: use shader->GetHash()
    combineHash(shader);
    return seed;
}

size_t RenderDevice::CalcSamplerHash(const rhi::RHISamplerCreateInfo& info)
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