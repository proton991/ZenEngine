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
//                                                                const RHITexture* handle,
//                                                                bool clear)
// {
//     m_rpLayout.AddColorRenderTarget(format, RHITextureUsage::eColorAttachment, handle,
//                                     m_renderDevice->GetTextureSubResourceRange(handle));
//     m_rpLayout.SetColorTargetLoadStoreOp(clear ? RHIRenderTargetLoadOp::eClear :
//                                                  RHIRenderTargetLoadOp::eLoad,
//                                          RHIRenderTargetStoreOp::eStore);
//     m_framebufferInfo.numRenderTarget++;
//     return *this;
// }

GraphicsPassBuilder::GraphicsPassBuilder(RenderDevice* renderDevice) : m_renderDevice(renderDevice)
{
    // m_pGfxPass                   = static_cast<GraphicsPass*>(ZEN_MEM_ALLOC(sizeof(GraphicsPass)));

    m_pGfxPass                   = ZEN_NEW() GraphicsPass();
    m_pGfxPass->pRenderingLayout = m_renderDevice->AcquireRenderingLayout();
}

GraphicsPassBuilder& GraphicsPassBuilder::AddViewportColorRT(RHIViewport* viewport,
                                                             RHIRenderTargetLoadOp loadOp,
                                                             RHIRenderTargetStoreOp storeOp)
{
    m_pGfxPass->pRenderingLayout->AddColorRenderTarget(
        viewport->GetSwapchainFormat(), viewport->GetColorBackBuffer(), loadOp, storeOp);
    // m_rpLayout.AddColorRenderTarget(viewport->GetSwapchainFormat(), viewport->GetColorBackBuffer(),
    //                                 viewport->GetColorBackBufferRange(), loadOp, storeOp);
    // m_rpLayout.SetColorTargetLoadStoreOp(clear ? RHIRenderTargetLoadOp::eClear :
    //                                              RHIRenderTargetLoadOp::eLoad,
    //                                      RHIRenderTargetStoreOp::eStore);
    m_framebufferInfo.numRenderTarget++;
    return *this;
}

GraphicsPassBuilder& GraphicsPassBuilder::SetViewportDepthStencilRT(RHIViewport* viewport,
                                                                    RHIRenderTargetLoadOp loadOp,
                                                                    RHIRenderTargetStoreOp storeOp)
{
    m_pGfxPass->pRenderingLayout->AddDepthStencilRenderTarget(
        viewport->GetDepthStencilFormat(), viewport->GetDepthStencilBackBuffer(), loadOp, storeOp);
    // m_rpLayout.SetDepthStencilRenderTarget(
    //     viewport->GetDepthStencilFormat(), viewport->GetDepthStencilBackBuffer(),
    //     viewport->GetDepthStencilBackBufferRange(), loadOp, storeOp);
    // m_rpLayout.SetDepthStencilTargetLoadStoreOp(loadOp, storeOp);
    m_framebufferInfo.numRenderTarget++;
    return *this;
}

GraphicsPassBuilder& GraphicsPassBuilder::AddColorRenderTarget(RHITexture* colorRT,
                                                               RHIRenderTargetLoadOp loadOp,
                                                               RHIRenderTargetStoreOp storeOp)
{
    m_pGfxPass->pRenderingLayout->AddColorRenderTarget(colorRT->GetFormat(), colorRT, loadOp,
                                                       storeOp);
    // m_rpLayout.AddColorRenderTarget(colorRT->GetFormat(), colorRT, colorRT->GetSubResourceRange(),
    //                                 loadOp, storeOp);
    // m_rpLayout.SetColorTargetLoadStoreOp(clear ? RHIRenderTargetLoadOp::eClear :
    //                                              RHIRenderTargetLoadOp::eLoad,
    // RHIRenderTargetStoreOp::eStore);
    m_framebufferInfo.numRenderTarget++;
    return *this;
}

GraphicsPassBuilder& GraphicsPassBuilder::SetDepthStencilTarget(RHITexture* depthStencilRT,
                                                                RHIRenderTargetLoadOp loadOp,
                                                                RHIRenderTargetStoreOp storeOp)
{
    m_pGfxPass->pRenderingLayout->AddDepthStencilRenderTarget(depthStencilRT->GetFormat(),
                                                              depthStencilRT, loadOp, storeOp);

    // m_rpLayout.SetDepthStencilRenderTarget(depthStencilRT->GetFormat(), depthStencilRT,
    //                                        depthStencilRT->GetSubResourceRange(), loadOp, storeOp);
    // m_rpLayout.SetDepthStencilTargetLoadStoreOp(loadOp, storeOp);
    m_framebufferInfo.numRenderTarget++;
    return *this;
}

// GraphicsPassBuilder& GraphicsPassBuilder::SetDepthStencilTarget(DataFormat format,
//                                                                 const RHITexture* handle,
//                                                                 RHIRenderTargetLoadOp loadOp,
//                                                                 RHIRenderTargetStoreOp storeOp)
// {
//     m_rpLayout.SetDepthStencilRenderTarget(format, handle,
//                                            m_renderDevice->GetTextureSubResourceRange(handle));
//     m_rpLayout.SetDepthStencilTargetLoadStoreOp(loadOp, storeOp);
//     m_framebufferInfo.numRenderTarget++;
//     return *this;
// }

