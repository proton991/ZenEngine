#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/RHI/ShaderUtil.h"
#include "Graphics/RenderCore/V2/RenderGraph.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Graphics/VulkanRHI/VulkanCommands.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "stb_image.h"
#include "Graphics/VulkanRHI/VulkanDebug.h"
#include "SceneGraph/Scene.h"

#include <fstream>
#include <execution>

namespace zen::rc
{
std::vector<uint8_t> LoadSpirvCode(const std::string& name)
{
    const auto path = std::string(SPV_SHADER_PATH) + name;
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    bool opened = file.is_open();
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

GraphicsPass GraphicsPassBuilder::Build()
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

    // set specialization constants
    for (auto& spc : shaderGroupInfo.specializationConstants)
    {
        switch (spc.type)
        {

            case ShaderSpecializationConstantType::eBool:
                spc.boolValue = static_cast<bool>(m_specializationConstants[spc.constantId]);
                break;
            case ShaderSpecializationConstantType::eInt:
                spc.intValue = m_specializationConstants[spc.constantId];
                break;
            case ShaderSpecializationConstantType::eFloat:
                spc.floatValue = static_cast<float>(m_specializationConstants[spc.constantId]);
                break;
            default: break;
        }
    }
    ShaderHandle shader = RHI->CreateShader(shaderGroupInfo);

    m_framebufferInfo.renderTargets = m_rpLayout.GetRenderTargetHandles();

    GraphicsPass gfxPass;
    gfxPass.renderPass = m_renderDevice->GetOrCreateRenderPass(m_rpLayout);
    gfxPass.framebuffer =
        m_viewport->GetCompatibleFramebuffer(gfxPass.renderPass, &m_framebufferInfo);
    gfxPass.pipeline = m_renderDevice->GetOrCreateGfxPipeline(
        m_PSO, shader, gfxPass.renderPass, shaderGroupInfo.specializationConstants);

    m_renderDevice->GetRHIDebug()->SetPipelineDebugName(gfxPass.pipeline, m_tag + "_Pipeline");
    m_renderDevice->GetRHIDebug()->SetRenderPassDebugName(gfxPass.renderPass,
                                                          m_tag + "_RenderPass");

    gfxPass.descriptorSets.resize(shaderGroupInfo.SRDs.size());
    // parallel build descriptor building process
    //    std::for_each(std::execution::par, m_dsBindings.begin(), m_dsBindings.end(),
    //                  [&](const auto& kv) {
    //                      const auto setIndex  = kv.first;
    //                      const auto& bindings = kv.second;
    //                      // create and update
    //                      DescriptorSetHandle dsHandle = RHI->CreateDescriptorSet(shader, setIndex);
    //                      RHI->UpdateDescriptorSet(dsHandle, bindings);
    //                      gfxPass.descriptorSets[setIndex] = dsHandle;
    //                  });
    for (const auto& kv : m_dsBindings)
    {
        const auto setIndex          = kv.first;
        const auto& bindings         = kv.second;
        DescriptorSetHandle dsHandle = RHI->CreateDescriptorSet(shader, setIndex);
        RHI->UpdateDescriptorSet(dsHandle, bindings);
        gfxPass.descriptorSets[setIndex] = dsHandle;
    }

    RHI->DestroyShader(shader);
    m_renderDevice->m_gfxPasses.push_back(gfxPass);
    return gfxPass;
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

void RenderDevice::Init()
{
    if (m_APIType == rhi::GraphicsAPIType::eVulkan)
    {
        m_RHI = new rhi::VulkanRHI();
        m_RHI->Init();

        m_RHIDebug = new rhi::VulkanDebug(m_RHI);
        m_bufferStagingMgr =
            new BufferStagingManager(this, STAGING_BLOCK_SIZE_BYTES, STAGING_POOL_SIZE_BYTES);
        m_bufferStagingMgr->Init(m_numFrames);
        m_textureStagingMgr = new TextureStagingManager(this);
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
        m_framesCounter = m_frames.size();
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

    for (auto& rp : m_gfxPasses)
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
    for (auto& kv : m_textureCache)
    {
        m_RHI->DestroyTexture(kv.second);
    }
    m_deletionQueue.Flush();

    m_bufferStagingMgr->Destroy();
    delete m_bufferStagingMgr;

    m_textureStagingMgr->Destroy();
    delete m_textureStagingMgr;

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

rhi::TextureHandle RenderDevice::CreateTexture(const rhi::TextureInfo& textureInfo,
                                               const std::string& tag)
{
    if (!m_textureCache.contains(tag))
    {
        m_textureCache[tag] = m_RHI->CreateTexture(textureInfo);
    }
    return m_textureCache.at(tag);
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

size_t RenderDevice::PadUniformBufferSize(size_t originalSize)
{
    auto minUboAlignment = m_RHI->GetUniformBufferAlignment();
    size_t alignedSize   = (originalSize + minUboAlignment - 1) & ~(minUboAlignment - 1);
    return alignedSize;
}

size_t RenderDevice::PadStorageBufferSize(size_t originalSize)
{
    auto minAlignment  = m_RHI->GetStorageBufferAlignment();
    size_t alignedSize = (originalSize + minAlignment - 1) & ~(minAlignment - 1);
    return alignedSize;
}

void RenderDevice::UpdateBuffer(rhi::BufferHandle bufferHandle,
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
        if (rhi::RHIOptions::GetInstance().ReuseSwapChainOnResize())
        {
            m_RHI->SubmitAllGPUCommands();
            (*viewport)->Resize(width, height);
            BeginFrame();
        }
        else
        {
            DestroyViewport(*viewport);
            *viewport = CreateViewport(window, width, height);
        }
    }
}


rhi::SamplerHandle RenderDevice::CreateSampler(const rhi::SamplerInfo& samplerInfo)
{
    rhi::SamplerHandle sampler = m_RHI->CreateSampler(samplerInfo);
    m_deletionQueue.Enqueue([=, this]() { m_RHI->DestroySampler(sampler); });
    return sampler;
}

rhi::TextureSubResourceRange RenderDevice::GetTextureSubResourceRange(rhi::TextureHandle handle)
{
    return m_RHI->GetTextureSubResourceRange(handle);
}

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

rhi::TextureHandle RenderDevice::RequestTexture2D(const std::string& file, bool requireMipmap)
{
    if (m_textureCache.contains(file))
    {
        return m_textureCache[file];
    }
    // TODO: Refactor AssetLib
    const std::string filepath = ZEN_TEXTURE_PATH + file;
    int width = 0, height = 0, channels = 0;
    stbi_set_flip_vertically_on_load(true);
    uint8_t* data = stbi_load(filepath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    stbi_set_flip_vertically_on_load(false);

    std::vector<uint8_t> vecData;
    // ONLY support RGBA format
    vecData.resize(width * height * 4 * sizeof(uint8_t));
    std::copy(data, data + vecData.size(), vecData.begin());
    stbi_image_free(data);

    rhi::TextureInfo textureInfo{};
    textureInfo.format      = rhi::DataFormat::eR8G8B8A8SRGB;
    textureInfo.type        = rhi::TextureType::e2D;
    textureInfo.width       = width;
    textureInfo.height      = height;
    textureInfo.depth       = 1;
    textureInfo.arrayLayers = 1;
    textureInfo.mipmaps     = 1;
    textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eTransferDst);
    textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eSampled);

    rhi::TextureHandle texture = m_RHI->CreateTexture(textureInfo);

    if (requireMipmap)
    {
        // TODO: generate mipmaps
    }
    else
    {
        UpdateTexture(texture, {textureInfo.width, textureInfo.height, textureInfo.depth},
                      vecData.size(), vecData.data());
    }

    m_textureCache[file] = texture;
    return texture;
}

void RenderDevice::RegisterSceneTextures(const sg::Scene* scene,
                                         std::vector<rhi::TextureHandle>& outTextures)
{
    std::vector<sg::Texture*> sgTextures = scene->GetComponents<sg::Texture>();
    for (sg::Texture* sgTexture : sgTextures)
    {
        if (!m_textureCache.contains(sgTexture->GetName()))
        {
            rhi::TextureInfo textureInfo{};
            textureInfo.format      = rhi::DataFormat::eR8G8B8A8SRGB;
            textureInfo.type        = rhi::TextureType::e2D;
            textureInfo.width       = sgTexture->width;
            textureInfo.height      = sgTexture->height;
            textureInfo.depth       = 1;
            textureInfo.arrayLayers = 1;
            textureInfo.mipmaps     = 1;
            textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eTransferDst);
            textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eSampled);

            rhi::TextureHandle texture = m_RHI->CreateTexture(textureInfo);

            UpdateTexture(texture, {textureInfo.width, textureInfo.height, textureInfo.depth},
                          sgTexture->bytesData.size(), sgTexture->bytesData.data());

            m_textureCache[sgTexture->GetName()] = texture;
        }
        outTextures.push_back(m_textureCache[sgTexture->GetName()]);
    }
}

void RenderDevice::UpdateTexture(rhi::TextureHandle textureHandle,
                                 const Vec3i& textureSize,
                                 uint32_t dataSize,
                                 const uint8_t* pData)
{
    // transfer layout to eTransferDst
    m_RHI->ChangeTextureLayout(m_frames[m_currentFrame].uploadCmdList, textureHandle,
                               rhi::TextureLayout::eUndefined, rhi::TextureLayout::eTransferDst);

    rhi::BufferHandle stagingBuffer = m_textureStagingMgr->RequireBuffer(dataSize);
    // map staging buffer
    uint8_t* dataPtr = m_RHI->MapBuffer(stagingBuffer);
    // copy
    memcpy(dataPtr, pData, dataSize);
    // unmap
    m_RHI->UnmapBuffer(stagingBuffer);
    // copy to gpu memory
    rhi::BufferTextureCopyRegion copyRegion{};
    copyRegion.textureSubresources.aspect.SetFlag(rhi::TextureAspectFlagBits::eColor);
    copyRegion.bufferOffset  = 0;
    copyRegion.textureOffset = {0, 0, 0};
    copyRegion.textureSize   = textureSize;
    m_frames[m_currentFrame].uploadCmdList->CopyBufferToTexture(stagingBuffer, textureHandle,
                                                                copyRegion);
    m_textureStagingMgr->ReleaseBuffer(stagingBuffer);

    // transfer layout to eShaderReadOnly
    m_RHI->ChangeTextureLayout(m_frames[m_currentFrame].uploadCmdList, textureHandle,
                               rhi::TextureLayout::eTransferDst,
                               rhi::TextureLayout::eShaderReadOnly);

    if (m_textureStagingMgr->GetPendingFreeMemorySize() > MAX_TEXTURE_STAGING_PENDING_FREE_SIZE)
    {
        WaitForAllFrames();
    }
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
    m_textureStagingMgr->ProcessPendingFrees();
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
} // namespace zen::rc