GraphicsPass* GraphicsPassBuilder::Build()
{

    // DynamicRHI* GDynamicRHI = m_renderDevice->GetRHI();
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

    // std::vector<RHITexture*> rtHandles;
    // rtHandles.resize(m_rpLayout.GetNumColorRenderTargets());
    //
    // for (uint32_t i = 0; i < m_rpLayout.GetNumColorRenderTargets(); i++)
    // {
    //     rtHandles[i] = m_rpLayout.GetColorRenderTargets()[i].texture;
    // }

    // m_framebufferInfo.renderTargets = m_rpLayout.GetRenderTargetHandles();
    // m_framebufferInfo.renderTargets = rtHandles.data();

    RHIShader* shader = shaderProgram->GetShader();



    m_pGfxPass->shaderProgram     = shaderProgram;
    m_pGfxPass->numDescriptorSets = shaderProgram->GetNumDescriptorSets();

    // m_pGfxPass->renderPassLayout  = m_rpLayout;

    m_pGfxPass->pipeline = m_renderDevice->GetOrCreateGfxPipeline(
        m_PSO, shader, m_pGfxPass->pRenderingLayout, m_specializationConstants);

    // set up resource trackers
    // build PassTextureTracker and PassBufferTracker
    // PassTextureTracker's handle is not available until UpdatePassResource is called
    for (auto& srd : shaderProgram->GetSampledTextureSRDs())
    {
        PassResourceTracker tracker;
        tracker.name = srd.name;
        // for combined image samplers,
        tracker.textureUsage = RHITextureUsage::eSampled;
        tracker.resourceType = PassResourceType::eTexture;
        tracker.accessMode   = RHIAccessMode::eRead;
        tracker.accessFlags.SetFlag(RHIAccessFlagBits::eShaderRead);
        m_pGfxPass->resourceTrackers[srd.set][srd.binding] = std::move(tracker);
    }
    for (auto& srd : shaderProgram->GetStorageImageSRDs())
    {
        PassResourceTracker tracker;
        tracker.name         = srd.name;
        tracker.resourceType = PassResourceType::eTexture;
        tracker.textureUsage = RHITextureUsage::eStorage;
        if (!srd.writable)
        {
            tracker.accessFlags.SetFlag(RHIAccessFlagBits::eShaderRead);
            tracker.accessMode = RHIAccessMode::eRead;
        }
        else
        {
            tracker.accessFlags.SetFlags(RHIAccessFlagBits::eShaderRead,
                                         RHIAccessFlagBits::eShaderWrite);
            tracker.accessMode = RHIAccessMode::eReadWrite;
        }
        m_pGfxPass->resourceTrackers[srd.set][srd.binding] = std::move(tracker);
    }
    for (auto& srd : shaderProgram->GetUniformBufferSRDs())
    {
        PassResourceTracker tracker;
        tracker.name         = srd.name;
        tracker.resourceType = PassResourceType::eBuffer;
        tracker.bufferUsage  = RHIBufferUsage::eUniformBuffer;
        tracker.accessMode   = RHIAccessMode::eRead;
        tracker.accessFlags.SetFlag(RHIAccessFlagBits::eShaderRead);
        m_pGfxPass->resourceTrackers[srd.set][srd.binding] = std::move(tracker);
    }
    for (auto& srd : shaderProgram->GetStorageBufferSRDs())
    {
        PassResourceTracker tracker;
        tracker.name         = srd.name;
        tracker.resourceType = PassResourceType::eBuffer;
        tracker.bufferUsage  = RHIBufferUsage::eStorageBuffer;
        if (!srd.writable)
        {
            tracker.accessFlags.SetFlag(RHIAccessFlagBits::eShaderRead);
            tracker.accessMode = RHIAccessMode::eRead;
        }
        else
        {
            tracker.accessFlags.SetFlags(RHIAccessFlagBits::eShaderRead,
                                         RHIAccessFlagBits::eShaderWrite);
            tracker.accessMode = RHIAccessMode::eReadWrite;
        }
        m_pGfxPass->resourceTrackers[srd.set][srd.binding] = std::move(tracker);
    }

    for (uint32_t setIndex = 0; setIndex < m_pGfxPass->numDescriptorSets; ++setIndex)
    {
        m_pGfxPass->descriptorSets[setIndex] = shader->CreateDescriptorSet(setIndex);
    }

    // note: support both descriptor set update at build time and late-update using GraphicsPassResourceUpdater
    for (const auto& kv : m_dsBindings)
    {
        const auto setIndex  = kv.first;
        const auto& bindings = kv.second;
        m_pGfxPass->descriptorSets[setIndex]->Update(bindings);
    }

    m_renderDevice->GetRHIDebug()->SetPipelineDebugName(m_pGfxPass->pipeline, m_tag + "_Pipeline");

    m_renderDevice->m_gfxPasses.push_back(m_pGfxPass);
    return m_pGfxPass;
}

void GraphicsPassResourceUpdater::Update()
{
    // DynamicRHI* GDynamicRHI = m_renderDevice->GetRHI();
    for (const auto& kv : m_dsBindings)
    {
        const auto setIndex        = kv.first;
        const auto& bindings       = kv.second;
        RHIDescriptorSet* dsHandle = m_gfxPass->descriptorSets[setIndex];
        dsHandle->Update(bindings);
        // set pass tracker handle values here
        for (auto& srb : bindings)
        {
            PassResourceTracker& tracker = m_gfxPass->resourceTrackers[setIndex][srb.binding];
            bool hasSampler              = srb.type == RHIShaderResourceType::eSamplerWithTexture ||
                srb.type == RHIShaderResourceType::eSamplerWithTextureBuffer;
            uint32_t index = 0;
            while (index < srb.resources.size())
            {
                RHIResource* resource = srb.resources[hasSampler ? index + 1 : index];
                if (tracker.resourceType == PassResourceType::eTexture)
                {
                    tracker.textures.emplace_back(dynamic_cast<RHITexture*>(resource));
                    // tracker.textureHandle = TO_TEX_HANDLE(handle);
                    // tracker.textureSubResRange =
                    //     m_renderDevice->GetTextureSubResourceRange(tracker.textureHandle);
                }
                else if (tracker.resourceType == PassResourceType::eBuffer)
                {
                    tracker.buffer = dynamic_cast<RHIBuffer*>(resource);
                }
                index = hasSampler ? index + 2 : index + 1;
            }
            // if (srb.type == RHIShaderResourceType::eSamplerWithTexture ||
            //     srb.type == RHIShaderResourceType::eSamplerWithTextureBuffer)
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
            //     // RHITexture* textureHandle = TO_TEX_HANDLE(handle);
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

ComputePass* ComputePassBuilder::Build()
{


    // DynamicRHI* GDynamicRHI = m_renderDevice->GetRHI();

    ShaderProgram* shaderProgram =
        ShaderProgramManager::GetInstance().RequestShaderProgram(m_shaderProgramName);
    RHIShader* shader = shaderProgram->GetShader();

    ComputePass* pComputePass = ZEN_NEW() ComputePass();

    pComputePass->shaderProgram     = shaderProgram;
    pComputePass->pipeline          = m_renderDevice->GetOrCreateComputePipeline(shader);
    pComputePass->numDescriptorSets = shaderProgram->GetNumDescriptorSets();

    // set up resource trackers
    // build PassTextureTracker and PassBufferTracker
    // PassTextureTracker's handle is not available until UpdatePassResource is called
    for (auto& srd : shaderProgram->GetSampledTextureSRDs())
    {
        PassResourceTracker tracker;
        tracker.name = srd.name;
        // for combined image samplers,
        tracker.resourceType = PassResourceType::eTexture;
        tracker.textureUsage = RHITextureUsage::eSampled;
        tracker.accessMode   = RHIAccessMode::eRead;
        tracker.accessFlags.SetFlag(RHIAccessFlagBits::eShaderRead);
        pComputePass->resourceTrackers[srd.set][srd.binding] = std::move(tracker);
    }
    for (auto& srd : shaderProgram->GetStorageImageSRDs())
    {
        PassResourceTracker tracker;
        tracker.name         = srd.name;
        tracker.resourceType = PassResourceType::eTexture;
        tracker.textureUsage = RHITextureUsage::eStorage;
        if (!srd.writable)
        {
            tracker.accessFlags.SetFlag(RHIAccessFlagBits::eShaderRead);
            tracker.accessMode = RHIAccessMode::eRead;
        }
        else
        {
            tracker.accessFlags.SetFlags(RHIAccessFlagBits::eShaderRead,
                                         RHIAccessFlagBits::eShaderWrite);
            tracker.accessMode = RHIAccessMode::eReadWrite;
        }
        pComputePass->resourceTrackers[srd.set][srd.binding] = std::move(tracker);
    }
    for (auto& srd : shaderProgram->GetUniformBufferSRDs())
    {
        PassResourceTracker tracker;
        tracker.name         = srd.name;
        tracker.resourceType = PassResourceType::eBuffer;
        tracker.bufferUsage  = RHIBufferUsage::eUniformBuffer;
        tracker.accessMode   = RHIAccessMode::eRead;
        tracker.accessFlags.SetFlag(RHIAccessFlagBits::eShaderRead);
        pComputePass->resourceTrackers[srd.set][srd.binding] = std::move(tracker);
    }
    for (auto& srd : shaderProgram->GetStorageBufferSRDs())
    {
        PassResourceTracker tracker;
        tracker.name         = srd.name;
        tracker.resourceType = PassResourceType::eBuffer;
        tracker.bufferUsage  = RHIBufferUsage::eStorageBuffer;
        if (!srd.writable)
        {
            tracker.accessFlags.SetFlag(RHIAccessFlagBits::eShaderRead);
            tracker.accessMode = RHIAccessMode::eRead;
        }
        else
        {
            tracker.accessFlags.SetFlags(RHIAccessFlagBits::eShaderRead,
                                         RHIAccessFlagBits::eShaderWrite);
            tracker.accessMode = RHIAccessMode::eReadWrite;
        }
        pComputePass->resourceTrackers[srd.set][srd.binding] = std::move(tracker);
    }

    for (uint32_t setIndex = 0; setIndex < pComputePass->numDescriptorSets; ++setIndex)
    {
        pComputePass->descriptorSets[setIndex] = shader->CreateDescriptorSet(setIndex);
    }

    m_renderDevice->GetRHIDebug()->SetPipelineDebugName(pComputePass->pipeline,
                                                        m_tag + "_Pipeline");

    m_renderDevice->m_computePasses.push_back(pComputePass);
    return pComputePass;
}


void ComputePassResourceUpdater::Update()
{
    // DynamicRHI* GDynamicRHI = m_renderDevice->GetRHI();
    for (const auto& kv : m_dsBindings)
    {
        const auto setIndex        = kv.first;
        const auto& bindings       = kv.second;
        RHIDescriptorSet* dsHandle = m_computePass->descriptorSets[setIndex];
        dsHandle->Update(bindings);

        for (auto& srb : bindings)
        {
            PassResourceTracker& tracker = m_computePass->resourceTrackers[setIndex][srb.binding];
            bool hasSampler              = srb.type == RHIShaderResourceType::eSamplerWithTexture ||
                srb.type == RHIShaderResourceType::eSamplerWithTextureBuffer;
            uint32_t index = 0;
            while (index < srb.resources.size())
            {
                RHIResource* resource = srb.resources[hasSampler ? index + 1 : index];
                if (tracker.resourceType == PassResourceType::eTexture)
                {
                    tracker.textures.emplace_back(dynamic_cast<RHITexture*>(resource));
                    // tracker.textureHandle = TO_TEX_HANDLE(handle);
                    // tracker.textureSubResRange =
                    //     m_renderDevice->GetTextureSubResourceRange(tracker.textureHandle);
                }
                else if (tracker.resourceType == PassResourceType::eBuffer)
                {
                    tracker.buffer = dynamic_cast<RHIBuffer*>(resource);
                }
                index = hasSampler ? index + 2 : index + 1;
            }
            // PassResourceTracker& tracker = m_computePass->resourceTrackers[setIndex][srb.binding];
            // if (srb.type == RHIShaderResourceType::eSamplerWithTexture ||
            //     srb.type == RHIShaderResourceType::eSamplerWithTextureBuffer)
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

RHIBuffer* TextureStagingManager::RequireBuffer(uint32_t requiredSize)
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

    RHIBufferCreateInfo createInfo{};
    createInfo.size = requiredSize;
    createInfo.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferSrcBuffer);
    createInfo.allocateType = RHIBufferAllocateType::eCPU;

    Entry newEntry{};
    newEntry.size      = requiredSize;
    newEntry.usedFrame = m_renderDevice->GetFramesCounter();
    newEntry.buffer    = GDynamicRHI->CreateBuffer(createInfo);

    m_usedBuffers.push_back(newEntry);

    m_allocatedBuffers.push_back(newEntry.buffer);

    m_usedMemory += requiredSize;

    return newEntry.buffer;
}

void TextureStagingManager::ReleaseBuffer(const RHIBuffer* buffer)
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
    for (RHIBuffer* buffer : m_allocatedBuffers)
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
    RHIBufferCreateInfo createInfo{};
    createInfo.size         = BUFFER_SIZE;
    createInfo.allocateType = RHIBufferAllocateType::eCPU;
    createInfo.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferSrcBuffer);

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

RenderDevice::RenderDevice(RHIAPIType APIType, uint32_t numFrames) :
    m_APIType(APIType), m_numFrames(numFrames)
{
    DynamicRHI::Create(m_APIType);
    m_RHIDebug = RHIDebug::Create();
}


void RenderDevice::Init(RHIViewport* mainViewport)
{
    m_bufferStagingMgr =
        ZEN_NEW() BufferStagingManager(this, STAGING_BLOCK_SIZE_BYTES, STAGING_POOL_SIZE_BYTES);
    m_bufferStagingMgr->Init(m_numFrames);
    m_textureStagingMgr = ZEN_NEW() TextureStagingManager(this);
    m_textureManager    = ZEN_NEW() TextureManager(this, m_textureStagingMgr);
    m_frames.reserve(m_numFrames);
    for (uint32_t i = 0; i < m_numFrames; i++)
    {
        RenderFrame frame{};
        frame.cmdListContext  = GDynamicRHI->CreateCmdListContext();
        frame.drawCmdList     = RHICommandList::Create(m_APIType, frame.cmdListContext);
        frame.transferCmdList = RHICommandList::Create(m_APIType, frame.cmdListContext);
        m_frames.emplace_back(frame);
    }
    m_framesCounter = m_frames.size();
    m_frames[m_currentFrame].transferCmdList->BeginTransferWorkload();
    m_frames[m_currentFrame].drawCmdList->BeginRenderWorkload();

    m_mainViewport = mainViewport;

    m_rendererServer = ZEN_NEW() RendererServer(this, m_mainViewport);
    m_rendererServer->Init();
}

void RenderDevice::Destroy()
{
    for (auto* viewport : m_viewports)
    {
        GDynamicRHI->DestroyViewport(viewport);
        // delete viewport;
    }
    // for (auto& kv : m_renderPassCache)
    // {
    //     GDynamicRHI->DestroyRenderPass(kv.second);
    // }
    for (auto& kv : m_pipelineCache)
    {
        GDynamicRHI->DestroyPipeline(kv.second);
    }
    for (auto& kv : m_samplerCache)
    {
        GDynamicRHI->DestroySampler(kv.second);
    }

    for (auto* pGfxPass : m_gfxPasses)
    {
        for (uint32_t i = 0; i < pGfxPass->numDescriptorSets; i++)
        {
            GDynamicRHI->DestroyDescriptorSet(pGfxPass->descriptorSets[i]);
        }
        if (pGfxPass->pRenderingLayout != nullptr)
        {
            DestroyRenderingLayout(pGfxPass->pRenderingLayout);
        }
        ZEN_DELETE(pGfxPass);
    }

    for (auto* pComputePass : m_computePasses)
    {
        for (uint32_t i = 0; i < pComputePass->numDescriptorSets; i++)
        {
            GDynamicRHI->DestroyDescriptorSet(pComputePass->descriptorSets[i]);
        }
        ZEN_DELETE(pComputePass);
    }

    for (auto& buffer : m_buffers)
    {
        GDynamicRHI->DestroyBuffer(buffer);
    }

    m_deletionQueue.Flush();

    m_bufferStagingMgr->Destroy();
    ZEN_DELETE(m_bufferStagingMgr);

    m_textureManager->Destroy();
    ZEN_DELETE(m_textureManager);

    m_textureStagingMgr->Destroy();
    ZEN_DELETE(m_textureStagingMgr);

    m_rendererServer->Destroy();
    ZEN_DELETE(m_rendererServer);

    for (uint32_t i = 0; i < m_numFrames; i++)
    {
        ProcessPendingFreeResources(i);
    }

    for (RenderFrame& frame : m_frames)
    {
        ZEN_DELETE(frame.transferCmdList);
        ZEN_DELETE(frame.drawCmdList);
    }

    ZEN_DELETE(m_RHIDebug);

    GDynamicRHI->Destroy();
    ZEN_DELETE(GDynamicRHI);
}

void RenderDevice::ExecuteFrame(RHIViewport* viewport, RenderGraph* rdg, bool present)
{
    GDynamicRHI->BeginDrawingViewport(viewport);
    rdg->Execute(m_frames[m_currentFrame].drawCmdList);
    EndFrame();
    GDynamicRHI->EndDrawingViewport(viewport, m_frames[m_currentFrame].cmdListContext, present);
}

void RenderDevice::ExecuteFrame(RHIViewport* viewport,
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

void RenderDevice::ExecuteImmediate(RHIViewport* viewport, RenderGraph* rdg)
{
    RHICommandList* cmdList = GDynamicRHI->GetImmediateCommandList();
    cmdList->BeginRenderWorkload();
    rdg->Execute(cmdList);
    cmdList->EndRenderWorkload();
    GDynamicRHI->WaitForCommandList(cmdList);
}

RHIRenderingLayout* RenderDevice::AcquireRenderingLayout()
{
    RHIRenderingLayout* pLayout = ZEN_NEW() RHIRenderingLayout();
    // static_cast<RHIRenderingLayout*>(ZEN_MEM_ALLOC(sizeof(RHIRenderingLayout)));
    // new (pLayout) RHIRenderingLayout;

    return pLayout;
}

void RenderDevice::DestroyRenderingLayout(RHIRenderingLayout* pLayout)
{
    ZEN_DELETE(pLayout);
}

RHITexture* RenderDevice::CreateTextureColorRT(const TextureFormat& texFormat,
                                               TextureUsageHint usageHint,
                                               std::string texName)
{
    RHITextureCreateInfo texInfo{};
    texInfo.type          = static_cast<RHITextureType>(texFormat.dimension);
    texInfo.format        = texFormat.format;
    texInfo.width         = texFormat.width;
    texInfo.height        = texFormat.height;
    texInfo.depth         = texFormat.depth;
    texInfo.mipmaps       = texFormat.mipmaps;
    texInfo.arrayLayers   = texFormat.arrayLayers;
    texInfo.samples       = texFormat.sampleCount;
    texInfo.mutableFormat = texFormat.mutableFormat;
    texInfo.tag           = std::move(texName);
    texInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eColorAttachment,
                                RHITextureUsageFlagBits::eSampled);

    if (usageHint.copyUsage)
    {
        texInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eTransferSrc,
                                    RHITextureUsageFlagBits::eTransferDst);
    }

    RHITexture* textureRD = GDynamicRHI->CreateTexture(texInfo);
    // RHITexture* handle    = GDynamicRHI->CreateTexture(texInfo);
    // textureRD->Init(this, handle);

    // m_textureMap[handle] = textureRD;

    return textureRD;
}

RHITexture* RenderDevice::CreateTextureDepthStencilRT(const TextureFormat& texFormat,
                                                      TextureUsageHint usageHint,
                                                      std::string texName)
{
    RHITextureCreateInfo texInfo{};
    texInfo.type          = static_cast<RHITextureType>(texFormat.dimension);
    texInfo.format        = texFormat.format;
    texInfo.width         = texFormat.width;
    texInfo.height        = texFormat.height;
    texInfo.depth         = texFormat.depth;
    texInfo.mipmaps       = texFormat.mipmaps;
    texInfo.arrayLayers   = texFormat.arrayLayers;
    texInfo.samples       = texFormat.sampleCount;
    texInfo.mutableFormat = texFormat.mutableFormat;
    texInfo.tag           = std::move(texName);
    texInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eDepthStencilAttachment,
                                RHITextureUsageFlagBits::eSampled);

    if (usageHint.copyUsage)
    {
        texInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eTransferSrc,
                                    RHITextureUsageFlagBits::eTransferDst);
    }

    RHITexture* texture = GDynamicRHI->CreateTexture(texInfo);

    return texture;
}

RHITexture* RenderDevice::CreateTextureStorage(const TextureFormat& texFormat,
                                               TextureUsageHint usageHint,
                                               std::string texName)
{
    RHITextureCreateInfo texInfo{};
    texInfo.type          = static_cast<RHITextureType>(texFormat.dimension);
    texInfo.format        = texFormat.format;
    texInfo.width         = texFormat.width;
    texInfo.height        = texFormat.height;
    texInfo.depth         = texFormat.depth;
    texInfo.mipmaps       = texFormat.mipmaps;
    texInfo.arrayLayers   = texFormat.arrayLayers;
    texInfo.samples       = texFormat.sampleCount;
    texInfo.mutableFormat = texFormat.mutableFormat;
    texInfo.tag           = std::move(texName);

    texInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage,
                                RHITextureUsageFlagBits::eSampled);

    if (usageHint.copyUsage)
    {
        texInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eTransferSrc,
                                    RHITextureUsageFlagBits::eTransferDst);
    }

    RHITexture* texture = GDynamicRHI->CreateTexture(texInfo);

    return texture;
}

RHITexture* RenderDevice::CreateTextureSampled(const TextureFormat& texFormat,
                                               TextureUsageHint usageHint,
                                               std::string texName)
{
    RHITextureCreateInfo texInfo{};
    texInfo.type          = static_cast<RHITextureType>(texFormat.dimension);
    texInfo.format        = texFormat.format;
    texInfo.width         = texFormat.width;
    texInfo.height        = texFormat.height;
    texInfo.depth         = texFormat.depth;
    texInfo.mipmaps       = texFormat.mipmaps;
    texInfo.arrayLayers   = texFormat.arrayLayers;
    texInfo.samples       = texFormat.sampleCount;
    texInfo.mutableFormat = texFormat.mutableFormat;
    texInfo.tag           = std::move(texName);
    texInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled);

    if (usageHint.copyUsage)
    {
        texInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eTransferSrc,
                                    RHITextureUsageFlagBits::eTransferDst);
    }

    RHITexture* texture = GDynamicRHI->CreateTexture(texInfo);

    return texture;
}

RHITexture* RenderDevice::CreateTextureDummy(const TextureFormat& texFormat,
                                             TextureUsageHint usageHint,
                                             std::string texName)
{
    RHITextureCreateInfo texInfo{};
    texInfo.type          = static_cast<RHITextureType>(texFormat.dimension);
    texInfo.format        = texFormat.format;
    texInfo.width         = texFormat.width;
    texInfo.height        = texFormat.height;
    texInfo.depth         = texFormat.depth;
    texInfo.mipmaps       = texFormat.mipmaps;
    texInfo.arrayLayers   = texFormat.arrayLayers;
    texInfo.samples       = texFormat.sampleCount;
    texInfo.mutableFormat = texFormat.mutableFormat;
    texInfo.tag           = std::move(texName);
    texInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled);

    if (usageHint.copyUsage)
    {
        texInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eTransferSrc,
                                    RHITextureUsageFlagBits::eTransferDst);
    }

    RHITexture* texture = GDynamicRHI->CreateTexture(texInfo);

    return texture;
}

RHITexture* RenderDevice::CreateTextureProxy(RHITexture* baseTexture,
                                             const TextureProxyFormat& proxyFormat,
                                             std::string texName)
{
    RHITextureProxyCreateInfo textureProxyInfo{};
    textureProxyInfo.type        = static_cast<RHITextureType>(proxyFormat.dimension);
    textureProxyInfo.arrayLayers = proxyFormat.arrayLayers;
    textureProxyInfo.mipmaps     = proxyFormat.mipmaps;
    textureProxyInfo.format      = proxyFormat.format;
    textureProxyInfo.tag         = std::move(texName);

    RHITexture* texture = GDynamicRHI->CreateTextureProxy(baseTexture, textureProxyInfo);

    return texture;
}

// RHITexture* RenderDevice::GetTextureRDFromHandle(const RHITexture* handle)
// {
//     return m_textureMap[handle];
// }

void RenderDevice::DestroyTexture(RHITexture* texture)
{
    // if (textureRD->IsProxy())
    // {
    //     textureRD->GetBaseTexture()->DecreaseRefCount();
    // }
    // textureRD->DecreaseRefCount();
    // m_textureMap.erase(textureRD->GetHandle());
    m_frames[m_currentFrame].texturesPendingFree.emplace_back(texture);
}

// RHITexture* RenderDevice::CreateTexture(const TextureInfo& textureInfo)
// {
//     ASSERT(!textureInfo.name.empty());
//     return m_textureManager->CreateTexture(textureInfo);
// }

// RHITexture* RenderDevice::CreateTextureProxy(const RHITexture* baseTexture,
//                                                     const TextureProxyInfo& proxyInfo)
// {
//     return m_textureManager->CreateTextureProxy(baseTexture, proxyInfo);
// }

// RHITexture* RenderDevice::GetBaseTextureForProxy(const RHITexture* handle) const
// {
//     return m_textureManager->GetBaseTextureForProxy(handle);
// }
//
// bool RenderDevice::IsProxyTexture(const RHITexture* handle) const
// {
//     return m_textureManager->IsProxyTexture(handle);
// }

void RenderDevice::GenerateTextureMipmaps(RHITexture* textureHandle, RHICommandList* cmdList)
{
    cmdList->GenerateTextureMipmaps(textureHandle);
}

RHIBuffer* RenderDevice::CreateVertexBuffer(uint32_t dataSize, const uint8_t* pData)
{
    BitField<RHIBufferUsageFlagBits> usages;
    usages.SetFlag(RHIBufferUsageFlagBits::eVertexBuffer);
    usages.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
    usages.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);

    RHIBufferCreateInfo createInfo{};
    createInfo.size         = dataSize;
    createInfo.usageFlags   = usages;
    createInfo.allocateType = RHIBufferAllocateType::eGPU;

    RHIBuffer* vertexBuffer = GDynamicRHI->CreateBuffer(createInfo);
    UpdateBufferInternal(vertexBuffer, 0, dataSize, pData);
    m_buffers.push_back(vertexBuffer);
    return vertexBuffer;
}

RHIBuffer* RenderDevice::CreateIndexBuffer(uint32_t dataSize, const uint8_t* pData)
{
    BitField<RHIBufferUsageFlagBits> usages;
    usages.SetFlag(RHIBufferUsageFlagBits::eIndexBuffer);
    usages.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
    usages.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);

    RHIBufferCreateInfo createInfo{};
    createInfo.size         = dataSize;
    createInfo.usageFlags   = usages;
    createInfo.allocateType = RHIBufferAllocateType::eGPU;

    RHIBuffer* indexBuffer = GDynamicRHI->CreateBuffer(createInfo);
    UpdateBufferInternal(indexBuffer, 0, dataSize, pData);
    m_buffers.push_back(indexBuffer);
    return indexBuffer;
}

RHIBuffer* RenderDevice::CreateUniformBuffer(uint32_t dataSize,
                                             const uint8_t* pData,
                                             std::string bufferName)
{
    BitField<RHIBufferUsageFlagBits> usages;
    usages.SetFlag(RHIBufferUsageFlagBits::eUniformBuffer);
    usages.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);

    uint32_t paddedSize = PadUniformBufferSize(dataSize);

    RHIBufferCreateInfo createInfo{};
    createInfo.size         = paddedSize;
    createInfo.usageFlags   = usages;
    createInfo.allocateType = RHIBufferAllocateType::eGPU;
    createInfo.tag          = std::move(bufferName);


    RHIBuffer* uniformBuffer = GDynamicRHI->CreateBuffer(createInfo);
    if (pData != nullptr)
    {
        UpdateBufferInternal(uniformBuffer, 0, paddedSize, pData);
    }
    m_buffers.push_back(uniformBuffer);
    return uniformBuffer;
}

RHIBuffer* RenderDevice::CreateStorageBuffer(uint32_t dataSize,
                                             const uint8_t* pData,
                                             std::string bufferName)
{
    BitField<RHIBufferUsageFlagBits> usages;
    usages.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
    usages.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);

    uint32_t paddedSize = PadStorageBufferSize(dataSize);

    RHIBufferCreateInfo createInfo{};
    createInfo.size         = paddedSize;
    createInfo.usageFlags   = usages;
    createInfo.allocateType = RHIBufferAllocateType::eGPU;
    createInfo.tag          = std::move(bufferName);

    RHIBuffer* storageBuffer = GDynamicRHI->CreateBuffer(createInfo);

    if (pData != nullptr)
    {
        UpdateBufferInternal(storageBuffer, 0, paddedSize, pData);
    }
    m_buffers.push_back(storageBuffer);
    return storageBuffer;
}

RHIBuffer* RenderDevice::CreateIndirectBuffer(uint32_t dataSize,
                                              const uint8_t* pData,
                                              std::string bufferName)
{
    BitField<RHIBufferUsageFlagBits> usages;
    usages.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
    usages.SetFlag(RHIBufferUsageFlagBits::eIndirectBuffer);
    usages.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);

    uint32_t paddedSize = PadStorageBufferSize(dataSize);

    RHIBufferCreateInfo createInfo{};
    createInfo.size         = paddedSize;
    createInfo.usageFlags   = usages;
    createInfo.allocateType = RHIBufferAllocateType::eGPU;
    createInfo.tag          = std::move(bufferName);

    RHIBuffer* indirectBuffer = GDynamicRHI->CreateBuffer(createInfo);

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

void RenderDevice::UpdateBuffer(RHIBuffer* buffer,
                                uint32_t dataSize,
                                const uint8_t* pData,
                                uint32_t offset)
{
    UpdateBufferInternal(buffer, offset, dataSize, pData);
}

void RenderDevice::DestroyBuffer(RHIBuffer* bufferHandle)
{
    GDynamicRHI->DestroyBuffer(bufferHandle);
}

// RenderPassHandle RenderDevice::GetOrCreateRenderPass(const RHIRenderPassLayout& layout)
// {
//     auto hash = CalcRenderPassLayoutHash(layout);
//     if (!m_renderPassCache.contains(hash))
//     {
//         // create new one
//         m_renderPassCache[hash] = GDynamicRHI->CreateRenderPass(layout);
//     }
//     return m_renderPassCache[hash];
// }

// RHIPipeline* RenderDevice::GetOrCreateGfxPipeline(
//     RHIGfxPipelineStates& PSO,
//     RHIShader* shader,
//     const RenderPassHandle& renderPass,
//     const HashMap<uint32_t, int>& specializationConstants)
// {
//     RHIGfxPipelineCreateInfo createInfo{};
//     createInfo.shader           = shader;
//     createInfo.states           = PSO;
//     createInfo.renderPassHandle = renderPass;
//     createInfo.subpassIdx       = 0;
//
//     auto hash = CalcGfxPipelineHash(PSO, shader, specializationConstants);
//     if (!m_pipelineCache.contains(hash))
//     {
//         // create new one
//         m_pipelineCache[hash] = GDynamicRHI->CreatePipeline(createInfo);
//     }
//     return m_pipelineCache[hash];
// }

RHIPipeline* RenderDevice::GetOrCreateGfxPipeline(
    RHIGfxPipelineStates& PSO,
    RHIShader* shader,
    const RHIRenderingLayout* pRenderingLayout,
    const HashMap<uint32_t, int>& specializationConstants)
{
    RHIGfxPipelineCreateInfo createInfo{};
    createInfo.shader           = shader;
    createInfo.states           = PSO;
    createInfo.pRenderingLayout = pRenderingLayout;
    createInfo.subpassIdx       = 0;

    auto hash = CalcGfxPipelineHash(PSO, shader, specializationConstants);
    if (!m_pipelineCache.contains(hash))
    {
        // create new one
        m_pipelineCache[hash] = GDynamicRHI->CreatePipeline(createInfo);
    }
    return m_pipelineCache[hash];
}

RHIPipeline* RenderDevice::GetOrCreateComputePipeline(RHIShader* shader)
{
    RHIComputePipelineCreateInfo createInfo{};
    createInfo.shader = shader;

    auto hash = CalcComputePipelineHash(shader);
    if (!m_pipelineCache.contains(hash))
    {
        m_pipelineCache[hash] = GDynamicRHI->CreatePipeline(createInfo);
    }
    return m_pipelineCache[hash];
}

RHIViewport* RenderDevice::CreateViewport(void* pWindow,
                                          uint32_t width,
                                          uint32_t height,
                                          bool enableVSync)
{
    // auto* viewport = GDynamicRHI->CreateViewport(pWindow, width, height, enableVSync);
    auto* viewport = GDynamicRHI->CreateViewport(pWindow, width, height, enableVSync);
    m_viewports.push_back(viewport);
    return viewport;
}

void RenderDevice::DestroyViewport(RHIViewport* viewport)
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

void RenderDevice::ResizeViewport(RHIViewport* viewport, uint32_t width, uint32_t height)
{
    if (viewport != nullptr && (viewport->GetWidth() != width || viewport->GetHeight() != height))
    {
        GDynamicRHI->SubmitAllGPUCommands();
        viewport->Resize(width, height);
        BeginFrame();
    }
}

void RenderDevice::UpdateGraphicsPassOnResize(GraphicsPass* pGfxPass, RHIViewport* viewport)
{
    RHIRenderingLayout* pRenderingLayout          = pGfxPass->pRenderingLayout;
    RHIRenderTargetLoadOp oldColorRTLoadOp        = pRenderingLayout->colorRenderTargets[0].loadOp;
    RHIRenderTargetStoreOp oldColorRTStoreOp      = pRenderingLayout->colorRenderTargets[0].storeOp;
    RHIRenderTargetLoadOp oldDepthStencilRTLoadOp = pRenderingLayout->colorRenderTargets[0].loadOp;
    RHIRenderTargetStoreOp oldDepthStencilRTStoreOp =
        pRenderingLayout->colorRenderTargets[0].storeOp;

    pRenderingLayout->ClearRenderTargetInfo();
    // pGfxPass->renderPassLayout.ClearRenderTargetInfo();
    pRenderingLayout->AddColorRenderTarget(viewport->GetSwapchainFormat(),
                                           viewport->GetColorBackBuffer(), oldColorRTLoadOp,
                                           oldColorRTStoreOp);


    pRenderingLayout->AddDepthStencilRenderTarget(
        viewport->GetDepthStencilFormat(), viewport->GetDepthStencilBackBuffer(),
        oldDepthStencilRTLoadOp, oldDepthStencilRTStoreOp);

    pRenderingLayout->SetRenderArea(0, 0, viewport->GetWidth(), viewport->GetHeight());
}

RHISampler* RenderDevice::CreateSampler(const RHISamplerCreateInfo& samplerInfo)
{
    const size_t samplerHash = CalcSamplerHash(samplerInfo);
    if (!m_samplerCache.contains(samplerHash))
    {
        m_samplerCache[samplerHash] = GDynamicRHI->CreateSampler(samplerInfo);
    }

    return m_samplerCache[samplerHash];
}

// RHITextureSubResourceRange RenderDevice::GetTextureSubResourceRange(RHITexture* handle)
// {
//     return GDynamicRHI->GetTextureSubResourceRange(handle);
// }

RHITexture* RenderDevice::LoadTexture2D(const std::string& file, bool requireMipmap)
{
    return m_textureManager->LoadTexture2D(file, requireMipmap);
}

void RenderDevice::LoadSceneTextures(const sg::Scene* scene, std::vector<RHITexture*>& outTextures)
{
    m_textureManager->LoadSceneTextures(scene, outTextures);
}

void RenderDevice::LoadTextureEnv(const std::string& file, EnvTexture* texture)
{
    auto fullPath = ZEN_TEXTURE_PATH + file;
    m_textureManager->LoadTextureEnv(fullPath, texture);
}

void RenderDevice::UpdateTextureOneTime(RHITexture* textureHandle,
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
    RHIBufferTextureCopyRegion copyRegion{};
    copyRegion.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    copyRegion.bufferOffset  = submitResult.writeOffset;
    copyRegion.textureOffset = {0, 0, 0};
    copyRegion.textureSize   = textureSize;
    m_frames[m_currentFrame].transferCmdList->CopyBufferToTexture(submitResult.buffer,
                                                                  textureHandle, copyRegion);

    m_bufferStagingMgr->EndSubmit(&submitResult);
}

void RenderDevice::UpdateTextureBatch(RHITexture* textureHandle,
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
                RHIBufferTextureCopyRegion copyRegion{};
                copyRegion.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
                copyRegion.bufferOffset  = submitResult.writeOffset;
                copyRegion.textureOffset = {x, y, z};
                copyRegion.textureSize   = {regionWidth, regionHeight, 1};
                m_frames[m_currentFrame].transferCmdList->CopyBufferToTexture(
                    submitResult.buffer, textureHandle, copyRegion);

                m_bufferStagingMgr->EndSubmit(&submitResult);
            }
        }
    }
}

void RenderDevice::UpdateBufferInternal(RHIBuffer* bufferHandle,
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
        RHIBufferCopyRegion copyRegion;
        copyRegion.srcOffset = submitResult.writeOffset;
        copyRegion.dstOffset = writePosition + offset;
        copyRegion.size      = submitResult.writeSize;
        m_frames[m_currentFrame].transferCmdList->CopyBuffer(submitResult.buffer, bufferHandle,
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
            GDynamicRHI->WaitForCommandList(m_frames[i].transferCmdList);
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
    // for (RHITexture* texture : GetCurrentFrame()->texturesPendingFree)
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
    m_frames[m_currentFrame].transferCmdList->BeginTransferWorkload();
    m_frames[m_currentFrame].drawCmdList->BeginRenderWorkload();
}

void RenderDevice::EndFrame()
{
    m_frames[m_currentFrame].transferCmdList->EndTransferWorkload();
    m_frames[m_currentFrame].drawCmdList->EndRenderWorkload();
    m_frames[m_currentFrame].cmdSubmitted = true;
}

void RenderDevice::ProcessPendingFreeResources(uint32_t frameIndex)
{
    for (RHITexture* texture : m_frames[frameIndex].texturesPendingFree)
    {
        texture->ReleaseReference();
    }
    m_frames[frameIndex].texturesPendingFree.clear();
}

// todo: consider attachment size when calculating hash
size_t RenderDevice::CalcRenderPassLayoutHash(const RHIRenderPassLayout& layout)
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
        // Assuming RHIRenderTarget is hashable
        combineHash(rt.format);
        combineHash(rt.numSamples);
        combineHash(rt.loadOp);
        combineHash(rt.storeOp);
        // combineHash(rt.usage);
    }
    if (layout.HasDepthStencilRenderTarget())
    {
        // Hash depth stencil render target (assuming RHIRenderTarget is hashable)
        combineHash(layout.GetDepthStencilRenderTarget().format);
    }

    // combineHash(layout.GetDepthStencilRenderTarget().usage);

    return seed;
}

// size_t RenderDevice::CalcFramebufferHash(const RHIFramebufferInfo& info,
//                                          RenderPassHandle renderPassHandle)
// {
//     std::size_t seed = 0;
//
//     // Hash each member and combine the result
//     std::hash<uint32_t> uint32Hasher;
//     std::hash<uint64_t> uint64Hasher;
//
//     // Hash the individual members
//     seed ^= uint32Hasher(info.numRenderTarget) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
//     seed ^= uint32Hasher(info.width) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
//     seed ^= uint32Hasher(info.height) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
//     seed ^= uint32Hasher(info.depth) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
//     seed ^= uint64Hasher(renderPassHandle.value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
//     for (uint32_t i = 0; i < info.numRenderTarget; i++)
//     {
//         seed ^= uint64Hasher(info.renderTargets[i]->GetHash32()) + 0x9e3779b9 + (seed << 6) +
//             (seed >> 2);
//     }
//     return seed;
// }

// size_t RenderDevice::CalcGfxPipelineHash(const RHIGfxPipelineStates& pso,
//                                          RHIShader* shader,
//                                          const RenderPassHandle& renderPass,
//                                          const HashMap<uint32_t, int>& specializationConstants)
// {
//     std::size_t seed = 0;
//     // Hashing utility
//     auto combineHash = [&seed](auto&& value) {
//         seed ^= std::hash<std::decay_t<decltype(value)>>{}(value) + 0x9e3779b9 + (seed << 6) +
//             (seed >> 2);
//     };
//     combineHash(shader->GetHash32());
//     combineHash(renderPass.value);
//     combineHash(pso.primitiveType);
//     for (auto& kv : specializationConstants)
//     {
//         combineHash(kv.first);
//         combineHash(kv.second);
//     }
//     return seed;
// }

size_t RenderDevice::CalcGfxPipelineHash(const RHIGfxPipelineStates& pso,
                                         RHIShader* shader,
                                         // const RHIRenderPassLayout& renderPassLayout,
                                         const HashMap<uint32_t, int>& specializationConstants)
{
    std::size_t seed = 0;
    // Hashing utility
    auto combineHash = [&seed](auto&& value) {
        seed ^= std::hash<std::decay_t<decltype(value)>>{}(value) + 0x9e3779b9 + (seed << 6) +
            (seed >> 2);
    };
    combineHash(shader->GetHash32());
    // for (uint32_t i = 0; i < renderPassLayout.GetNumRenderTargets(); i++)
    // for (auto& colorRT : renderPassLayout.GetColorRenderTargets())
    // {
    //     combineHash(colorRT.texture);
    // }
    // if (renderPassLayout.HasDepthStencilRenderTarget())
    // {
    //     combineHash(renderPassLayout.GetDepthStencilRenderTarget().texture);
    // }
    combineHash(pso.primitiveType);
    for (auto& kv : specializationConstants)
    {
        combineHash(kv.first);
        combineHash(kv.second);
    }
    return seed;
}

size_t RenderDevice::CalcComputePipelineHash(RHIShader* shader)
{
    std::size_t seed = 0;
    // Hashing utility
    auto combineHash = [&seed](auto&& value) {
        seed ^= std::hash<std::decay_t<decltype(value)>>{}(value) + 0x9e3779b9 + (seed << 6) +
            (seed >> 2);
    };
    combineHash(shader->GetHash32());
    return seed;
}

size_t RenderDevice::CalcSamplerHash(const RHISamplerCreateInfo& info)
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