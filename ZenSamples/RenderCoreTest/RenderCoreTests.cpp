#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RHI/RHIShaderUtil.h"
#include "Platform/FileSystem.h"
#include "Graphics/RenderCore/V2/TextureManager.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "SceneGraph/Camera.h"
#include "SceneGraph/Scene.h"
#include "AssetLib/TextureLoader.h"
#include <gli/gli.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <array>
#include <cstring>
#include <cstdio>
#include <memory>
#include <random>
#include <numeric>
#include <string_view>
#include <sstream>
#include <unordered_set>
#include <unordered_map>

using namespace zen;
using namespace zen::rc;

namespace
{
std::unordered_set<uint64_t> destroyed;
std::unordered_map<NameID, RHIShaderCreateInfo> reflectedShaderInfos;

ShaderProgram* CreateTestShaderProgram(RenderDevice* device, NameID name)
{
    ShaderProgram* program = ShaderProgramManager::GetInstance().CreateShaderProgram(device, name);
    EXPECT_TRUE(program->Init());

    return program;
}

std::unordered_map<std::string, asset::TextureInfo> textureFiles;

// Mutable asset facade used with the real renderer/graph implementations below.
struct SceneInputs
{
    RHIBuffer* vertices{};
    RHIBuffer* indices{};
    RHIBuffer* nodes{};
    RHIBuffer* materials{};
    std::vector<RHITexture*> textures;
    EnvTexture environment;
    sg::CameraUniformData camera{};
    SceneUniformData uniforms{};
} sceneInputs;

class TestBuffer : public RHIBuffer
{
public:
    explicit TestBuffer(const RHIBufferCreateInfo& info) : RHIBuffer(info), bytes(info.size, 0xCD)
    {}

    uint8_t* Map() override
    {
        return bytes.data();
    }

    void Unmap() override {}

    void SetTexelFormat(DataFormat) override {}

    std::vector<uint8_t> bytes;

private:
    void Init() override {}

    void Destroy() override
    {
        destroyed.insert(GetStableId());
        ZEN_DELETE(this);
    }
};

class TestTextureView : public RHITextureView
{
public:
    TestTextureView(RHITexture* texture, const RHITextureViewCreateInfo& info) :
        RHITextureView(texture, info), m_textureIdentity(texture->GetStableId())
    {}

private:
    uint64_t m_textureIdentity;

    void Init() override {}

    void Destroy() override
    {
        EXPECT_FALSE(destroyed.contains(m_textureIdentity));
        destroyed.insert(GetStableId());
        ZEN_DELETE(this);
    }
};

class TestSampler : public RHISampler
{
public:
    explicit TestSampler(const RHISamplerCreateInfo& info) : RHISampler(info) {}

private:
    void Init() override {}

    void Destroy() override
    {
        destroyed.insert(GetStableId());
        ZEN_DELETE(this);
    }
};

class TestTexture : public RHITexture
{
public:
    explicit TestTexture(const RHITextureCreateInfo& info) : RHITexture(info)
    {
        RHITextureViewCreateInfo view{};
        view.format      = info.format;
        view.type        = info.type;
        view.mipLevels   = info.mipmaps;
        view.arrayLayers = info.arrayLayers;
        m_pDefaultView   = CreateView(view);
    }

    RHITextureView* CreateView(const RHITextureViewCreateInfo& info) override
    {
        TestTextureView* view = ZEN_NEW() TestTextureView(this, info);
        RegisterOwnedView(view);

        return view;
    }

private:
    void Init() override {}

    void Destroy() override
    {
        DestroyOwnedViews();
        destroyed.insert(GetStableId());
        ZEN_DELETE(this);
    }
};

class TestViewport : public RHIViewport
{
public:
    TestViewport() : RHIViewport(nullptr, 8, 8, false) {}

    ~TestViewport() override
    {
        ReleaseReference();
    }

    RHITexture* color{};
    RHITexture* depth{};
    uint32_t preparePresents{0};
    uint32_t presents{0};
    bool presentResult{true};

    void PrepareForPresent(RHICommandList*) override
    {
        ++preparePresents;
    }

    bool Present() override
    {
        ++presents;
        return presentResult;
    }

    uint32_t GetWidth() const override
    {
        return m_width;
    }

    uint32_t GetHeight() const override
    {
        return m_height;
    }

    DataFormat GetSwapchainFormat() override
    {
        return color->GetFormat();
    }

    DataFormat GetDepthStencilFormat() override
    {
        return depth->GetFormat();
    }

    RHITexture* GetColorBackBuffer() override
    {
        return color;
    }

    RHITexture* GetDepthStencilBackBuffer() override
    {
        return depth;
    }

    RHITextureSubResourceRange GetColorBackBufferRange() override
    {
        return color->GetSubResourceRange();
    }

    RHITextureSubResourceRange GetDepthStencilBackBufferRange() override
    {
        return depth->GetSubResourceRange();
    }

    void Resize(uint32_t width, uint32_t height) override
    {
        m_width  = width;
        m_height = height;
    }

private:
    void Init() override {}

    void Destroy() override {}
};

class TestShader : public RHIShader
{
public:
    void SetBenchmarkSPIRV(size_t size)
    {
        HeapVector<uint8_t> bytes(size);

        for (size_t i = 0; i < size; ++i)
        {
            bytes[i] = uint8_t(i * 37);
        }

        m_shaderGroupSPIRV->SetStageSPIRV(RHIShaderStage::eCompute, std::move(bytes));
    }

    void EnableGeometryStage()
    {
        m_shaderStageFlags.SetFlag(RHIShaderStageFlagBits::eGeometry);
    }

    void SetDescriptorStages(NameID name, RHIShaderStageFlagBits stage)
    {
        for (RHIShaderResourceDescriptor& descriptor : m_SRDTable[0])
        {
            if (descriptor.name == name)
            {
                descriptor.stageFlags = int64_t(stage);
            }
        }
    }

    void SetDescriptorArray(NameID name, uint32_t count, uint32_t blockSize = 0)
    {
        for (RHIShaderResourceDescriptor& descriptor : m_SRDTable[0])
        {
            if (descriptor.name == name)
            {
                descriptor.arraySize = count;
                descriptor.blockSize = blockSize;
            }
        }
    }

    void SetDescriptorReadOnly(NameID name)
    {
        for (RHIShaderResourceDescriptor& descriptor : m_SRDTable[0])
        {
            if (descriptor.name == name)
            {
                descriptor.writable = false;
            }
        }
    }

    explicit TestShader(const RHIShaderCreateInfo& info) : RHIShader(info)
    {
        if (!info.stageFlags.IsEmpty())
        {
            RefCountPtr<RHIShaderGroupSPIRV> spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

            for (uint32_t i = 0; i < ToUnderlying(RHIShaderStage::eMax); ++i)
            {
                const RHIShaderStage stage = static_cast<RHIShaderStage>(i);

                if (info.stageFlags.HasFlag(RHIShaderStageToFlagBits(stage)))
                {
                    spirv->SetStageSPIRV(stage,
                                         platform::FileSystem::LoadSpvFile(info.spirvFileName[i]));
                }
            }

            RHIShaderGroupInfo reflected{};
            RHIShaderUtil::ReflectShaderGroupInfo(spirv, reflected);
            m_SRDTable = std::move(reflected.SRDTable);

            for (SmallVector<RHIShaderResourceDescriptor> const& set : m_SRDTable)
            {
                for (RHIShaderResourceDescriptor const& descriptor : set)
                {
                    m_namedSRDLut[descriptor.name] = &descriptor;
                }
            }

            return;
        }

        m_SRDTable.resize(1);

        for (const std::pair<NameID, RHIShaderResourceType>& binding :
             std::vector<std::pair<NameID, RHIShaderResourceType>>{
                 {"buffer", RHIShaderResourceType::eStorageBuffer},
                 {"texture", RHIShaderResourceType::eSamplerWithTexture},
                 {"uTextureArray", RHIShaderResourceType::eSamplerWithTexture},
                 {"uTexture2DHeap", RHIShaderResourceType::eTexture},
                 {"uSamplerHeap", RHIShaderResourceType::eSampler},
                 {"image", RHIShaderResourceType::eImage},
                 {"value", RHIShaderResourceType::eUniformBuffer},
                 {"read_buffer", RHIShaderResourceType::eStorageBuffer},
                 {"write_buffer", RHIShaderResourceType::eStorageBuffer},
                 {"write_image", RHIShaderResourceType::eImage}})
        {
            RHIShaderResourceDescriptor descriptor{};
            descriptor.name      = binding.first;
            descriptor.type      = binding.second;
            descriptor.binding   = static_cast<uint32_t>(m_SRDTable[0].size());
            descriptor.arraySize = descriptor.name == NameID("uTextureArray") ? 1024 : 1;
            descriptor.bindless  = descriptor.name == NameID("uTexture2DHeap") ||
                descriptor.name == NameID("uSamplerHeap");
            descriptor.writable  = binding.second == RHIShaderResourceType::eImage ||
                binding.second == RHIShaderResourceType::eStorageBuffer;

            if (descriptor.name == NameID("read_buffer"))
            {
                descriptor.writable = false;
            }

            if (descriptor.name == NameID("write_buffer") ||
                descriptor.name == NameID("write_image"))
            {
                descriptor.readable = false;
            }

            m_SRDTable[0].push_back(descriptor);
        }

        for (RHIShaderResourceDescriptor const& descriptor : m_SRDTable[0])
        {
            m_namedSRDLut[descriptor.name] = &descriptor;
        }
    }

private:
    void Init() override {}

    void Destroy() override
    {
        destroyed.insert(GetStableId());
        ZEN_DELETE(this);
    }
};

class TestPipeline : public RHIPipeline
{
public:
    explicit TestPipeline(const RHIGfxPipelineCreateInfo& info) : RHIPipeline(info) {}

    explicit TestPipeline(const RHIComputePipelineCreateInfo& info) : RHIPipeline(info) {}

private:
    void Init() override {}

    void Destroy() override
    {
        destroyed.insert(GetStableId());
        ZEN_DELETE(this);
    }
};

class TestContext : public IRHICommandContext
{
public:
    explicit TestContext(RHICommandContextType type) : type(type) {}

    RHICommandContextType type;
    std::function<void()> wait;
    uint32_t proxyDestructions{0};
    std::thread::id lastProxyDestroyThread;
    struct BarrierBatch
    {
        BitField<RHIPipelineStageFlagBits> source;
        BitField<RHIPipelineStageFlagBits> destination;
        std::vector<RHIBufferTransition> buffers;
        std::vector<RHITextureTransition> textures;
    };
    std::vector<BarrierBatch> barrierBatches;
    struct TextureClear
    {
        RHITexture* texture;
        Color color;
        RHITextureSubResourceRange range;
    };
    std::vector<TextureClear> textureClears;
    std::vector<size_t> clearsBeforeDraw;
    std::vector<size_t> clearsBeforeDispatch;
    std::vector<RHIBufferTransition> bufferTransitions;
    std::vector<RHITextureTransition> textureTransitions;
    std::vector<BitField<RHIPipelineStageFlagBits>> barrierSources;
    std::vector<RHIMemoryTransition> memoryBarriers;
    std::vector<RHIBufferTextureCopyRegion> textureCopies;
    std::vector<RHITextureCopyRegion> imageCopies;
    std::vector<RHITextureBlitRegion> blits;
    std::vector<RHIBufferCopyRegion> bufferCopies;
    std::vector<std::vector<uint8_t>> values;
    std::vector<std::vector<uint8_t>> pushConstants;
    std::vector<RHIResource*> boundResources;
    std::vector<uint32_t> boundArrayIndices;
    std::vector<RHIBuffer*> indirectDraws;
    std::vector<RHIBuffer*> indirectDispatches;
    uint32_t drawCount{0};
    uint32_t dispatchCount{0};
    uint32_t renderingCount{0};
    uint32_t vertexBufferCount{0};
    std::vector<RHIBuffer*> vertexBuffers;
    std::vector<RHIRenderingLayout> renderingLayouts;
    std::vector<std::pair<size_t, size_t>> renderingCommandOffsets;
    std::vector<std::array<uint32_t, 2>> indexedDraws;

    RHICommandContextType GetContextType() override
    {
        return type;
    }

    void RHIBeginRendering(const RHIRenderingLayout* pRenderingLayout) override
    {
        EXPECT_NE(pRenderingLayout, nullptr);
        renderingLayouts.push_back(*pRenderingLayout);
        renderingCommandOffsets.emplace_back(pushConstants.size(), indexedDraws.size());
        ++renderingCount;
    }

    void RHIEndRendering() override {}

    void RHISetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) override {}

    void RHISetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) override {}

    void RHISetDepthBias(float depthBiasConstantFactor,
                         float depthBiasClamp,
                         float depthBiasSlopeFactor) override
    {}

    void RHISetLineWidth(float lineWidth) override {}

    void RHISetBlendConstants(const Color& blendConstants) override {}

    void RHIBindPipeline(RHIPipeline* pPipeline) override {}

    void RHISetShaderParameters(const RHIBatchedShaderParameters& parameters) override
    {
        for (const RHIShaderValueParameter& value : parameters.GetValueParams())
        {
            VectorView<const uint8_t> bytes = parameters.GetValueBytes(value);
            values.emplace_back(bytes.begin(), bytes.end());
        }

        for (const RHIShaderResourceParameter& resource : parameters.GetResourceParams())
        {
            boundResources.push_back(resource.pResource);
            boundArrayIndices.push_back(resource.arrayIndex);
        }

        for (const RHIShaderResourceParameter& resource : parameters.GetBindlessParams())
        {
            boundResources.push_back(resource.pResource);
            boundArrayIndices.push_back(resource.arrayIndex);
        }
    }

    void RHIBindVertexBuffers(VectorView<RHIBuffer*> pBuffers,
                              VectorView<uint64_t> offsets) override
    {
        vertexBufferCount = static_cast<uint32_t>(pBuffers.size());
        vertexBuffers.insert(vertexBuffers.end(), pBuffers.begin(), pBuffers.end());
    }

    void RHIBindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset) override {}

    void RHIDraw(uint32_t vertexCount,
                 uint32_t instanceCount,
                 uint32_t firstVertex,
                 uint32_t firstInstance) override
    {
        clearsBeforeDraw.push_back(textureClears.size());
        ++drawCount;
    }

    void RHIDrawIndexed(RHIBuffer* pIndexBuffer,
                        DataFormat indexFormat,
                        uint32_t indexBufferOffset,
                        uint32_t indexCount,
                        uint32_t instanceCount,
                        uint32_t firstIndex,
                        int32_t vertexOffset,
                        uint32_t firstInstance) override
    {
        indexedDraws.push_back({firstIndex, indexCount});
    }

    void RHIDrawIndexedIndirect(RHIBuffer* pIndirectBuffer,
                                RHIBuffer* pIndexBuffer,
                                DataFormat indexFormat,
                                uint32_t indexBufferOffset,
                                uint32_t offset,
                                uint32_t drawCount,
                                uint32_t stride) override
    {
        indirectDraws.push_back(pIndirectBuffer);
    }

    void RHIDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override
    {
        clearsBeforeDispatch.push_back(textureClears.size());
        ++dispatchCount;
    }

    void RHIDispatchIndirect(RHIBuffer* pIndirectBuffer, uint32_t offset) override
    {
        indirectDispatches.push_back(pIndirectBuffer);
    }

    void RHISetPushConstants(RHIPipeline* pPipeline,
                             VectorView<const uint8_t> data,
                             uint32_t offset = 0) override
    {
        pushConstants.emplace_back(data.begin(), data.end());
    }

    void RHIAddTransitions(BitField<RHIPipelineStageFlagBits> srcStages,
                           BitField<RHIPipelineStageFlagBits> dstStages,
                           VectorView<RHIMemoryTransition> memoryTransitions,
                           VectorView<RHIBufferTransition> bufferTransitionsView,
                           VectorView<RHITextureTransition> textureTransitionsView) override
    {
        barrierBatches.push_back({srcStages,
                                  dstStages,
                                  {bufferTransitionsView.begin(), bufferTransitionsView.end()},
                                  {textureTransitionsView.begin(), textureTransitionsView.end()}});
        barrierSources.push_back(srcStages);
        memoryBarriers.insert(memoryBarriers.end(), memoryTransitions.begin(),
                              memoryTransitions.end());
        bufferTransitions.insert(bufferTransitions.end(), bufferTransitionsView.begin(),
                                 bufferTransitionsView.end());
        textureTransitions.insert(textureTransitions.end(), textureTransitionsView.begin(),
                                  textureTransitionsView.end());
    }

    void RHIAddTextureTransition(RHITexture* pTexture, RHITextureLayout newLayout) override {}

    void RHIClearBuffer(RHIBuffer* pBuffer, uint32_t offset, uint32_t size) override {}

    void RHICopyBuffer(RHIBuffer* pSrcBuffer,
                       RHIBuffer* pDstBuffer,
                       const RHIBufferCopyRegion& region) override
    {
        TestBuffer* source      = static_cast<TestBuffer*>(pSrcBuffer);
        TestBuffer* destination = static_cast<TestBuffer*>(pDstBuffer);
        EXPECT_LE(region.srcOffset + region.size, source->bytes.size());
        EXPECT_LE(region.dstOffset + region.size, destination->bytes.size());
        std::memcpy(destination->bytes.data() + region.dstOffset,
                    source->bytes.data() + region.srcOffset, region.size);
        bufferCopies.push_back(region);
    }

    void RHIClearTexture(RHITexture* pTex,
                         const Color& color,
                         const RHITextureSubResourceRange& range) override
    {
        textureClears.push_back({pTex, color, range});
    }

    void RHICopyTexture(RHITexture* pSrcTexture,
                        RHITexture* pDstTexture,
                        VectorView<RHITextureCopyRegion> regions) override
    {
        imageCopies.insert(imageCopies.end(), regions.begin(), regions.end());
    }

    void RHIBlitTexture(RHITexture* pSrcTexture,
                        RHITexture* pDstTexture,
                        VectorView<RHITextureBlitRegion> regions,
                        RHISamplerFilter filter) override
    {
        blits.insert(blits.end(), regions.begin(), regions.end());
    }

    void RHICopyTextureToBuffer(RHITexture* pSrcTex,
                                RHIBuffer* pDstBuffer,
                                VectorView<RHIBufferTextureCopyRegion> regions) override
    {}

    void RHICopyBufferToTexture(RHIBuffer* pSrcBuffer,
                                RHITexture* pDstTexture,
                                VectorView<RHIBufferTextureCopyRegion> regions) override
    {
        textureCopies.insert(textureCopies.end(), regions.begin(), regions.end());
    }

    void RHIResolveTexture(RHITexture* pSrcTexture,
                           RHITexture* pDstTexture,
                           uint32_t srcLayer,
                           uint32_t srcMipmap,
                           uint32_t dstLayer,
                           uint32_t dstMipmap) override
    {}

    void RHIWaitUntilCompleted() override
    {
        wait();
    }
};
// Command lists own their contexts; proxies delegate to shared observation logs.
class TestContextProxy : public IRHICommandContext
{
public:
    explicit TestContextProxy(TestContext& log) : log(log) {}

    ~TestContextProxy() override
    {
        ++log.proxyDestructions;
        log.lastProxyDestroyThread = std::this_thread::get_id();
    }

    TestContext& log;

    RHICommandContextType GetContextType() override
    {
        return log.GetContextType();
    }

    void RHIBeginRendering(const RHIRenderingLayout* pRenderingLayout) override
    {
        return log.RHIBeginRendering(pRenderingLayout);
    }

    void RHIEndRendering() override
    {
        return log.RHIEndRendering();
    }

    void RHISetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) override
    {
        return log.RHISetScissor(minX, minY, maxX, maxY);
    }

    void RHISetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) override
    {
        return log.RHISetViewport(minX, minY, maxX, maxY);
    }

    void RHISetDepthBias(float depthBiasConstantFactor,
                         float depthBiasClamp,
                         float depthBiasSlopeFactor) override
    {
        return log.RHISetDepthBias(depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
    }

    void RHISetLineWidth(float lineWidth) override
    {
        return log.RHISetLineWidth(lineWidth);
    }

    void RHISetBlendConstants(const Color& blendConstants) override
    {
        return log.RHISetBlendConstants(blendConstants);
    }

    void RHIBindPipeline(RHIPipeline* pPipeline) override
    {
        return log.RHIBindPipeline(pPipeline);
    }

    void RHISetShaderParameters(const RHIBatchedShaderParameters& parameters) override
    {
        return log.RHISetShaderParameters(parameters);
    }

    void RHIBindVertexBuffers(VectorView<RHIBuffer*> pBuffers,
                              VectorView<uint64_t> offsets) override
    {
        return log.RHIBindVertexBuffers(pBuffers, offsets);
    }

    void RHIBindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset) override
    {
        return log.RHIBindVertexBuffer(pBuffer, offset);
    }

    void RHIDraw(uint32_t vertexCount,
                 uint32_t instanceCount,
                 uint32_t firstVertex,
                 uint32_t firstInstance) override
    {
        return log.RHIDraw(vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void RHIDrawIndexed(RHIBuffer* pIndexBuffer,
                        DataFormat indexFormat,
                        uint32_t indexBufferOffset,
                        uint32_t indexCount,
                        uint32_t instanceCount,
                        uint32_t firstIndex,
                        int32_t vertexOffset,
                        uint32_t firstInstance) override
    {
        return log.RHIDrawIndexed(pIndexBuffer, indexFormat, indexBufferOffset, indexCount,
                                  instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void RHIDrawIndexedIndirect(RHIBuffer* pIndirectBuffer,
                                RHIBuffer* pIndexBuffer,
                                DataFormat indexFormat,
                                uint32_t indexBufferOffset,
                                uint32_t offset,
                                uint32_t drawCount,
                                uint32_t stride) override
    {
        return log.RHIDrawIndexedIndirect(pIndirectBuffer, pIndexBuffer, indexFormat,
                                          indexBufferOffset, offset, drawCount, stride);
    }

    void RHIDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override
    {
        return log.RHIDispatch(groupCountX, groupCountY, groupCountZ);
    }

    void RHIDispatchIndirect(RHIBuffer* pIndirectBuffer, uint32_t offset) override
    {
        return log.RHIDispatchIndirect(pIndirectBuffer, offset);
    }

    void RHISetPushConstants(RHIPipeline* pPipeline,
                             VectorView<const uint8_t> data,
                             uint32_t offset = 0) override
    {
        return log.RHISetPushConstants(pPipeline, data, offset);
    }

    void RHIAddTransitions(BitField<RHIPipelineStageFlagBits> srcStages,
                           BitField<RHIPipelineStageFlagBits> dstStages,
                           VectorView<RHIMemoryTransition> memoryTransitions,
                           VectorView<RHIBufferTransition> bufferTransitions,
                           VectorView<RHITextureTransition> textureTransitions) override
    {
        return log.RHIAddTransitions(srcStages, dstStages, memoryTransitions, bufferTransitions,
                                     textureTransitions);
    }

    void RHIAddTextureTransition(RHITexture* pTexture, RHITextureLayout newLayout) override
    {
        return log.RHIAddTextureTransition(pTexture, newLayout);
    }

    void RHIClearBuffer(RHIBuffer* pBuffer, uint32_t offset, uint32_t size) override
    {
        return log.RHIClearBuffer(pBuffer, offset, size);
    }

    void RHICopyBuffer(RHIBuffer* pSrcBuffer,
                       RHIBuffer* pDstBuffer,
                       const RHIBufferCopyRegion& region) override
    {
        return log.RHICopyBuffer(pSrcBuffer, pDstBuffer, region);
    }

    void RHIClearTexture(RHITexture* pTex,
                         const Color& color,
                         const RHITextureSubResourceRange& range) override
    {
        return log.RHIClearTexture(pTex, color, range);
    }

    void RHICopyTexture(RHITexture* pSrcTexture,
                        RHITexture* pDstTexture,
                        VectorView<RHITextureCopyRegion> regions) override
    {
        return log.RHICopyTexture(pSrcTexture, pDstTexture, regions);
    }

    void RHIBlitTexture(RHITexture* pSrcTexture,
                        RHITexture* pDstTexture,
                        VectorView<RHITextureBlitRegion> regions,
                        RHISamplerFilter filter) override
    {
        return log.RHIBlitTexture(pSrcTexture, pDstTexture, regions, filter);
    }

    void RHICopyTextureToBuffer(RHITexture* pSrcTex,
                                RHIBuffer* pDstBuffer,
                                VectorView<RHIBufferTextureCopyRegion> regions) override
    {
        return log.RHICopyTextureToBuffer(pSrcTex, pDstBuffer, regions);
    }

    void RHICopyBufferToTexture(RHIBuffer* pSrcBuffer,
                                RHITexture* pDstTexture,
                                VectorView<RHIBufferTextureCopyRegion> regions) override
    {
        return log.RHICopyBufferToTexture(pSrcBuffer, pDstTexture, regions);
    }

    void RHIResolveTexture(RHITexture* pSrcTexture,
                           RHITexture* pDstTexture,
                           uint32_t srcLayer,
                           uint32_t srcMipmap,
                           uint32_t dstLayer,
                           uint32_t dstMipmap) override
    {
        return log.RHIResolveTexture(pSrcTexture, pDstTexture, srcLayer, srcMipmap, dstLayer,
                                     dstMipmap);
    }

    void RHIWaitUntilCompleted() override
    {
        return log.RHIWaitUntilCompleted();
    }
};
class TestRHI : public DynamicRHI
{
public:
    TestRHI()
    {
        graphics.wait = transfer.wait = [this] {
            completed = submitted;
        };
        info.uniformBufferAlignment = 16;
        info.storageBufferAlignment = 16;
    }

    TestContext graphics{RHICommandContextType::eGraphics};
    TestContext transfer{RHICommandContextType::eTransfer};
    std::array<uint64_t, 3> submitted{};
    std::array<uint64_t, 3> completed{};
    std::array<bool, 3> pending{};
    RHIGPUInfo info{};
    RHIQueueCopyCapabilities graphicsCopy{true, true, true, {1, 1, 1}};
    RHIQueueCopyCapabilities transferCopy{false, false, true, {1, 1, 1}};
    std::unordered_map<DataFormat, RHITextureCopyCapabilities> copyCapabilities;
    bool shared{false};
    uint32_t pipelineCount{0};
    uint32_t frameBegins{0};
    uint32_t deviceIdleWaits{0};
    uint32_t contextCreations{0};
    bool failSubmissionWait{false};
    bool failProgressQuery{false};
    bool submissionsBlocked{false};
    std::vector<std::pair<RHICommandContextType, uint64_t>> submissionWaits;
    uint32_t textureCreations{0};
    uint32_t finalizedLists{0};
    uint32_t submissionAttempts{0};
    uint32_t failSubmissionAt{0};
    RHISubmissionResult submissionFailure{RHISubmissionResult::eRejected};
    bool submitBeforeFailure{false};
    std::function<void()> beforeSubmission;
    std::vector<RHICommandList*> pendingCommandLists;
    uint32_t failTextureCreationAt{0};
    uint32_t failPipelineCreationAt{0};
    const RHIRenderingLayout* lastPipelineLayout{nullptr};
    std::vector<RHIPipeline*> createdPipelines;

    void Init() override {}

    void Destroy() override {}

    void BeginFrame() override
    {
        ++frameBegins;
    }

    IRHICommandContext* GetCommandContext(RHICommandContextType) override
    {
        ++contextCreations;
        return ZEN_NEW() TestContextProxy(graphics);
    }

    IRHICommandContext* GetTransferCommandContext() override
    {
        return ZEN_NEW() TestContextProxy(transfer);
    }

    RHIAPIType GetAPIType() override
    {
        return RHIAPIType::eVulkan;
    }

    NameID GetName() override
    {
        static const NameID name("test");

        return name;
    }

    DataFormat GetSupportedDepthFormat() override
    {
        return DataFormat::eD32SFloat;
    }

    RHIViewport* CreateViewport(void*, uint32_t, uint32_t, bool) override
    {
        return nullptr;
    }

    void DestroyViewport(RHIViewport*) override {}

    bool failShaderCreation{false};

    RHIShader* CreateShader(const RHIShaderCreateInfo& info) override
    {
        RHIShader* result{};

        if (!(failShaderCreation))
        {
            if (std::unordered_map<NameID, RHIShaderCreateInfo>::iterator it =
                    reflectedShaderInfos.find(info.name);
                it != reflectedShaderInfos.end())
            {
                RHIShaderCreateInfo reflectedInfo     = it->second;
                reflectedInfo.name                    = info.name;
                reflectedInfo.specializationConstants = info.specializationConstants;
                result                                = ZEN_NEW() TestShader(reflectedInfo);
            }
            else
            {
                result = ZEN_NEW() TestShader(info);
            }
        }

        return result;
    }

    void DestroyShader(RHIShader* shader) override
    {
        shader->ReleaseReference();
    }

    RHIPipeline* CreatePipeline(const RHIGfxPipelineCreateInfo& info) override
    {
        RHIPipeline* pResult = nullptr;
        bool valid           = true;

        EXPECT_NE(info.pRenderingLayout, nullptr);

        ++pipelineCount;
        lastPipelineLayout = info.pRenderingLayout;
        valid              = !(pipelineCount == failPipelineCreationAt);

        if (valid)
        {
            TestPipeline* pipeline = ZEN_NEW() TestPipeline(info);
            createdPipelines.push_back(pipeline);

            pResult = pipeline;
        }

        return pResult;
    }

    RHIPipeline* CreatePipeline(const RHIComputePipelineCreateInfo& info) override
    {
        RHIPipeline* result{};

        ++pipelineCount;

        if (!(pipelineCount == failPipelineCreationAt))
        {
            TestPipeline* pipeline = ZEN_NEW() TestPipeline(info);
            createdPipelines.push_back(pipeline);
            result = pipeline;
        }

        return result;
    }

    void DestroyPipeline(RHIPipeline* pipeline) override
    {
        pipeline->ReleaseReference();
    }

    RHISampler* CreateSampler(const RHISamplerCreateInfo& info) override
    {
        return ZEN_NEW() TestSampler(info);
    }

    void DestroySampler(RHISampler* sampler) override
    {
        sampler->ReleaseReference();
    }

    RHITexture* CreateTexture(const RHITextureCreateInfo& info) override
    {
        RHITexture* result{};

        ++textureCreations;

        if (!(textureCreations == failTextureCreationAt))
        {
            result = ZEN_NEW() TestTexture(info);
        }

        return result;
    }

    RHITextureView* CreateTextureView(RHITexture* texture,
                                      const RHITextureViewCreateInfo& info) override
    {
        return texture->CreateView(info);
    }

    void DestroyTexture(RHITexture* texture) override
    {
        texture->ReleaseReference();
    }

    RHIBuffer* CreateBuffer(const RHIBufferCreateInfo& info) override
    {
        return ZEN_NEW() TestBuffer(info);
    }

    void DestroyBuffer(RHIBuffer* buffer) override
    {
        buffer->ReleaseReference();
    }

    void FinalizeCommandLists(VectorView<RHICommandList*> lists,
                              HeapVector<RHIPlatformCommandList*>&) override
    {
        for (RHICommandList* list : lists)
        {
            ++finalizedLists;
            pendingCommandLists.push_back(list);
            pending[Index(list->GetContext()->GetContextType())] = true;
        }
    }

    void SubmitPlatformCommandLists(VectorView<RHIPlatformCommandList*>) override {}

    RHISubmissionResult FlushAllGPUCommands() override
    {
        RHISubmissionResult result{};

        if (pendingCommandLists.empty())
        {
            result = RHISubmissionResult::eSuccess;
        }
        else
        {
            ++submissionAttempts;

            if (beforeSubmission)
            {
                beforeSubmission();
            }

            const bool fail = submissionAttempts == failSubmissionAt;

            if (fail && !submitBeforeFailure)
            {
                pending.fill(false);
                pendingCommandLists.clear();
                result = submissionFailure;
            }
            else
            {
                // Execute accepted commands only. Failed native submission has no GPU byte effects.
                for (RHICommandList* list : pendingCommandLists)
                {
                    list->Execute();
                }

                pendingCommandLists.clear();

                for (size_t i = 0; i < pending.size(); ++i)
                {
                    if (pending[i])
                    {
                        ++submitted[i];
                        pending[i] = false;
                    }
                }

                result = fail ? submissionFailure : RHISubmissionResult::eSuccess;
            }
        }

        return result;
    }

    size_t Index(RHICommandContextType type) const
    {
        return shared && type == RHICommandContextType::eTransfer ? 0 : static_cast<size_t>(type);
    }

    bool IsTransferQueueSharedWithGraphics() const override
    {
        return shared;
    }

    bool AreSubmissionsBlocked() const override
    {
        return submissionsBlocked;
    }

    uint64_t GetLastSubmittedSerial(RHICommandContextType type) const override
    {
        return submitted[Index(type)];
    }

    uint64_t GetLastCompletedSerial(RHICommandContextType type) override
    {
        if (failProgressQuery)
        {
            throw std::runtime_error("test GPU progress query failure");
        }
        return completed[Index(type)];
    }

    bool WaitForSubmission(RHICommandContextType type,
                           uint64_t serial,
                           uint64_t timeoutNS = UINT64_MAX) override
    {
        bool result{};

        submissionWaits.emplace_back(type, serial);

        if (!(failSubmissionWait || serial > submitted[Index(type)]))
        {
            if (completed[Index(type)] >= serial)
            {
                result = true;
            }
            else if (timeoutNS == 0)
            {
                result = false;
            }
            else
            {
                completed[Index(type)] = serial;
                result                 = true;
            }
        }

        return result;
    }

    void WaitDeviceIdle() override
    {
        ++deviceIdleWaits;
        completed = submitted;
    }

    const RHIGPUInfo& QueryGPUInfo() const override
    {
        return info;
    }

    RHITextureCopyCapabilities GetTextureCopyCapabilities(DataFormat format) const override
    {
        RHITextureCopyCapabilities result{};

        if (const std::unordered_map<DataFormat, RHITextureCopyCapabilities>::const_iterator it =
                copyCapabilities.find(format);
            it != copyCapabilities.end())
        {
            result = it->second;
        }
        else
        {
            result = {true, true, true, true, true};
        }

        return result;
    }

    RHIQueueCopyCapabilities GetQueueCopyCapabilities(RHICommandContextType type) const override
    {
        return type == RHICommandContextType::eTransfer && !shared ? transferCopy : graphicsCopy;
    }
};
} // namespace

// Stand-ins for the backend factory, image decoding, and renderer/scene facades.
// The test target links the actual device, graph, pass compiler, staging manager,
// resource manager, shader/texture managers, and command-list implementations without a Vulkan device.
DynamicRHI* GDynamicRHI = nullptr;
RHIFrameState GRHIFrameState;
namespace zen
{
DynamicRHI* DynamicRHI::Create(RHIAPIType)
{
    return GDynamicRHI;
}

RHIDebug* RHIDebug::Create()
{
    return nullptr;
}
} // namespace zen
namespace zen::asset
{
void TextureLoader::LoadTexture2DFromFile(const std::string& file, TextureInfo* result)
{
    const std::string key = std::filesystem::path(file).lexically_normal().generic_string();
    *result               = textureFiles.contains(key) ? textureFiles.at(key) : TextureInfo{};
}
} // namespace zen::asset
namespace zen::rc
{
RendererServer::RendererServer(RenderDevice* device, RHIViewport* viewport) :
    m_pViewport(viewport), m_pRenderDevice(device)
{}

void RendererServer::Init()
{
    m_pSkyboxRenderer = ZEN_NEW() SkyboxRenderer(m_pRenderDevice, m_pViewport);
}

void RendererServer::Destroy()
{
    if (m_pSkyboxRenderer != nullptr)
    {
        m_pSkyboxRenderer->Destroy();
        ZEN_DELETE(m_pSkyboxRenderer);
        m_pSkyboxRenderer = nullptr;
    }
}

RenderScene::RenderScene(RenderDevice* device, const SceneData& data) :
    m_pRenderDevice(device), m_pScene(data.pScene), m_pCamera(data.pCamera)
{
    PrepareBuffers();
    LoadSceneTextures();
}

void RenderScene::PrepareBuffers()
{
    m_pVertexBuffer = sceneInputs.vertices;
    m_pIndexBuffer  = sceneInputs.indices;
    m_pNodeSSBO     = sceneInputs.nodes;
    m_pMaterialSSBO = sceneInputs.materials;
}

void RenderScene::LoadSceneTextures()
{
    m_sceneTextures = sceneInputs.textures;
    m_envTexture    = sceneInputs.environment;
}

const uint8_t* RenderScene::GetCameraUniformData() const
{
    return reinterpret_cast<const uint8_t*>(&sceneInputs.camera);
}

const uint8_t* RenderScene::GetSceneUniformData() const
{
    return reinterpret_cast<const uint8_t*>(&sceneInputs.uniforms);
}
} // namespace zen::rc

namespace zen::rc
{
struct RDGSubmissionTestAccess
{
    static ResourceStateTracker& Tracker(RenderDevice& device)
    {
        return device.m_rdgExecutor.GetResourceStateTracker();
    }

    static bool HasHistory(const ResourceStateTracker& tracker, uint64_t id)
    {
        return tracker.m_textureStates.contains(id) || tracker.m_bufferStates.contains(id) ||
            tracker.m_contents.contains(id);
    }

    static bool HasHistory(const RenderDevice& device, uint64_t id)
    {
        bool found = HasHistory(device.m_rdgExecutor.GetResourceStateTracker(), id) ||
            HasHistory(device.m_confirmedResourceState, id) ||
            device.m_rdgExecutor.GetMetrics().m_validator.HasState(id);
        for (const RenderDevice::PendingFrame& pending : device.m_pendingFrames)
        {
            found |= HasHistory(pending.scheduledState, id);
        }
        return found;
    }

    static size_t PendingFrameCount(const RenderDevice& device)
    {
        return device.m_pendingFrames.size();
    }
};
} // namespace zen::rc

namespace
{
class RenderCoreTest : public testing::Test
{
protected:
    void SetUp() override
    {
        InitializeDevice(nullptr);
    }

    void InitializeDevice(RHIViewport* viewport,
                          uint32_t frameCount   = 2,
                          RHIExecutionMode mode = RHIExecutionMode::eInline)
    {
        destroyed.clear();
        reflectedShaderInfos.clear();
        textureFiles.clear();
        sceneInputs = {};
        rhi         = ZEN_NEW() TestRHI();
        GDynamicRHI = rhi;
        device      = ZEN_NEW() RenderDevice(RHIAPIType::eVulkan, frameCount, mode);
        device->Init(viewport);
    }

    void TearDown() override
    {
        ShaderProgramManager::GetInstance().Destroy();
        device->Destroy();
        ZEN_DELETE(device);
    }

    TestBuffer* Buffer(uint32_t size = 64)
    {
        RHIBufferCreateInfo info{};
        info.size = size;
        info.usageFlags =
            0x1ff; // This fixture serves all buffer roles; restricted-usage tests create their own.

        return static_cast<TestBuffer*>(rhi->CreateBuffer(info));
    }

    RHITexture* Texture(uint32_t mipmaps = 1)
    {
        RHITextureCreateInfo info{};
        info.format = DataFormat::eR8G8B8A8UNORM;
        info.type   = RHITextureType::e2D;
        info.width = info.height = 8;
        info.mipmaps             = mipmaps;
        info.usageFlags.SetFlags(
            RHITextureUsageFlagBits::eTransferDst, RHITextureUsageFlagBits::eTransferSrc,
            RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eStorage,
            RHITextureUsageFlagBits::eColorAttachment);

        return rhi->CreateTexture(info);
    }

    void AllocateRendererInputs(int epoch,
                                TestViewport& viewport,
                                std::vector<RHIBuffer*>& ownedBuffers,
                                std::vector<RHITexture*>& ownedTextures,
                                RenderConfig& config);
    TestRHI* rhi;
    RenderDevice* device;
};

class RenderCoreEnvironmentTest : public RenderCoreTest
{
protected:
    TestViewport viewport;

    void SetUp() override
    {
        InitializeDevice(&viewport);
    }
};

static_assert(!std::is_convertible_v<RDGBuffer, RDGTexture>);
static_assert(!std::is_convertible_v<RDGTexture, RDGBuffer>);
static_assert(!std::is_constructible_v<RDGTexture, RDGResource>);
static_assert(!std::is_copy_constructible_v<RDGExtractedTexture>);
static_assert(!std::is_copy_constructible_v<RDGExtractedBuffer>);

static_assert(!std::is_constructible_v<RDGBuffer, RDGResource>);
static_assert(!std::is_assignable_v<RDGTexture&, RDGBuffer>);
static_assert(!std::is_assignable_v<RDGBuffer&, RDGTexture>);
static_assert(std::is_trivially_copyable_v<RDGResource>);
static_assert(std::is_trivially_copyable_v<RDGTexture>);
static_assert(std::is_trivially_copyable_v<RDGBuffer>);

static RDGResourceInfo DescribeResource(RDGResourceManager* resources, RDGResource resource)
{
    RDGResourceInfo info;
    EXPECT_TRUE(resources->GetResourceInfo(resource, info));

    return info;
}

static RDGTextureDesc LogicalTexture(uint32_t mips = 1)
{
    RDGTextureDesc desc{};
    desc.name      = "logical_texture";
    desc.texFormat = {DataFormat::eR8G8B8A8UNORM,
                      TextureDimension::e2D,
                      SampleCount::e1,
                      8,
                      8,
                      1,
                      1,
                      mips,
                      false};
    desc.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);

    return desc;
}

static RDGBufferDesc LogicalBuffer()
{
    RDGBufferDesc desc{};
    desc.name = "logical_buffer";
    desc.size = 64;
    desc.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);

    return desc;
}

static RDGComputePassDesc IntentPass(NameID tag = "intent")
{
    RDGComputePassDesc desc;
    desc.SetShaderProgramName("intent");
    desc.SetPassTag(tag);

    return desc;
}

TEST_F(RenderCoreTest, ResourceValuesKeepVersionsImmutableAndShareAllocationMetadata)
{
    TestBuffer* source = Buffer();
    std::fill(source->bytes.begin(), source->bytes.end(), uint8_t(91));
    RenderGraph graph("resource_values");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer input         = resources->ImportHostWrittenBuffer(source);
    EXPECT_EQ(input, resources->ImportBuffer(source));
    EXPECT_EQ(DescribeResource(resources, input).lifeCycle, RDGResourceLifeCycle::eImported);
    EXPECT_EQ(DescribeResource(resources, input).physicalStableId, source->GetStableId());

    const RDGBuffer initialBuffer =
        resources->InitialVersion(resources->CreateBuffer(LogicalBuffer()));
    const RDGBuffer firstBuffer   = resources->CreateVersion(initialBuffer);
    const RDGResource savedBuffer = firstBuffer;
    const RDGBuffer lastBuffer    = resources->CreateVersion(firstBuffer);
    const RDGTexture initialTexture =
        resources->InitialVersion(resources->CreateTexture(LogicalTexture()));
    const RDGTexture firstTexture = resources->CreateVersion(initialTexture);
    const RDGTexture savedTexture = firstTexture;
    const RDGTexture lastTexture  = resources->CreateVersion(firstTexture);
    EXPECT_EQ(DescribeResource(resources, initialBuffer).version, 0);
    EXPECT_EQ(DescribeResource(resources, savedBuffer).version, 1);
    EXPECT_EQ(DescribeResource(resources, lastBuffer).version, 2);
    EXPECT_EQ(DescribeResource(resources, initialTexture).version, 0);
    EXPECT_EQ(DescribeResource(resources, savedTexture).version, 1);
    EXPECT_EQ(DescribeResource(resources, lastTexture).version, 2);

    const RDGResourceInfo before = DescribeResource(resources, savedBuffer);
    EXPECT_EQ(before.physicalStableId, 0u);
    EXPECT_EQ(before.lifeCycle, RDGResourceLifeCycle::eTransient);

    RDGBufferDesc oldBufferDesc;
    RDGTextureDesc oldTextureDesc;
    ASSERT_TRUE(resources->GetBufferDesc(firstBuffer, oldBufferDesc));
    ASSERT_TRUE(resources->GetTextureDesc(firstTexture, oldTextureDesc));

    graph.AddTransferPass("first_buffer").CopyBuffer(input, firstBuffer, {0, 0, 64});
    graph.AddTransferPass("last_buffer").CopyBuffer(input, lastBuffer, {0, 0, 64});
    graph.AddTransferPass("first_texture").ClearTexture(firstTexture, Color(0.f));
    graph.AddTransferPass("last_texture").ClearTexture(lastTexture, Color(1.f));
    RDGExtractedBuffer bufferOutput   = resources->QueueBufferExtraction(lastBuffer);
    RDGExtractedTexture textureOutput = resources->QueueTextureExtraction(lastTexture);
    RDGBufferDesc currentBufferDesc;
    RDGTextureDesc currentTextureDesc;
    ASSERT_TRUE(resources->GetBufferDesc(firstBuffer, currentBufferDesc));
    ASSERT_TRUE(resources->GetTextureDesc(lastTexture, currentTextureDesc));
    EXPECT_FALSE(oldBufferDesc.usageFlags.HasFlag(RHIBufferUsageFlagBits::eTransferDstBuffer));
    EXPECT_TRUE(currentBufferDesc.usageFlags.HasFlag(RHIBufferUsageFlagBits::eTransferDstBuffer));
    EXPECT_FALSE(oldTextureDesc.usageFlags.HasFlag(RHITextureUsageFlagBits::eTransferDst));
    EXPECT_TRUE(currentTextureDesc.usageFlags.HasFlag(RHITextureUsageFlagBits::eTransferDst));
    EXPECT_EQ(DescribeResource(resources, savedBuffer).lifeCycle,
              RDGResourceLifeCycle::ePersistent);
    ASSERT_TRUE(graph.End());

    for (uint32_t replay = 0; replay < 2; ++replay)
    {
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        ASSERT_TRUE(bufferOutput);
        ASSERT_TRUE(textureOutput);
        EXPECT_EQ(static_cast<TestBuffer*>(bufferOutput.Get())->bytes, source->bytes);

        for (const RDGResource value :
             {RDGResource(initialBuffer), savedBuffer, RDGResource(lastBuffer)})
        {
            EXPECT_EQ(DescribeResource(resources, value).physicalStableId,
                      bufferOutput.Get()->GetStableId());
            EXPECT_EQ(DescribeResource(resources, value).type, RDGResourceType::eBuffer);
        }

        for (const RDGTexture value : {initialTexture, savedTexture, lastTexture})
        {
            EXPECT_EQ(DescribeResource(resources, value).physicalStableId,
                      textureOutput.Get()->GetStableId());
            EXPECT_EQ(DescribeResource(resources, value).type, RDGResourceType::eTexture);
        }

        EXPECT_EQ(DescribeResource(resources, savedBuffer).version, 1);
        EXPECT_EQ(DescribeResource(resources, savedTexture).version, 1);
        EXPECT_TRUE(graph.GetWarnings().empty());
    }

    // Returned descriptions remain snapshots; editing one cannot modify the graph's allocation.
    currentBufferDesc.size             = 1;
    currentTextureDesc.texFormat.width = 1;

    ASSERT_TRUE(resources->GetBufferDesc(lastBuffer, currentBufferDesc));
    ASSERT_TRUE(resources->GetTextureDesc(savedTexture, currentTextureDesc));
    EXPECT_EQ(currentBufferDesc.size, 64u);
    EXPECT_EQ(currentTextureDesc.texFormat.width, 8u);
    EXPECT_EQ(before.physicalStableId, 0u);
    EXPECT_EQ(before.lifeCycle, RDGResourceLifeCycle::eTransient);

    device->DestroyBuffer(source);
}

TEST_F(RenderCoreTest, ResourceQueriesRejectReusedBuildSlotsWithoutChangingOutput)
{
    RenderGraph graph("reused_resource_storage");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGTexture staleTexture =
        resources->CreateVersion(resources->CreateTexture(LogicalTexture()));
    const RDGBuffer staleBuffer =
        resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));
    const RDGResource staleValue = staleBuffer;
    RDGTextureDesc textureDesc   = LogicalTexture();
    textureDesc.name             = "replacement_texture";
    textureDesc.texFormat.width  = 16;
    RDGBufferDesc bufferDesc     = LogicalBuffer();
    bufferDesc.name              = "replacement_buffer";
    bufferDesc.size              = 128;

    for (uint32_t build = 0; build < 4; ++build)
    {
        // Reset reuses the arena and first allocation/version slots in the same manager.
        ASSERT_TRUE(graph.Reset());
        ASSERT_TRUE(graph.Begin());

        const RDGTexture currentTexture =
            resources->CreateVersion(resources->CreateTexture(textureDesc));
        const RDGBuffer currentBuffer =
            resources->CreateVersion(resources->CreateBuffer(bufferDesc));
        EXPECT_NE(staleTexture, currentTexture);
        EXPECT_NE(staleBuffer, currentBuffer);
        ASSERT_TRUE(resources->IsValid(currentTexture));
        ASSERT_TRUE(resources->IsValid(currentBuffer));

        RDGResourceInfo info = DescribeResource(resources, currentBuffer);
        EXPECT_FALSE(resources->GetResourceInfo(staleValue, info));
        EXPECT_EQ(info.name, bufferDesc.name);
        EXPECT_EQ(info.version, 1);
        EXPECT_FALSE(resources->GetTextureDesc(staleTexture, textureDesc));
        EXPECT_EQ(textureDesc.name, NameID("replacement_texture"));
        EXPECT_EQ(textureDesc.texFormat.width, 16u);
        EXPECT_FALSE(resources->GetBufferDesc(staleBuffer, bufferDesc));
        EXPECT_EQ(bufferDesc.name, NameID("replacement_buffer"));
        EXPECT_EQ(bufferDesc.size, 128u);
        EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eLifecycle);
        EXPECT_EQ(DescribeResource(resources, currentTexture).name, textureDesc.name);
        EXPECT_EQ(DescribeResource(resources, currentBuffer).name, bufferDesc.name);
        EXPECT_FALSE(graph.End());
        EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    }

    EXPECT_EQ(rhi->textureCreations, 0u);
    EXPECT_EQ(rhi->finalizedLists, 0u);
}

TEST_F(RenderCoreTest, LogicalBuffersBindCopyAndExtractWithoutExposingPhysicalAllocation)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer* source = Buffer();
    std::fill(source->bytes.begin(), source->bytes.end(), uint8_t(42));
    RenderGraph graph("logical_buffers");
    graph.Begin();
    RDGResourceManager* resources = graph.GetResourceManager();
    RDGBuffer input               = resources->ImportHostWrittenBuffer(source);
    RDGBuffer scratch             = resources->CreateBuffer(LogicalBuffer());
    EXPECT_EQ(DescribeResource(resources, scratch).physicalStableId, 0u);

    graph.AddTransferPass("initialize").CopyBuffer(input, scratch, {0, 0, 64});

    RDGComputePassDesc read = IntentPass();
    read.BindStorageBuffer("read_buffer", scratch, RDGContentGuarantee::eNone);
    read.BindUniformBuffer("value", scratch);
    graph.AddComputePass(read).RecordPassCommands(
        [](RDGPassCmdEncoder& e) { e.Dispatch(1, 1, 1); });
    RDGExtractedBuffer output = resources->QueueBufferExtraction(scratch);
    ASSERT_TRUE(graph.End());

    RDGExecutor executor(device);
    ASSERT_TRUE(executor.Prepare(&graph));
    EXPECT_FALSE(output); // Prepare does not publish ownership.
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    ASSERT_TRUE(output);
    EXPECT_EQ(static_cast<TestBuffer*>(output.Get())->bytes[63], 42);
    EXPECT_TRUE(output.Get()->GetUsageFlags().HasFlag(RHIBufferUsageFlagBits::eUniformBuffer));

    const uint64_t id   = output.Get()->GetStableId();
    const uint32_t refs = output.Get()->GetRefCount();
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(output.Get()->GetStableId(), id);
    EXPECT_EQ(output.Get()->GetRefCount(), refs);

    graph.Reset();
    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    EXPECT_FALSE(destroyed.contains(id));
    EXPECT_EQ(output.Get()->GetRefCount(), 1u);

    RDGExtractedBuffer moved = std::move(output);
    EXPECT_FALSE(output);
    EXPECT_EQ(moved.Get()->GetStableId(), id);

    moved.Reset();
    device->CollectCompletedResources();

    EXPECT_TRUE(destroyed.contains(id));

    device->DestroyBuffer(source);
}

TEST_F(RenderCoreTest, TransientStorageRequiresInitializationAndFullWriteIsExplicit)
{
    CreateTestShaderProgram(device, "intent");

    for (const bool readsPredecessor : {false, true})
    {
        for (RDGContentGuarantee const contents :
             {RDGContentGuarantee::eNone, RDGContentGuarantee::eDiscard,
              RDGContentGuarantee::eFullWrite})
        {
            RenderGraph graph("storage_initialization");
            graph.Begin();
            RDGBuffer buffer = graph.GetResourceManager()->CreateBuffer(LogicalBuffer());

            RDGComputePassDesc write = IntentPass("write");
            write.BindStorageBuffer(readsPredecessor ? "buffer" : "write_buffer", buffer, contents);
            graph.AddComputePass(write);

            RDGComputePassDesc read = IntentPass("read");
            read.BindStorageBuffer("read_buffer", buffer);
            graph.AddComputePass(read);

            ASSERT_TRUE(graph.End());

            const bool success = device->ExecuteRenderGraph(graph);
            EXPECT_EQ(success, !readsPredecessor && contents == RDGContentGuarantee::eFullWrite);

            if (!success)
            {
                EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eUninitialized);
            }
        }
    }

    RenderGraph simultaneous("simultaneous_bindings");
    simultaneous.Begin();
    RDGBuffer buffer = simultaneous.GetResourceManager()->CreateBuffer(LogicalBuffer());

    RDGComputePassDesc desc = IntentPass();
    desc.BindStorageBuffer("write_buffer", buffer, RDGContentGuarantee::eFullWrite);
    desc.BindUniformBuffer("value", buffer);
    simultaneous.AddComputePass(desc);

    ASSERT_TRUE(simultaneous.End());
    EXPECT_FALSE(device->ExecuteRenderGraph(simultaneous));
    EXPECT_EQ(simultaneous.GetResult().code, RDGErrorCode::eUninitialized);
}

TEST_F(RenderCoreTest, LogicalAttachmentsValidateLoadStoreAndPartialClearContents)
{
    CreateTestShaderProgram(device, "intent");

    for (int scenario = 0; scenario < 5; ++scenario)
    {
        RenderGraph graph("attachment_contents");
        graph.Begin();
        RDGTexture texture = graph.GetResourceManager()->CreateTexture(LogicalTexture());

        RDGGraphicsPassDesc draw;
        draw.SetShaderProgramName("intent");
        draw.SetRenderArea(0, 0, scenario == 3 ? 4 : 8, 8);
        draw.AddColorOutput(texture,
                            scenario == 0     ? RHIRenderTargetLoadOp::eLoad :
                                scenario == 4 ? RHIRenderTargetLoadOp::eNone :
                                                RHIRenderTargetLoadOp::eClear,
                            scenario == 2 ? RHIRenderTargetStoreOp::eNone :
                                            RHIRenderTargetStoreOp::eStore,
                            scenario == 4);
        graph.AddGraphicsPass(draw);

        RDGComputePassDesc read = IntentPass("sample");
        read.BindSampledTexture("texture", nullptr, texture);
        graph.AddComputePass(read);

        ASSERT_TRUE(graph.End());

        const uint32_t allocations = rhi->textureCreations;
        EXPECT_EQ(device->ExecuteRenderGraph(graph), scenario == 1 || scenario == 4);

        if (scenario == 0 || scenario == 2 || scenario == 3)
        {
            EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eUninitialized);
            EXPECT_EQ(rhi->textureCreations, allocations);
        }
    }
}

TEST_F(RenderCoreTest, PartialAttachmentClearPreservesPreviouslyDefinedContents)
{
    CreateTestShaderProgram(device, "intent");

    for (bool depth : {false, true})
    {
        for (RHIRenderTargetLoadOp load :
             {RHIRenderTargetLoadOp::eClear, RHIRenderTargetLoadOp::eNone})
        {
            RenderGraph graph("partial_clear_after_initialization");
            graph.Begin();
            RDGTextureDesc desc = LogicalTexture();

            if (depth)
            {
                desc.texFormat.format = DataFormat::eD32SFloat;
            }

            RDGTexture texture = graph.GetResourceManager()->CreateTexture(desc);

            for (bool partial : {false, true})
            {
                RDGGraphicsPassDesc pass;
                pass.SetShaderProgramName("intent");
                pass.SetRenderArea(0, 0, partial ? 4 : 8, 8);

                if (depth)
                {
                    pass.AddDepthStencilOutput(texture,
                                               partial ? load : RHIRenderTargetLoadOp::eClear);
                }
                else
                {
                    pass.AddColorOutput(texture, partial ? load : RHIRenderTargetLoadOp::eClear);
                }

                graph.AddGraphicsPass(pass);
            }

            RDGComputePassDesc sample = IntentPass();
            sample.BindSampledTexture("texture", nullptr, texture);
            graph.AddComputePass(sample);

            ASSERT_TRUE(graph.End());
            EXPECT_EQ(device->ExecuteRenderGraph(graph), load == RHIRenderTargetLoadOp::eClear);

            if (load == RHIRenderTargetLoadOp::eNone)
            {
                EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eUninitialized);
            }
        }
    }
}

TEST_F(RenderCoreTest, UnknownContentWarningsSurviveRebuildWithoutRepeatingLogs)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer* buffer = Buffer();
    std::ostringstream output;
    std::shared_ptr<spdlog::logger> logger = std::make_shared<spdlog::logger>(
        "content_warning_test", std::make_shared<spdlog::sinks::ostream_sink_mt>(output));

    struct RestoreLogger
    {
        std::shared_ptr<spdlog::logger> previous{spdlog::default_logger()};

        ~RestoreLogger()
        {
            spdlog::set_default_logger(previous);
        }
    } restore;

    spdlog::set_default_logger(logger);
    RenderGraph graph("warning_rebuild");

    for (uint32_t frame = 0; frame < 3; ++frame)
    {
        graph.Begin();

        RDGComputePassDesc read = IntentPass();
        read.BindStorageBuffer("read_buffer", buffer, RDGContentGuarantee::eNone);
        graph.AddComputePass(read);
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        ASSERT_EQ(graph.GetWarnings().size(), 1u);
        EXPECT_NE(graph.GetWarnings()[0].message.find("buffer #"), std::string::npos);
    }

    std::string log    = output.str();
    const size_t first = log.find("RDG [12]");
    ASSERT_NE(first, std::string::npos);
    EXPECT_EQ(log.find("RDG [12]", first + 1), std::string::npos);

    // A new external write/invalidation starts a new diagnostic lifetime.
    device->InvalidateExternalBufferState(buffer);

    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_NE(output.str().find("RDG [12]", first + 1), std::string::npos);

    device->DestroyBuffer(buffer);
}

TEST_F(RenderCoreTest, LogicalViewsTrackMipInitializationAndRejectExpiredResources)
{
    CreateTestShaderProgram(device, "intent");
    RenderGraph graph("view_contents");
    graph.Begin();
    RDGResourceManager* resources    = graph.GetResourceManager();
    RDGTexture texture               = resources->CreateTexture(LogicalTexture(3));
    RHITextureSubResourceRange range = RHITextureSubResourceRange::Color();
    range.baseMipLevel               = 1;
    RDGTextureViewDesc view{range};
    ASSERT_TRUE(resources->IsValid(texture, view));

    RDGComputePassDesc writer = IntentPass("write_mip_one");
    writer.BindStorageImage("write_image", texture, view, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(writer);

    RDGComputePassDesc reader = IntentPass("read_mip_one");
    reader.BindSampledTexture("texture", nullptr, texture, view);
    graph.AddComputePass(reader);

    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    ASSERT_FALSE(rhi->graphics.boundResources.empty());

    RHITextureView* bound = static_cast<RHITextureView*>(rhi->graphics.boundResources.back());
    EXPECT_EQ(bound->GetSubResourceRange().baseMipLevel, 1u);
    EXPECT_EQ(bound->GetSubResourceRange().levelCount, 1u);

    graph.Begin();

    EXPECT_FALSE(resources->IsValid(texture, view));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eLifecycle);

    graph.Reset();
    graph.Begin();
    texture = resources->CreateTexture(LogicalTexture(3));
    view    = RDGTextureViewDesc{range};
    writer  = IntentPass();
    writer.BindStorageImage("write_image", texture, view, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(writer);
    reader = IntentPass();
    reader.BindSampledTexture("texture", nullptr, texture); // Mips zero and two remain undefined.
    graph.AddComputePass(reader);

    ASSERT_TRUE(graph.End());
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eUninitialized);
}

TEST_F(RenderCoreTest, TextureExtractionRetainsOneOwnerAndNeverReturnsToTransientPool)
{
    rhi->shared = false;
    CreateTestShaderProgram(device, "intent");
    RenderGraph graph("extract_texture");
    graph.Begin();
    RDGResourceManager* resources = graph.GetResourceManager();
    RDGTexture texture            = resources->CreateTexture(LogicalTexture());
    graph.AddTransferPass("clear").ClearTexture(texture, Color(0.f));
    RDGExtractedTexture output = resources->QueueTextureExtraction(texture);
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    ASSERT_TRUE(output);

    const uint64_t id = output.Get()->GetStableId();
    bool sampled      = false;

    for (TestContext::BarrierBatch const& batch : rhi->graphics.barrierBatches)
    {
        for (const RHITextureTransition& transition : batch.textures)
        {
            sampled |= transition.pTexture == output.Get() &&
                transition.newUsage == RHITextureUsage::eSampled;
        }
    }

    EXPECT_TRUE(sampled);

    graph.Begin();
    texture = resources->CreateTexture(LogicalTexture());
    graph.AddTransferPass("new_clear").NeverCull().ClearTexture(texture, Color(1.f));

    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_NE(DescribeResource(resources, texture).physicalStableId, id);

    graph.Reset();
    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    EXPECT_EQ(output.Get()->GetRefCount(), 1u);

    {
        RenderGraph reader("extracted_import");
        reader.Begin();
        RDGTexture input = reader.GetResourceManager()->ImportTexture(output.Get());

        RDGComputePassDesc desc = IntentPass();
        desc.BindSampledTexture("texture", nullptr, input);
        reader.AddComputePass(desc);
        ASSERT_TRUE(reader.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(reader));
        EXPECT_TRUE(reader.GetWarnings().empty());
    }

    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();
    const size_t graphics = rhi->Index(RHICommandContextType::eGraphics);
    const size_t transfer = rhi->Index(RHICommandContextType::eTransfer);
    ++rhi->submitted[graphics];
    ++rhi->submitted[transfer];
    output.Reset();
    device->CollectCompletedResources();

    EXPECT_FALSE(destroyed.contains(id));

    rhi->completed[graphics] = rhi->submitted[graphics];
    device->CollectCompletedResources();

    EXPECT_FALSE(destroyed.contains(id));

    rhi->completed[transfer] = rhi->submitted[transfer];
    device->CollectCompletedResources();

    EXPECT_TRUE(destroyed.contains(id));
}

TEST_F(RenderCoreTest, ExtractionFailureCancellationAndDuplicateOwnershipDoNotPublish)
{
    CreateTestShaderProgram(device, "intent");

    for (int scenario = 0; scenario < 4; ++scenario)
    {
        RenderGraph graph("extract_failure");
        graph.Begin();
        RDGResourceManager* resources = graph.GetResourceManager();
        RDGTexture texture            = resources->CreateTexture(LogicalTexture());

        if (scenario != 0)
        {
            graph.AddTransferPass("clear").ClearTexture(texture, Color(0.f));
        }

        RDGExtractedTexture output = resources->QueueTextureExtraction(texture);

        if (scenario == 1)
        {
            RDGComputePassDesc desc = IntentPass();
            graph.AddComputePass(desc).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                encoder.Fail(RDGErrorCode::eCallback, "cancel extraction");
            });
        }

        if (scenario == 2)
        {
            RDGExtractedTexture duplicate = resources->QueueTextureExtraction(texture);
            EXPECT_FALSE(graph.End());
            EXPECT_FALSE(duplicate);
        }
        else
        {
            ASSERT_TRUE(graph.End());
        }

        if (scenario == 3)
        {
            RDGExecutor executor(device);
            ASSERT_TRUE(executor.Prepare(&graph));
            graph.Reset(); // Prepared allocations and unpublished tickets retire safely.
        }
        else
        {
            EXPECT_FALSE(device->ExecuteRenderGraph(graph));
        }

        EXPECT_FALSE(output);

        graph.Reset();
    }
}

TEST_F(RenderCoreTest, PooledPhysicalHistoryDoesNotInitializeANewLogicalResource)
{
    CreateTestShaderProgram(device, "intent");
    RenderGraph graph("pool_contents");
    graph.Begin();
    RDGResourceManager* resources = graph.GetResourceManager();
    RDGTexture texture            = resources->CreateTexture(LogicalTexture());
    graph.AddTransferPass("clear").ClearTexture(texture, Color(0.f));

    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));

    const uint64_t id = DescribeResource(resources, texture).physicalStableId;
    graph.Begin();
    texture = resources->CreateTexture(LogicalTexture());

    RDGComputePassDesc read = IntentPass();
    read.BindSampledTexture("texture", nullptr, texture);
    graph.AddComputePass(read);

    ASSERT_TRUE(graph.End());
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eUninitialized);
    EXPECT_FALSE(destroyed.contains(id));
}

TEST_F(RenderCoreTest, ImportsDistinguishUnknownUndefinedAndDefinedContents)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer* buffer = Buffer();

    for (RDGImportContents const contract :
         {RDGImportContents::eUnknown, RDGImportContents::eUndefined, RDGImportContents::eDefined})
    {
        RenderGraph graph("import_contract");
        graph.Begin();
        RDGBuffer handle = graph.GetResourceManager()->ImportBuffer(buffer, contract);

        RDGComputePassDesc desc = IntentPass();
        desc.BindStorageBuffer("read_buffer", handle, RDGContentGuarantee::eNone);
        graph.AddComputePass(desc);

        ASSERT_TRUE(graph.End());
        EXPECT_EQ(device->ExecuteRenderGraph(graph), contract != RDGImportContents::eUndefined);

        if (contract == RDGImportContents::eUnknown)
        {
            ASSERT_EQ(graph.GetWarnings().size(), 1u);
            EXPECT_EQ(graph.GetWarnings()[0].code, RDGErrorCode::eUnknownContents);
        }

        if (contract == RDGImportContents::eUndefined)
        {
            EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eUninitialized);
        }

        if (contract == RDGImportContents::eDefined)
        {
            EXPECT_TRUE(graph.GetWarnings().empty());
        }
    }

    RenderGraph conflicting("conflicting_import");
    conflicting.Begin();
    conflicting.GetResourceManager()->ImportBuffer(buffer, RDGImportContents::eDefined);

    EXPECT_FALSE(
        conflicting.GetResourceManager()->ImportBuffer(buffer, RDGImportContents::eUndefined));
    EXPECT_FALSE(conflicting.End());

    device->DestroyBuffer(buffer);
}

TEST_F(RenderCoreTest, ExternalTextureStateDrivesFirstBarrierWithoutPrepareSideEffects)
{
    CreateTestShaderProgram(device, "intent");
    RHITexture* texture = Texture();
    RenderGraph graph("external_state");
    graph.Begin();
    RDGTextureImportState initial;
    initial.contents   = RDGImportContents::eDefined;
    initial.accessMode = RHIAccessMode::eReadWrite;
    initial.usage      = RHITextureUsage::eTransferDst;
    initial.stages.SetFlag(RHIPipelineStageFlagBits::eTransfer);
    RDGTexture handle = graph.GetResourceManager()->ImportTexture(texture, initial);

    RDGComputePassDesc desc = IntentPass();
    desc.BindSampledTexture("texture", nullptr, handle);
    graph.AddComputePass(desc);

    ASSERT_TRUE(graph.End());

    RDGExecutor executor(device);
    ASSERT_TRUE(executor.Prepare(&graph));
    EXPECT_EQ(executor.GetResourceStateTracker().GetTextureState(texture).usage,
              RHITextureUsage::eNone);
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));

    bool correct = false;

    for (TestContext::BarrierBatch const& batch : rhi->graphics.barrierBatches)
    {
        for (const RHITextureTransition& transition : batch.textures)
        {
            correct |= transition.pTexture == texture &&
                transition.oldUsage == RHITextureUsage::eTransferDst &&
                transition.newUsage == RHITextureUsage::eSampled;
        }
    }

    EXPECT_TRUE(correct);

    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, DiscardedContentsPersistAcrossGraphsAndCallbackFailureRollsBackValidity)
{
    CreateTestShaderProgram(device, "intent");
    RHITexture* texture = Texture();

    {
        RenderGraph discard("discard");
        discard.Begin();

        RDGGraphicsPassDesc desc;
        desc.SetShaderProgramName("intent");
        desc.SetRenderArea(0, 0, 8, 8);
        desc.AddColorOutput(texture, RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eNone);
        discard.AddGraphicsPass(desc);
        ASSERT_TRUE(discard.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(discard));
    }

    {
        RenderGraph failed("failed_clear");
        failed.Begin();
        failed.AddTransferPass("clear").ClearTexture(texture, Color(0.f));

        RDGComputePassDesc desc = IntentPass();
        failed.AddComputePass(desc).RecordPassCommands(
            [](RDGPassCmdEncoder& encoder) { encoder.Fail(RDGErrorCode::eCallback, "fail"); });
        ASSERT_TRUE(failed.End());
        EXPECT_FALSE(device->ExecuteRenderGraph(failed));
    }

    RenderGraph reader("read_discarded");
    reader.Begin();

    RDGComputePassDesc desc = IntentPass();
    desc.BindSampledTexture("texture", nullptr,
                            reader.GetResourceManager()->ImportTexture(texture));
    reader.AddComputePass(desc);

    ASSERT_TRUE(reader.End());
    EXPECT_FALSE(device->ExecuteRenderGraph(reader));
    EXPECT_EQ(reader.GetResult().code, RDGErrorCode::eUninitialized);

    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, PartialUploadsCannotInitializeUntouchedBufferBytesOrTextureMips)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer* source  = Buffer(256);
    RHITexture* texture = Texture(3);

    for (bool fullBase : {false, true})
    {
        RenderGraph graph("partial_upload");
        graph.Begin();
        graph.GetResourceManager()->ImportHostWrittenBuffer(source);
        graph.GetResourceManager()->ImportTexture(texture, RDGImportContents::eUndefined);
        RHIBufferTextureCopyRegion region{};
        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.textureSubresources.layerCount = 1;
        region.textureSize                    = {fullBase ? 8 : 4, 8, 1};
        graph.AddTransferPass("upload_base").CopyBufferToTexture(source, texture, region);
        graph.AddTransferPass("mip_chain").GenerateMipmaps(texture);

        RDGComputePassDesc read = IntentPass();
        read.BindSampledTexture("texture", nullptr, texture->GetDefaultView());
        graph.AddComputePass(read);

        ASSERT_TRUE(graph.End());
        EXPECT_EQ(device->ExecuteRenderGraph(graph), fullBase);

        if (!fullBase)
        {
            EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eUninitialized);
        }
    }

    RenderGraph bufferGraph("partial_buffer");
    bufferGraph.Begin();
    RDGBuffer src = bufferGraph.GetResourceManager()->ImportHostWrittenBuffer(source);
    RDGBuffer dst = bufferGraph.GetResourceManager()->CreateBuffer(LogicalBuffer());
    bufferGraph.AddTransferPass("partial").CopyBuffer(src, dst, {0, 0, 32});

    RDGComputePassDesc read = IntentPass();
    read.BindStorageBuffer("read_buffer", dst, RDGContentGuarantee::eNone);
    bufferGraph.AddComputePass(read);

    ASSERT_TRUE(bufferGraph.End());
    EXPECT_FALSE(device->ExecuteRenderGraph(bufferGraph));
    EXPECT_EQ(bufferGraph.GetResult().code, RDGErrorCode::eUninitialized);

    device->DestroyBuffer(source);
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, ChunkedBufferUploadsTrackCoverageAcrossFlushes)
{
    CreateTestShaderProgram(device, "intent");
    StagingBufferManager staging(16, 16);
    StagingUploadQueue uploads(device, &staging);
    TestBuffer* buffer = Buffer();
    std::array<uint8_t, 64> bytes;

    for (uint32_t i = 0; i < bytes.size(); ++i)
    {
        bytes[i] = uint8_t(i);
    }

    uploads.EnqueueBuffer(buffer, 0, bytes.size(), bytes.data());
    uploads.Flush();

    EXPECT_FALSE(uploads.HasPending());
    EXPECT_EQ(buffer->bytes, (std::vector<uint8_t>(bytes.begin(), bytes.end())));

    RenderGraph graph("uploaded_chunks");
    graph.Begin();

    RDGComputePassDesc pass = IntentPass();
    pass.BindStorageBuffer("read_buffer", buffer, RDGContentGuarantee::eNone);
    graph.AddComputePass(pass);

    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_TRUE(graph.GetWarnings().empty());

    uploads.Destroy();
    staging.Destroy();
    device->DestroyBuffer(buffer);
}

TEST_F(RenderCoreTest, BufferCopyCoverageTracksHolesAndExactSourceBytes)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer* source = Buffer();

    for (uint32_t scenario = 0; scenario < 3; ++scenario)
    {
        RenderGraph graph("copy_coverage");
        graph.Begin();
        RDGResourceManager* resources = graph.GetResourceManager();
        RDGBuffer input               = resources->ImportHostWrittenBuffer(source);
        RDGBuffer scratch             = resources->CreateBuffer(LogicalBuffer());
        // Nonsequential writes leave a hole at [16, 32).
        graph.AddTransferPass("upper").CopyBuffer(input, scratch, {32, 32, 32});
        graph.AddTransferPass("lower").CopyBuffer(input, scratch, {0, 0, 16});

        if (scenario == 2)
        {
            graph.AddTransferPass("bridge").CopyBuffer(input, scratch, {16, 16, 16});
        }

        RDGBufferDesc outputDesc = LogicalBuffer();
        outputDesc.size          = scenario == 2 ? 64 : 16;
        RDGBuffer output         = resources->CreateBuffer(outputDesc);
        graph.AddTransferPass("read_selected_bytes")
            .CopyBuffer(scratch, output, {scenario == 1 ? 16u : 0u, 0, outputDesc.size});

        RDGComputePassDesc read = IntentPass();
        read.BindStorageBuffer("read_buffer", output, RDGContentGuarantee::eNone);
        graph.AddComputePass(read);

        ASSERT_TRUE(graph.End());
        EXPECT_EQ(device->ExecuteRenderGraph(graph), scenario != 1);

        if (scenario == 1)
        {
            EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eUninitialized);
        }
        else
        {
            EXPECT_TRUE(graph.GetWarnings().empty());
        }
    }

    device->DestroyBuffer(source);
}

TEST_F(RenderCoreTest, BufferTextureUploadsReadOnlyTheirDeclaredFootprint)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer* initialized = Buffer(256);
    TestBuffer* partial     = Buffer(512);
    RHITexture* texture     = Texture();

    for (uint64_t offset : {0u, 128u})
    {
        RenderGraph graph("buffer_texture_footprint");
        graph.Begin();
        graph.GetResourceManager()->ImportHostWrittenBuffer(initialized);
        graph.GetResourceManager()->ImportBuffer(partial, RDGImportContents::eUndefined);
        graph.AddTransferPass("write_middle").CopyBuffer(initialized, partial, {0, 128, 256});
        RHIBufferTextureCopyRegion region{};
        region.bufferOffset = offset;
        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.textureSize = {8, 8, 1};
        graph.AddTransferPass("upload_texture").CopyBufferToTexture(partial, texture, region);

        RDGComputePassDesc read = IntentPass();
        read.BindSampledTexture("texture", nullptr, texture->GetDefaultView());
        graph.AddComputePass(read);

        ASSERT_TRUE(graph.End());
        EXPECT_EQ(device->ExecuteRenderGraph(graph), offset == 128);

        if (offset == 0)
        {
            EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eUninitialized);
        }
        else
        {
            EXPECT_TRUE(graph.GetWarnings().empty());
        }
    }

    device->DestroyBuffer(initialized);
    device->DestroyBuffer(partial);
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, PartialUnknownCopiesInvalidateOnlyOverwrittenBytes)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer* known       = Buffer();
    TestBuffer* unknown     = Buffer();
    TestBuffer* destination = Buffer();
    RenderGraph writer("partial_unknown_overwrite");
    writer.Begin();
    writer.GetResourceManager()->ImportHostWrittenBuffer(known);
    writer.AddTransferPass("initialize").CopyBuffer(known, destination, {0, 0, 64});
    writer.AddTransferPass("unknown_middle").CopyBuffer(unknown, destination, {0, 16, 16});

    ASSERT_TRUE(writer.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(writer));

    for (uint64_t offset : {0u, 16u, 32u})
    {
        RenderGraph reader("verify_coverage_after_overwrite");
        reader.Begin();
        RDGResourceManager* resources = reader.GetResourceManager();
        RDGBuffer input               = resources->ImportBuffer(destination);
        RDGBufferDesc desc            = LogicalBuffer();
        desc.size                     = 16;
        RDGBuffer output              = resources->CreateBuffer(desc);
        reader.AddTransferPass("slice").CopyBuffer(input, output, {offset, 0, 16});
        RDGExtractedBuffer extracted = resources->QueueBufferExtraction(output);
        ASSERT_TRUE(reader.End());
        EXPECT_EQ(device->ExecuteRenderGraph(reader), offset != 16);

        if (offset == 16)
        {
            EXPECT_EQ(reader.GetResult().code, RDGErrorCode::eExport);
        }
        else
        {
            EXPECT_TRUE(reader.GetWarnings().empty());
        }
    }

    writer.Begin();
    writer.GetResourceManager()->ImportHostWrittenBuffer(known);
    writer.AddTransferPass("repair_middle").CopyBuffer(known, destination, {0, 16, 16});

    RDGComputePassDesc read = IntentPass();
    read.BindStorageBuffer("read_buffer", destination, RDGContentGuarantee::eNone);
    writer.AddComputePass(read);

    ASSERT_TRUE(writer.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(writer));
    EXPECT_TRUE(writer.GetWarnings().empty());

    device->DestroyBuffer(known);
    device->DestroyBuffer(unknown);
    device->DestroyBuffer(destination);
}

TEST_F(RenderCoreTest, ProducedElementReadsRequireProducerAndDoNotDefineUnusedCapacity)
{
    CreateTestShaderProgram(device, "intent");

    for (uint32_t scenario = 0; scenario < 5; ++scenario)
    {
        RenderGraph graph("produced_elements");
        graph.Begin();
        RDGBuffer buffer = graph.GetResourceManager()->CreateBuffer(LogicalBuffer());

        if (scenario != 1)
        {
            RDGComputePassDesc producer = IntentPass("emit_elements");
            producer.BindStorageBuffer("write_buffer", buffer,
                                       RDGContentGuarantee::eProducedElements);
            graph.AddComputePass(producer);
        }

        if (scenario == 3)
        {
            RDGComputePassDesc discard = IntentPass("discard_elements");
            discard.BindStorageBuffer("write_buffer", buffer, RDGContentGuarantee::eDiscard);
            graph.AddComputePass(discard);
        }

        RDGComputePassDesc consumer = IntentPass("consume_elements");
        consumer.BindStorageBuffer("read_buffer", buffer,
                                   scenario == 2 ? RDGContentGuarantee::eNone :
                                                   RDGContentGuarantee::eConsumeProducedElements);
        graph.AddComputePass(consumer);
        RDGExtractedBuffer extracted = scenario == 4 ?
            graph.GetResourceManager()->QueueBufferExtraction(buffer) :
            RDGExtractedBuffer{};
        ASSERT_TRUE(graph.End());
        EXPECT_EQ(device->ExecuteRenderGraph(graph), scenario == 0);

        if (scenario != 0)
        {
            EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eUninitialized);
        }
        else
        {
            EXPECT_TRUE(graph.GetWarnings().empty());
        }
    }

    RenderGraph textureGraph("invalid_stream_texture");
    textureGraph.Begin();
    RDGTexture texture = textureGraph.GetResourceManager()->CreateTexture(LogicalTexture());

    RDGComputePassDesc pass = IntentPass();
    pass.BindStorageImage("write_image", texture, RDGContentGuarantee::eProducedElements);
    textureGraph.AddComputePass(pass);

    EXPECT_FALSE(textureGraph.End());
    EXPECT_EQ(textureGraph.GetResult().code, RDGErrorCode::eBinding);
}

TEST_F(RenderCoreTest, ProducedElementContractsCommitOnlyAfterSuccessfulRecording)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer* buffer = Buffer();

    for (bool fail : {true, false})
    {
        RenderGraph producer("stream_producer");
        producer.Begin();

        RDGComputePassDesc pass = IntentPass();
        pass.BindStorageBuffer("write_buffer", buffer, RDGContentGuarantee::eProducedElements);
        producer.AddComputePass(pass).RecordPassCommands([fail](RDGPassCmdEncoder& encoder) {
            if (fail)
            {
                encoder.Fail(RDGErrorCode::eCallback, "reject stream producer");
            }
        });

        ASSERT_TRUE(producer.End());
        EXPECT_EQ(device->ExecuteRenderGraph(producer), !fail);

        RenderGraph consumer("stream_consumer");
        consumer.Begin();

        RDGComputePassDesc read = IntentPass();
        read.BindStorageBuffer("read_buffer", buffer,
                               RDGContentGuarantee::eConsumeProducedElements);
        consumer.AddComputePass(read);

        ASSERT_TRUE(consumer.End());
        EXPECT_EQ(device->ExecuteRenderGraph(consumer), !fail);

        if (!fail)
        {
            EXPECT_TRUE(consumer.GetWarnings().empty());
            device->InvalidateExternalBufferState(buffer);
            EXPECT_FALSE(device->ExecuteRenderGraph(consumer));
            EXPECT_EQ(consumer.GetResult().code, RDGErrorCode::eUninitialized);
        }
    }

    device->DestroyBuffer(buffer);
}

TEST_F(RenderCoreTest, ImportedCapabilitiesAreValidatedInsteadOfExpanded)
{
    CreateTestShaderProgram(device, "intent");

    RHIBufferCreateInfo bufferInfo{};
    bufferInfo.size = 64;
    bufferInfo.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferSrcBuffer);
    RHIBuffer* buffer = rhi->CreateBuffer(bufferInfo);

    RHITextureCreateInfo textureInfo{};
    textureInfo.format = DataFormat::eR8G8B8A8UNORM;
    textureInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);
    RHITexture* texture = rhi->CreateTexture(textureInfo);
    RenderGraph graph("capabilities");
    graph.Begin();

    RDGComputePassDesc desc = IntentPass();
    desc.BindStorageBuffer("buffer", graph.GetResourceManager()->ImportBuffer(buffer));
    graph.AddComputePass(desc);

    EXPECT_FALSE(graph.End());
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eBinding);

    graph.Begin();
    desc = IntentPass();
    desc.BindStorageImage("write_image", graph.GetResourceManager()->ImportTexture(texture),
                          RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(desc);

    EXPECT_FALSE(graph.End());
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eBinding);
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));

    device->DestroyBuffer(buffer);
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, RecordedGraphRetainsImportsBeforeCompilationAndReleasesOnReset)
{
    rhi->shared             = false;
    TestBuffer* source      = Buffer();
    TestBuffer* target      = Buffer();
    source->bytes[0]        = 37;
    const uint64_t sourceId = source->GetStableId();
    const uint64_t targetId = target->GetStableId();
    RenderGraph graph("retained_imports");
    ASSERT_TRUE(graph.Begin());

    graph.AddTransferPass("copy").CopyBuffer(source, target, {0, 0, 4});
    graph.AddTransferPass("repeat").CopyBuffer(source, target, {0, 4, 4});

    ASSERT_TRUE(graph.End());
    EXPECT_EQ(source->GetRefCount(), 2u); // One graph reference despite multiple declarations.

    device->DestroyBuffer(source);
    device->DestroyBuffer(target);
    device->CollectCompletedResources();

    ASSERT_FALSE(destroyed.contains(sourceId));
    ASSERT_FALSE(destroyed.contains(targetId));
    EXPECT_EQ(source->GetRefCount(), 1u);
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(target->bytes[0], 37);
    EXPECT_EQ(target->bytes[4], 37);
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));

    const size_t graphics = rhi->Index(RHICommandContextType::eGraphics);
    const size_t transfer = rhi->Index(RHICommandContextType::eTransfer);
    ASSERT_NE(graphics, transfer);

    // Include both queues in retirement and prove that completion of only one is insufficient.
    rhi->submitted[graphics] = std::max(rhi->submitted[graphics], rhi->completed[graphics]) + 1;
    rhi->submitted[transfer] = std::max(rhi->submitted[transfer], rhi->completed[transfer]) + 1;

    ASSERT_TRUE(graph.Reset());

    device->CollectCompletedResources();

    EXPECT_FALSE(destroyed.contains(sourceId));

    rhi->completed[graphics] = rhi->submitted[graphics];
    device->CollectCompletedResources();

    EXPECT_FALSE(destroyed.contains(targetId));

    rhi->completed[transfer] = rhi->submitted[transfer];
    device->CollectCompletedResources();

    EXPECT_TRUE(destroyed.contains(sourceId));
    EXPECT_TRUE(destroyed.contains(targetId));
    EXPECT_TRUE(graph.Reset());
}

TEST_F(RenderCoreTest, ExecutorWithoutRetirementDeviceRejectsRecording)
{
    TestBuffer* source = Buffer();
    TestBuffer* target = Buffer();
    RenderGraph graph("missing_retirement_device");
    graph.Begin();
    graph.AddTransferPass("copy").CopyBuffer(source, target, {0, 0, 4});
    ASSERT_TRUE(graph.End());
    RDGExecutor executor;
    RHICommandList list;
    EXPECT_FALSE(executor.Execute(&graph, &list));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eLifecycle);
    EXPECT_EQ(list.GetCommandCount(), 0u);
    graph.Reset();
    EXPECT_EQ(source->GetRefCount(), 1u);
    device->DestroyBuffer(source);
    device->DestroyBuffer(target);
}

TEST_F(RenderCoreTest, OwnerRetirementPreservesHazardsForGraphReplay)
{
    rhi->shared = true;
    CreateTestShaderProgram(device, "retained_writer");
    TestBuffer* buffer = Buffer();
    const uint64_t id  = buffer->GetStableId();
    RenderGraph graph("retained_writer");
    graph.Begin();

    RDGComputePassDesc desc;
    desc.SetShaderProgramName("retained_writer");
    desc.BindStorageBuffer("buffer", buffer);
    graph.AddComputePass(desc).RecordPassCommands(
        [](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });

    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));

    device->DestroyBuffer(buffer);
    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    ASSERT_FALSE(destroyed.contains(id));

    rhi->graphics.barrierBatches.clear();

    ASSERT_TRUE(device->ExecuteRenderGraph(graph));

    bool previousWriter = false;

    for (TestContext::BarrierBatch const& batch : rhi->graphics.barrierBatches)
    {
        for (const RHIBufferTransition& transition : batch.buffers)
        {
            previousWriter |= transition.pBuffer == buffer &&
                transition.oldAccessMode == RHIAccessMode::eReadWrite;
        }
    }

    EXPECT_TRUE(previousWriter);

    graph.Reset();
    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    EXPECT_TRUE(destroyed.contains(id));
}

TEST_F(RenderCoreTest, GraphRetainsViewsBackingTexturesAndSamplers)
{
    CreateTestShaderProgram(device, "sample_import");
    RHITexture* texture = Texture(2);
    TextureViewFormat format{};
    format.format            = texture->GetFormat();
    format.baseMipLevel      = 1;
    format.mipmaps           = 1;
    RHITextureView* view     = device->CreateTextureView(texture, format, "retained_view");
    RHISampler* sampler      = rhi->CreateSampler({});
    const uint64_t textureId = texture->GetStableId();
    const uint64_t viewId    = view->GetStableId();
    const uint64_t samplerId = sampler->GetStableId();
    RenderGraph graph("retained_views");
    graph.Begin();

    RDGComputePassDesc desc;
    desc.SetShaderProgramName("sample_import");
    desc.BindSampledTexture("texture", sampler, view);
    graph.AddComputePass(desc);

    ASSERT_TRUE(graph.End());
    EXPECT_EQ(view->GetRefCount(), 2u);
    EXPECT_EQ(texture->GetRefCount(), 2u);

    sampler->ReleaseReference();
    device->DestroyTexture(texture);
    device->CollectCompletedResources();

    ASSERT_FALSE(destroyed.contains(textureId));
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(view->GetTexture(), texture);

    graph.Reset();
    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    EXPECT_TRUE(destroyed.contains(textureId));
    EXPECT_TRUE(destroyed.contains(viewId));
    EXPECT_TRUE(destroyed.contains(samplerId));
}

TEST_F(RenderCoreTest, ResourceValuesRejectReuseAndForeignGraphsBeforeDereference)
{
    CreateTestShaderProgram(device, "handle_validation");
    RHITexture* texture = Texture();
    RenderGraph first("first"), second("second");
    first.Begin();
    RDGTexture old = first.GetResourceManager()->ImportTexture(texture);
    ASSERT_TRUE(old);
    ASSERT_TRUE(first.End());

    first.Begin();
    RDGTexture current = first.GetResourceManager()->ImportTexture(texture);
    EXPECT_NE(old, current);

    RDGComputePassDesc desc;
    desc.SetShaderProgramName("handle_validation");
    desc.BindSampledTexture("texture", nullptr, old);
    first.AddComputePass(desc);

    EXPECT_FALSE(first.End());
    EXPECT_EQ(first.GetResult().code, RDGErrorCode::eLifecycle);
    EXPECT_FALSE(device->ExecuteRenderGraph(first));

    second.Begin();
    desc.resourceBindings.clear();
    desc.BindSampledTexture("texture", nullptr, current);
    second.AddComputePass(desc);

    EXPECT_FALSE(second.End());
    EXPECT_EQ(second.GetResult().code, RDGErrorCode::eLifecycle);
    ASSERT_TRUE(first.Reset());
    ASSERT_TRUE(first.Begin());

    current = first.GetResourceManager()->ImportTexture(texture);
    desc.resourceBindings.clear();
    desc.BindSampledTexture("texture", nullptr, current);
    first.AddComputePass(desc);

    ASSERT_TRUE(first.End());
    EXPECT_TRUE(device->ExecuteRenderGraph(first));

    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, ResourceValuesRejectDestroyedGraphAtSameAddress)
{
    alignas(RenderGraph) std::byte storage[sizeof(RenderGraph)];
    RHITexture* texture = Texture();
    RenderGraph* first  = new (storage) RenderGraph("first_lifetime");
    first->Begin();
    const RDGTexture stale = first->GetResourceManager()->ImportTexture(texture);
    first->~RenderGraph();
    RenderGraph* second = new (storage) RenderGraph("second_lifetime");
    second->Begin();
    const RDGTexture current = second->GetResourceManager()->ImportTexture(texture);
    EXPECT_NE(stale, current);
    EXPECT_FALSE(second->GetResourceManager()->IsValid(stale));
    EXPECT_EQ(second->GetResult().code, RDGErrorCode::eLifecycle);
    second->~RenderGraph();
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, ShaderReplacementInvalidatesRecordedAndCompiledGraphs)
{
    for (bool reinitialize : {false, true})
    {
        for (bool compiled : {false, true})
        {
            SCOPED_TRACE(compiled);
            SCOPED_TRACE(reinitialize);
            ShaderProgram* program = CreateTestShaderProgram(device, "reload");
            const uint64_t oldId   = program->GetShader()->GetStableId();
            RenderGraph graph("reload");
            graph.Begin();

            RDGComputePassDesc desc;
            desc.SetShaderProgramName("reload");
            graph.AddComputePass(desc);

            ASSERT_TRUE(graph.End());

            if (compiled)
            {
                ASSERT_TRUE(device->ExecuteRenderGraph(graph));
            }

            const uint32_t finalized   = rhi->finalizedLists;
            ShaderProgram* replacement = program;

            if (reinitialize)
            {
                ASSERT_TRUE(program->Init());
            }
            else
            {
                replacement = CreateTestShaderProgram(device, "reload");
            }

            EXPECT_NE(replacement->GetShader()->GetStableId(), oldId);
            EXPECT_FALSE(device->ExecuteRenderGraph(graph));
            EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eShader);
            EXPECT_EQ(rhi->finalizedLists, finalized);
            ASSERT_TRUE(graph.Begin());

            graph.AddComputePass(desc);

            ASSERT_TRUE(graph.End());
            EXPECT_TRUE(device->ExecuteRenderGraph(graph));
            EXPECT_EQ(rhi->createdPipelines.back()->GetShader(), replacement->GetShader());
        }
    }
}

TEST_F(RenderCoreTest, ProductionShaderReinitializationAndDestructionAreIdempotent)
{
    ShaderProgram* program = CreateTestShaderProgram(device, "reinit");
    std::vector<uint64_t> ids;

    for (uint32_t i = 0; i < 3; ++i)
    {
        ids.push_back(program->GetShader()->GetStableId());
        ASSERT_TRUE(program->Init());
        EXPECT_EQ(program->GetStorageBufferSRDs().size(), 3u);
        EXPECT_EQ(program->GetSampledTextureSRDs().size(), 3u);
        EXPECT_EQ(program->GetStorageImageSRDs().size(), 2u);
        EXPECT_NE(program->GetShaderResourceDescriptor("buffer"), nullptr);
    }

    ids.push_back(program->GetShader()->GetStableId());
    rhi->failShaderCreation = true;

    EXPECT_FALSE(program->Init());
    EXPECT_EQ(program->GetShader()->GetStableId(), ids.back());

    rhi->failShaderCreation = false;
    ShaderProgramManager::GetInstance().Destroy();
    ShaderProgramManager::GetInstance().Destroy();

    EXPECT_EQ(ShaderProgramManager::GetInstance().RequestShaderProgram("reinit"), nullptr);

    device->CollectCompletedResources();

    for (uint64_t id : ids)
    {
        EXPECT_TRUE(destroyed.contains(id));
    }

    EXPECT_NE(CreateTestShaderProgram(device, "reinit"), nullptr);
}

TEST_F(RenderCoreTest, ProductionTextureCacheSeparatesMipPolicyAndRetiresEveryInstance)
{
    asset::TextureInfo input{};
    input.width = input.height = 2;
    input.data.resize(16, 81);
    textureFiles["test_texture.png"] = input;
    StagingBufferManager staging(1024, 4096);
    StagingUploadQueue uploads(device, &staging);
    TextureManager textures(device, &uploads);
    RHITexture* plain  = textures.LoadTexture2D("./test_texture.png", false);
    RHITexture* mipped = textures.LoadTexture2D("test_texture.png", true);
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(mipped, nullptr);
    EXPECT_EQ(textures.LoadTexture2D("test_texture.png", false), plain);
    EXPECT_EQ(textures.LoadTexture2D("./test_texture.png", true), mipped);
    EXPECT_NE(plain, mipped);
    EXPECT_EQ(plain->GetNumMipmaps(), 1u);
    EXPECT_EQ(mipped->GetNumMipmaps(), 2u);

    const uint64_t plainId = plain->GetStableId(), mippedId = mipped->GetStableId();
    EXPECT_EQ(textures.LoadTexture2D("missing.png"), nullptr);

    textures.Destroy();
    textures.Destroy();
    uploads.Destroy();
    staging.Destroy();
    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    EXPECT_TRUE(destroyed.contains(plainId));
    EXPECT_TRUE(destroyed.contains(mippedId));
}

TEST_F(RenderCoreTest, ProductionSceneTextureNamesDoNotOverwriteOwners)
{
    sg::Scene scene;

    for (uint32_t i = 0; i < 2; ++i)
    {
        UniquePtr<sg::Texture> texture = MakeUnique<sg::Texture>("duplicate");
        texture->width = texture->height = 2;
        texture->bytesData.resize(16, uint8_t(i + 1));
        scene.AddComponent(std::move(texture));
    }

    StagingBufferManager staging(1024, 4096);
    StagingUploadQueue uploads(device, &staging);
    TextureManager textures(device, &uploads);
    std::vector<RHITexture*> first, second;
    textures.LoadSceneTextures(&scene, first);
    textures.LoadSceneTextures(&scene, second);

    ASSERT_EQ(first.size(), 2u);
    ASSERT_EQ(second.size(), 2u);

    std::unordered_set<uint64_t> ids;

    for (RHITexture* texture : {first[0], first[1], second[0], second[1]})
    {
        ASSERT_NE(texture, nullptr);
        ids.insert(texture->GetStableId());
    }

    EXPECT_EQ(ids.size(), 4u);

    textures.Destroy();
    uploads.Destroy();
    staging.Destroy();
    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    for (uint64_t id : ids)
    {
        EXPECT_TRUE(destroyed.contains(id));
    }
}

TEST_F(RenderCoreEnvironmentTest, EnvironmentReplacementRetainsAndRetiresAllOutputs)
{
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "zen_phase1c_environment.dds";
    gli::texture_cube cube(gli::FORMAT_RGBA16_SFLOAT_PACK16, {2, 2}, 1);
    std::memset(cube.data(), 0, cube.size());

    ASSERT_TRUE(gli::save(cube, file.string()));

    StagingBufferManager staging(1024, 4096);
    StagingUploadQueue uploads(device, &staging);
    TextureManager textures(device, &uploads);
    EnvTexture environment{};
    std::unordered_set<uint64_t> ids;

    for (uint32_t i = 0; i < 2; ++i)
    {
        textures.LoadTextureEnv(file.string(), &environment);

        for (RHITexture* texture : {environment.pSkybox, environment.pIrradiance,
                                    environment.pPrefiltered, environment.pLutBRDF})
        {
            ASSERT_NE(texture, nullptr);
            ids.insert(texture->GetStableId());
        }
    }

    EXPECT_EQ(ids.size(), 8u);

    for (uint64_t id : ids)
    {
        EXPECT_FALSE(destroyed.contains(id));
    }

    textures.Destroy();
    textures.Destroy();
    uploads.Destroy();
    staging.Destroy();
    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    for (uint64_t id : ids)
    {
        EXPECT_TRUE(destroyed.contains(id));
    }

    std::filesystem::remove(file);
}

// Exercise the actual shared allocation/reset contract without loading a scene or GPU shaders.
class TestVoxelVolumes : public VoxelizerBase
{
public:
    TestVoxelVolumes(RenderDevice* device, DataFormat format, bool radianceInputs = false) :
        VoxelizerBase(device, nullptr), m_radianceInputs(radianceInputs)
    {
        m_voxelTexResolution = 8;
        m_voxelTexFormat     = format;
    }

    void Init() override
    {
        PrepareTextures();
    }

    bool BeginVolumeUpdate(RenderGraph& graph)
    {
        return BeginVoxelization(graph);
    }

    bool ProducesRadianceInputs() const override
    {
        return m_radianceInputs;
    }

private:
    bool m_radianceInputs;

    void BuildRenderGraph() override {}
};

TEST_F(RenderCoreTest, FirstBufferWritesSkipEmptyBarriersButKeepLaterDependencies)
{
    RDGMetrics& metrics = device->GetRDGMetrics();
    metrics.SetSink({});
    CreateTestShaderProgram(device, "first_buffer_write");
    std::array<RHIBuffer*, 3> buffers{};

    for (size_t i = 0; i < buffers.size(); ++i)
    {
        RHIBufferCreateInfo info{};
        info.size = 64;
        info.tag  = "output_" + std::to_string(i);
        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer,
                                 RHIBufferUsageFlagBits::eVertexBuffer);
        buffers[i] = rhi->CreateBuffer(info);
    }

    RenderGraph graph("new_buffers");
    graph.Begin();

    for (RHIBuffer* buffer : buffers)
    {
        RDGComputePassDesc writer{};
        writer.SetShaderProgramName("first_buffer_write");
        writer.BindStorageBuffer("buffer", buffer);
        graph.AddComputePass(writer);
    }

    graph.End();
    device->ExecuteRenderGraph(graph);

    EXPECT_TRUE(rhi->graphics.bufferTransitions.empty());
    EXPECT_EQ(metrics.GetLastSnapshot().issues[size_t(RDGMetricIssue::eRedundantBarrier)], 0u);
    EXPECT_EQ(metrics.GetLastSnapshot().issues[size_t(RDGMetricIssue::eUnknownImportState)], 0u);

    graph.Begin();

    RDGGraphicsPassDesc reader{};
    reader.SetShaderProgramName("first_buffer_write");
    reader.BindVertexBuffer(buffers[0]);
    graph.AddGraphicsPass(reader);
    graph.End();
    device->ExecuteRenderGraph(graph);

    ASSERT_EQ(rhi->graphics.barrierBatches.size(), 1u);

    const TestContext::BarrierBatch& batch = rhi->graphics.barrierBatches.front();
    EXPECT_TRUE(batch.source.HasFlag(RHIPipelineStageFlagBits::eComputeShader));
    EXPECT_TRUE(batch.destination.HasFlag(RHIPipelineStageFlagBits::eVertexInput));
    ASSERT_EQ(batch.buffers.size(), 1u);
    EXPECT_EQ(batch.buffers.front().oldUsage, RHIBufferUsage::eStorageBuffer);
    EXPECT_EQ(batch.buffers.front().oldAccessMode, RHIAccessMode::eReadWrite);
    EXPECT_EQ(batch.buffers.front().newUsage, RHIBufferUsage::eVertexBuffer);

    for (RHIBuffer* buffer : buffers)
    {
        device->DestroyBuffer(buffer);
    }
}

TEST_F(RenderCoreTest, TextureClearsRequireGraphicsCapability)
{
    RHITexture* texture = Texture();
    RenderGraph graph("clear_only");
    graph.Begin();
    graph.AddTransferPass("clear").ClearTexture(texture, Color(0.0f));
    graph.End();
    const uint64_t transferSerial = rhi->submitted[2];
    device->ExecuteRenderGraph(graph);
    EXPECT_EQ(rhi->submitted[2], transferSerial);
    EXPECT_TRUE(rhi->transfer.textureClears.empty());
    ASSERT_EQ(rhi->graphics.textureClears.size(), 1u);
    EXPECT_EQ(rhi->graphics.textureClears.front().texture, texture);
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, VoxelVolumesAreZeroedBeforeFirstAndRequestedVoxelizations)
{
    RDGMetrics& metrics          = device->GetRDGMetrics();
    RDGMetricsOptions options    = metrics.GetOptions();
    options.logging.sampleEvery  = 1;
    options.logging.minInterval  = std::chrono::milliseconds(0);
    options.includeTransferNodes = true;
    metrics.Configure(options);
    metrics.SetSink({});
    CreateTestShaderProgram(device, "volume_writer");

    // Geometry atomics use packed UINT storage; compute uses normalized RGBA storage.
    for (const std::pair<DataFormat, bool> configuration : {std::pair{DataFormat::eR32UInt, false},
                                                            {DataFormat::eR8G8B8A8UNORM, false},
                                                            {DataFormat::eR32UInt, true},
                                                            {DataFormat::eR8G8B8A8UNORM, true}})
    {
        TestVoxelVolumes volumes(device, configuration.first, configuration.second);
        volumes.Init();
        const VoxelTextures& textures = volumes.GetVoxelTextures();
        std::vector<RHITexture*> all{textures.pAlbedo};

        if (configuration.second)
        {
            all.push_back(textures.pNormal);
            all.push_back(textures.pEmissive);
        }

        const size_t volumeCount = all.size();

        for (RHITexture* texture : all)
        {
            EXPECT_EQ(texture->GetBaseInfo().type, RHITextureType::e3D);
            EXPECT_TRUE(
                texture->GetBaseInfo().usageFlags.HasFlag(RHITextureUsageFlagBits::eTransferDst));
        }

        rhi->graphics.textureClears.clear();
        rhi->graphics.clearsBeforeDraw.clear();
        rhi->graphics.clearsBeforeDispatch.clear();
        RenderGraph graph("voxel_volume_lifecycle");

        for (int frame = 0; frame < 3; ++frame)
        {
            if (frame == 2)
            {
                volumes.RequestVoxelization();
            }

            graph.Begin();

            EXPECT_EQ(volumes.BeginVolumeUpdate(graph), frame != 1);
            // A second request in the same recording must not reset a producer's results.
            EXPECT_FALSE(volumes.BeginVolumeUpdate(graph));

            for (RHITexture* texture : all)
            {
                if (configuration.first == DataFormat::eR32UInt)
                {
                    RDGGraphicsPassDesc pass{};
                    pass.SetShaderProgramName("volume_writer");
                    pass.BindStorageImage("image", texture->GetDefaultView());
                    graph.AddGraphicsPass(pass).RecordPassCommands(
                        [](RDGPassCmdEncoder& encoder) { encoder.Draw(1, 1); });
                }
                else
                {
                    RDGComputePassDesc pass{};
                    pass.SetShaderProgramName("volume_writer");
                    pass.BindStorageImage("image", texture->GetDefaultView());
                    graph.AddComputePass(pass).RecordPassCommands(
                        [](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
                }
            }

            graph.End();
            rhi->graphics.barrierBatches.clear();
            device->ExecuteRenderGraph(graph);
            const size_t expectedClears = (frame == 2 ? 2 : 1) * volumeCount;
            ASSERT_EQ(rhi->graphics.textureClears.size(), expectedClears);

            const std::vector<size_t>& observations = configuration.first == DataFormat::eR32UInt ?
                rhi->graphics.clearsBeforeDraw :
                rhi->graphics.clearsBeforeDispatch;
            ASSERT_EQ(observations.size(), (frame + 1) * volumeCount);

            for (size_t i = observations.size() - volumeCount; i < observations.size(); ++i)
            {
                EXPECT_EQ(observations[i], expectedClears);
            }

            for (size_t i = 0; i < expectedClears; ++i)
            {
                const TestContext::TextureClear& clear = rhi->graphics.textureClears[i];
                EXPECT_EQ(clear.texture, all[i % volumeCount]);
                EXPECT_EQ(clear.color, Color(0.0f)); // Alpha must be zero too.
                EXPECT_EQ(clear.range.baseMipLevel, 0u);
                EXPECT_EQ(clear.range.levelCount, 1u);
                EXPECT_EQ(clear.range.baseArrayLayer, 0u);
                EXPECT_EQ(clear.range.layerCount, 1u);
            }

            if (frame != 1)
            {
                std::unordered_set<RHITexture*> synchronized;

                for (TestContext::BarrierBatch const& batch : rhi->graphics.barrierBatches)
                {
                    for (const RHITextureTransition& barrier : batch.textures)
                    {
                        if (barrier.oldUsage == RHITextureUsage::eTransferDst &&
                            barrier.newUsage == RHITextureUsage::eStorage)
                        {
                            EXPECT_TRUE(batch.source.HasFlag(RHIPipelineStageFlagBits::eTransfer));
                            EXPECT_TRUE(batch.destination.HasFlag(
                                configuration.first == DataFormat::eR32UInt ?
                                    RHIPipelineStageFlagBits::eFragmentShader :
                                    RHIPipelineStageFlagBits::eComputeShader));
                            synchronized.insert(barrier.pTexture);
                        }
                    }
                }

                EXPECT_EQ(synchronized.size(), volumeCount);
            }

            for (RDGMetricIssue issue :
                 {RDGMetricIssue::eMissingBarrier, RDGMetricIssue::eStageCoverage,
                  RDGMetricIssue::eAccessCoverage, RDGMetricIssue::eUnknownImportState})
            {
                EXPECT_EQ(metrics.GetLastSnapshot().issues[size_t(issue)], 0u)
                    << RDGMetrics::Format(metrics.GetLastSnapshot());
            }
        }

        volumes.Destroy();
    }
}

void RenderCoreTest::AllocateRendererInputs(int epoch,
                                            TestViewport& viewport,
                                            std::vector<RHIBuffer*>& ownedBuffers,
                                            std::vector<RHITexture*>& ownedTextures,
                                            RenderConfig& config)
{
    sceneInputs.vertices  = Buffer();
    sceneInputs.indices   = Buffer();
    sceneInputs.nodes     = Buffer();
    sceneInputs.materials = Buffer();

    for (RHIBuffer* buffer :
         {sceneInputs.vertices, sceneInputs.indices, sceneInputs.nodes, sceneInputs.materials})
    {
        ownedBuffers.push_back(buffer);
    }

    sceneInputs.textures.clear();

    for (int i = 0; i < 2 - epoch; ++i)
    {
        RHITexture* texture = Texture();
        ownedTextures.push_back(texture);
        sceneInputs.textures.push_back(texture);
    }

    for (RHITexture** texture :
         {&sceneInputs.environment.pSkybox, &sceneInputs.environment.pIrradiance,
          &sceneInputs.environment.pPrefiltered, &sceneInputs.environment.pLutBRDF})
    {
        *texture = Texture();
        ownedTextures.push_back(*texture);
    }

    RHITextureCreateInfo colorInfo{};
    colorInfo.format = DataFormat::eR8G8B8A8UNORM;
    colorInfo.type   = RHITextureType::e2D;
    colorInfo.width  = 8 + epoch * 8;
    colorInfo.height = 8 + epoch * 4;
    colorInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eColorAttachment);
    viewport.color = rhi->CreateTexture(colorInfo);

    RHITextureCreateInfo depthInfo{};
    depthInfo.format = DataFormat::eD32SFloat;
    depthInfo.type   = RHITextureType::e2D;
    depthInfo.width  = colorInfo.width;
    depthInfo.height = colorInfo.height;
    depthInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eDepthStencilAttachment);
    viewport.depth = rhi->CreateTexture(depthInfo);
    ownedTextures.push_back(viewport.color);
    ownedTextures.push_back(viewport.depth);
    viewport.Resize(8 + epoch * 8, 8 + epoch * 4);
    config.offScreenFbSize = 4 + epoch * 4;
}

static bool IsResourceBound(const TestContext& context, RHIResource* resource)
{
    return std::find(context.boundResources.begin(), context.boundResources.end(), resource) !=
        context.boundResources.end();
}

template <typename T> static bool HasShaderValue(const TestContext& context, const T& value)
{
    return std::any_of(context.values.begin(), context.values.end(),
                       [&value](const std::vector<uint8_t>& bytes) {
                           return bytes.size() == sizeof(value) &&
                               std::memcmp(bytes.data(), &value, sizeof(value)) == 0;
                       });
}

TEST_F(RenderCoreTest, RenderersRebuildCurrentBindingsTargetsAndSnapshotDrawData)
{
    CreateTestShaderProgram(device, "intent");

    for (const std::array<const char*, 3>& shaderFiles :
         {std::array<const char*, 3>{"GBufferSP", "SceneRenderer/offscreen.vert.spv",
                                     "SceneRenderer/offscreen.frag.spv"},
          {"DeferredLightingSP", "SceneRenderer/deferred.vert.spv",
           "SceneRenderer/deferred.frag.spv"},
          {"SkyboxRenderSP", "Environment/skybox.vert.spv", "Environment/skybox.frag.spv"},
          {"EnvMapIrradianceSP", "Environment/filtercube.vert.spv",
           "Environment/irradiancecube.frag.spv"},
          {"EnvMapPrefilteredSP", "Environment/filtercube.vert.spv",
           "Environment/prefilterenvmap.frag.spv"},
          {"EnvMapBRDFLutGenSP", "Environment/genbrdflut.vert.spv",
           "Environment/genbrdflut.frag.spv"}})
    {
        RHIShaderCreateInfo info{};
        info.stageFlags.SetFlags(RHIShaderStageFlagBits::eVertex,
                                 RHIShaderStageFlagBits::eFragment);
        info.spirvFileName[ToUnderlying(RHIShaderStage::eVertex)]   = shaderFiles[1];
        info.spirvFileName[ToUnderlying(RHIShaderStage::eFragment)] = shaderFiles[2];
        reflectedShaderInfos[shaderFiles[0]]                        = info;
        CreateTestShaderProgram(device, shaderFiles[0]);
    }

    RDGMetrics& metrics         = device->GetRDGMetrics();
    RDGMetricsOptions options   = metrics.GetOptions();
    options.logging.sampleEvery = 1;
    options.logging.minInterval = std::chrono::milliseconds(0);
    metrics.Configure(options);
    metrics.SetSink({});
    RenderConfig& config                 = RenderConfig::GetInstance();
    const uint32_t originalOffscreenSize = config.offScreenFbSize;

    struct RestoreConfig
    {
        uint32_t size;

        ~RestoreConfig()
        {
            RenderConfig::GetInstance().offScreenFbSize = size;
        }
    } restore{originalOffscreenSize};

    TestViewport viewport;
    std::vector<RHIBuffer*> ownedBuffers;
    std::vector<RHITexture*> ownedTextures;

    AllocateRendererInputs(0, viewport, ownedBuffers, ownedTextures, config);
    sg::Scene source;
    sg::Material material("material");
    sg::SubMesh mesh("mesh", 0, 3, 3);
    mesh.SetMaterial(0, &material);
    sg::Mesh meshes("meshes");
    meshes.AddSubMesh(&mesh);
    sg::Node node(0, "node");
    node.AddComponent(&meshes);
    source.AddRenderableNode(&node);
    SceneData data{};
    data.pScene = &source;
    RenderScene scene(device, data);
    DeferredLightingRenderer lighting(device, &viewport);
    SkyboxRenderer skybox(device, &viewport);
    lighting.Init();
    skybox.Init();
    lighting.SetRenderScene(&scene);
    skybox.SetRenderScene(&scene);

    std::vector<RHIResource*> priorBindings;
    uint32_t pipelinesAfterResize = 0;

    for (int frame = 0; frame < 3; ++frame)
    {
        if (frame == 1)
        {
            AllocateRendererInputs(1, viewport, ownedBuffers, ownedTextures, config);
            scene.PrepareBuffers();
            scene.LoadSceneTextures();
            // No SetRenderScene or renderer resize/update callback: same scene, new handles.
        }

        material.index = 10 + frame;
        node.SetData(20 + frame, Mat4(1.0f));
        mesh.SetFirstIndex(frame * 3);
        sceneInputs.camera.projViewMatrix[0][0] = 30.f + frame;
        sceneInputs.uniforms.viewPos.x          = 40.f + frame;
        const sg::CameraUniformData camera      = sceneInputs.camera;
        const SceneUniformData uniforms         = sceneInputs.uniforms;
        RenderGraph* graph                      = device->GetCurrentFrameRDG();

        if (frame < 2)
        {
            skybox.PreprocessEnvTexture(&sceneInputs.environment);

            for (RHITexture* texture :
                 {sceneInputs.environment.pIrradiance, sceneInputs.environment.pPrefiltered,
                  sceneInputs.environment.pLutBRDF})
            {
                ownedTextures.push_back(texture);
            }

            scene.LoadSceneTextures();
        }

        graph->Begin();
        skybox.BuildRenderGraph();
        lighting.BuildRenderGraph();

        if (frame == 0)
        {
            graph->AddComputePass(IntentPass("reject_initial_frame"))
                .RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                    encoder.Fail(RDGErrorCode::eCallback, "simulated frame failure");
                });
            ASSERT_TRUE(graph->End());
            EXPECT_FALSE(device->ExecuteRenderGraph(*graph));
            skybox.OnRenderGraphExecuted(false);
            // Rebuild without loading/preprocessing the environment again.
            ASSERT_TRUE(graph->Begin());
            skybox.BuildRenderGraph();
            lighting.BuildRenderGraph();
        }

        ASSERT_TRUE(graph->End());

        // A completed graph must retain its own values and draw ranges through execution.
        material.index = 999;
        node.SetData(999, Mat4(1.0f));
        mesh.SetFirstIndex(999);
        sceneInputs.camera.projViewMatrix[0][0] = 999;
        sceneInputs.uniforms.viewPos.x          = 999;
        rhi->graphics.boundResources.clear();
        rhi->graphics.vertexBuffers.clear();
        rhi->graphics.renderingLayouts.clear();
        rhi->graphics.renderingCommandOffsets.clear();
        rhi->graphics.values.clear();
        rhi->graphics.pushConstants.clear();
        rhi->graphics.indexedDraws.clear();

        ASSERT_TRUE(device->ExecuteRenderGraph(*graph)) << graph->GetResult().message;

        skybox.OnRenderGraphExecuted(true);

        for (RDGResult const& warning : graph->GetWarnings())
        {
            EXPECT_EQ(warning.message.find("env_"), std::string::npos) << warning.message;
        }

        const TestContext& context        = rhi->graphics;
        const uint32_t preprocessingDraws = frame < 2 ? 6 * (7 + 10) + 1 : 0;
        EXPECT_EQ(metrics.GetLastSnapshot().nodeCount, 3u + (frame < 2 ? 2 * 6 * (7 + 10) + 1 : 0));
        ASSERT_EQ(context.renderingLayouts.size(), 3u + preprocessingDraws);

        uint32_t viewportPasses = 0;
        size_t offscreenIndex   = context.renderingLayouts.size();

        for (size_t index = 0; index < context.renderingLayouts.size(); ++index)
        {
            const RHIRenderingLayout& layout = context.renderingLayouts[index];

            if (layout.numColorRenderTargets == 5)
            {
                offscreenIndex = index;
            }

            if (layout.colorRenderTargets[0].pTexture != viewport.color)
            {
                continue;
            }

            ++viewportPasses;
            EXPECT_EQ(layout.depthStencilRenderTarget.pTexture, viewport.depth);
            EXPECT_EQ(layout.renderArea.maxX, viewport.GetWidth());
            EXPECT_EQ(layout.renderArea.maxY, viewport.GetHeight());
        }

        EXPECT_EQ(viewportPasses, 2u);
        ASSERT_LT(offscreenIndex, context.renderingLayouts.size());

        const RHIRenderingLayout& offscreen = context.renderingLayouts[offscreenIndex];
        EXPECT_EQ(offscreen.numColorRenderTargets, 5u);
        EXPECT_EQ(offscreen.renderArea.maxX, config.offScreenFbSize);
        EXPECT_EQ(offscreen.colorRenderTargets[0].pTexture->GetWidth(), config.offScreenFbSize);
        EXPECT_NE(std::find(context.vertexBuffers.begin(), context.vertexBuffers.end(),
                            sceneInputs.vertices),
                  context.vertexBuffers.end());

        EXPECT_TRUE(IsResourceBound(context, sceneInputs.nodes));
        EXPECT_TRUE(IsResourceBound(context, sceneInputs.materials));

        for (RHITexture* texture : sceneInputs.textures)
        {
            EXPECT_TRUE(IsResourceBound(context, texture->GetDefaultView()));
        }

        for (RHITexture* texture :
             {sceneInputs.environment.pSkybox, sceneInputs.environment.pIrradiance,
              sceneInputs.environment.pPrefiltered, sceneInputs.environment.pLutBRDF})
        {
            EXPECT_TRUE(IsResourceBound(context, texture->GetDefaultView()));
        }

        if (frame == 1)
        {
            for (RHIResource* previous : priorBindings)
            {
                EXPECT_FALSE(IsResourceBound(context, previous));
            }

            pipelinesAfterResize = rhi->pipelineCount;
        }
        else if (frame == 2)
        {
            EXPECT_EQ(rhi->pipelineCount, pipelinesAfterResize);
        }

        priorBindings = {sceneInputs.nodes, sceneInputs.materials};

        for (RHITexture* texture : sceneInputs.textures)
        {
            priorBindings.push_back(texture->GetDefaultView());
        }

        for (RHITexture* texture :
             {sceneInputs.environment.pSkybox, sceneInputs.environment.pIrradiance,
              sceneInputs.environment.pPrefiltered, sceneInputs.environment.pLutBRDF})
        {
            priorBindings.push_back(texture->GetDefaultView());
        }

        EXPECT_TRUE(HasShaderValue(context, camera));
        EXPECT_TRUE(HasShaderValue(context, uniforms));
        ASSERT_EQ(context.pushConstants.size(), 1u + (frame < 2 ? 6 * (7 + 10) : 0));

        GBufferSP::PushConstantsData constants{};
        const std::pair<size_t, size_t> pushIndexResult =
            context.renderingCommandOffsets[offscreenIndex];
        size_t pushIndex = pushIndexResult.first;
        size_t drawIndex = pushIndexResult.second;
        ASSERT_LT(pushIndex, context.pushConstants.size());
        ASSERT_EQ(context.pushConstants[pushIndex].size(), sizeof(constants));

        std::memcpy(&constants, context.pushConstants[pushIndex].data(), sizeof(constants));

        EXPECT_EQ(constants.nodeIndex, 20 + frame);
        EXPECT_EQ(constants.materialIndex, 10 + frame);
        ASSERT_EQ(context.indexedDraws.size(), 2u + (frame < 2 ? 6 * (7 + 10) : 0));
        ASSERT_LT(drawIndex, context.indexedDraws.size());
        EXPECT_EQ(context.indexedDraws[drawIndex][0], frame * 3);
    }

    skybox.Destroy();
    lighting.Destroy();

    for (RHITexture* texture : ownedTextures)
    {
        device->DestroyTexture(texture);
    }

    for (RHIBuffer* buffer : ownedBuffers)
    {
        device->DestroyBuffer(buffer);
    }
}

TEST_F(RenderCoreTest, AlbedoVoxelizersDoNotAllocateOrScheduleRadiance)
{
    RDGMetrics& metrics         = device->GetRDGMetrics();
    RDGMetricsOptions options   = metrics.GetOptions();
    options.logging.sampleEvery = 1;
    options.logging.minInterval = std::chrono::milliseconds(0);
    metrics.Configure(options);
    metrics.SetSink({});

    for (DataFormat format : {DataFormat::eR32UInt, DataFormat::eR8G8B8A8UNORM})
    {
        const uint32_t before = rhi->textureCreations;
        TestVoxelVolumes volumes(device, format);
        volumes.Init();

        EXPECT_FALSE(volumes.ProducesRadianceInputs());
        EXPECT_EQ(rhi->textureCreations, before + 1);
        EXPECT_NE(volumes.GetVoxelTextures().pAlbedo, nullptr);
        EXPECT_NE(volumes.GetVoxelTextures().pAlbedoView, nullptr);
        EXPECT_EQ(volumes.GetVoxelTextures().pNormal, nullptr);
        EXPECT_EQ(volumes.GetVoxelTextures().pNormalView, nullptr);
        EXPECT_EQ(volumes.GetVoxelTextures().pEmissive, nullptr);
        EXPECT_EQ(volumes.GetVoxelTextures().pEmissiveView, nullptr);

        // An albedo-only producer also keeps the standalone GI lifecycle inert.
        VoxelGIRenderer gi(device, nullptr, &volumes, nullptr);
        gi.Init();
        gi.SetRenderScene(nullptr);
        RenderGraph* graph = device->GetCurrentFrameRDG();
        graph->Begin();
        gi.BuildRenderGraph();
        graph->End();
        device->ExecuteRenderGraph(*graph);

        EXPECT_EQ(metrics.GetLastSnapshot().nodeCount, 0u);
        EXPECT_EQ(rhi->textureCreations, before + 1);

        gi.Destroy();
        volumes.Destroy();
    }
}

TEST_F(RenderCoreTest, StagingBlocksWaitForBothQueuesAndUnsubmittedAllocations)
{
    StagingBufferManager manager(16, 16);
    StagingAllocation first, second;
    ASSERT_EQ(manager.Allocate(5, 4, &first), StagingFlushAction::eNone);
    ASSERT_EQ(manager.Allocate(5, 4, &second), StagingFlushAction::eNone);
    EXPECT_EQ(second.offset, 8u);
    manager.Release(first, {2, 3});
    rhi->completed = {3, 0, 2};
    StagingAllocation blocked;
    EXPECT_EQ(manager.Allocate(16, 4, &blocked), StagingFlushAction::eFlush);
    manager.Release(second, {2, 4});
    EXPECT_EQ(manager.Allocate(16, 4, &blocked), StagingFlushAction::eFlush);
    rhi->completed[0] = 4;
    EXPECT_EQ(manager.Allocate(16, 4, &blocked), StagingFlushAction::eNone);
    EXPECT_EQ(blocked.offset, 0u);
    manager.Release(blocked, {});
    manager.Destroy();
}

TEST_F(RenderCoreTest, StagingHandlesOversizeRequests)
{
    StagingBufferManager manager(16, 32);
    StagingAllocation allocation;
    ASSERT_EQ(manager.Allocate(80, 16, &allocation), StagingFlushAction::eNone);
    EXPECT_GE(allocation.pBuffer->GetRequiredSize(), 80u);
    manager.Release(allocation, {});
    manager.Destroy();
}

TEST_F(RenderCoreTest, UploadsSnapshotInputAndFlushUnderPoolPressure)
{
    StagingBufferManager manager(16, 16);
    StagingUploadQueue queue(device, &manager);
    TestBuffer* buffer = Buffer();
    std::vector<uint8_t> input(40);

    for (size_t i = 0; i < input.size(); ++i)
    {
        input[i] = static_cast<uint8_t>(i);
    }

    std::vector<uint8_t> expected = input;
    queue.EnqueueBuffer(buffer, 4, input.size(), input.data());
    std::fill(input.begin(), input.end(), 0xFF);

    EXPECT_TRUE(queue.HasPending());

    queue.Flush();

    EXPECT_FALSE(queue.HashPending());
    EXPECT_EQ(std::vector<uint8_t>(buffer->bytes.begin() + 4, buffer->bytes.begin() + 44),
              expected);
    EXPECT_GT(rhi->submitted[2], 0u);
    EXPECT_EQ(rhi->completed[2], rhi->submitted[2]);

    queue.Destroy();
    manager.Destroy();
    device->DestroyBuffer(buffer);
}

TEST_F(RenderCoreTest, RejectedUploadDestroyReleasesEveryChunkAndTextureOwner)
{
    StagingBufferManager manager(16, 80);
    StagingUploadQueue queue(device, &manager);
    RHITexture* texture              = Texture();
    TestBuffer* buffer               = Buffer(64);
    const uint32_t textureReferences = texture->GetRefCount();
    const uint64_t bufferId          = buffer->GetStableId();
    RHIBufferTextureCopyRegion region{};
    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.textureSize = {1, 1, 1};
    std::array<uint8_t, 4> pixel{1, 2, 3, 4};
    queue.EnqueueTexture(texture, MakeVecView(&region, 1), pixel.size(), pixel.data());
    std::array<uint8_t, 64> bytes{};
    // The final chunk exceeds the destination. Rejection must cancel the entire graph,
    // including the valid texture upload and the earlier buffer chunks.
    queue.EnqueueBuffer(buffer, 4, bytes.size(), bytes.data());

    EXPECT_EQ(buffer->GetRefCount(), 5u);

    queue.Flush();

    EXPECT_TRUE(queue.HasPending());
    EXPECT_EQ(rhi->finalizedLists, 0u);
    EXPECT_EQ(buffer->bytes, (std::vector<uint8_t>(64, 0xCD)));

    device->DestroyBuffer(buffer);
    device->CollectCompletedResources();

    EXPECT_FALSE(destroyed.contains(bufferId));

    queue.Destroy();

    EXPECT_FALSE(queue.HasPending());

    device->CollectCompletedResources();

    EXPECT_TRUE(destroyed.contains(bufferId));
    EXPECT_EQ(texture->GetRefCount(), textureReferences);
    EXPECT_EQ(rhi->finalizedLists, 0u);

    queue.Destroy();
    device->CollectCompletedResources();

    EXPECT_EQ(texture->GetRefCount(), textureReferences);

    // All five staging allocations are reusable, without increasing the pool budget.
    StagingAllocation reuse;
    EXPECT_EQ(manager.Allocate(80, 16, &reuse), StagingFlushAction::eNone);

    if (reuse.pBuffer != nullptr)
    {
        manager.Release(reuse, {});
    }

    manager.Destroy();
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, CancelledUploadsPreserveSharedStagingUsersAndBothCompletionGates)
{
    StagingBufferManager manager(64, 64);
    StagingAllocation submitted, unrelated;
    ASSERT_EQ(manager.Allocate(16, 16, &submitted), StagingFlushAction::eNone);

    manager.Release(submitted, {5, 7});

    ASSERT_EQ(manager.Allocate(16, 16, &unrelated), StagingFlushAction::eNone);

    // Reject otherwise valid uploads at execution, while older work on both queues is pending.
    rhi->submitted          = {7, 0, 5};
    rhi->failSubmissionWait = true;
    device->NextFrame();
    device->NextFrame();
    StagingUploadQueue queue(device, &manager);
    TestBuffer* buffer       = Buffer(16);
    RHITexture* texture      = Texture();
    const uint64_t bufferId  = buffer->GetStableId();
    const uint64_t textureId = texture->GetStableId();
    std::array<uint8_t, 16> bytes{};
    RHIBufferTextureCopyRegion region{};
    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.textureSize = {1, 1, 1};
    queue.EnqueueBuffer(buffer, 0, bytes.size(), bytes.data());
    queue.EnqueueTexture(texture, MakeVecView(&region, 1), 4, bytes.data());
    queue.Flush();

    EXPECT_TRUE(queue.HasPending());

    device->DestroyBuffer(buffer);
    device->DestroyTexture(texture);
    queue.Destroy();

    EXPECT_FALSE(queue.HasPending());
    EXPECT_EQ(rhi->finalizedLists, 0u);
    EXPECT_EQ(rhi->deviceIdleWaits, 0u);

    rhi->completed = {7, 0, 4};
    device->CollectCompletedResources();

    EXPECT_FALSE(destroyed.contains(bufferId));
    EXPECT_FALSE(destroyed.contains(textureId));

    StagingAllocation reuse;
    EXPECT_EQ(manager.Allocate(64, 16, &reuse), StagingFlushAction::eFlush);

    rhi->completed[2] = 5;
    device->CollectCompletedResources();

    EXPECT_TRUE(destroyed.contains(bufferId));
    EXPECT_TRUE(destroyed.contains(textureId));
    // Cancellation cannot release another caller's unsubmitted allocation in the same block.
    EXPECT_EQ(manager.Allocate(64, 16, &reuse), StagingFlushAction::eFlush);

    manager.Release(unrelated, {});

    EXPECT_EQ(manager.Allocate(64, 16, &reuse), StagingFlushAction::eNone);
    EXPECT_EQ(reuse.pBuffer, submitted.pBuffer);
    EXPECT_EQ(reuse.offset, 0u);

    if (reuse.pBuffer != nullptr)
    {
        manager.Release(reuse, {});
    }

    queue.Destroy();

    EXPECT_EQ(rhi->finalizedLists, 0u);

    rhi->failSubmissionWait = false;
    device->NextFrame();
    manager.Destroy();
}

TEST_F(RenderCoreTest, FailedUploadFlushRetainsSnapshotsForRetryAndDestroyStillFlushesValidWork)
{
    rhi->submitted          = {1, 0, 1};
    rhi->failSubmissionWait = true;
    device->NextFrame();
    device->NextFrame();
    StagingBufferManager manager(512, 512);
    StagingUploadQueue queue(device, &manager);
    TestBuffer* buffer  = Buffer(16);
    RHITexture* texture = Texture();
    std::array<uint8_t, 16> bytes{};
    bytes.fill(37);
    const std::array<uint8_t, 16> expected = bytes;
    std::array<uint8_t, 256> pixels{};
    RHIBufferTextureCopyRegion region{};
    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.textureSize = {8, 8, 1};
    queue.EnqueueBuffer(buffer, 0, bytes.size(), bytes.data());
    queue.EnqueueTexture(texture, MakeVecView(&region, 1), pixels.size(), pixels.data());
    bytes.fill(0xFF);
    queue.Flush();

    EXPECT_TRUE(queue.HasPending());

    const uint32_t bufferReferences  = buffer->GetRefCount();
    const uint32_t textureReferences = texture->GetRefCount();
    queue.Flush();

    EXPECT_TRUE(queue.HasPending());
    EXPECT_EQ(buffer->GetRefCount(), bufferReferences);
    EXPECT_EQ(texture->GetRefCount(), textureReferences);
    EXPECT_EQ(rhi->finalizedLists, 0u);

    rhi->failSubmissionWait = false;
    device->NextFrame();
    queue.Flush();

    EXPECT_FALSE(queue.HasPending());
    EXPECT_EQ(buffer->bytes, (std::vector<uint8_t>(expected.begin(), expected.end())));
    EXPECT_EQ(rhi->transfer.textureCopies.size(), 1u);

    device->CollectCompletedResources();

    EXPECT_EQ(buffer->GetRefCount(), 1u);
    EXPECT_EQ(texture->GetRefCount(), 1u);

    // A final valid upload still submits during Destroy, including the shared-queue wait path.
    rhi->shared = true;
    bytes.fill(73);
    queue.EnqueueBuffer(buffer, 0, bytes.size(), bytes.data());
    queue.Destroy();

    EXPECT_FALSE(queue.HasPending());
    EXPECT_EQ(buffer->bytes, (std::vector<uint8_t>(bytes.begin(), bytes.end())));
    EXPECT_EQ(rhi->deviceIdleWaits, 1u);

    device->CollectCompletedResources();

    EXPECT_EQ(buffer->GetRefCount(), 1u);
    EXPECT_EQ(texture->GetRefCount(), 1u);

    const uint32_t finalized = rhi->finalizedLists;
    queue.Destroy();

    EXPECT_EQ(rhi->finalizedLists, finalized);
    EXPECT_EQ(rhi->deviceIdleWaits, 1u);

    manager.Destroy();
    device->DestroyBuffer(buffer);
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, MetricsRecognizeHostWrittenStagingUploads)
{
    RDGMetrics& metrics         = device->GetRDGMetrics();
    RDGMetricsOptions options   = metrics.GetOptions();
    options.logging.sampleEvery = 1;
    options.logging.minInterval = std::chrono::milliseconds(0);
    metrics.Configure(options);
    metrics.SetSink({});
    StagingBufferManager manager(8, 16);
    StagingUploadQueue queue(device, &manager);
    const std::array<uint8_t, 4> bytes{1, 2, 3, 4};

    // The two copies share a CPU-filled source, matching the reported startup graph.
    // Later batches reuse the completed staging block, including after a history reset.
    for (int batch = 0; batch < 3; ++batch)
    {
        if (batch == 2)
        {
            options.validate = false;
            metrics.Configure(options);
            options.validate = true;
            metrics.Configure(options);
        }

        TestBuffer* first  = Buffer();
        TestBuffer* second = Buffer();
        queue.EnqueueBuffer(first, 0, bytes.size(), bytes.data());
        queue.EnqueueBuffer(second, 0, bytes.size(), bytes.data());
        const uint64_t serial = rhi->submitted[2];
        queue.Flush();
        const RDGMetricsSnapshot& sample = metrics.GetLastSnapshot();
        EXPECT_EQ(sample.graph, "staging_upload");
        EXPECT_TRUE(sample.validated);
        EXPECT_EQ(sample.nodeCount, 2u);
        EXPECT_EQ(sample.resources, 3u);
        EXPECT_EQ(sample.totals.barrierCalls, 0u);
        EXPECT_EQ(sample.omittedDiagnostics, 0u) << RDGMetrics::Format(sample);

        for (uint32_t count : sample.issues)
        {
            EXPECT_EQ(count, 0u) << RDGMetrics::Format(sample);
        }

        EXPECT_GT(rhi->submitted[2], serial);
        EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), first->bytes.begin()));
        EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), second->bytes.begin()));

        device->DestroyBuffer(first);
        device->DestroyBuffer(second);
    }

    queue.Destroy();
    manager.Destroy();
}

TEST_F(RenderCoreTest, BufferInitializationZeroFillsPaddingWithoutReadingPastCallerData)
{
    CreateTestShaderProgram(device, "intent");
    const std::array<uint8_t, 3> input{1, 2, 3};

    for (RHIBuffer* buffer : {device->CreateUniformBuffer(input.size(), input.data(), "uniform"),
                              device->CreateStorageBuffer(input.size(), input.data(), "storage"),
                              device->CreateIndirectBuffer(input.size(), input.data(), "indirect")})
    {
        device->NextFrame();
        TestBuffer* actual = static_cast<TestBuffer*>(buffer);
        EXPECT_EQ(buffer->GetRequiredSize(), 16u);
        EXPECT_TRUE(std::equal(input.begin(), input.end(), actual->bytes.begin()));
        EXPECT_TRUE(std::all_of(actual->bytes.begin() + input.size(), actual->bytes.end(),
                                [](uint8_t value) { return value == 0; }));

        RenderGraph reader("padded_buffer_read");
        reader.Begin();

        RDGComputePassDesc read = IntentPass();

        if (buffer->GetUsageFlags().HasFlag(RHIBufferUsageFlagBits::eUniformBuffer))
        {
            read.BindUniformBuffer("value", reader.GetResourceManager()->ImportBuffer(buffer));
        }
        else
        {
            read.BindStorageBuffer("read_buffer", buffer, RDGContentGuarantee::eNone);
        }

        reader.AddComputePass(read);

        ASSERT_TRUE(reader.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(reader));
        EXPECT_TRUE(reader.GetWarnings().empty());
    }
}

TEST_F(RenderCoreTest, DeferredBufferDestructionUsesCompletionSerials)
{
    rhi->shared = true;
    std::array<uint8_t, 4> bytes{};
    RHIBuffer* buffer = device->CreateVertexBuffer(bytes.size(), bytes.data());
    const uint64_t id = buffer->GetStableId();
    device->DestroyBuffer(buffer);
    EXPECT_GT(rhi->submitted[0], 0u);
    EXPECT_FALSE(destroyed.contains(id));
    device->NextFrame();
    EXPECT_FALSE(destroyed.contains(id));
    device->NextFrame();
    EXPECT_TRUE(destroyed.contains(id));
}

TEST_F(RenderCoreTest, GraphRecordsTransfersAndSurvivesRebuildAndRepeatedExecution)
{
    TestBuffer* source      = Buffer();
    TestBuffer* destination = Buffer();
    source->bytes[0]        = 42;
    RenderGraph graph("copy");
    graph.Begin();

    {
        RDGTransferPassCmdRecorder first  = graph.AddTransferPass("first");
        RDGTransferPassCmdRecorder second = graph.AddTransferPass("second");
        first.CopyBuffer(source, destination, {0, 0, 4});
        second.CopyBuffer(destination, source, {0, 4, 4});
    }

    graph.End();
    device->ExecuteRenderGraph(graph);

    EXPECT_EQ(source->bytes[4], 42);

    device->ExecuteRenderGraph(graph);
    graph.Begin();
    graph.AddTransferPass("again").CopyBuffer(source, destination, {4, 8, 4});
    graph.End();
    device->ExecuteRenderGraph(graph);

    EXPECT_EQ(destination->bytes[8], 42);

    device->DestroyBuffer(source);
    device->DestroyBuffer(destination);
}

TEST_F(RenderCoreTest, InvalidLifecycleRejectsExecutionAndBeginRecovers)
{
    RenderGraph graph("state");
    EXPECT_FALSE(graph.End());
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eLifecycle);
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    ASSERT_TRUE(graph.Begin());
    EXPECT_FALSE(graph.Begin());
    EXPECT_FALSE(graph.End());
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(rhi->finalizedLists, 0u);
    ASSERT_TRUE(graph.Begin());
    ASSERT_TRUE(graph.End());
    EXPECT_TRUE(device->ExecuteRenderGraph(graph));
}

TEST_F(RenderCoreTest, InvalidShaderAndBindingDeclarationsEmitNoCommands)
{
    CreateTestShaderProgram(device, "validation");
    TestBuffer* buffer             = Buffer();
    RHITexture* texture            = Texture();
    const uint32_t initialTextures = rhi->textureCreations;

    for (int scenario = 0; scenario < 9; ++scenario)
    {
        SCOPED_TRACE(scenario);
        RenderGraph graph("invalid_binding");
        ASSERT_TRUE(graph.Begin());

        RDGComputePassDesc desc{};
        desc.SetShaderProgramName("validation");
        desc.SetPassTag("bad_pass");

        switch (scenario)
        {
            case 0: desc.SetShaderProgramName("missing_shader"); break;
            case 1: desc.BindStorageBuffer("missing_binding", buffer); break;
            case 2: desc.BindStorageBuffer("texture", buffer); break;
            case 3:
                desc.BindStorageBuffer("buffer", buffer);
                desc.UAVBufferBindings[0].buffers.offset = UINT32_MAX;
                break;

            case 4: desc.BindSampledTexture("texture", nullptr, "missing_producer"); break;
            case 5: desc.BindStorageBuffer("buffer", nullptr); break;
            case 6:
                desc.BindSampledTexture("texture", nullptr, texture->GetDefaultView());
                desc.sampledTexBindings[0].views.count = UINT32_MAX;
                break;

            case 7:
                desc.BindValue("value", uint32_t(7));
                desc.valueBindings[0].bytes.offset = UINT32_MAX;
                break;

            case 8: desc.BindValue("value", nullptr, 4); break;
        }

        bool callbackRan = false;
        graph.AddComputePass(desc).RecordPassCommands(
            [&](RDGPassCmdEncoder&) { callbackRan = true; });

        EXPECT_FALSE(graph.End());
        EXPECT_FALSE(device->ExecuteRenderGraph(graph));
        EXPECT_EQ(graph.GetExecutionState(), RDGExecutionState::eInvalid);
        EXPECT_FALSE(graph.GetResult().message.empty());
        EXPECT_FALSE(callbackRan);
        EXPECT_EQ(rhi->finalizedLists, 0u);
        EXPECT_EQ(rhi->pipelineCount, 0u);
        EXPECT_EQ(rhi->textureCreations, initialTextures);
    }

    device->DestroyBuffer(buffer);
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, AttachmentCountsAndDuplicateProducerTagsFailBeforeAllocation)
{
    CreateTestShaderProgram(device, "validation");

    for (int scenario = 0; scenario < 5; ++scenario)
    {
        SCOPED_TRACE(scenario);
        RenderGraph graph("invalid_outputs");
        graph.Begin();

        RDGGraphicsPassDesc desc{};
        desc.SetShaderProgramName("validation");

        if (scenario == 0)
        {
            for (uint32_t i = 0; i <= MAX_NUM_COLOR_ATTACHMENTS; ++i)
            {
                desc.AddColorOutput(DataFormat::eR8G8B8A8UNORM, 8, 8,
                                    "output_" + std::to_string(i));
            }
        }
        else if (scenario == 1)
        {
            desc.colorOutputCount = MAX_NUM_COLOR_ATTACHMENTS + 1;
        }
        else if (scenario == 2)
        {
            desc.AddColorOutput(static_cast<RHITexture*>(nullptr));
        }
        else
        {
            desc.AddColorOutput(DataFormat::eR8G8B8A8UNORM, 8, 8, "duplicate");

            if (scenario == 3)
            {
                graph.AddGraphicsPass(desc);
            }
            else
            {
                desc.AddColorOutput(DataFormat::eR8G8B8A8UNORM, 8, 8, "duplicate");
            }
        }

        graph.AddGraphicsPass(desc);

        EXPECT_FALSE(graph.End());
        EXPECT_FALSE(device->ExecuteRenderGraph(graph));
        EXPECT_EQ(graph.GetResult().code,
                  scenario >= 3 ? RDGErrorCode::eDuplicateTag : RDGErrorCode::eAttachment);
        EXPECT_EQ(rhi->textureCreations, 0u);
        EXPECT_EQ(rhi->pipelineCount, 0u);
        EXPECT_EQ(rhi->finalizedLists, 0u);
    }
}

TEST_F(RenderCoreTest, ConflictingTransferLayoutsRejectTheEntireGraph)
{
    RHITexture* first  = Texture();
    RHITexture* middle = Texture();
    RHITexture* last   = Texture();
    RHITextureCopyRegion region{};
    region.size = {1, 1, 1};
    region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    RenderGraph graph("conflicting_layouts");
    graph.Begin();

    {
        RDGTransferPassCmdRecorder pass = graph.AddTransferPass("bad_chain");
        pass.CopyTexture(first, middle, MakeVecView(&region, 1));
        pass.CopyTexture(middle, last, MakeVecView(&region, 1));
    }

    EXPECT_FALSE(graph.End());
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eConflictingLayout);
    EXPECT_NE(graph.GetResult().message.find("bad_chain"), std::string::npos);
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(rhi->finalizedLists, 0u);
    EXPECT_TRUE(rhi->transfer.textureTransitions.empty());
    EXPECT_TRUE(rhi->graphics.textureTransitions.empty());

    device->DestroyTexture(first);
    device->DestroyTexture(middle);
    device->DestroyTexture(last);
}

TEST_F(RenderCoreTest, TruncatedBufferTextureCopyIsRejectedBeforeSubmission)
{
    TestBuffer* source  = Buffer(64);
    RHITexture* texture = Texture();
    RHIBufferTextureCopyRegion region{};
    region.textureSize = {8, 8, 1};
    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    RenderGraph graph("truncated_upload");
    graph.Begin();
    graph.AddTransferPass("bad_upload").CopyBufferToTexture(source, texture, region);
    EXPECT_FALSE(graph.End());
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eRange);
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(rhi->finalizedLists, 0u);
    device->DestroyBuffer(source);
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, InvalidRangesAndLateRecordersFailSafely)
{
    TestBuffer* source = Buffer();
    TestBuffer* target = Buffer();

    for (RHIBufferCopyRegion const& region :
         {RHIBufferCopyRegion{UINT64_MAX, 0, 4}, RHIBufferCopyRegion{0, 63, 4},
          RHIBufferCopyRegion{0, 0, 0}})
    {
        RenderGraph graph("invalid_range");
        graph.Begin();
        graph.AddTransferPass("bad_copy").CopyBuffer(source, target, region);
        EXPECT_FALSE(graph.End());
        EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eRange);
        EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    }

    RenderGraph graph("late_recorder");
    graph.Begin();
    RDGTransferPassCmdRecorder recorder = graph.AddTransferPass("late_copy");
    graph.End();
    recorder.CopyBuffer(source, target, {0, 0, 4});

    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eLifecycle);
    EXPECT_EQ(rhi->finalizedLists, 0u);

    device->DestroyBuffer(source);
    device->DestroyBuffer(target);
}

TEST_F(RenderCoreTest, CallbackFailureRollsBackCommandsStateAndMetrics)
{
    CreateTestShaderProgram(device, "validation");
    TestBuffer* source = Buffer();
    TestBuffer* target = Buffer();
    RDGExecutor executor(device);
    executor.GetMetrics().SetSink({});
    RHICommandList* list = RHICommandList::Create(ZEN_NEW() TestContextProxy(rhi->graphics));
    list->CopyBuffer(source, target, {0, 4, 4});
    const uint32_t originalCommands     = list->GetCommandCount();
    const RDGMetricsSnapshot oldMetrics = executor.GetMetrics().GetLastSnapshot();
    executor.GetResourceStateTracker().UpdateBufferState(
        target, RHIAccessMode::eRead,
        BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eVertexBuffer),
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eVertexInput));
    const RDGBufferResourceState oldState =
        executor.GetResourceStateTracker().GetBufferState(target);
    RenderGraph graph("bad_callback");
    graph.Begin();
    graph.AddTransferPass("copy_before_failure").CopyBuffer(source, target, {0, 0, 4});

    RDGComputePassDesc desc{};
    desc.SetShaderProgramName("validation");
    desc.independentDispatches = true;
    graph.AddComputePass(desc).RecordPassCommands([target](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(1, 1, 1);
        encoder.DispatchIndirect(target,
                                 0); // Missing declaration after an already recorded command.
    });
    uint32_t laterCallbacks = 0;
    graph.AddComputePass(desc).RecordPassCommands([&](RDGPassCmdEncoder& encoder) {
        ++laterCallbacks;
        encoder.Dispatch(1, 1, 1);
    });
    graph.End();

    EXPECT_FALSE(executor.Execute(&graph, list));
    EXPECT_EQ(list->GetCommandCount(), originalCommands);
    EXPECT_EQ(laterCallbacks, 0u);

    const RDGBufferResourceState restoredState =
        executor.GetResourceStateTracker().GetBufferState(target);
    EXPECT_EQ(restoredState.accessMode, oldState.accessMode);
    EXPECT_EQ(int64_t(restoredState.usage), int64_t(oldState.usage));
    EXPECT_EQ(int64_t(restoredState.pipelineStages), int64_t(oldState.pipelineStages));
    EXPECT_EQ(restoredState.writer.visibleStages, oldState.writer.visibleStages);
    EXPECT_EQ(executor.GetMetrics().GetLastSnapshot().execution, oldMetrics.execution);
    EXPECT_EQ(executor.GetMetrics().GetLastSnapshot().graph, oldMetrics.graph);
    EXPECT_TRUE(rhi->graphics.bufferCopies.empty());
    EXPECT_EQ(target->bytes[0], 0xCD);
    EXPECT_EQ(rhi->finalizedLists, 0u);

    ZEN_DELETE(list);
    device->DestroyBuffer(source);
    device->DestroyBuffer(target);
}

TEST_F(RenderCoreTest, UnsupportedCommandsAndExplicitCallbackErrorsPreventSubmission)
{
    CreateTestShaderProgram(device, "validation");
    TestBuffer* source = Buffer();
    TestBuffer* target = Buffer();

    for (int scenario = 0; scenario < 7; ++scenario)
    {
        SCOPED_TRACE(scenario);
        RenderGraph graph("callback_contract");
        graph.Begin();
        graph.AddTransferPass("valid_earlier_pass").CopyBuffer(source, target, {0, 0, 4});

        if (scenario == 0)
        {
            RDGGraphicsPassDesc desc{};
            desc.SetShaderProgramName("validation");
            graph.AddGraphicsPass(desc).RecordPassCommands(
                [source, target](RDGPassCmdEncoder& encoder) {
                    encoder.CopyBuffer(source, target, {0, 0, 4});
                });
        }
        else
        {
            RDGComputePassDesc desc{};
            desc.SetShaderProgramName("validation");
            graph.AddComputePass(desc).RecordPassCommands(
                [&graph, scenario, target](RDGPassCmdEncoder& encoder) {
                    switch (scenario)
                    {
                        case 1: encoder.Draw(3, 1); break;
                        case 2: encoder.DispatchIndirect(target, 0); break;
                        case 3:
                            encoder.Dispatch(1, 1, 1);
                            encoder.Dispatch(1, 1, 1);
                            break;

                        case 4:
                            encoder.Fail(RDGErrorCode::eCallback, "callback failure");
                            encoder.Dispatch(1, 1, 1);
                            break;

                        case 5:
                            graph.Begin();
                            graph.Begin();
                            break;

                        case 6: encoder.SetShaderValue("missing", uint32_t(42)); break;
                    }
                });
        }

        graph.End();

        EXPECT_FALSE(device->ExecuteRenderGraph(graph));
        EXPECT_EQ(graph.GetExecutionState(), RDGExecutionState::eInvalid);

        if (scenario == 4)
        {
            EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eCallback);
            EXPECT_NE(graph.GetResult().message.find("callback failure"), std::string::npos);
        }

        EXPECT_EQ(rhi->finalizedLists, 0u);
        EXPECT_EQ(rhi->graphics.drawCount, 0u);
        EXPECT_EQ(rhi->graphics.dispatchCount, 0u);
        EXPECT_TRUE(rhi->graphics.bufferCopies.empty());
        EXPECT_TRUE(rhi->transfer.bufferCopies.empty());
        EXPECT_EQ(target->bytes[0], 0xCD);
    }

    device->DestroyBuffer(source);
    device->DestroyBuffer(target);
}

TEST_F(RenderCoreTest, GraphicsShaderCannotBeUsedByAComputePass)
{
    ShaderProgram* program = CreateTestShaderProgram(device, "geometry_only");
    static_cast<TestShader*>(program->GetShader())->EnableGeometryStage();
    RenderGraph graph("wrong_shader_stage");
    graph.Begin();

    RDGComputePassDesc desc{};
    desc.SetShaderProgramName("geometry_only");
    graph.AddComputePass(desc);
    EXPECT_FALSE(graph.End());
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eShader);
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(rhi->pipelineCount, 0u);
    EXPECT_EQ(rhi->finalizedLists, 0u);
}

TEST(RenderCoreUtilities, CommandRollbackDestroysOnlyAppendedCommands)
{
    struct TrackedCommand : RHICommand
    {
        explicit TrackedCommand(uint32_t& count) : destroyed(count) {}

        ~TrackedCommand() override
        {
            ++destroyed;
        }

        void Execute(RHICommandListBase&) override {}

        uint32_t& destroyed;
    };

    uint32_t originalDestroyed = 0;
    uint32_t appendedDestroyed = 0;
    RHICommandList list;
    list.AllocateCmdTyped<TrackedCommand>(originalDestroyed);
    const RHICommandListBase::CommandCheckpoint checkpoint = list.GetCommandCheckpoint();
    list.AllocateCmdTyped<TrackedCommand>(appendedDestroyed);
    list.AllocateCmdTyped<TrackedCommand>(appendedDestroyed);

    EXPECT_EQ(list.GetCommandCount(), 3u);

    list.RollbackCommands(checkpoint);

    EXPECT_EQ(list.GetCommandCount(), 1u);
    EXPECT_EQ(originalDestroyed, 0u);
    EXPECT_EQ(appendedDestroyed, 2u);

    // The restored tail remains usable, and reset must not destroy rolled-back commands twice.
    list.AllocateCmdTyped<TrackedCommand>(appendedDestroyed);

    EXPECT_EQ(list.GetCommandCount(), 2u);

    list.Reset();

    EXPECT_EQ(list.GetCommandCount(), 0u);
    EXPECT_EQ(originalDestroyed, 1u);
    EXPECT_EQ(appendedDestroyed, 3u);
}

TEST(RenderCoreUtilities, EncoderFailureStopsCommandsAndRetainsFirstError)
{
    for (bool explicitFailure : {false, true})
    {
        SCOPED_TRACE(explicitFailure);
        RHICommandList list;
        RDGComputePass pass;

        RDGComputePassDesc desc;
        desc.independentDispatches = true;
        RDGPassCmdEncoder encoder(&list, &pass, nullptr, &desc);
        encoder.Dispatch(1, 1, 1);

        ASSERT_TRUE(encoder.GetResult());
        ASSERT_EQ(list.GetCommandCount(), 1u);

        if (explicitFailure)
        {
            EXPECT_FALSE(encoder.Fail(RDGErrorCode::eCallback, "application failure"));
        }
        else
        {
            encoder.Draw(3, 1);
        }

        const RDGResult first = encoder.GetResult();
        EXPECT_EQ(first.code,
                  explicitFailure ? RDGErrorCode::eCallback : RDGErrorCode::eUnsupportedCommand);

        encoder.Dispatch(1, 1, 1);
        encoder.DispatchIndirect(nullptr, 0);
        encoder.DrawIndexed(3, 1, 0, 0, 0);
        encoder.DrawIndexedIndirect(nullptr, 0, 1, 20);
        encoder.SetViewport(0, 0, 1, 1);
        encoder.SetScissor(0, 0, 1, 1);
        encoder.SetShaderValue("missing", uint32_t(1));
        encoder.SetPushConstants(uint32_t(1));
        encoder.CopyBuffer(nullptr, nullptr, {0, 0, 4});
        encoder.CopyTexture(nullptr, nullptr, {});
        encoder.CopyBufferToTexture(nullptr, nullptr, {});
        encoder.ClearTexture(nullptr, {}, {});
        encoder.GenerateMipmaps(nullptr);

        EXPECT_FALSE(encoder.Fail(RDGErrorCode::eBinding, "later failure"));
        EXPECT_EQ(encoder.GetResult().code, first.code);
        EXPECT_EQ(encoder.GetResult().message, first.message);
        EXPECT_EQ(list.GetCommandCount(), 1u);

        RDGPassCmdEncoder next(&list, &pass, nullptr, &desc);
        next.Dispatch(1, 1, 1);

        EXPECT_TRUE(next.GetResult());
        EXPECT_EQ(list.GetCommandCount(), 2u);
    }
}

TEST(RenderCoreUtilities, NullCommandListReportsLifecycleErrorWithoutRecording)
{
    RDGPassCmdEncoder encoder(nullptr, nullptr);
    EXPECT_EQ(encoder.GetResult().code, RDGErrorCode::eLifecycle);
    encoder.Draw(3, 1);
    encoder.Dispatch(1, 1, 1);
    encoder.CopyBuffer(nullptr, nullptr, {0, 0, 4});
    encoder.SetPushConstants(uint32_t(1));
    EXPECT_EQ(encoder.GetResult().code, RDGErrorCode::eLifecycle);
}

TEST_F(RenderCoreTest, ShaderRemovalBeforeCompilationFailsBeforeMaterialization)
{
    CreateTestShaderProgram(device, "removed");
    RenderGraph graph("shader_removed");
    graph.Begin();

    RDGGraphicsPassDesc desc{};
    desc.SetShaderProgramName("removed");
    desc.AddColorOutput(DataFormat::eR8G8B8A8UNORM, 8, 8, "output");
    graph.AddGraphicsPass(desc);
    graph.End();
    ShaderProgramManager::GetInstance().Destroy();
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eShader);
    EXPECT_EQ(rhi->textureCreations, 0u);
    EXPECT_EQ(rhi->finalizedLists, 0u);
}

TEST_F(RenderCoreTest, InvalidTextureViewRangesAndResourceManagerLifecycleAreRejected)
{
    CreateTestShaderProgram(device, "validation");
    RHITexture* texture = Texture();

    RHITextureViewCreateInfo viewInfo{};
    viewInfo.format       = texture->GetFormat();
    viewInfo.type         = RHITextureType::e2D;
    viewInfo.baseMipLevel = 1;
    RHITextureView* view  = texture->CreateView(viewInfo);
    RenderGraph graph("invalid_view");
    graph.Begin();

    RDGComputePassDesc desc{};
    desc.SetShaderProgramName("validation");
    desc.BindSampledTexture("texture", nullptr, view);
    graph.AddComputePass(desc);

    EXPECT_FALSE(graph.End());
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eRange);
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    ASSERT_TRUE(graph.Begin());
    ASSERT_TRUE(graph.End());
    EXPECT_FALSE(graph.GetResourceManager()->ImportTexture(texture));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eLifecycle);
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(rhi->finalizedLists, 0u);

    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, ReusedShaderRecorderCannotModifyTheNextBuild)
{
    CreateTestShaderProgram(device, "validation");
    RenderGraph graph("stale_recorder");
    graph.Begin();

    RDGComputePassDesc desc{};
    desc.SetShaderProgramName("validation");
    RDGShaderPassCmdRecorder recorder = graph.AddComputePass(desc);
    graph.End();
    graph.Begin();
    graph.AddComputePass(desc);
    recorder.RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    EXPECT_FALSE(graph.End());
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eLifecycle);
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(rhi->finalizedLists, 0u);
}

static void RecordAllocationFailureGraph(RenderGraph& graph, RDGTexture& output)
{
    graph.Begin();

    RDGComputePassDesc compute{};
    compute.SetShaderProgramName("validation");
    graph.AddComputePass(compute);

    RDGGraphicsPassDesc graphics{};
    graphics.SetShaderProgramName("validation");
    output = graph.GetResourceManager()->CreateTexture(LogicalTexture());
    graphics.AddColorOutput(output);
    graph.AddGraphicsPass(graphics);
    graph.End();
}

TEST_F(RenderCoreTest, FailedMaterializationAndPipelineCreationReleasePartialResources)
{
    CreateTestShaderProgram(device, "validation");
    RenderGraph graph("allocation_failure");
    RDGTexture output;

    rhi->failTextureCreationAt = 1;
    RecordAllocationFailureGraph(graph, output);

    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eAllocation);
    EXPECT_EQ(rhi->pipelineCount, 0u);
    EXPECT_EQ(rhi->finalizedLists, 0u);

    rhi->failPipelineCreationAt = 2;
    RecordAllocationFailureGraph(graph, output);

    ASSERT_TRUE(output);
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eAllocation);
    EXPECT_EQ(DescribeResource(graph.GetResourceManager(), output).physicalStableId,
              0u); // Returned to the owned pool on failure.
    EXPECT_EQ(rhi->finalizedLists, 0u);

    RHIRenderingLayout* layout = device->AcquireRenderingLayout();
    EXPECT_EQ(layout, rhi->lastPipelineLayout);

    device->ReleaseRenderingLayout(layout);

    ASSERT_EQ(rhi->createdPipelines.size(), 1u);

    RHIPipeline* cachedCompute = rhi->createdPipelines[0];
    device->NextFrame();
    device->NextFrame();

    EXPECT_EQ(cachedCompute->GetRefCount(), 1u); // Only the pipeline cache retains it.

    const uint32_t textureCreations = rhi->textureCreations;
    RecordAllocationFailureGraph(graph, output);

    EXPECT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(rhi->textureCreations, textureCreations); // Partial allocation is reusable.
    EXPECT_TRUE(graph.GetResult());
}

static void RecordSubmissionGraph(RenderGraph& graph,
                                  RHIBuffer* source,
                                  RHIBuffer* target,
                                  RHITexture* texture,
                                  bool graphics,
                                  RDGExtractedBuffer& extracted)
{
    EXPECT_TRUE(graph.Begin());
    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer input         = resources->ImportHostWrittenBuffer(source);
    const RDGBuffer output        = resources->CreateBuffer(LogicalBuffer());
    graph.AddTransferPass("write").CopyBuffer(source, target, {0, 0, 64});
    graph.AddTransferPass("extract").CopyBuffer(input, output, {0, 0, 64});

    if (graphics)
    {
        graph.AddTransferPass("graphics").ClearTexture(texture, {});
    }

    extracted = resources->QueueBufferExtraction(output);
    EXPECT_TRUE(graph.End());
}

TEST_F(RenderCoreTest, RejectedSubmissionKeepsStatePrivateAndDoesNotPublishExtractionsOrMetrics)
{
    for (const bool graphics : {false, true})
    {
        SCOPED_TRACE(graphics);
        TestBuffer* source  = Buffer();
        TestBuffer* target  = Buffer();
        RHITexture* texture = Texture();
        std::fill(source->bytes.begin(), source->bytes.end(), uint8_t(73));
        std::fill(target->bytes.begin(), target->bytes.end(), uint8_t(17));
        ResourceStateTracker& tracker = RDGSubmissionTestAccess::Tracker(*device);
        tracker.UpdateBufferState(
            target, RHIAccessMode::eRead,
            BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferSrcBuffer),
            BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eTransfer));
        RDGResourceContent contents;
        contents.status = RDGContentStatus::eUndefined;
        tracker.SetContents(target, contents);
        const RDGBufferResourceState previousState = tracker.GetBufferState(target);
        RDGMetrics& metrics                        = device->GetRDGMetrics();
        RDGMetricsOptions options                  = metrics.GetOptions();
        options.logging.sampleEvery                = 1;
        options.logging.minInterval                = std::chrono::milliseconds(0);
        metrics.Configure(options);
        std::vector<RDGMetricsSnapshot> samples;
        metrics.SetSink([&](const RDGMetricsSnapshot& sample) { samples.push_back(sample); });
        RenderGraph graph("submission_commit");
        RDGExtractedBuffer extracted;

        RecordSubmissionGraph(graph, source, target, texture, graphics, extracted);
        rhi->beforeSubmission = [&] {
            EXPECT_FALSE(extracted);
            EXPECT_TRUE(samples.empty());
            EXPECT_EQ(tracker.GetContents(target).status, RDGContentStatus::eUndefined);
            EXPECT_EQ(tracker.GetBufferState(target).accessMode, previousState.accessMode);
        };
        const std::array<uint64_t, 3> submitted = rhi->submitted;
        rhi->failSubmissionAt                   = rhi->submissionAttempts + 1;

        EXPECT_FALSE(device->ExecuteRenderGraph(graph));
        EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eSubmission);
        EXPECT_EQ(rhi->submitted, submitted);
        EXPECT_EQ(target->bytes, std::vector<uint8_t>(64, 17));
        EXPECT_EQ(tracker.GetContents(target).status, RDGContentStatus::eUndefined);
        EXPECT_EQ(int64_t(tracker.GetBufferState(target).usage), int64_t(previousState.usage));
        EXPECT_TRUE(samples.empty());
        EXPECT_FALSE(extracted);

        RecordSubmissionGraph(graph, source, target, texture, graphics, extracted);

        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        EXPECT_EQ(target->bytes, source->bytes);
        ASSERT_TRUE(extracted);
        EXPECT_EQ(static_cast<TestBuffer*>(extracted.Get())->bytes, source->bytes);
        EXPECT_EQ(tracker.GetContents(target).status, RDGContentStatus::eDefined);
        EXPECT_EQ(samples.size(), 1u);

        rhi->beforeSubmission = {};
        metrics.SetSink({});
        device->DestroyBuffer(source);
        device->DestroyBuffer(target);
        device->DestroyTexture(texture);
    }
}

TEST_F(RenderCoreTest, RejectedUploadsRetainOwnedBytesAndRetryWithoutDuplicateCommands)
{
    StagingBufferManager manager(64, 64);
    StagingUploadQueue queue(device, &manager);
    TestBuffer* target = Buffer(64);
    std::array<uint8_t, 64> payload{};
    payload.fill(91);
    queue.EnqueueBuffer(target, 0, payload.size(), payload.data());
    rhi->failSubmissionAt = rhi->submissionAttempts + 1;

    EXPECT_FALSE(queue.Flush());
    EXPECT_TRUE(queue.HasPending());
    EXPECT_EQ(target->bytes, std::vector<uint8_t>(64, 0xCD));
    EXPECT_TRUE(rhi->transfer.bufferCopies.empty());
    EXPECT_EQ(rhi->submitted[static_cast<size_t>(RHICommandContextType::eTransfer)], 0u);

    payload.fill(12);

    ASSERT_TRUE(queue.Flush());
    EXPECT_FALSE(queue.HasPending());
    EXPECT_EQ(target->bytes, std::vector<uint8_t>(64, 91));
    EXPECT_EQ(rhi->transfer.bufferCopies.size(), 1u);

    const std::array<uint64_t, 3> submitted = rhi->submitted;
    EXPECT_TRUE(queue.Flush());
    EXPECT_EQ(rhi->FlushAllGPUCommands(), RHISubmissionResult::eSuccess);
    EXPECT_EQ(rhi->submitted, submitted);

    queue.Destroy();
    manager.Destroy();
    device->DestroyBuffer(target);
}

TEST_F(RenderCoreTest, RecordingWithoutSubmissionDoesNotPublishExtractionOwnership)
{
    TestBuffer* source = Buffer();
    RenderGraph graph("record_only_extraction");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer output        = resources->CreateBuffer(LogicalBuffer());
    graph.AddTransferPass("copy").CopyBuffer(resources->ImportHostWrittenBuffer(source), output,
                                             {0, 0, 64});
    RDGExtractedBuffer extracted = resources->QueueBufferExtraction(output);
    ASSERT_TRUE(graph.End());

    RDGExecutor executor(device);
    RHICommandList commands;
    ASSERT_TRUE(executor.Execute(&graph, &commands));
    EXPECT_GT(commands.GetCommandCount(), 0u);
    EXPECT_FALSE(extracted);
    EXPECT_EQ(rhi->finalizedLists, 0u);

    commands.Reset();
    device->DestroyBuffer(source);
}

static void RecordUploadConsumer(RenderGraph& graph, RHIBuffer* source, uint32_t& callbacks)
{
    EXPECT_TRUE(graph.Begin());

    RDGComputePassDesc pass = IntentPass();
    pass.BindStorageBuffer("read_buffer", source);
    graph.AddComputePass(pass).RecordPassCommands([&](RDGPassCmdEncoder&) { ++callbacks; });
    EXPECT_TRUE(graph.End());
}

TEST_F(RenderCoreTest, FailedUploadSubmissionPreventsDependentConsumerRecording)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer* source = Buffer();
    std::array<uint8_t, 64> payload{};
    payload.fill(37);
    device->UpdateBuffer(source, payload.size(), payload.data());
    RenderGraph graph("upload_consumer");
    uint32_t callbacks = 0;

    RecordUploadConsumer(graph, source, callbacks);
    rhi->failSubmissionAt = rhi->submissionAttempts + 1;

    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eSubmission);
    EXPECT_EQ(callbacks, 0u);
    EXPECT_EQ(rhi->finalizedLists, 1u);

    RecordUploadConsumer(graph, source, callbacks);

    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(callbacks, 1u);
    EXPECT_EQ(source->bytes, std::vector<uint8_t>(64, 37));

    device->DestroyBuffer(source);
}

TEST_F(RenderCoreTest, FatalSubmissionBlocksFurtherExecutionAndPreservesAcceptedRetirementGates)
{
    TestBuffer* source  = Buffer();
    TestBuffer* target  = Buffer();
    RHITexture* texture = Texture();
    target->AddReference(); // Observer survives both owner and graph retirement.
    RenderGraph graph("partial_submission");
    ASSERT_TRUE(graph.Begin());

    graph.GetResourceManager()->ImportHostWrittenBuffer(source);
    graph.AddTransferPass("write").CopyBuffer(source, target, {0, 0, 64}).ClearTexture(texture, {});

    ASSERT_TRUE(graph.End());

    rhi->failSubmissionAt    = rhi->submissionAttempts + 1;
    rhi->submissionFailure   = RHISubmissionResult::eFatal;
    rhi->submitBeforeFailure = true;

    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eSubmission);
    EXPECT_EQ(rhi->submitted[0], 1u);
    EXPECT_EQ(RDGSubmissionTestAccess::Tracker(*device).GetBufferState(target).accessMode,
              RHIAccessMode::eNone);

    const uint32_t attempts     = rhi->submissionAttempts;
    const RenderFrameSlot frame = GRenderFrameState.GetFrameSlot();
    device->NextFrame();

    EXPECT_EQ(GRenderFrameState.GetFrameSlot(), frame);
    ASSERT_TRUE(graph.Begin());

    graph.AddTransferPass("retry").CopyBuffer(source, target, {0, 0, 64});

    ASSERT_TRUE(graph.End());
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(rhi->submissionAttempts, attempts);
    ASSERT_TRUE(graph.Reset());

    device->DestroyBuffer(target);
    device->CollectCompletedResources();

    EXPECT_GT(target->GetRefCount(), 1u);

    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    EXPECT_GT(target->GetRefCount(), 1u); // Uncertain work stays retained until device teardown.

    target->ReleaseReference();
    device->DestroyBuffer(source);
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, FailedTransferCompletionKeepsCancelledStagingInFlight)
{
    StagingBufferManager manager(64, 64);
    StagingUploadQueue queue(device, &manager);
    TestBuffer* target = Buffer();
    std::array<uint8_t, 64> payload{};
    queue.EnqueueBuffer(target, 0, payload.size(), payload.data());
    rhi->failSubmissionWait = true;

    EXPECT_FALSE(queue.Flush());
    EXPECT_TRUE(queue.HasPending());

    const size_t transfer = static_cast<size_t>(RHICommandContextType::eTransfer);
    EXPECT_EQ(rhi->submitted[transfer], 1u);
    EXPECT_EQ(rhi->completed[transfer], 0u);

    queue.Destroy();
    StagingAllocation allocation;
    EXPECT_EQ(manager.Allocate(64, 4, &allocation), StagingFlushAction::eFlush);

    rhi->failSubmissionWait = false;
    rhi->completed          = rhi->submitted;

    EXPECT_EQ(manager.Allocate(64, 4, &allocation), StagingFlushAction::eFlush);

    const uint32_t attempts = rhi->submissionAttempts;
    RenderGraph graph("blocked_after_wait_failure");
    ASSERT_TRUE(graph.Begin());

    graph.AddTransferPass("write").CopyBuffer(target, target, {0, 32, 4});

    ASSERT_TRUE(graph.End());
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eSubmission);
    EXPECT_EQ(rhi->submissionAttempts, attempts);

    manager.Destroy();
    device->DestroyBuffer(target);
}

static void RecordPresentationGraph(RenderGraph* graph, RHIBuffer* source, RHIBuffer* target)
{
    EXPECT_TRUE(graph->Begin());
    graph->GetResourceManager()->ImportHostWrittenBuffer(source);
    graph->AddTransferPass("frame").CopyBuffer(source, target, {0, 0, 64});
    EXPECT_TRUE(graph->End());
}

TEST_F(RenderCoreTest, FrameAndPresentationSubmissionFailuresDoNotPresentUnsignaledWork)
{
    TestViewport viewport;
    viewport.color     = Texture();
    TestBuffer* source = Buffer();
    TestBuffer* target = Buffer();
    RenderGraph* graph = device->GetCurrentFrameRDG();

    RecordPresentationGraph(graph, source, target);
    rhi->failSubmissionAt = rhi->submissionAttempts + 1;

    EXPECT_FALSE(device->ExecuteRenderGraph(&viewport));
    EXPECT_EQ(viewport.preparePresents, 0u);
    EXPECT_EQ(viewport.presents, 0u);
    EXPECT_EQ(rhi->submitted[0], 0u);

    RecordPresentationGraph(graph, source, target);
    rhi->failSubmissionAt = rhi->submissionAttempts + 2;

    EXPECT_FALSE(device->ExecuteRenderGraph(&viewport));
    EXPECT_EQ(viewport.preparePresents, 1u);
    EXPECT_EQ(viewport.presents, 0u);
    EXPECT_EQ(rhi->submitted[0], 1u);
    EXPECT_EQ(RDGSubmissionTestAccess::Tracker(*device).GetContents(target).status,
              RDGContentStatus::eDefined);

    RecordPresentationGraph(graph, source, target);

    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    EXPECT_EQ(viewport.presents, 1u);

    device->NextFrame();
    viewport.presentResult = false;
    RecordPresentationGraph(graph, source, target);

    EXPECT_FALSE(device->ExecuteRenderGraph(&viewport));
    EXPECT_EQ(viewport.presents, 2u);

    device->DestroyBuffer(source);
    device->DestroyBuffer(target);
    device->DestroyTexture(viewport.color);
}

TEST_F(RenderCoreTest, BufferImageCopiesSelectQueuesUsingMipGranularity)
{
    struct Case
    {
        uint32_t granularity, mip;
        int32_t offset, size;
        bool graphics;
        bool volume{false};
    };

    const Case cases[] = {
        {4, 0, 1, 4, true},        {4, 0, 4, 4, false},      {4, 0, 4, 6, false},
        {0, 0, 0, 10, false},      {0, 0, 0, 4, true},       {4, 1, 4, 1, false},
        {4, 0, 0, 3, true},        {0, 1, 0, 5, false},      {4, 0, 1, 4, true, true},
        {4, 0, 4, 6, false, true}, {0, 1, 0, 5, false, true}};

    for (const bool logical : {false, true})
    {
        for (const Case& test : cases)
        {
            SCOPED_TRACE(logical);
            SCOPED_TRACE(test.granularity);
            SCOPED_TRACE(test.offset);
            SCOPED_TRACE(test.size);
            rhi->transferCopy.minImageTransferGranularity.fill(test.granularity);
            TestBuffer* buffer = Buffer(4096);

            RHITextureCreateInfo info{};
            info.format = DataFormat::eR8G8B8A8UNORM;
            info.type   = test.volume ? RHITextureType::e3D : RHITextureType::e2D;
            info.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);
            // Use a fresh texture for every case so earlier graphics use cannot affect routing.
            info.width = info.height = 10;
            info.depth               = test.volume ? 10 : 1;
            info.mipmaps             = 2;
            RHITexture* texture      = rhi->CreateTexture(info);
            RHIBufferTextureCopyRegion region{};
            region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
            region.textureSubresources.mipmap = test.mip;
            region.textureOffset = {test.offset, test.offset, test.volume ? test.offset : 0};
            region.textureSize   = {test.size, test.size, test.volume ? test.size : 1};
            rhi->graphics.textureCopies.clear();
            rhi->transfer.textureCopies.clear();
            RenderGraph graph("copy_granularity");
            ASSERT_TRUE(graph.Begin());

            RDGResourceManager* resources = graph.GetResourceManager();
            const RDGBuffer input         = resources->ImportHostWrittenBuffer(buffer);

            if (logical)
            {
                graph.AddTransferPass("copy").CopyBufferToTexture(
                    input, resources->ImportTexture(texture), region);
            }
            else
            {
                graph.AddTransferPass("copy").CopyBufferToTexture(buffer, texture, region);
            }

            ASSERT_TRUE(graph.End());
            ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
            EXPECT_EQ(rhi->graphics.textureCopies.size(), test.graphics ? 1u : 0u);
            EXPECT_EQ(rhi->transfer.textureCopies.size(), test.graphics ? 0u : 1u);

            device->DestroyBuffer(buffer);
            device->DestroyTexture(texture);
        }
    }
}

TEST_F(RenderCoreTest, ImageCopiesCheckBothQueueBoxesAndKeepLegalTransferCopies)
{
    rhi->transferCopy.minImageTransferGranularity = {4, 4, 4};

    for (const bool logical : {false, true})
    {
        for (uint32_t scenario = 0; scenario < 3; ++scenario)
        {
            SCOPED_TRACE(logical);
            SCOPED_TRACE(scenario);
            RHITexture* source      = Texture();
            RHITexture* destination = Texture();
            RHITextureCopyRegion region{};
            region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
            region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
            region.size = {4, 4, 1};

            if (scenario == 0)
            {
                region.srcOffset.x = 1;
            }

            if (scenario == 1)
            {
                region.dstOffset.x = 1;
            }

            rhi->graphics.imageCopies.clear();
            rhi->transfer.imageCopies.clear();
            RenderGraph graph("image_copy_granularity");
            ASSERT_TRUE(graph.Begin());

            RDGResourceManager* resources = graph.GetResourceManager();

            if (logical)
            {
                graph.AddTransferPass("copy").CopyTexture(resources->ImportTexture(source),
                                                          resources->ImportTexture(destination),
                                                          MakeVecView(&region, 1));
            }
            else
            {
                graph.AddTransferPass("copy").CopyTexture(source, destination,
                                                          MakeVecView(&region, 1));
            }

            ASSERT_TRUE(graph.End());
            ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
            EXPECT_EQ(rhi->graphics.imageCopies.size(), scenario == 2 ? 0u : 1u);
            EXPECT_EQ(rhi->transfer.imageCopies.size(), scenario == 2 ? 1u : 0u);

            device->DestroyTexture(source);
            device->DestroyTexture(destination);
        }
    }
}

TEST_F(RenderCoreTest, ByteOffsetCopiesUseActualTransferQueueFlags)
{
    for (const bool logical : {false, true})
    {
        for (uint32_t scenario = 0; scenario < 3; ++scenario)
        {
            SCOPED_TRACE(logical);
            SCOPED_TRACE(scenario);
            rhi->transferCopy.compute = scenario == 1;
            rhi->shared               = scenario == 2;
            TestBuffer* buffer        = Buffer();

            RHITextureCreateInfo info{};
            info.format = DataFormat::eR8UNORM;
            info.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);
            RHITexture* texture = rhi->CreateTexture(info);
            RHIBufferTextureCopyRegion region{};
            region.bufferOffset = 1;
            region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
            region.textureSize = {1, 1, 1};
            rhi->graphics.textureCopies.clear();
            rhi->transfer.textureCopies.clear();
            RenderGraph graph("queue_flags");
            ASSERT_TRUE(graph.Begin());

            RDGResourceManager* resources = graph.GetResourceManager();
            const RDGBuffer input         = resources->ImportHostWrittenBuffer(buffer);

            if (logical)
            {
                graph.AddTransferPass("copy").CopyBufferToTexture(
                    input, resources->ImportTexture(texture), region);
            }
            else
            {
                graph.AddTransferPass("copy").CopyBufferToTexture(buffer, texture, region);
            }

            ASSERT_TRUE(graph.End());
            ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
            // A shared transfer context records onto a graphics-capable physical queue.
            EXPECT_EQ(rhi->graphics.textureCopies.size(), scenario == 0 ? 1u : 0u);
            EXPECT_EQ(rhi->transfer.textureCopies.size(), scenario == 0 ? 0u : 1u);

            device->DestroyBuffer(buffer);
            device->DestroyTexture(texture);
        }
    }
}

TEST_F(RenderCoreTest, DepthStencilUploadsUseGraphicsAndValidateIndividualAspects)
{
    for (const bool logical : {false, true})
    {
        for (DataFormat const format : {DataFormat::eD16UNORM, DataFormat::eD32SFloat,
                                        DataFormat::eS8UInt, DataFormat::eD16UNORMS8UInt,
                                        DataFormat::eD24UNORMS8UInt, DataFormat::eD32SFloatS8UInt})
        {
            for (RHITextureAspectFlagBits const aspect :
                 {RHITextureAspectFlagBits::eDepth, RHITextureAspectFlagBits::eStencil})
            {
                if ((FormatIsDepthOnly(format) && aspect == RHITextureAspectFlagBits::eStencil) ||
                    (FormatIsStencilOnly(format) && aspect == RHITextureAspectFlagBits::eDepth))
                {
                    continue;
                }

                SCOPED_TRACE(uint32_t(format));
                const uint32_t bytes = aspect == RHITextureAspectFlagBits::eStencil            ? 1 :
                    (format == DataFormat::eD16UNORM || format == DataFormat::eD16UNORMS8UInt) ? 2 :
                                                                                                 4;
                TestBuffer* buffer   = Buffer(4 + bytes);

                RHITextureCreateInfo info{};
                info.format = format;
                info.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);
                RHITexture* texture = rhi->CreateTexture(info);
                RHIBufferTextureCopyRegion region{};
                region.bufferOffset = 4;
                region.textureSubresources.aspect.SetFlag(aspect);
                region.textureSize = {1, 1, 1};
                rhi->graphics.textureCopies.clear();
                rhi->transfer.textureCopies.clear();
                // Even a compute-capable transfer queue needs graphics for depth/stencil uploads.
                rhi->transferCopy.compute = true;
                RenderGraph graph("depth_stencil_upload");
                ASSERT_TRUE(graph.Begin());

                RDGResourceManager* resources = graph.GetResourceManager();
                const RDGBuffer input         = resources->ImportHostWrittenBuffer(buffer);

                if (logical)
                {
                    graph.AddTransferPass("copy").CopyBufferToTexture(
                        input, resources->ImportTexture(texture), region);
                }
                else
                {
                    graph.AddTransferPass("copy").CopyBufferToTexture(buffer, texture, region);
                }

                ASSERT_TRUE(graph.End());
                ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
                EXPECT_EQ(rhi->graphics.textureCopies.size(), 1u);
                EXPECT_TRUE(rhi->transfer.textureCopies.empty());

                device->DestroyBuffer(buffer);
                device->DestroyTexture(texture);
            }
        }
    }
}

TEST_F(RenderCoreTest, BufferImageCopiesRejectMisalignmentCombinedAspectsAndInvalidDimensions)
{
    for (const bool logical : {false, true})
    {
        for (uint32_t scenario = 0; scenario < 7; ++scenario)
        {
            SCOPED_TRACE(logical);
            SCOPED_TRACE(scenario);
            TestBuffer* buffer = Buffer(256);

            RHITextureCreateInfo info{};
            info.type   = RHITextureType::e2D;
            info.format = DataFormat::eR8G8B8A8UNORM;
            info.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);
            RHIBufferTextureCopyRegion region{};
            region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
            region.textureSize = {1, 1, 1};

            switch (scenario)
            {
                case 0: region.bufferOffset = 1; break;
                case 1:
                    info.format         = DataFormat::eR8G8B8UNORM;
                    region.bufferOffset = 4;
                    break;

                case 2:
                    info.format = DataFormat::eD24UNORMS8UInt;
                    region.textureSubresources.aspect =
                        RHITextureSubResourceRange::DepthStencil().aspect;
                    break;

                case 3:
                    info.format                       = DataFormat::eD16UNORM;
                    region.bufferOffset               = 2;
                    region.textureSubresources.aspect = RHITextureSubResourceRange::Depth().aspect;
                    break;

                case 4:
                    info.type            = RHITextureType::e1D;
                    info.height          = 2;
                    region.textureSize.y = 2;
                    break;

                case 5:
                    info.depth           = 2;
                    region.textureSize.z = 2;
                    break;

                case 6:
                    info.type                             = RHITextureType::e3D;
                    info.arrayLayers                      = 2;
                    region.textureSubresources.layerCount = 2;
                    break;
            }

            RHITexture* texture = rhi->CreateTexture(info);
            RenderGraph graph("invalid_buffer_image_copy");
            ASSERT_TRUE(graph.Begin());

            RDGResourceManager* resources = graph.GetResourceManager();

            if (logical)
            {
                graph.AddTransferPass("copy").CopyBufferToTexture(
                    resources->ImportHostWrittenBuffer(buffer), resources->ImportTexture(texture),
                    region);
            }
            else
            {
                graph.AddTransferPass("copy").CopyBufferToTexture(buffer, texture, region);
            }

            EXPECT_FALSE(graph.End());
            EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eRange);
            EXPECT_FALSE(device->ExecuteRenderGraph(graph));

            device->DestroyBuffer(buffer);
            device->DestroyTexture(texture);
        }
    }

    EXPECT_EQ(rhi->finalizedLists, 0u);
}

TEST_F(RenderCoreTest, CopyFormatAndSampleFailuresRejectRawAndLogicalDeclarations)
{
    for (const bool logical : {false, true})
    {
        for (uint32_t scenario = 0; scenario < 5; ++scenario)
        {
            SCOPED_TRACE(logical);
            SCOPED_TRACE(scenario);
            RHITexture* source = Texture();

            RHITextureCreateInfo info = source->GetBaseInfo();

            if (scenario == 0)
            {
                info.format = DataFormat::eR16SFloat;
            }

            if (scenario == 1)
            {
                info.samples = SampleCount::e2;
            }

            RHITexture* destination            = rhi->CreateTexture(info);
            TestBuffer* buffer                 = Buffer();
            rhi->copyCapabilities[info.format] = {scenario != 2, scenario < 3, true, true, true};
            RHITextureCopyRegion region{};
            region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
            region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
            region.size = {1, 1, 1};
            RenderGraph graph("invalid_copy_capabilities");
            ASSERT_TRUE(graph.Begin());

            RDGResourceManager* resources = graph.GetResourceManager();

            if (scenario == 4)
            {
                RHIBufferTextureCopyRegion upload{};
                upload.textureSubresources = region.dstSubresources;
                upload.textureSize         = region.size;

                if (logical)
                {
                    graph.AddTransferPass("copy").CopyBufferToTexture(
                        resources->ImportHostWrittenBuffer(buffer),
                        resources->ImportTexture(destination), upload);
                }
                else
                {
                    graph.AddTransferPass("copy").CopyBufferToTexture(buffer, destination, upload);
                }
            }
            else if (logical)
            {
                graph.AddTransferPass("copy").CopyTexture(resources->ImportTexture(source),
                                                          resources->ImportTexture(destination),
                                                          MakeVecView(&region, 1));
            }
            else
            {
                graph.AddTransferPass("copy").CopyTexture(source, destination,
                                                          MakeVecView(&region, 1));
            }

            EXPECT_FALSE(graph.End());
            EXPECT_EQ(graph.GetResult().code,
                      scenario < 2 ? RDGErrorCode::eRange : RDGErrorCode::eBinding);
            EXPECT_FALSE(device->ExecuteRenderGraph(graph));

            rhi->copyCapabilities.clear();
            device->DestroyBuffer(buffer);
            device->DestroyTexture(source);
            device->DestroyTexture(destination);
        }
    }

    EXPECT_TRUE(rhi->graphics.imageCopies.empty());
    EXPECT_TRUE(rhi->transfer.imageCopies.empty());
    EXPECT_EQ(rhi->finalizedLists, 0u);
}

TEST_F(RenderCoreTest, LinearMipGenerationRejectsUnsupportedFormatsBeforeMaterialization)
{
    for (const bool logical : {false, true})
    {
        for (uint32_t scenario = 0; scenario < 7; ++scenario)
        {
            SCOPED_TRACE(logical);
            SCOPED_TRACE(scenario);
            RDGTextureDesc desc = LogicalTexture(3);

            if (scenario == 5)
            {
                desc.texFormat.sampleCount = SampleCount::e2;
            }

            if (scenario == 6)
            {
                desc.texFormat.format = DataFormat::eD32SFloat;
            }

            RHITextureCopyCapabilities& caps = rhi->copyCapabilities[desc.texFormat.format];
            caps = {scenario != 0, scenario != 1, scenario != 2, scenario != 3, scenario != 4};
            RenderGraph graph("unsupported_mip_blits");
            ASSERT_TRUE(graph.Begin());

            RHITexture* texture = nullptr;

            if (logical)
            {
                const uint32_t creations = rhi->textureCreations;
                graph.AddTransferPass("mips").GenerateMipmaps(
                    graph.GetResourceManager()->CreateTexture(desc));
                EXPECT_EQ(rhi->textureCreations, creations);
            }
            else
            {
                RHITextureCreateInfo info{};
                info.type    = RHITextureType::e2D;
                info.format  = desc.texFormat.format;
                info.samples = desc.texFormat.sampleCount;
                info.width = info.height = 8;
                info.mipmaps             = 3;
                info.usageFlags.SetFlags(RHITextureUsageFlagBits::eTransferSrc,
                                         RHITextureUsageFlagBits::eTransferDst);
                texture = rhi->CreateTexture(info);
                graph.AddTransferPass("mips").GenerateMipmaps(texture);
            }

            const uint32_t creations = rhi->textureCreations;
            EXPECT_FALSE(graph.End());
            EXPECT_EQ(graph.GetResult().code,
                      scenario < 5 ? RDGErrorCode::eBinding : RDGErrorCode::eRange);
            EXPECT_FALSE(device->ExecuteRenderGraph(graph));
            EXPECT_EQ(rhi->textureCreations, creations);

            if (texture != nullptr)
            {
                device->DestroyTexture(texture);
            }
        }
    }

    EXPECT_TRUE(rhi->graphics.blits.empty());
    EXPECT_EQ(rhi->finalizedLists, 0u);
}

TEST_F(RenderCoreTest, UnsupportedUploadCapabilitiesDoNotFlushEarlierPendingWork)
{
    StagingBufferManager manager(32, 32);
    StagingUploadQueue queue(device, &manager);
    TestBuffer* buffer        = Buffer(32);
    RHITexture* texture       = Texture(3);
    const uint32_t references = texture->GetRefCount();
    std::array<uint8_t, 32> payload{};
    payload.fill(61);
    queue.EnqueueBuffer(buffer, 0, payload.size(), payload.data());
    RHIBufferTextureCopyRegion region{};
    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.textureSize = {1, 1, 1};

    for (uint32_t scenario = 0; scenario < 2; ++scenario)
    {
        rhi->copyCapabilities[texture->GetBaseInfo().format] = {true, scenario != 0, true, true,
                                                                false};
        queue.EnqueueTexture(texture, MakeVecView(&region, 1), payload.size(), payload.data(),
                             scenario == 1);
        EXPECT_TRUE(queue.HasPending());
        EXPECT_EQ(texture->GetRefCount(), references);
        EXPECT_EQ(rhi->finalizedLists, 0u);
    }

    rhi->copyCapabilities.clear();
    queue.Flush();

    EXPECT_FALSE(queue.HasPending());
    EXPECT_EQ(buffer->bytes, (std::vector<uint8_t>(payload.begin(), payload.end())));
    EXPECT_TRUE(rhi->graphics.textureCopies.empty());
    EXPECT_TRUE(rhi->transfer.textureCopies.empty());

    queue.Destroy();
    manager.Destroy();
    device->DestroyBuffer(buffer);
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, TextureStagingRebasingPreservesTexelAlignmentAndPayloadOffsets)
{
    const std::pair<DataFormat, uint32_t> formats[] = {{DataFormat::eR8G8B8UNORM, 3},
                                                       {DataFormat::eR16G16B16SFloat, 6},
                                                       {DataFormat::eR32G32B32SFloat, 12},
                                                       {DataFormat::eR64G64B64SFloat, 24},
                                                       {DataFormat::eR64G64B64A64SFloat, 32}};

    for (const std::pair<DataFormat, uint32_t> formatSize : formats)
    {
        for (const uint32_t payloadOffset : {0u, formatSize.second})
        {
            SCOPED_TRACE(uint32_t(formatSize.first));
            SCOPED_TRACE(payloadOffset);
            StagingBufferManager manager(256, 256);
            StagingUploadQueue queue(device, &manager);
            StagingAllocation prefix;
            ASSERT_EQ(manager.Allocate(16, 16, &prefix), StagingFlushAction::eNone);

            RHITextureCreateInfo info{};
            info.type   = RHITextureType::e2D;
            info.format = formatSize.first;
            info.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);
            RHITexture* texture = rhi->CreateTexture(info);
            RHIBufferTextureCopyRegion region{};
            region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
            region.textureSize  = {1, 1, 1};
            region.bufferOffset = payloadOffset;
            std::vector<uint8_t> payload(payloadOffset + formatSize.second, 73);
            queue.EnqueueTexture(texture, MakeVecView(&region, 1), payload.size(), payload.data());

            ASSERT_TRUE(queue.HasPending());

            rhi->graphics.textureCopies.clear();
            rhi->transfer.textureCopies.clear();
            queue.Flush();

            EXPECT_FALSE(queue.HasPending());

            const bool graphics = payloadOffset % 4 != 0;
            const std::vector<RHIBufferTextureCopyRegion>& copies =
                graphics ? rhi->graphics.textureCopies : rhi->transfer.textureCopies;
            ASSERT_EQ(copies.size(), 1u);

            const uint64_t baseOffset = formatSize.second == 32 ? 32 : 48;
            EXPECT_EQ(copies[0].bufferOffset, baseOffset + payloadOffset);
            EXPECT_EQ(copies[0].bufferOffset % formatSize.second, 0u);

            const std::vector<uint8_t>& staged = static_cast<TestBuffer*>(prefix.pBuffer)->bytes;
            EXPECT_EQ(std::vector<uint8_t>(staged.begin() + baseOffset,
                                           staged.begin() + baseOffset + payload.size()),
                      payload);

            queue.Destroy();
            manager.Release(prefix, {});
            manager.Destroy();
            device->DestroyTexture(texture);
        }
    }
}

TEST_F(RenderCoreTest, TextureUploadsRejectTruncatedPayloadBeforeStaging)
{
    StagingBufferManager manager(1024, 1024);
    StagingUploadQueue queue(device, &manager);
    RHITexture* texture       = Texture();
    const uint32_t references = texture->GetRefCount();
    RHIBufferTextureCopyRegion region{};
    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.textureSize = {8, 8, 1};
    std::array<uint8_t, 4> truncated{1, 2, 3, 4};
    queue.EnqueueTexture(texture, MakeVecView(&region, 1), truncated.size(), truncated.data());

    EXPECT_FALSE(queue.HasPending());
    EXPECT_EQ(texture->GetRefCount(), references);

    queue.Flush();

    EXPECT_EQ(rhi->finalizedLists, 0u);
    EXPECT_TRUE(rhi->transfer.textureCopies.empty());
    EXPECT_TRUE(rhi->graphics.textureCopies.empty());

    // The full 256-byte payload fits exactly. The rejected request consumed no staging space.
    std::array<uint8_t, 256> complete{};
    queue.EnqueueTexture(texture, MakeVecView(&region, 1), complete.size(), complete.data());
    queue.Flush();

    EXPECT_FALSE(queue.HasPending());
    EXPECT_EQ(rhi->transfer.textureCopies.size(), 1u);

    if (!rhi->transfer.textureCopies.empty())
    {
        EXPECT_EQ(rhi->transfer.textureCopies[0].bufferOffset, 0u);
    }

    queue.Destroy();
    manager.Destroy();
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, InvalidTextureUploadDoesNotFlushOrChangeEarlierPendingUploads)
{
    StagingBufferManager manager(32, 32);
    StagingUploadQueue queue(device, &manager);
    TestBuffer* buffer  = Buffer(32);
    RHITexture* texture = Texture();
    std::array<uint8_t, 32> earlier{};
    earlier.fill(71);
    queue.EnqueueBuffer(buffer, 0, earlier.size(), earlier.data());

    EXPECT_TRUE(queue.HasPending());

    const uint32_t references = texture->GetRefCount();

    std::array<RHIBufferTextureCopyRegion, 2> regions{};

    for (RHIBufferTextureCopyRegion& region : regions)
    {
        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.textureSize = {1, 1, 1};
    }

    regions[1].bufferOffset = 4;
    regions[1].textureSize  = {2, 1, 1};
    std::array<uint8_t, 8> truncated{};
    // Region 0 fits; region 1 exceeds the payload. Staging it would force the earlier flush.
    queue.EnqueueTexture(texture, regions, truncated.size(), truncated.data());

    EXPECT_EQ(rhi->finalizedLists, 0u);
    EXPECT_EQ(texture->GetRefCount(), references);
    EXPECT_TRUE(queue.HasPending());

    queue.Flush();

    EXPECT_FALSE(queue.HasPending());
    EXPECT_EQ(buffer->bytes, (std::vector<uint8_t>(earlier.begin(), earlier.end())));
    EXPECT_EQ(rhi->transfer.bufferCopies.size(), 1u);
    EXPECT_TRUE(rhi->transfer.textureCopies.empty());
    EXPECT_TRUE(rhi->graphics.textureCopies.empty());

    queue.Destroy();
    manager.Destroy();
    device->DestroyBuffer(buffer);
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, TextureUploadPayloadBoundsIncludeOffsetsMipsLayersAndDepth)
{
    for (uint32_t scenario = 0; scenario < 4; ++scenario)
    {
        SCOPED_TRACE(scenario);
        StagingBufferManager manager(1024, 1024);
        StagingUploadQueue queue(device, &manager);
        // An outstanding prefix forces a nonzero staging offset for the accepted upload.
        StagingAllocation prefix;
        ASSERT_EQ(manager.Allocate(16, 16, &prefix), StagingFlushAction::eNone);

        RHITextureCreateInfo info{};
        info.format = DataFormat::eR8G8B8A8UNORM;
        info.type   = RHITextureType::e2D;
        info.width = info.height = 8;
        info.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);
        RHIBufferTextureCopyRegion region{};
        region.bufferOffset = 16;
        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.textureSize = {8, 8, 1};
        uint32_t copyBytes = 0;

        switch (scenario)
        {
            case 0:
                info.mipmaps                          = 2;
                info.arrayLayers                      = 2;
                region.textureSubresources.mipmap     = 1;
                region.textureSubresources.layerCount = 2;
                region.textureSize                    = {4, 4, 1};
                copyBytes                             = 128; // Two 4x4 RGBA8 layers at mip 1.
                break;

            case 1:
                info.type  = RHITextureType::e3D;
                info.width = info.height = info.depth = 4;
                region.textureOffset                  = {1, 1, 1};
                region.textureSize                    = {2, 2, 2};
                copyBytes = 32; // Eight RGBA8 texels in a partial 3D box.
                break;

            case 2:
                info.format = DataFormat::eR8UNORM;
                copyBytes   = 64; // 8x8 single-byte texels.
                break;

            case 3:
                info.format        = DataFormat::eR32G32B32A32SFloat;
                region.textureSize = {2, 2, 1};
                copyBytes          = 64; // Four 16-byte texels.
                break;
        }

        RHITexture* texture       = rhi->CreateTexture(info);
        const uint32_t references = texture->GetRefCount();
        std::vector<uint8_t> payload(16 + copyBytes, uint8_t(29 + scenario));
        queue.EnqueueTexture(texture, MakeVecView(&region, 1), payload.size() - 1, payload.data());

        EXPECT_FALSE(queue.HasPending());
        EXPECT_EQ(texture->GetRefCount(), references);

        queue.EnqueueTexture(texture, MakeVecView(&region, 1), payload.size(), payload.data());

        EXPECT_TRUE(queue.HasPending());

        // StageBytes owns a snapshot at offset 16, independently of the copy's payload offset.
        const std::vector<uint8_t>& staged = static_cast<TestBuffer*>(prefix.pBuffer)->bytes;
        EXPECT_EQ(std::vector<uint8_t>(staged.begin() + 16, staged.begin() + 16 + payload.size()),
                  payload);

        std::fill(payload.begin(), payload.end(), 0xFF);
        rhi->transfer.textureCopies.clear();
        queue.Flush();

        EXPECT_FALSE(queue.HasPending());
        EXPECT_EQ(rhi->transfer.textureCopies.size(), 1u);

        if (!rhi->transfer.textureCopies.empty())
        {
            EXPECT_EQ(rhi->transfer.textureCopies[0].bufferOffset, 32u);
        }

        EXPECT_EQ(staged[32], uint8_t(29 + scenario));

        queue.Destroy();
        manager.Release(prefix, {});
        manager.Destroy();
        device->DestroyTexture(texture);
    }
}

TEST_F(RenderCoreTest, TextureUploadsRejectMalformedRegionsBeforeStaging)
{
    StagingBufferManager manager(1024, 1024);
    StagingUploadQueue queue(device, &manager);
    RHITexture* texture       = Texture();
    const uint32_t references = texture->GetRefCount();
    std::array<uint8_t, 256> payload{};
    RHIBufferTextureCopyRegion valid{};
    valid.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    valid.textureSize = {8, 8, 1};

    for (uint32_t scenario = 0; scenario < 8; ++scenario)
    {
        SCOPED_TRACE(scenario);
        RHIBufferTextureCopyRegion region = valid;

        switch (scenario)
        {
            case 0: region.bufferOffset = UINT64_MAX; break;
            case 1: region.bufferOffset = payload.size(); break;
            case 2: region.textureSize.x = 0; break;
            case 3: region.textureSize.y = -1; break;
            case 4: region.textureSubresources.mipmap = UINT32_MAX; break;
            case 5: region.textureSubresources.layerCount = UINT32_MAX; break;
            case 6: region.textureOffset.z = 1; break;
            case 7: region.textureSubresources.aspect.Clear(); break;
        }

        queue.EnqueueTexture(texture, MakeVecView(&region, 1), payload.size(), payload.data());

        EXPECT_FALSE(queue.HasPending());
        EXPECT_EQ(texture->GetRefCount(), references);
    }

    queue.EnqueueTexture(nullptr, MakeVecView(&valid, 1), payload.size(), payload.data());
    queue.EnqueueTexture(texture, {nullptr, 1}, payload.size(), payload.data());

    EXPECT_FALSE(queue.HasPending());
    EXPECT_EQ(texture->GetRefCount(), references);

    queue.Flush();

    EXPECT_EQ(rhi->finalizedLists, 0u);

    queue.Destroy();
    manager.Destroy();
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, TextureUploadsCopyRegionsAndGenerateEachMip)
{
    device->GetRDGMetrics().SetSink({});
    StagingBufferManager manager(32, 512);
    StagingUploadQueue queue(device, &manager);
    RHITexture* texture = Texture(4);
    RHIBufferTextureCopyRegion region{};
    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.textureSize = {8, 8, 1};
    std::vector<uint8_t> data(8 * 8 * 4);
    queue.EnqueueTexture(texture, MakeVecView(&region, 1), data.size(), data.data(), true);
    region.textureSize = {1, 1, 1};
    queue.Flush();
    const RDGMetricsSnapshot& sample = device->GetRDGMetrics().GetLastSnapshot();
    EXPECT_TRUE(sample.validated);
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eUnknownImportState)], 0u);
    ASSERT_EQ(rhi->graphics.textureCopies.size(), 1u);
    EXPECT_EQ(rhi->graphics.textureCopies[0].textureSize.x, 8);
    EXPECT_EQ(rhi->graphics.blits.size(), 3u);

    std::vector<uint32_t> transitionedMips;

    for (RHITextureTransition const& transition : rhi->graphics.textureTransitions)
    {
        if (transition.newUsage == RHITextureUsage::eTransferSrc)
        {
            EXPECT_EQ(transition.subResourceRange.levelCount, 1u);
            transitionedMips.push_back(transition.subResourceRange.baseMipLevel);
        }
    }

    EXPECT_EQ(transitionedMips, (std::vector<uint32_t>{0, 1, 2, 3}));

    queue.Destroy();
    manager.Destroy();
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, TextureCopyDeclaresSourceAsRead)
{
    RHITexture* source      = Texture();
    RHITexture* destination = Texture();
    RHITextureCopyRegion region{};
    region.size = {1, 1, 1};
    region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    RenderGraph graph("texture_copy");
    graph.Begin();
    graph.AddTransferPass("copy").CopyTexture(source, destination, MakeVecView(&region, 1));
    graph.End();
    device->ExecuteRenderGraph(graph);
    bool found = false;

    for (RHITextureTransition const& transition : rhi->transfer.textureTransitions)
    {
        if (transition.pTexture == source)
        {
            found = true;
            EXPECT_EQ(transition.newAccessMode, RHIAccessMode::eRead);
            EXPECT_EQ(transition.newUsage, RHITextureUsage::eTransferSrc);
        }
    }

    EXPECT_TRUE(found);

    device->DestroyTexture(source);
    device->DestroyTexture(destination);
}

TEST_F(RenderCoreTest, GraphicsAndComputePassesCompileBindingsAndTransientOutputs)
{
    CreateTestShaderProgram(device, "test");
    TestBuffer* vertex     = Buffer();
    std::string outputText = "output";
    const NameID outputName(outputText);
    const NameID sampledBinding("texture");
    RenderGraph graph("shader_passes");
    graph.Begin();

    RDGGraphicsPassDesc graphics{};
    graphics.SetShaderProgramName("test");
    graphics.SetPassTag("graphics");
    graphics.SetRenderArea(0, 0, 8, 8);
    graphics.BindVertexBuffer(vertex);
    graphics.AddColorOutput(DataFormat::eR8G8B8A8UNORM, 8, 8, outputName);
    graph.AddGraphicsPass(std::move(graphics)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
        encoder.Draw(3, 1);
    });

    RDGComputePassDesc compute{};
    compute.SetShaderProgramName("test");
    compute.SetPassTag("compute");
    compute.BindSampledTexture(sampledBinding, nullptr, outputName);
    outputText.clear();

    graph.AddComputePass(std::move(compute)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(1, 1, 1);
    });
    graph.End();
    device->ExecuteRenderGraph(graph);

    EXPECT_EQ(rhi->graphics.drawCount, 1u);
    EXPECT_EQ(rhi->graphics.dispatchCount, 1u);
    EXPECT_EQ(rhi->graphics.vertexBufferCount, 1u);
    EXPECT_EQ(rhi->graphics.renderingCount, 1u);
    EXPECT_EQ(rhi->pipelineCount, 2u);
    ASSERT_FALSE(rhi->graphics.boundResources.empty());
    EXPECT_NE(rhi->graphics.boundResources.back(), nullptr);

    graph.Begin();
    graph.End();
    device->DestroyBuffer(vertex);
}

TEST_F(RenderCoreTest, AttachmentClearToLoadBarriersCoverBlendAndDepthTestReads)
{
    CreateTestShaderProgram(device, "attachment_access");

    RHITextureCreateInfo colorInfo{};
    colorInfo.format = DataFormat::eR8G8B8A8UNORM;
    colorInfo.type   = RHITextureType::e2D;
    colorInfo.width = colorInfo.height = 8;
    colorInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eColorAttachment);
    RHITexture* color = rhi->CreateTexture(colorInfo);

    RHITextureCreateInfo depthInfo = colorInfo;
    depthInfo.format               = DataFormat::eD32SFloat;
    depthInfo.usageFlags.Clear();
    depthInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eDepthStencilAttachment);
    RHITexture* depth = rhi->CreateTexture(depthInfo);

    RenderGraph graph("attachment_clear_load");
    graph.Begin();

    RDGGraphicsPassDesc clear{};
    clear.SetShaderProgramName("attachment_access");
    clear.SetPassTag("clear");
    clear.SetRenderArea(0, 0, 8, 8);
    clear.pipelineStates.colorBlendState.AddAttachment();
    clear.pipelineStates.depthStencilState =
        RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);
    clear.AddColorOutput(color, RHIRenderTargetLoadOp::eClear);
    clear.AddDepthStencilOutput(depth, RHIRenderTargetLoadOp::eClear);
    graph.AddGraphicsPass(clear).RecordPassCommands(
        [](RDGPassCmdEncoder& encoder) { encoder.Draw(3, 1); });

    RDGGraphicsPassDesc load = clear;
    load.SetPassTag("load_blend_depth_test");
    load.colorOutputs[0].loadOp                            = RHIRenderTargetLoadOp::eLoad;
    load.depthStencilOutput.loadOp                         = RHIRenderTargetLoadOp::eLoad;
    load.pipelineStates.depthStencilState.enableDepthWrite = false;
    RHIGfxPipelineColorBlendState::Attachment& blend =
        load.pipelineStates.colorBlendState.attachments[0];
    blend.enableBlend         = true;
    blend.srcColorBlendFactor = RHIBlendFactor::eSrcAlpha;
    blend.dstColorBlendFactor = RHIBlendFactor::eOneMinusSrcAlpha;
    graph.AddGraphicsPass(std::move(load)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
        encoder.Draw(3, 1);
    });
    graph.End();
    device->ExecuteRenderGraph(graph);

    ASSERT_EQ(rhi->graphics.renderingLayouts.size(), 2u);

    const RHIRenderingLayout& loadLayout = rhi->graphics.renderingLayouts[1];
    EXPECT_EQ(loadLayout.colorRenderTargets[0].loadOp, RHIRenderTargetLoadOp::eLoad);
    EXPECT_EQ(loadLayout.depthStencilRenderTarget.loadOp, RHIRenderTargetLoadOp::eLoad);

    uint32_t loadTransitions = 0;

    for (TestContext::BarrierBatch const& batch : rhi->graphics.barrierBatches)
    {
        for (const RHITextureTransition& transition : batch.textures)
        {
            // Skip the initial undefined-layout transitions into the clear pass.
            if (transition.oldUsage == RHITextureUsage::eNone)
            {
                continue;
            }

            ASSERT_TRUE(transition.pTexture == color || transition.pTexture == depth);

            const bool isColor                  = transition.pTexture == color;
            const RHITextureUsage expectedUsage = isColor ?
                RHITextureUsage::eColorAttachment :
                RHITextureUsage::eDepthStencilAttachment;
            EXPECT_EQ(transition.oldUsage, expectedUsage);
            EXPECT_EQ(transition.newUsage, expectedUsage);

            const BitField<RHIAccessFlagBits> access =
                RHITextureUsageToAccessFlagBits(transition.newUsage, transition.newAccessMode);
            EXPECT_TRUE(access.HasFlag(isColor ? RHIAccessFlagBits::eColorAttachmentRead :
                                                 RHIAccessFlagBits::eDepthStencilAttachmentRead));

            const BitField<RHIAccessFlagBits> sourceAccess =
                RHITextureUsageToAccessFlagBits(transition.oldUsage, transition.oldAccessMode);
            EXPECT_TRUE(sourceAccess.HasFlag(isColor ?
                                                 RHIAccessFlagBits::eColorAttachmentWrite :
                                                 RHIAccessFlagBits::eDepthStencilAttachmentWrite));

            if (isColor)
            {
                EXPECT_TRUE(access.HasFlag(RHIAccessFlagBits::eColorAttachmentWrite));
                EXPECT_TRUE(batch.source.HasFlag(RHIPipelineStageFlagBits::eColorAttachmentOutput));
                EXPECT_TRUE(
                    batch.destination.HasFlag(RHIPipelineStageFlagBits::eColorAttachmentOutput));
            }
            else
            {
                EXPECT_TRUE(batch.source.HasFlag(RHIPipelineStageFlagBits::eLateFragmentTests));
                EXPECT_TRUE(
                    batch.destination.HasFlag(RHIPipelineStageFlagBits::eEarlyFragmentTests));
                EXPECT_TRUE(
                    batch.destination.HasFlag(RHIPipelineStageFlagBits::eLateFragmentTests));
            }

            ++loadTransitions;
        }
    }

    EXPECT_EQ(loadTransitions, 2u);

    graph.Begin();
    graph.End();
    device->DestroyTexture(color);
    device->DestroyTexture(depth);
}

TEST_F(RenderCoreTest, PipelineCacheHashesStateAndDefersEviction)
{
    ShaderProgram* program = CreateTestShaderProgram(device, "cache");
    RHIRenderingLayout layout{};

    RHIGfxPipelineStates states{};
    RHIPipeline* first =
        device->GetOrCreateGfxPipeline(states, program->GetShader(), &layout, {{1, 2}, {3, 4}});
    EXPECT_EQ(first->GetShader()->GetCreateInfo().specializationConstants.at(1), 2);
    EXPECT_TRUE(program->GetShader()->GetCreateInfo().specializationConstants.empty());
    EXPECT_EQ(
        first,
        device->GetOrCreateGfxPipeline(states, program->GetShader(), &layout, {{3, 4}, {1, 2}}));

    const uint64_t firstId = first->GetStableId();

    for (uint32_t i = 0; i < 260; ++i)
    {
        states.rasterizationState.lineWidth = static_cast<float>(i + 2);
        EXPECT_NE(first, device->GetOrCreateGfxPipeline(states, program->GetShader(), &layout, {}));
    }

    EXPECT_FALSE(destroyed.contains(firstId));

    device->NextFrame();
    device->NextFrame();

    EXPECT_TRUE(destroyed.contains(firstId));
}

TEST_F(RenderCoreTest, RecordedShaderParametersOwnTheirBytes)
{
    RHICommandList* commands =
        RHICommandList::Create(rhi->GetCommandContext(RHICommandContextType::eGraphics));

    {
        RHIBatchedShaderParameters parameters;
        RHIShaderResourceDescriptor descriptor{};
        const uint32_t value = 0x12345678;
        parameters.AddValueParam(descriptor, value);
        commands->SetShaderParameters(parameters);
    }

    commands->Execute();

    ASSERT_EQ(rhi->graphics.values.size(), 1u);

    uint32_t result = 0;
    std::memcpy(&result, rhi->graphics.values[0].data(), sizeof(result));

    EXPECT_EQ(result, 0x12345678u);

    commands->Reset();
    ZEN_DELETE(commands);
}

TEST_F(RenderCoreTest, RecordedPushConstantsSnapshotEachDraw)
{
    RHICommandList* commands =
        RHICommandList::Create(rhi->GetCommandContext(RHICommandContextType::eGraphics));
    std::array<uint32_t, 2> constants{0, 7};
    commands->SetPushConstants(nullptr, reinterpret_cast<const uint8_t*>(constants.data()),
                               sizeof(constants), 0);
    constants = {1, 19};
    commands->SetPushConstants(nullptr, reinterpret_cast<const uint8_t*>(constants.data()),
                               sizeof(constants), 0);
    constants.fill(0xFFFFFFFF);

    commands->Execute();
    commands->Reset();
    ZEN_DELETE(commands);

    ASSERT_EQ(rhi->graphics.pushConstants.size(), 2u);

    const std::array<uint32_t, 2> expected[] = {{0, 7}, {1, 19}};

    for (uint32_t i = 0; i < 2; ++i)
    {
        ASSERT_EQ(rhi->graphics.pushConstants[i].size(), sizeof(constants));
        std::array<uint32_t, 2> recorded{};
        std::memcpy(recorded.data(), rhi->graphics.pushConstants[i].data(), sizeof(recorded));
        EXPECT_EQ(recorded, expected[i]);
    }
}

TEST_F(RenderCoreTest, TextureViewUsesRequestedBaseMip)
{
    RHITexture* texture = Texture(4);
    TextureViewFormat format{};
    format.format        = texture->GetFormat();
    format.dimension     = TextureDimension::e2D;
    format.baseMipLevel  = 2;
    RHITextureView* view = device->CreateTextureView(texture, format, "mip_two");
    EXPECT_EQ(view->GetSubResourceRange().baseMipLevel, 2u);
    EXPECT_EQ(view->GetSubResourceRange().levelCount, 1u);
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, BufferReaderStagesSurviveGraphBoundariesAndForceGraphicsQueue)
{
    CreateTestShaderProgram(device, "readers");
    TestBuffer* vertex  = Buffer();
    TestBuffer* scratch = Buffer();
    RenderGraph graphics("graphics_reader");
    graphics.Begin();

    RDGGraphicsPassDesc desc{};
    desc.SetShaderProgramName("readers");
    desc.BindVertexBuffer(vertex);
    graphics.AddGraphicsPass(std::move(desc));
    graphics.End();
    device->ExecuteRenderGraph(graphics);
    rhi->graphics.bufferTransitions.clear();
    rhi->graphics.barrierSources.clear();
    const uint64_t transferSerial = rhi->submitted[2];
    RenderGraph copies("read_then_write");
    copies.Begin();
    copies.AddTransferPass("read").CopyBuffer(vertex, scratch, {0, 0, 4});
    copies.AddTransferPass("write").CopyBuffer(scratch, vertex, {0, 0, 4});
    copies.End();
    device->ExecuteRenderGraph(copies);

    EXPECT_EQ(rhi->submitted[2], transferSerial);

    bool vertexRead = false, transferRead = false;

    for (RHIBufferTransition const& transition : rhi->graphics.bufferTransitions)
    {
        if (transition.pBuffer == vertex && transition.newUsage == RHIBufferUsage::eTransferDst)
        {
            vertexRead |= transition.oldUsage == RHIBufferUsage::eVertexBuffer;
            transferRead |= transition.oldUsage == RHIBufferUsage::eTransferSrc;
        }
    }

    EXPECT_TRUE(vertexRead);
    EXPECT_TRUE(transferRead);
    EXPECT_TRUE(
        rhi->graphics.barrierSources.back().HasFlag(RHIPipelineStageFlagBits::eVertexInput));

    device->DestroyBuffer(vertex);
    device->DestroyBuffer(scratch);
}

TEST_F(RenderCoreTest, TransferOperationsWithinOnePassAreOrdered)
{
    TestBuffer* first  = Buffer();
    TestBuffer* second = Buffer();
    first->bytes[0]    = 57;
    RenderGraph graph("dependent_copies");
    graph.Begin();
    graph.AddTransferPass("copy_chain")
        .CopyBuffer(first, second, {0, 0, 4})
        .CopyBuffer(second, first, {0, 4, 4});
    graph.End();
    device->ExecuteRenderGraph(graph);

    EXPECT_EQ(first->bytes[4], 57);
    ASSERT_EQ(rhi->transfer.memoryBarriers.size(), 1u);
    EXPECT_TRUE(
        rhi->transfer.memoryBarriers[0].srcAccess.HasFlag(RHIAccessFlagBits::eTransferWrite));
    EXPECT_TRUE(
        rhi->transfer.memoryBarriers[0].dstAccess.HasFlag(RHIAccessFlagBits::eTransferRead));

    device->DestroyBuffer(first);
    device->DestroyBuffer(second);
}

TEST_F(RenderCoreTest, CompiledPassKeepsEvictedPipelineAliveUntilGraphReset)
{
    ShaderProgram* program = CreateTestShaderProgram(device, "pinned");
    RenderGraph graph("pinned_pipeline");
    graph.Begin();

    RDGComputePassDesc desc{};
    desc.SetShaderProgramName("pinned");
    graph.AddComputePass(std::move(desc)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(1, 1, 1);
    });
    graph.End();
    device->ExecuteRenderGraph(graph);
    RHIPipeline* pipeline = device->GetOrCreateComputePipeline(program->GetShader());
    const uint64_t id     = pipeline->GetStableId();
    RHIRenderingLayout layout{};

    RHIGfxPipelineStates states{};

    for (uint32_t i = 0; i < 260; ++i)
    {
        states.rasterizationState.lineWidth = static_cast<float>(i + 1);
        device->GetOrCreateGfxPipeline(states, program->GetShader(), &layout, {});
    }

    device->NextFrame();
    device->NextFrame();

    EXPECT_FALSE(destroyed.contains(id));

    device->ExecuteRenderGraph(graph);

    EXPECT_EQ(rhi->graphics.dispatchCount, 2u);

    graph.Begin();
    graph.End();
    device->NextFrame();
    device->NextFrame();

    EXPECT_TRUE(destroyed.contains(id));
}

TEST(RenderCoreUtilities, NameIdsRetrieveAndInternDistinctRecords)
{
    const NameID first("render_core_first");
    const NameID second("render_core_second");
    EXPECT_EQ(first.ToString(), "render_core_first");
    EXPECT_EQ(second.ToString(), "render_core_second");
    EXPECT_EQ(first, NameID("render_core_first"));
    EXPECT_NE(first, second);
    EXPECT_TRUE(NameID().ToString().empty());
}

TEST_F(RenderCoreTest, InternedNamesSurviveCallerStorageAndGraphReset)
{
    std::string shaderText = "interned_shader";
    const NameID shaderName(shaderText);
    ShaderProgram* program = CreateTestShaderProgram(device, shaderName);
    shaderText.assign("changed_shader");

    EXPECT_EQ(ShaderProgramManager::GetInstance().RequestShaderProgram("interned_shader"), program);
    EXPECT_EQ(program->GetName(), shaderName);
    EXPECT_EQ(program->GetShader()->GetCreateInfo().name, shaderName);

    std::string resourceText = "interned_buffer";
    const NameID resourceName(resourceText);
    RHIBuffer* buffer = device->CreateStorageBuffer(64, nullptr, resourceName);
    ASSERT_NE(buffer, nullptr);
    resourceText.clear();

    EXPECT_EQ(buffer->GetResourceTag(), resourceName);

    RDGMetrics& metrics         = device->GetRDGMetrics();
    RDGMetricsOptions options   = metrics.GetOptions();
    options.logging.enabled     = true;
    options.logging.sampleEvery = 1;
    options.logging.minInterval = std::chrono::milliseconds(0);
    metrics.Configure(options);
    metrics.SetSink({});

    RenderGraph graph("interned_graph");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer imported      = resources->ImportBuffer(buffer);
    EXPECT_EQ(DescribeResource(resources, imported).name, resourceName);

    RDGComputePassDesc pass;
    pass.SetShaderProgramName(shaderName);
    pass.SetPassTag(NameID(std::string("interned_pass")));
    std::string bindingText = "read_buffer";
    const NameID bindingName(bindingText);
    pass.BindStorageBuffer(bindingName, imported);
    bindingText.clear();

    graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(1, 1, 1);
    });

    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    const RDGMetricsSnapshot snapshot = metrics.GetLastSnapshot();
    ASSERT_EQ(snapshot.nodes.size(), 1u);
    ASSERT_FALSE(snapshot.diagnostics.empty());
    EXPECT_EQ(snapshot.diagnostics[0].resourceName, resourceName);

    ASSERT_TRUE(graph.Reset());
    EXPECT_EQ(snapshot.graph, NameID("interned_graph"));
    EXPECT_EQ(snapshot.nodes[0].name, NameID("interned_pass"));
    EXPECT_NE(RDGMetrics::Format(snapshot).find("interned_buffer"), std::string::npos);

    device->DestroyBuffer(buffer);
}

TEST(RenderCoreUtilities, VectorRemainsUsableAfterAssigningEmptyStorage)
{
    HeapVector<int> vector{1, 2, 3};
    const HeapVector<int> empty;
    vector = empty;
    EXPECT_TRUE(vector.empty());
    vector.push_back(42);
    EXPECT_EQ(vector[0], 42);
}

TEST_F(RenderCoreTest, PassValuesSnapshotDataAndSupportUpdatingReusableDescriptions)
{
    CreateTestShaderProgram(device, "uniforms");

    const NameID valueName("value");
    RDGComputePassDesc desc{};
    desc.SetShaderProgramName("uniforms");
    std::array<uint32_t, 2> value{7, 11};
    desc.BindValue(valueName, value);
    RenderGraph graph("uniform_snapshots");
    graph.Begin();
    graph.AddComputePass(desc);
    value = {19, 23};
    desc.BindValue(valueName, value);
    graph.AddComputePass(desc);
    desc.BindValue(valueName, uint32_t(29));
    graph.AddComputePass(desc);
    graph.End();
    device->ExecuteRenderGraph(graph);

    ASSERT_EQ(rhi->graphics.values.size(), 3u);

    for (size_t i = 0; i < 3; ++i)
    {
        uint32_t recorded;
        std::memcpy(&recorded, rhi->graphics.values[i].data(), sizeof(recorded));
        EXPECT_EQ(recorded, (std::array<uint32_t, 3>{7, 19, 29})[i]);
    }

    EXPECT_EQ(rhi->graphics.values[2].size(), sizeof(uint32_t));
}

TEST_F(RenderCoreTest, ComputeWritesTransitionToIndirectDrawAndDispatchReads)
{
    CreateTestShaderProgram(device, "indirect");
    TestBuffer* buffer = Buffer();
    TestBuffer* index  = Buffer();
    RenderGraph graph("indirect_commands");
    graph.Begin();

    RDGComputePassDesc producer{};
    producer.SetShaderProgramName("indirect");
    producer.BindStorageBuffer("buffer", buffer);
    graph.AddComputePass(producer);

    RDGComputePassDesc dispatch{};
    dispatch.SetShaderProgramName("indirect");
    dispatch.UseIndirectBuffer(buffer);
    graph.AddComputePass(dispatch).RecordPassCommands(
        [buffer](RDGPassCmdEncoder& encoder) { encoder.DispatchIndirect(buffer, 0); });
    graph.AddComputePass(producer);

    RDGGraphicsPassDesc draw{};
    draw.SetShaderProgramName("indirect");
    draw.BindIndexBuffer(index);
    draw.UseIndirectBuffer(buffer);
    graph.AddGraphicsPass(draw).RecordPassCommands(
        [buffer](RDGPassCmdEncoder& encoder) { encoder.DrawIndexedIndirect(buffer, 0, 1, 20); });
    graph.End();
    device->ExecuteRenderGraph(graph);
    uint32_t barriers = 0;

    for (RHIBufferTransition const& transition : rhi->graphics.bufferTransitions)
    {
        if (transition.pBuffer == buffer && transition.oldUsage == RHIBufferUsage::eStorageBuffer &&
            transition.newUsage == RHIBufferUsage::eIndirectBuffer)
        {
            EXPECT_EQ(transition.oldAccessMode, RHIAccessMode::eReadWrite);
            EXPECT_EQ(transition.newAccessMode, RHIAccessMode::eRead);
            ++barriers;
        }
    }

    EXPECT_EQ(barriers, 2u);
    EXPECT_EQ(rhi->graphics.indirectDraws, std::vector<RHIBuffer*>{buffer});
    EXPECT_EQ(rhi->graphics.indirectDispatches, std::vector<RHIBuffer*>{buffer});

    device->DestroyBuffer(buffer);
    device->DestroyBuffer(index);
}

static void AddVoxelDraw(const RDGGraphicsPassDesc& draw, RHIBuffer* arguments, RenderGraph& graph)
{
    graph.AddGraphicsPass(draw).RecordPassCommands([arguments](RDGPassCmdEncoder& encoder) {
        encoder.DrawIndexedIndirect(arguments, 0, 1, 20);
    });
}

static void CheckVoxelMetrics(const RDGMetrics& metrics)
{
    const RDGMetricsSnapshot& sample = metrics.GetLastSnapshot();

    for (RDGMetricIssue issue :
         {RDGMetricIssue::eMissingBarrier, RDGMetricIssue::eStageCoverage,
          RDGMetricIssue::eAccessCoverage, RDGMetricIssue::eUnknownImportState})
    {
        EXPECT_EQ(sample.issues[size_t(issue)], 0u) << RDGMetrics::Format(sample);
    }

    ASSERT_FALSE(sample.nodes.empty());
    EXPECT_EQ(sample.nodes.back().name, "VoxelDraw2");
    EXPECT_EQ(sample.nodes.back().writes, 0u);
}

TEST_F(RenderCoreTest, ReflectedVoxelDrawReadsKeepInitializationAndSkipSteadyBarriers)
{
    RHIShaderCreateInfo& preDrawInfo = reflectedShaderInfos["reflected_pre_draw"];
    preDrawInfo.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);
    preDrawInfo.spirvFileName[ToUnderlying(RHIShaderStage::eCompute)] =
        "VoxelGI/voxel_pre_draw.comp.spv";

    RHIShaderCreateInfo& drawInfo = reflectedShaderInfos["reflected_draw"];
    drawInfo.stageFlags.SetFlags(RHIShaderStageFlagBits::eVertex,
                                 RHIShaderStageFlagBits::eFragment);
    drawInfo.spirvFileName[ToUnderlying(RHIShaderStage::eVertex)]   = "VoxelGI/voxel_vis.vert.spv";
    drawInfo.spirvFileName[ToUnderlying(RHIShaderStage::eFragment)] = "VoxelGI/voxel_vis.frag.spv";
    CreateTestShaderProgram(device, "reflected_pre_draw");
    CreateTestShaderProgram(device, "reflected_draw");
    RDGMetrics& metrics         = device->GetRDGMetrics();
    RDGMetricsOptions options   = metrics.GetOptions();
    options.logging.sampleEvery = 1;
    options.logging.minInterval = std::chrono::milliseconds(0);
    metrics.Configure(options);
    metrics.SetSink({});
    TestBuffer* positions = Buffer();
    TestBuffer* colors    = Buffer();
    TestBuffer* arguments = Buffer();
    TestBuffer* indices   = Buffer();
    RHITexture* albedo    = Texture();
    const std::array<uint8_t, 64> zeros{};
    device->UpdateBuffer(arguments, zeros.size(), zeros.data());
    device->UpdateBuffer(indices, zeros.size(), zeros.data());

    RDGComputePassDesc preDraw{};
    preDraw.SetShaderProgramName("reflected_pre_draw");
    preDraw.SetPassTag("VoxelPreDrawComp");
    preDraw.BindStorageImage("voxelTexture", albedo->GetDefaultView());
    preDraw.BindStorageBuffer("InstancePositionBuffer", positions);
    preDraw.BindStorageBuffer("InstanceColorBuffer", colors);
    preDraw.BindStorageBuffer("IndirectBuffer", arguments);

    RDGGraphicsPassDesc draw{};
    draw.SetShaderProgramName("reflected_draw");
    draw.SetPassTag("VoxelDraw2");
    draw.BindStorageBuffer("InstanceBuffer", positions);
    draw.BindStorageBuffer("InstanceColorBuffer", colors);
    draw.BindIndexBuffer(indices);
    draw.UseIndirectBuffer(arguments);

    RenderGraph voxelization("reflected_voxelization");
    voxelization.Begin();
    voxelization.AddTransferPass("reset").ClearTexture(albedo, Color(0.0f));
    voxelization.AddComputePass(preDraw).RecordPassCommands(
        [](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    AddVoxelDraw(draw, arguments, voxelization);
    voxelization.End();
    device->ExecuteRenderGraph(voxelization);
    CheckVoxelMetrics(metrics);
    bool colorVisibility = false;

    for (TestContext::BarrierBatch const& batch : rhi->graphics.barrierBatches)
    {
        for (const RHIBufferTransition& barrier : batch.buffers)
        {
            if (barrier.pBuffer == colors && barrier.newAccessMode == RHIAccessMode::eRead)
            {
                EXPECT_EQ(barrier.oldAccessMode, RHIAccessMode::eReadWrite);
                EXPECT_EQ(barrier.oldUsage, RHIBufferUsage::eStorageBuffer);
                EXPECT_EQ(barrier.newUsage, RHIBufferUsage::eStorageBuffer);
                EXPECT_TRUE(batch.source.HasFlag(RHIPipelineStageFlagBits::eComputeShader));
                EXPECT_TRUE(batch.destination.HasFlag(RHIPipelineStageFlagBits::eFragmentShader));
                colorVisibility = true;
            }
        }
    }

    EXPECT_TRUE(colorVisibility);

    RenderGraph steady("reflected_steady_draw");
    steady.Begin();
    AddVoxelDraw(draw, arguments, steady);
    steady.End();

    for (int frame = 0; frame < 3; ++frame)
    {
        rhi->graphics.bufferTransitions.clear();
        device->ExecuteRenderGraph(steady);
        EXPECT_TRUE(rhi->graphics.bufferTransitions.empty());
        CheckVoxelMetrics(metrics);
    }

    rhi->graphics.barrierBatches.clear();
    device->ExecuteRenderGraph(voxelization);
    CheckVoxelMetrics(metrics);
    bool orderedOverwrite = false;

    for (TestContext::BarrierBatch const& batch : rhi->graphics.barrierBatches)
    {
        for (const RHIBufferTransition& barrier : batch.buffers)
        {
            if (barrier.pBuffer == colors && barrier.newAccessMode == RHIAccessMode::eReadWrite)
            {
                EXPECT_EQ(barrier.oldAccessMode, RHIAccessMode::eRead);
                EXPECT_TRUE(batch.source.HasFlag(RHIPipelineStageFlagBits::eFragmentShader));
                EXPECT_TRUE(batch.destination.HasFlag(RHIPipelineStageFlagBits::eComputeShader));
                orderedOverwrite = true;
            }
        }
    }

    EXPECT_TRUE(orderedOverwrite);
    EXPECT_EQ(rhi->graphics.indirectDraws.size(), 5u);

    for (TestBuffer* buffer : {positions, colors, arguments, indices})
    {
        device->DestroyBuffer(buffer);
    }

    device->DestroyTexture(albedo);
}

TEST_F(RenderCoreTest, RendererTextureBindingsUseViewsAndCanBeRebuilt)
{
    CreateTestShaderProgram(device, "textures");
    RHITexture* first   = Texture();
    RHITexture* second  = Texture();
    RHISampler* sampler = device->CreateSampler({});

    RDGComputePassDesc desc{};
    desc.SetShaderProgramName("textures");
    desc.BindValue("value", uint32_t(42));
    BindSceneTextureArray(desc, sampler, {first, second});
    RenderGraph graph("texture_bindings");
    ASSERT_TRUE(graph.Begin());
    graph.AddComputePass(desc);
    ClearPassResourceBindings(desc);
    BindSceneTextureArray(desc, sampler, {second});
    graph.AddComputePass(desc);
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));

    EXPECT_EQ(rhi->graphics.boundResources,
              (std::vector<RHIResource*>{first->GetDefaultView(), second->GetDefaultView(), sampler,
                                         second->GetDefaultView(), sampler}));
    EXPECT_EQ(rhi->graphics.values.size(), 2u);

    device->DestroyTexture(first);
    device->DestroyTexture(second);
}

TEST_F(RenderCoreTest, MetricsCountActualTransitionsAndResetOnRebuild)
{
    RDGMetrics& metrics = device->GetRDGMetrics();
    RDGMetricsOptions options;
    options.logging.sampleEvery  = 1;
    options.logging.minInterval  = std::chrono::milliseconds(0);
    options.includeTransferNodes = true;
    options.nodeTimings          = true;
    metrics.Configure(options);
    std::vector<RDGMetricsSnapshot> samples;
    metrics.SetSink([&](const RDGMetricsSnapshot& sample) { samples.push_back(sample); });
    TestBuffer* source      = Buffer();
    TestBuffer* destination = Buffer();
    RenderGraph graph("metrics_copy");
    graph.Begin();
    graph.AddTransferPass("first").CopyBuffer(source, destination, {0, 0, 4});
    graph.AddTransferPass("second").CopyBuffer(destination, source, {0, 4, 4});
    graph.End();
    device->ExecuteRenderGraph(graph);

    ASSERT_EQ(samples.size(), 1u);

    const RDGMetricsSnapshot& sample = samples.back();
    EXPECT_EQ(sample.nodeCount, 2u);
    EXPECT_EQ(sample.resources, 2u);
    EXPECT_EQ(sample.importedResources, 2u);
    EXPECT_EQ(sample.dependencyEdges, 1u);
    ASSERT_EQ(graph.GetDependencies().size(), 2u); // Preserve reasons for both allocations.
    EXPECT_NE(graph.GetDependencies()[0].resourceId, graph.GetDependencies()[1].resourceId);
    EXPECT_NE(graph.GetDependencies()[0].reason, graph.GetDependencies()[1].reason);
    EXPECT_EQ(sample.dependencyHazards, 2u);
    ASSERT_EQ(sample.nodes.size(), 2u);
    EXPECT_EQ(sample.nodes[0].name, "first");
    EXPECT_EQ(sample.nodes[1].order, 1u);
    EXPECT_EQ(sample.totals.barrierCalls, rhi->transfer.barrierSources.size());
    EXPECT_EQ(sample.totals.bufferTransitions, rhi->transfer.bufferTransitions.size());
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eMissingBarrier)], 0u);
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eOrdering)], 0u);
    EXPECT_GT(sample.nodes[0].recordCPUUs, 0);
    EXPECT_TRUE(sample.nodeTimingsEnabled);
    EXPECT_EQ(RDGMetrics::Format(sample).find("record_cpu_us=disabled"), std::string::npos);

    device->ExecuteRenderGraph(graph);

    EXPECT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples.back().nodes.front().order, 0u);

    options.nodeTimings = false;
    metrics.Configure(options);
    graph.Begin();
    graph.AddTransferPass("rebuilt").CopyBuffer(source, destination, {0, 0, 4});
    graph.End();
    device->ExecuteRenderGraph(graph);

    EXPECT_EQ(samples.size(), 3u);
    EXPECT_EQ(samples.back().nodeCount, 1u);
    EXPECT_EQ(samples.back().dependencyEdges, 0u);
    EXPECT_EQ(samples.back().nodes.front().order, 0u);
    EXPECT_EQ(samples.back().nodes.front().name, "rebuilt");
    EXPECT_FALSE(samples.back().nodeTimingsEnabled);
    EXPECT_NE(RDGMetrics::Format(samples.back()).find("record_cpu_us=disabled"), std::string::npos);
    // Formatting a retained timed sample must not depend on the current collector options.
    EXPECT_TRUE(samples.front().nodeTimingsEnabled);
    EXPECT_EQ(RDGMetrics::Format(samples.front()).find("record_cpu_us=disabled"),
              std::string::npos);

    device->DestroyBuffer(source);
    device->DestroyBuffer(destination);
}

TEST_F(RenderCoreTest, MetricsSummarizeBroadRangesAndKeepDiagnosticDetailsAvailable)
{
    RDGMetrics& metrics = device->GetRDGMetrics();

    for (bool includeOptimizations : {false, true})
    {
        RDGMetricsOptions options;
        options.includeTransferNodes       = true;
        options.maxDiagnosticDetails       = 2;
        options.includeOptimizationDetails = includeOptimizations;
        metrics.Configure(options);
        metrics.SetSink({});
        RHITexture* source            = Texture(3);
        RHITexture* destination       = Texture(3);
        TestBuffer* externalBuffer    = Buffer();
        TestBuffer* bufferDestination = Buffer();
        RenderGraph graph("broad_range_details");
        graph.Begin();
        graph.AddTransferPass("clear_source").ClearTexture(source, Color(0.f));
        RHITextureCopyRegion region{};
        region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.size = {8, 8, 1};

        for (uint32_t copy = 0; copy < 4; ++copy)
        {
            graph.AddTransferPass("copy_mip")
                .CopyTexture(source, destination, MakeVecView(&region, 1));
        }

        // This diagnostic occurs after enough broad ranges to fill the detail budget.
        graph.AddTransferPass("external_read")
            .CopyBuffer(externalBuffer, bufferDestination, {0, 0, 64});

        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));

        const RDGMetricsSnapshot& sample = metrics.GetLastSnapshot();
        EXPECT_GT(sample.issues[size_t(RDGMetricIssue::eBroadTextureRange)],
                  options.maxDiagnosticDetails);
        EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eUnknownImportState)], 1u);
        EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eMissingBarrier)], 0u);
        ASSERT_EQ(sample.diagnostics.size(), includeOptimizations ? 2u : 1u);
        EXPECT_TRUE(std::any_of(sample.diagnostics.begin(), sample.diagnostics.end(),
                                [](const RDGMetricDiagnostic& detail) {
                                    return detail.issue == RDGMetricIssue::eUnknownImportState &&
                                        detail.nodeName == "external_read";
                                }));

        const std::string report = RDGMetrics::Format(sample);
        EXPECT_NE(report.find("optimization_candidates(broad_texture_range="), std::string::npos);
        EXPECT_NE(report.find("diagnostic=unknown_import_state"), std::string::npos);
        EXPECT_EQ(report.find("diagnostic=broad_texture_range"), std::string::npos);
        EXPECT_EQ(report.find("\n  optimization=broad_texture_range") != std::string::npos,
                  includeOptimizations);
        EXPECT_EQ(sample.omittedDiagnostics > 0, includeOptimizations);

        device->DestroyTexture(source);
        device->DestroyTexture(destination);
        device->DestroyBuffer(externalBuffer);
        device->DestroyBuffer(bufferDestination);
    }
}

TEST_F(RenderCoreTest, MetricsThrottleShortLivedTransferGraphsAndBoundDetails)
{
    RDGMetrics& metrics = device->GetRDGMetrics();
    RDGMetricsOptions options;
    options.logging.sampleEvery  = 1;
    options.logging.minInterval  = std::chrono::hours(1);
    options.maxDiagnosticDetails = 0;
    metrics.Configure(options);
    std::vector<RDGMetricsSnapshot> samples;
    metrics.SetSink([&](const RDGMetricsSnapshot& sample) { samples.push_back(sample); });
    TestBuffer* source      = Buffer();
    TestBuffer* destination = Buffer();

    for (uint32_t i = 0; i < 20; ++i)
    {
        RenderGraph graph(NameID("upload_" + std::to_string(i)));
        graph.Begin();
        graph.AddTransferPass("copy").CopyBuffer(source, destination, {0, 0, 4});
        graph.End();
        device->ExecuteRenderGraph(graph);
    }

    ASSERT_EQ(samples.size(), 1u);
    EXPECT_TRUE(samples[0].nodes.empty());
    EXPECT_EQ(samples[0].omittedNodes, 1u);
    EXPECT_TRUE(samples[0].diagnostics.empty());
    EXPECT_GT(samples[0].omittedDiagnostics, 0u);

    metrics.RequestCapture(true);
    RenderGraph graph("last_upload");
    graph.Begin();
    graph.AddTransferPass("copy").CopyBuffer(source, destination, {0, 0, 4});
    graph.End();
    device->ExecuteRenderGraph(graph);

    ASSERT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples.back().windowExecutions, 20u);
    EXPECT_EQ(samples.back().windowNodes, 20u);

    // Transfer traffic must not consume the frame stream's first sample.
    CreateTestShaderProgram(device, "metrics_empty");
    graph.Begin();

    RDGComputePassDesc compute{};
    compute.SetShaderProgramName("metrics_empty");
    compute.SetPassTag("compute");
    graph.AddComputePass(std::move(compute));
    graph.End();
    device->ExecuteRenderGraph(graph);

    ASSERT_EQ(samples.size(), 3u);
    EXPECT_FALSE(samples.back().transferOnly);
    ASSERT_EQ(samples.back().nodes.size(), 1u);
    EXPECT_EQ(samples.back().nodes.front().name, "compute");

    device->DestroyBuffer(source);
    device->DestroyBuffer(destination);
}

TEST_F(RenderCoreTest, MetricsCountBarriersInsideTransfersAndMipGeneration)
{
    RDGMetrics& metrics = device->GetRDGMetrics();
    RDGMetricsOptions options;
    options.includeTransferNodes = true;
    options.maxNodeDetails       = 1;
    metrics.Configure(options);
    metrics.SetSink({});
    RHITexture* texture = Texture(3);
    RenderGraph graph("metrics_mips");
    graph.Begin();
    graph.AddTransferPass("mips").ClearTexture(texture, {}).GenerateMipmaps(texture);
    graph.AddTransferPass("clear").ClearTexture(texture, {});
    graph.End();
    device->ExecuteRenderGraph(graph);
    const RDGMetricsSnapshot& sample = metrics.GetLastSnapshot();
    EXPECT_EQ(sample.totals.internalMemoryTransitions, 1u);
    EXPECT_EQ(sample.totals.internalTextureTransitions, 4u);
    EXPECT_EQ(sample.totals.barrierCalls, rhi->graphics.barrierSources.size());
    EXPECT_EQ(sample.totals.textureTransitions + sample.totals.internalTextureTransitions,
              rhi->graphics.textureTransitions.size());
    EXPECT_EQ(sample.totals.internalMemoryTransitions, rhi->graphics.memoryBarriers.size());
    EXPECT_EQ(sample.nodes.size(), 1u);
    EXPECT_EQ(sample.omittedNodes, 1u);
    EXPECT_EQ(sample.nodes.front().internalTextureTransitions, 4u);

    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, WriterVisibilityReachesLaterReaderUsagesInOneGraph)
{
    RDGMetrics& metrics = device->GetRDGMetrics();
    metrics.SetSink({});
    CreateTestShaderProgram(device, "metrics_readers");
    TestBuffer* source  = Buffer();
    TestBuffer* shared  = Buffer();
    TestBuffer* scratch = Buffer();
    RenderGraph graph("reader_visibility");
    graph.Begin();
    graph.AddTransferPass("writer").CopyBuffer(source, shared, {0, 0, 4});
    graph.AddTransferPass("transfer_reader").CopyBuffer(shared, scratch, {0, 0, 4});

    RDGGraphicsPassDesc graphics{};
    graphics.SetShaderProgramName("metrics_readers");
    graphics.SetPassTag("vertex_reader");
    graphics.BindVertexBuffer(shared);
    graph.AddGraphicsPass(std::move(graphics));
    graph.End();
    device->ExecuteRenderGraph(graph);
    const RDGMetricsSnapshot& sample = metrics.GetLastSnapshot();
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eMissingBarrier)], 0u)
        << RDGMetrics::Format(sample);
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eStageCoverage)], 0u);
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eAccessCoverage)], 0u);

    bool synchronized = false;

    for (TestContext::BarrierBatch const& batch : rhi->graphics.barrierBatches)
    {
        for (const RHIBufferTransition& barrier : batch.buffers)
        {
            if (barrier.pBuffer == shared && barrier.newUsage == RHIBufferUsage::eVertexBuffer)
            {
                EXPECT_TRUE(barrier.additionalSrcAccess.HasFlag(RHIAccessFlagBits::eTransferWrite));
                EXPECT_TRUE(batch.source.HasFlag(RHIPipelineStageFlagBits::eTransfer));
                EXPECT_EQ(int64_t(batch.destination),
                          int64_t(RHIPipelineStageFlagBits::eVertexInput));
                synchronized = true;
            }
        }
    }

    EXPECT_TRUE(synchronized);
    EXPECT_EQ(sample.nodes.size(), 1u); // Transfer details suppressed; totals still include them.
    EXPECT_EQ(sample.nodeCount, 3u);

    device->DestroyBuffer(source);
    device->DestroyBuffer(shared);
    device->DestroyBuffer(scratch);
}

static void CheckBufferReader(TestRHI* rhi,
                              RenderDevice* device,
                              RHIBuffer* shared,
                              int kind,
                              bool needsBarrier)
{
    rhi->graphics.barrierBatches.clear();
    RenderGraph graph("read_buffer");
    graph.Begin();

    if (kind == 3)
    {
        RDGComputePassDesc desc{};
        desc.SetShaderProgramName("read_compute");
        desc.BindStorageBuffer("buffer", shared);
        graph.AddComputePass(desc);
    }
    else
    {
        RDGGraphicsPassDesc desc{};
        desc.SetShaderProgramName("read_fragment");

        if (kind == 0)
        {
            desc.BindStorageBuffer("buffer", shared);
        }
        else if (kind == 1)
        {
            desc.BindVertexBuffer(shared);
        }
        else
        {
            desc.BindIndexBuffer(shared);
        }

        graph.AddGraphicsPass(desc);
    }

    graph.End();
    device->ExecuteRenderGraph(graph);
    uint32_t barriers = 0;

    for (TestContext::BarrierBatch const& batch : rhi->graphics.barrierBatches)
    {
        for (const RHIBufferTransition& barrier : batch.buffers)
        {
            if (barrier.pBuffer == shared)
            {
                ++barriers;
                EXPECT_TRUE(barrier.additionalSrcAccess.HasFlag(RHIAccessFlagBits::eTransferWrite));
                EXPECT_TRUE(batch.source.HasFlag(RHIPipelineStageFlagBits::eTransfer));
                EXPECT_EQ(int64_t(batch.destination),
                          int64_t(kind == 3     ? RHIPipelineStageFlagBits::eComputeShader :
                                      kind == 0 ? RHIPipelineStageFlagBits::eFragmentShader :
                                                  RHIPipelineStageFlagBits::eVertexInput));
                EXPECT_EQ(barrier.newUsage,
                          kind == 1     ? RHIBufferUsage::eVertexBuffer :
                              kind == 2 ? RHIBufferUsage::eIndexBuffer :
                                          RHIBufferUsage::eStorageBuffer);
            }
        }
    }

    EXPECT_EQ(barriers != 0, needsBarrier) << "reader kind=" << kind;
}

TEST_F(RenderCoreTest, BufferVisibilityPersistsAndNewWritesResetCoverageWithoutMetrics)
{
    RDGMetricsOptions options = device->GetRDGMetrics().GetOptions();
    options.logging.enabled   = false;
    device->GetRDGMetrics().Configure(options);
    ShaderProgram* fragment = CreateTestShaderProgram(device, "read_fragment");
    ShaderProgram* compute  = CreateTestShaderProgram(device, "read_compute");
    static_cast<TestShader*>(fragment->GetShader())->SetDescriptorReadOnly("buffer");
    static_cast<TestShader*>(fragment->GetShader())
        ->SetDescriptorStages("buffer", RHIShaderStageFlagBits::eFragment);
    static_cast<TestShader*>(compute->GetShader())->SetDescriptorReadOnly("buffer");
    static_cast<TestShader*>(compute->GetShader())
        ->SetDescriptorStages("buffer", RHIShaderStageFlagBits::eCompute);
    TestBuffer* source = Buffer();
    TestBuffer* shared = Buffer();
    RenderGraph upload("write_buffer");
    upload.Begin();
    upload.AddTransferPass("write").CopyBuffer(source, shared, {0, 0, 4});
    upload.End();
    device->ExecuteRenderGraph(upload);

    CheckBufferReader(rhi, device, shared, 0, true); // Fragment storage read.
    CheckBufferReader(rhi, device, shared, 1, true); // A new access and stage.
    CheckBufferReader(
        rhi, device, shared, 2,
        true); // Index read needs its own access scope at the same vertex-input stage.
    CheckBufferReader(rhi, device, shared, 3,
                      true); // Shader reads were only visible to fragment, not compute.

    for (int kind = 0; kind < 4; ++kind)
    {
        CheckBufferReader(rhi, device, shared, kind, false);
    }

    // Re-executing a compiled graph must refresh barriers against all intervening readers.
    rhi->graphics.barrierBatches.clear();
    const uint64_t transferSerial = rhi->submitted[2];
    device->ExecuteRenderGraph(upload);

    EXPECT_EQ(rhi->submitted[2], transferSerial);

    bool orderedReaders = false;

    for (TestContext::BarrierBatch const& batch : rhi->graphics.barrierBatches)
    {
        for (const RHIBufferTransition& barrier : batch.buffers)
        {
            if (barrier.pBuffer == shared && barrier.newUsage == RHIBufferUsage::eTransferDst)
            {
                EXPECT_TRUE(batch.source.HasFlag(RHIPipelineStageFlagBits::eVertexInput));
                EXPECT_TRUE(batch.source.HasFlag(RHIPipelineStageFlagBits::eFragmentShader));
                EXPECT_TRUE(batch.source.HasFlag(RHIPipelineStageFlagBits::eComputeShader));
                orderedReaders = true;
            }
        }
    }

    EXPECT_TRUE(orderedReaders);

    CheckBufferReader(rhi, device, shared, 3, true);
    CheckBufferReader(rhi, device, shared, 3, false);
    CheckBufferReader(rhi, device, shared, 0, true);
    device->DestroyBuffer(source);
    device->DestroyBuffer(shared);
}

static void CheckTextureReader(TestRHI* rhi,
                               RenderDevice* device,
                               const RDGMetrics& metrics,
                               RHITexture* texture,
                               bool isCompute,
                               bool needsBarrier,
                               RHITextureUsage oldUsage)
{
    rhi->graphics.barrierBatches.clear();
    RenderGraph graph("sample_image");
    graph.Begin();

    if (isCompute)
    {
        RDGComputePassDesc desc{};
        desc.SetShaderProgramName("texture_compute");
        desc.BindSampledTexture("texture", nullptr, texture->GetDefaultView());
        graph.AddComputePass(desc);
    }
    else
    {
        RDGGraphicsPassDesc desc{};
        desc.SetShaderProgramName("texture_fragment");
        desc.BindSampledTexture("texture", nullptr, texture->GetDefaultView());
        graph.AddGraphicsPass(desc);
    }

    graph.End();
    device->ExecuteRenderGraph(graph);
    uint32_t count = 0;

    for (TestContext::BarrierBatch const& batch : rhi->graphics.barrierBatches)
    {
        for (const RHITextureTransition& barrier : batch.textures)
        {
            if (barrier.pTexture == texture)
            {
                ++count;
                EXPECT_EQ(barrier.oldUsage, oldUsage);
                EXPECT_EQ(barrier.newUsage, RHITextureUsage::eSampled);
                EXPECT_TRUE(barrier.additionalSrcAccess.HasFlag(RHIAccessFlagBits::eTransferWrite));
                EXPECT_TRUE(batch.source.HasFlag(RHIPipelineStageFlagBits::eTransfer));
                EXPECT_EQ(int64_t(batch.destination),
                          int64_t(isCompute ? RHIPipelineStageFlagBits::eComputeShader :
                                              RHIPipelineStageFlagBits::eFragmentShader));
            }
        }
    }

    EXPECT_EQ(count, needsBarrier ? 1u : 0u);

    const RDGMetricsSnapshot& report = metrics.GetLastSnapshot();

    for (RDGMetricIssue issue : {RDGMetricIssue::eMissingBarrier, RDGMetricIssue::eStageCoverage,
                                 RDGMetricIssue::eAccessCoverage, RDGMetricIssue::eLayoutMismatch})
    {
        EXPECT_EQ(report.issues[size_t(issue)], 0u) << RDGMetrics::Format(report);
    }
}

TEST_F(RenderCoreTest, TextureVisibilityPreservesCurrentLayoutAcrossGraphs)
{
    RDGMetrics& metrics         = device->GetRDGMetrics();
    RDGMetricsOptions options   = metrics.GetOptions();
    options.logging.sampleEvery = 1;
    options.logging.minInterval = std::chrono::milliseconds(0);
    metrics.Configure(options);
    metrics.SetSink({});
    ShaderProgram* fragment = CreateTestShaderProgram(device, "texture_fragment");
    ShaderProgram* compute  = CreateTestShaderProgram(device, "texture_compute");
    static_cast<TestShader*>(fragment->GetShader())
        ->SetDescriptorStages("texture", RHIShaderStageFlagBits::eFragment);
    static_cast<TestShader*>(compute->GetShader())
        ->SetDescriptorStages("texture", RHIShaderStageFlagBits::eCompute);
    RHITexture* texture = Texture();
    RHITexture* copy    = Texture();
    RenderGraph clear("clear_image");
    clear.Begin();
    clear.AddTransferPass("write").ClearTexture(texture, {});
    clear.End();
    device->ExecuteRenderGraph(clear);

    CheckTextureReader(rhi, device, metrics, texture, false, true, RHITextureUsage::eTransferDst);
    CheckTextureReader(rhi, device, metrics, texture, true, true, RHITextureUsage::eSampled);
    CheckTextureReader(rhi, device, metrics, texture, true, false, RHITextureUsage::eSampled);
    CheckTextureReader(rhi, device, metrics, texture, false, false, RHITextureUsage::eSampled);
    RenderGraph transfer("read_image");
    transfer.Begin();
    RHITextureCopyRegion region{};
    region.size = {1, 1, 1};
    region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    transfer.AddTransferPass("copy").CopyTexture(texture, copy, MakeVecView(&region, 1));
    transfer.End();
    device->ExecuteRenderGraph(transfer);
    CheckTextureReader(rhi, device, metrics, texture, false, true, RHITextureUsage::eTransferSrc);
    // A layout change invalidates the previous layout's compute visibility.
    CheckTextureReader(rhi, device, metrics, texture, true, true, RHITextureUsage::eSampled);
    CheckTextureReader(rhi, device, metrics, texture, true, false, RHITextureUsage::eSampled);
    device->ExecuteRenderGraph(clear);
    CheckTextureReader(rhi, device, metrics, texture, true, true, RHITextureUsage::eTransferDst);
    CheckTextureReader(rhi, device, metrics, texture, false, true, RHITextureUsage::eSampled);
    device->DestroyTexture(texture);
    device->DestroyTexture(copy);
}

TEST_F(RenderCoreTest, MetricsDoNotAssignDepthStagesToGeometryAndSampledBindings)
{
    RDGMetrics& metrics = device->GetRDGMetrics();
    metrics.SetSink({});
    ShaderProgram* program = CreateTestShaderProgram(device, "metric_stages");
    static_cast<TestShader*>(program->GetShader())
        ->SetDescriptorStages("texture", RHIShaderStageFlagBits::eFragment);
    ShaderProgram* geometry = CreateTestShaderProgram(device, "metric_stages_geometry");
    static_cast<TestShader*>(geometry->GetShader())->EnableGeometryStage();
    static_cast<TestShader*>(geometry->GetShader())
        ->SetDescriptorStages("texture", RHIShaderStageFlagBits::eFragment);
    TestBuffer* source  = Buffer();
    TestBuffer* vertex  = Buffer();
    TestBuffer* index   = Buffer();
    RHITexture* texture = Texture();

    RHITextureCreateInfo depthInfo{};
    depthInfo.format = DataFormat::eD32SFloat;
    depthInfo.width = depthInfo.height = 8;
    RHITexture* depth                  = rhi->CreateTexture(depthInfo);
    RenderGraph graph("mixed_stage_readers");
    graph.Begin();
    graph.AddTransferPass("vertices").CopyBuffer(source, vertex, {0, 0, 4});
    graph.AddTransferPass("indices").CopyBuffer(source, index, {0, 0, 4});
    graph.AddTransferPass("texture").ClearTexture(texture, {});

    RDGGraphicsPassDesc reader{};
    reader.SetShaderProgramName("metric_stages");
    reader.SetPassTag("without_depth");
    reader.BindVertexBuffer(vertex);
    reader.BindIndexBuffer(index);
    reader.BindSampledTexture("texture", nullptr, texture->GetDefaultView());
    graph.AddGraphicsPass(reader);
    reader.SetPassTag("with_depth");
    reader.SetShaderProgramName("metric_stages_geometry");
    reader.AddDepthStencilOutput(depth);
    graph.AddGraphicsPass(reader);
    graph.End();
    device->ExecuteRenderGraph(graph);
    const RDGMetricsSnapshot& sample = metrics.GetLastSnapshot();
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eMissingBarrier)], 0u)
        << RDGMetrics::Format(sample);
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eStageCoverage)], 0u);
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eAccessCoverage)], 0u);

    device->DestroyBuffer(source);
    device->DestroyBuffer(vertex);
    device->DestroyBuffer(index);
    device->DestroyTexture(texture);
    device->DestroyTexture(depth);
}

TEST_F(RenderCoreTest, MetricsDoNotAssignIndirectStagesToComputeTextures)
{
    RDGMetrics& metrics = device->GetRDGMetrics();
    metrics.SetSink({});
    ShaderProgram* program = CreateTestShaderProgram(device, "metric_indirect");
    static_cast<TestShader*>(program->GetShader())
        ->SetDescriptorStages("texture", RHIShaderStageFlagBits::eCompute);
    RHITexture* texture  = Texture();
    TestBuffer* source   = Buffer();
    TestBuffer* indirect = Buffer();
    RenderGraph graph("indirect_readers");
    graph.Begin();
    graph.AddTransferPass("arguments").CopyBuffer(source, indirect, {0, 0, 12});
    graph.AddTransferPass("texture").ClearTexture(texture, {});

    RDGComputePassDesc reader{};
    reader.SetShaderProgramName("metric_indirect");
    reader.SetPassTag("direct");
    reader.BindSampledTexture("texture", nullptr, texture->GetDefaultView());
    graph.AddComputePass(reader).RecordPassCommands(
        [](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    reader.SetPassTag("indirect");
    reader.UseIndirectBuffer(indirect);
    graph.AddComputePass(reader).RecordPassCommands(
        [indirect](RDGPassCmdEncoder& encoder) { encoder.DispatchIndirect(indirect, 0); });
    graph.End();
    device->ExecuteRenderGraph(graph);
    const RDGMetricsSnapshot& sample = metrics.GetLastSnapshot();
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eMissingBarrier)], 0u)
        << RDGMetrics::Format(sample);
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eStageCoverage)], 0u);
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eAccessCoverage)], 0u);

    device->DestroyTexture(texture);
    device->DestroyBuffer(source);
    device->DestroyBuffer(indirect);
}

TEST_F(RenderCoreTest, MetricsRetainUnsampledUploadsAndReadersAcrossGraphs)
{
    RDGMetrics& metrics         = device->GetRDGMetrics();
    RDGMetricsOptions options   = metrics.GetOptions();
    options.logging.sampleEvery = 1;
    options.logging.minInterval = std::chrono::hours(1);
    metrics.Configure(options);
    std::vector<RDGMetricsSnapshot> samples;
    metrics.SetSink([&](const RDGMetricsSnapshot& sample) { samples.push_back(sample); });
    CreateTestShaderProgram(device, "metric_cross_graph");
    TestBuffer* source  = Buffer();
    TestBuffer* shared  = Buffer();
    TestBuffer* scratch = Buffer();
    // Consume the transfer stream's first report before uploading the resource under test.
    RenderGraph prime("prime");
    prime.Begin();
    prime.AddTransferPass("prime").CopyBuffer(source, scratch, {0, 0, 4});
    prime.End();
    device->ExecuteRenderGraph(prime);

    ASSERT_EQ(samples.size(), 1u);

    const std::array<uint8_t, 4> bytes{1, 2, 3, 4};
    device->UpdateBuffer(shared, bytes.size(), bytes.data());
    RenderGraph transferReader("unsampled_reader");
    transferReader.Begin();
    transferReader.AddTransferPass("transfer_reader").CopyBuffer(shared, scratch, {0, 0, 4});
    transferReader.End();
    // Flushes the upload through a separate, unsampled transfer graph before this reader.
    device->ExecuteRenderGraph(transferReader);

    EXPECT_EQ(samples.size(), 1u);
    EXPECT_EQ(metrics.GetLastSnapshot().graph, "prime");

    RenderGraph graphics("captured_reader");
    graphics.Begin();

    RDGGraphicsPassDesc reader{};
    reader.SetShaderProgramName("metric_cross_graph");
    reader.SetPassTag("vertex_reader");
    reader.BindVertexBuffer(shared);
    graphics.AddGraphicsPass(reader);
    graphics.End();
    device->ExecuteRenderGraph(graphics);

    ASSERT_EQ(samples.size(), 2u);

    const RDGMetricsSnapshot& sample = samples.back();
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eMissingBarrier)], 0u)
        << RDGMetrics::Format(sample);
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eStageCoverage)], 0u);
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eAccessCoverage)], 0u);

    bool synchronized = false;

    for (TestContext::BarrierBatch const& batch : rhi->graphics.barrierBatches)
    {
        for (const RHIBufferTransition& barrier : batch.buffers)
        {
            if (barrier.pBuffer == shared && barrier.newUsage == RHIBufferUsage::eVertexBuffer)
            {
                EXPECT_TRUE(barrier.additionalSrcAccess.HasFlag(RHIAccessFlagBits::eTransferWrite));
                EXPECT_TRUE(batch.source.HasFlag(RHIPipelineStageFlagBits::eTransfer));
                EXPECT_TRUE(batch.destination.HasFlag(RHIPipelineStageFlagBits::eVertexInput));
                synchronized = true;
            }
        }
    }

    EXPECT_TRUE(synchronized);

    device->DestroyBuffer(source);
    device->DestroyBuffer(shared);
    device->DestroyBuffer(scratch);
}

TEST_F(RenderCoreTest, MetricsDiscardHistoryForExternalUpdatesAndInvalidation)
{
    CreateTestShaderProgram(device, "metric_external");
    RDGExecutor executor(device);
    RDGMetrics& metrics         = executor.GetMetrics();
    RDGMetricsOptions options   = metrics.GetOptions();
    options.logging.sampleEvery = 1;
    options.logging.minInterval = std::chrono::milliseconds(0);
    metrics.Configure(options);
    metrics.SetSink({});
    RHICommandList commands;
    TestBuffer* shared            = Buffer();
    ResourceStateTracker& tracker = executor.GetResourceStateTracker();
    RenderGraph graph("external_reader");
    graph.Begin();

    RDGGraphicsPassDesc reader{};
    reader.SetShaderProgramName("metric_external");
    reader.BindVertexBuffer(shared);
    graph.AddGraphicsPass(reader);
    graph.End();

    for (int write = 0; write < 2; ++write)
    {
        tracker.UpdateBufferState(
            shared, RHIAccessMode::eReadWrite,
            BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer),
            BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eTransfer));
        executor.Execute(&graph, &commands);
        commands.Reset();
        const RDGMetricsSnapshot& sample = metrics.GetLastSnapshot();
        EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eMissingBarrier)], 0u);
        EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eStageCoverage)], 0u);
        EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eAccessCoverage)], 0u);
    }

    tracker.RemoveResourceState(shared->GetStableId(), true);
    executor.Execute(&graph, &commands);
    commands.Reset();

    EXPECT_EQ(metrics.GetLastSnapshot().issues[size_t(RDGMetricIssue::eUnknownImportState)], 1u);

    device->DestroyBuffer(shared);
}

TEST_F(RenderCoreTest, MetricsResetHistoryAcrossValidationDisablement)
{
    CreateTestShaderProgram(device, "metric_disabled_gap");
    RDGMetrics& metrics = device->GetRDGMetrics();
    metrics.SetSink({});
    TestBuffer* source  = Buffer();
    TestBuffer* shared  = Buffer();
    TestBuffer* scratch = Buffer();
    RenderGraph first("old_transfer_visibility");
    first.Begin();
    first.AddTransferPass("write").CopyBuffer(source, shared, {0, 0, 4});
    first.AddTransferPass("read").CopyBuffer(shared, scratch, {0, 0, 4});
    first.End();
    device->ExecuteRenderGraph(first);

    RDGMetricsOptions options = metrics.GetOptions();
    options.validate          = false;
    metrics.Configure(options);
    RenderGraph replacement("unobserved_replacement");
    replacement.Begin();
    replacement.AddTransferPass("new_write").CopyBuffer(source, shared, {0, 0, 4});

    RDGGraphicsPassDesc reader{};
    reader.SetShaderProgramName("metric_disabled_gap");
    reader.BindVertexBuffer(shared);
    replacement.AddGraphicsPass(reader);
    replacement.End();
    device->ExecuteRenderGraph(replacement);

    EXPECT_FALSE(metrics.GetLastSnapshot().validated);

    options.validate = true;
    metrics.Configure(options);
    RenderGraph last("new_vertex_visibility");
    last.Begin();
    last.AddGraphicsPass(reader);
    last.End();
    device->ExecuteRenderGraph(last);

    EXPECT_TRUE(metrics.GetLastSnapshot().validated);
    EXPECT_EQ(metrics.GetLastSnapshot().issues[size_t(RDGMetricIssue::eMissingBarrier)], 0u);

    device->DestroyBuffer(source);
    device->DestroyBuffer(shared);
    device->DestroyBuffer(scratch);
}

TEST_F(RenderCoreTest, DisabledMetricsLeaveExecutionAndLastSampleUntouched)
{
    RDGMetrics& metrics = device->GetRDGMetrics();
    RDGMetricsOptions options;
    options.logging.enabled = false;
    metrics.Configure(options);
    uint32_t callbacks = 0;
    metrics.SetSink([&](const RDGMetricsSnapshot&) { ++callbacks; });
    metrics.RequestCapture(true);
    TestBuffer* source      = Buffer();
    TestBuffer* destination = Buffer();
    source->bytes[0]        = 42;
    RenderGraph graph("disabled_metrics");
    graph.Begin();
    graph.AddTransferPass("copy").CopyBuffer(source, destination, {0, 0, 4});
    graph.End();
    device->ExecuteRenderGraph(graph);

    EXPECT_EQ(destination->bytes[0], 42);
    EXPECT_EQ(callbacks, 0u);
    EXPECT_EQ(metrics.GetLastSnapshot().execution, 0u);

    device->DestroyBuffer(source);
    device->DestroyBuffer(destination);
}

} // namespace

static void AddEmptyReader(RenderGraph& graph, std::vector<NameID>& names)
{
    RDGGraphicsPassDesc desc;
    desc.SetShaderProgramName("readers");
    desc.SetPassTag("empty");
    graph.AddGraphicsPass(std::move(desc));
    names.emplace_back("empty");
}

TEST_F(RenderCoreTest, InterleavedAccessRecordingPreservesPassSlicesThroughReplayAndRebuild)
{
    CreateTestShaderProgram(device, "readers");
    RDGMetrics& metrics = device->GetRDGMetrics();
    RDGMetricsOptions options;
    options.logging.sampleEvery  = 1;
    options.logging.minInterval  = std::chrono::milliseconds(0);
    options.includeTransferNodes = true;
    options.maxNodeDetails       = 128;
    metrics.Configure(options);
    metrics.SetSink({});
    constexpr uint32_t copies = 32;
    std::array<TestBuffer*, copies + 1> buffers;

    for (TestBuffer*& buffer : buffers)
    {
        buffer = Buffer();
    }

    RenderGraph graph("interleaved_slices");

    for (uint32_t build = 0; build < 2; ++build)
    {
        ASSERT_TRUE(graph.Begin());

        for (TestBuffer* buffer : buffers)
        {
            graph.GetResourceManager()->ImportBuffer(buffer, RDGImportContents::eDefined);
        }

        std::vector<NameID> names;

        AddEmptyReader(graph, names);

        {
            std::vector<std::unique_ptr<RDGTransferPassCmdRecorder>> recorders;

            for (uint32_t i = 0; i < copies; ++i)
            {
                names.push_back("copy_" + std::to_string(i));
                recorders.emplace_back(
                    new RDGTransferPassCmdRecorder(graph.AddTransferPass(names.back())));

                if (i % 4 == 0)
                {
                    AddEmptyReader(graph, names);
                }
            }

            // Fill non-tail passes in reverse order and merge repeated accesses in each pass.
            for (uint32_t i = copies; i-- > 0;)
            {
                recorders[i]->CopyBuffer(buffers[i], buffers[i + 1], {0, 0, 4});
                recorders[i]->CopyBuffer(buffers[i], buffers[i + 1], {4, 4, 4});
            }
        }

        ASSERT_TRUE(graph.End());

        for (uint32_t replay = 0; replay < 2; ++replay)
        {
            for (uint32_t i = 0; i < 8; ++i)
            {
                buffers[0]->bytes[i] = uint8_t(20 * build + 10 * replay + i);
            }

            ASSERT_TRUE(device->ExecuteRenderGraph(graph));
            EXPECT_TRUE(std::equal(buffers[0]->bytes.begin(), buffers[0]->bytes.begin() + 8,
                                   buffers.back()->bytes.begin()));

            const RDGMetricsSnapshot& sample = metrics.GetLastSnapshot();
            ASSERT_EQ(sample.nodes.size(), names.size());
            EXPECT_EQ(sample.totals.reads, copies);
            EXPECT_EQ(sample.totals.writes, copies);
            EXPECT_EQ(sample.dependencyEdges, copies - 1);

            for (uint32_t i = 0; i < names.size(); ++i)
            {
                ASSERT_GE(sample.nodes[i].id, 0);
                ASSERT_LT(size_t(sample.nodes[i].id), names.size());
                EXPECT_EQ(sample.nodes[i].name, names[sample.nodes[i].id]);
                EXPECT_EQ(sample.nodes[i].order, i);
            }

            EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eOrdering)], 0u);
            EXPECT_TRUE(graph.GetWarnings().empty());
        }
    }

    ASSERT_TRUE(graph.Begin());
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(metrics.GetLastSnapshot().nodeCount, 0u);

    for (TestBuffer* buffer : buffers)
    {
        device->DestroyBuffer(buffer);
    }
}

TEST_F(RenderCoreTest, CompatibleBufferReadersRetainAllWriterDependencies)
{
    CreateTestShaderProgram(device, "readers");
    RDGMetrics& metrics = device->GetRDGMetrics();
    RDGMetricsOptions options;
    options.logging.sampleEvery = 1;
    options.logging.minInterval = std::chrono::milliseconds(0);
    metrics.Configure(options);
    metrics.SetSink({});
    TestBuffer* shared         = Buffer();
    TestBuffer* source         = Buffer();
    constexpr uint32_t readers = 256;
    RenderGraph graph("reader_frontier");

    for (const bool initialWriter : {false, true})
    {
        ASSERT_TRUE(graph.Begin());

        graph.GetResourceManager()->ImportBuffer(shared, RDGImportContents::eDefined);
        graph.GetResourceManager()->ImportBuffer(source, RDGImportContents::eDefined);

        if (initialWriter)
        {
            graph.AddTransferPass("initial_writer").CopyBuffer(source, shared, {0, 0, 64});
        }

        for (uint32_t i = 0; i < readers; ++i)
        {
            RDGGraphicsPassDesc desc;
            desc.SetShaderProgramName("readers");
            desc.BindVertexBuffer(shared);

            if (i % 2)
            {
                desc.BindIndexBuffer(shared); // Different read scopes are still compatible.
            }

            graph.AddGraphicsPass(std::move(desc));
        }

        graph.AddTransferPass("next_writer").CopyBuffer(source, shared, {0, 0, 64});

        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));

        const RDGMetricsSnapshot& sample = metrics.GetLastSnapshot();
        EXPECT_EQ(sample.dependencyEdges, initialWriter ? 2 * readers + 1 : readers);
        EXPECT_EQ(sample.reorderedNodes, 0u);

        for (RDGMetricIssue const issue :
             {RDGMetricIssue::eOrdering, RDGMetricIssue::eMissingBarrier,
              RDGMetricIssue::eStageCoverage, RDGMetricIssue::eAccessCoverage})
        {
            EXPECT_EQ(sample.issues[size_t(issue)], 0u) << RDGMetrics::Format(sample);
        }

        bool keptBothScopes = false;

        for (TestContext::BarrierBatch const& batch : rhi->graphics.barrierBatches)
        {
            bool vertex = false, index = false;

            for (const RHIBufferTransition& transition : batch.buffers)
            {
                if (transition.pBuffer == shared &&
                    transition.newUsage == RHIBufferUsage::eTransferDst)
                {
                    vertex |= transition.oldUsage == RHIBufferUsage::eVertexBuffer;
                    index |= transition.oldUsage == RHIBufferUsage::eIndexBuffer;
                }
            }

            keptBothScopes |= vertex && index;
        }

        EXPECT_TRUE(keptBothScopes);

        rhi->graphics.barrierBatches.clear();
    }

    device->DestroyBuffer(shared);
    device->DestroyBuffer(source);
}

static void AddTextureReaders(RenderGraph& graph, RHITexture* shared, uint32_t readers)
{
    for (uint32_t i = 0; i < readers; ++i)
    {
        RDGComputePassDesc desc;
        desc.SetShaderProgramName("texture_compute");
        desc.BindSampledTexture("texture", nullptr, shared->GetDefaultView());
        graph.AddComputePass(std::move(desc));
    }
}

TEST_F(RenderCoreTest, TextureReaderGroupsFlushAtLayoutChangesAndWrites)
{
    ShaderProgram* shader = CreateTestShaderProgram(device, "texture_compute");
    static_cast<TestShader*>(shader->GetShader())
        ->SetDescriptorStages("texture", RHIShaderStageFlagBits::eCompute);
    RDGMetrics& metrics = device->GetRDGMetrics();
    RDGMetricsOptions options;
    options.logging.sampleEvery  = 1;
    options.logging.minInterval  = std::chrono::milliseconds(0);
    options.includeTransferNodes = true;
    options.maxNodeDetails       = 128;
    metrics.Configure(options);
    metrics.SetSink({});
    RHITexture* shared         = Texture();
    RHITexture* destination    = Texture();
    constexpr uint32_t readers = 32;
    RenderGraph graph("texture_reader_groups");
    ASSERT_TRUE(graph.Begin());

    graph.AddTransferPass("initialize").ClearTexture(shared, {});

    AddTextureReaders(graph, shared, readers);
    RHITextureCopyRegion region{};
    region.size = {8, 8, 1};
    region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    graph.AddTransferPass("transfer_reader")
        .CopyTexture(shared, destination, MakeVecView(&region, 1));
    graph.AddTransferPass("overwrite").ClearTexture(shared, {});
    AddTextureReaders(graph, shared, readers);

    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));

    const RDGMetricsSnapshot& sample = metrics.GetLastSnapshot();
    EXPECT_EQ(sample.dependencyEdges, 3 * readers + 3);
    ASSERT_EQ(sample.nodes.size(), 2 * readers + 3);
    EXPECT_EQ(sample.reorderedNodes, 0u);

    for (RDGMetricIssue const issue :
         {RDGMetricIssue::eOrdering, RDGMetricIssue::eMissingBarrier,
          RDGMetricIssue::eStageCoverage, RDGMetricIssue::eAccessCoverage,
          RDGMetricIssue::eLayoutMismatch})
    {
        EXPECT_EQ(sample.issues[size_t(issue)], 0u) << RDGMetrics::Format(sample);
    }

    EXPECT_TRUE(graph.GetWarnings().empty());

    device->DestroyTexture(shared);
    device->DestroyTexture(destination);
}

static void CaptureVersionGraph(RenderDevice* device)
{
    RDGMetricsOptions options;
    options.logging.sampleEvery  = 1;
    options.logging.minInterval  = std::chrono::milliseconds(0);
    options.includeTransferNodes = true;
    options.maxNodeDetails       = 128;
    device->GetRDGMetrics().Configure(options);
    device->GetRDGMetrics().SetSink({});
}

static std::vector<int32_t> CapturedOrder(RenderDevice* device)
{
    const RDGMetricsSnapshot& sample = device->GetRDGMetrics().GetLastSnapshot();
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eOrdering)], 0u) << RDGMetrics::Format(sample);
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eMissingBarrier)], 0u);
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eAccessCoverage)], 0u);
    std::vector<int32_t> order;

    for (const RDGNodeMetrics& node : sample.nodes)
    {
        order.push_back(node.id);
    }

    return order;
}

TEST_F(RenderCoreTest, VersionsScheduleLaterProducersAndProtectScratchReaders)
{
    CaptureVersionGraph(device);
    TestBuffer* input1  = Buffer();
    TestBuffer* input2  = Buffer();
    TestBuffer* result1 = Buffer();
    TestBuffer* result2 = Buffer();
    std::fill(input1->bytes.begin(), input1->bytes.end(), uint8_t(11));
    std::fill(input2->bytes.begin(), input2->bytes.end(), uint8_t(77));
    RenderGraph graph("versioned_scratch");

    for (uint32_t build = 0; build < 2; ++build)
    {
        ASSERT_TRUE(graph.Begin());

        RDGResourceManager* resources = graph.GetResourceManager();
        const RDGBuffer source1       = resources->ImportHostWrittenBuffer(input1);
        const RDGBuffer source2       = resources->ImportHostWrittenBuffer(input2);
        const RDGBuffer output1       = resources->ImportBuffer(result1);
        const RDGBuffer output2       = resources->ImportBuffer(result2);
        const RDGBuffer base          = resources->CreateBuffer(LogicalBuffer());
        const RDGBuffer first         = resources->CreateVersion(base);
        const RDGBuffer second        = resources->CreateVersion(first);
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        EXPECT_NE(first, second);

        graph.AddTransferPass("consume_first").CopyBuffer(first, output1, {0, 0, 64});
        graph.AddTransferPass("consume_second").CopyBuffer(second, output2, {0, 0, 64});
        graph.AddTransferPass("produce_second").CopyBuffer(source2, second, {0, 0, 64});
        graph.AddTransferPass("produce_first").CopyBuffer(source1, first, {0, 0, 64});
        RDGExtractedBuffer extracted = resources->QueueBufferExtraction(second);
        ASSERT_TRUE(graph.End());

        for (uint32_t replay = 0; replay < 2; ++replay)
        {
            device->GetRDGMetrics().RequestCapture(true);
            ASSERT_TRUE(device->ExecuteRenderGraph(graph));
            EXPECT_EQ(result1->bytes, input1->bytes);
            EXPECT_EQ(result2->bytes, input2->bytes);
            ASSERT_TRUE(extracted);
            EXPECT_EQ(static_cast<TestBuffer*>(extracted.Get())->bytes, input2->bytes);
            EXPECT_EQ(DescribeResource(resources, first).physicalStableId,
                      extracted.Get()->GetStableId());
            EXPECT_EQ(DescribeResource(resources, second).physicalStableId,
                      extracted.Get()->GetStableId());
            EXPECT_EQ(CapturedOrder(device), (std::vector<int32_t>{3, 0, 2, 1, 4}));
            EXPECT_TRUE(graph.GetWarnings().empty());
        }

        bool war = false, waw = false, producer = false;

        for (RDGDependency const& edge : graph.GetDependencies())
        {
            EXPECT_EQ(edge.bufferSize, 64u);
            war |= edge.source == RDG_ID(0) && edge.destination == RDG_ID(2) &&
                edge.reason == RDGDependencyReason::eWriteAfterRead && edge.version == 1;
            waw |= edge.source == RDG_ID(3) && edge.destination == RDG_ID(2) &&
                edge.reason == RDGDependencyReason::eWriteAfterWrite && edge.version == 2;
            producer |= edge.source == RDG_ID(3) && edge.destination == RDG_ID(0) &&
                edge.reason == RDGDependencyReason::eVersionProducer && edge.version == 1;
        }

        EXPECT_TRUE(war);
        EXPECT_TRUE(waw);
        EXPECT_TRUE(producer);
    }

    ASSERT_TRUE(graph.Reset());
    EXPECT_TRUE(graph.GetDependencies().empty());

    for (TestBuffer* buffer : {input1, input2, result1, result2})
    {
        device->DestroyBuffer(buffer);
    }
}

TEST_F(RenderCoreTest, VersionsConnectAttachmentsCopiesViewsAndLoadPredecessors)
{
    CaptureVersionGraph(device);
    CreateTestShaderProgram(device, "intent");
    RenderGraph graph("versioned_texture");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGTexture first  = resources->CreateVersion(resources->CreateTexture(LogicalTexture()));
    const RDGTexture second = resources->CreateVersion(first);
    const RDGTexture copied = resources->CreateTexture(LogicalTexture());
    const RDGTextureViewDesc view{RHITextureSubResourceRange::Color()};
    RHITextureCopyRegion region{};
    region.size = {8, 8, 1};
    region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    graph.AddTransferPass("copy_first")
        .NeverCull()
        .CopyTexture(first, copied, MakeVecView(&region, 1));

    RDGGraphicsPassDesc load;
    load.SetShaderProgramName("intent");
    load.SetPassTag("load_second");
    load.SetRenderArea(0, 0, 8, 8);
    load.AddColorOutput(second, RHIRenderTargetLoadOp::eLoad);
    graph.AddGraphicsPass(load);

    RDGComputePassDesc sample = IntentPass("sample_second");
    sample.BindSampledTexture("texture", nullptr, second, view);
    graph.AddComputePass(sample);

    RDGGraphicsPassDesc clear;
    clear.SetShaderProgramName("intent");
    clear.SetPassTag("produce_first");
    clear.SetRenderArea(0, 0, 8, 8);
    clear.AddColorOutput(first);
    graph.AddGraphicsPass(clear);
    RDGExtractedTexture extracted = resources->QueueTextureExtraction(second);
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(CapturedOrder(device), (std::vector<int32_t>{3, 0, 1, 2, 4}));
    ASSERT_TRUE(extracted);
    EXPECT_EQ(DescribeResource(resources, first).physicalStableId, extracted.Get()->GetStableId());
    EXPECT_TRUE(graph.GetWarnings().empty());
    EXPECT_EQ(rhi->graphics.renderingLayouts.size(), 2u);
}

TEST_F(RenderCoreTest, VersionFailuresRejectMissingDuplicateAndCyclicProducersBeforeAllocation)
{
    CreateTestShaderProgram(device, "intent");

    for (uint32_t kind = 0; kind < 3; ++kind)
    {
        SCOPED_TRACE(kind);
        RenderGraph graph("invalid_versions");
        ASSERT_TRUE(graph.Begin());

        RDGResourceManager* resources = graph.GetResourceManager();
        RDGBufferDesc desc            = LogicalBuffer();
        desc.name                     = "scratch_a";
        const RDGBuffer a             = resources->CreateVersion(resources->CreateBuffer(desc));

        RDGComputePassDesc first = IntentPass("first");

        if (kind == 0)
        {
            first.BindStorageBuffer("read_buffer", a, RDGContentGuarantee::eNone);
        }
        else
        {
            first.BindStorageBuffer("write_buffer", a, RDGContentGuarantee::eFullWrite);
        }

        if (kind == 2)
        {
            desc.name         = "scratch_b";
            const RDGBuffer b = resources->CreateVersion(resources->CreateBuffer(desc));
            first.BindUniformBuffer("value", b);

            RDGComputePassDesc second = IntentPass("second");
            second.BindStorageBuffer("write_buffer", b, RDGContentGuarantee::eFullWrite);
            second.BindUniformBuffer("value", a);
            graph.AddComputePass(first);
            graph.AddComputePass(second);
        }
        else
        {
            graph.AddComputePass(first);

            if (kind == 1)
            {
                first.SetPassTag("second");
                graph.AddComputePass(first);
            }
        }

        ASSERT_TRUE(graph.End());

        const uint32_t pipelines                = rhi->pipelineCount;
        const std::array<uint64_t, 3> submitted = rhi->submitted;
        EXPECT_FALSE(device->ExecuteRenderGraph(graph));
        EXPECT_EQ(graph.GetResult().code,
                  kind == 0     ? RDGErrorCode::eMissingProducer :
                      kind == 1 ? RDGErrorCode::eDuplicateProducer :
                                  RDGErrorCode::eDependencyCycle);
        EXPECT_EQ(DescribeResource(resources, a).physicalStableId, 0u);
        EXPECT_EQ(rhi->pipelineCount, pipelines);
        EXPECT_EQ(rhi->submitted, submitted);
        EXPECT_NE(graph.GetResult().message.find("scratch_a"), std::string::npos);

        if (kind != 0)
        {
            EXPECT_NE(graph.GetResult().message.find("first"), std::string::npos);
            EXPECT_NE(graph.GetResult().message.find("second"), std::string::npos);
        }

        if (kind == 2)
        {
            EXPECT_NE(graph.GetResult().message.find("scratch_b"), std::string::npos);
            EXPECT_NE(graph.GetResult().message.find("producer"), std::string::npos);
            EXPECT_NE(graph.GetResult().message.find("v1"), std::string::npos);
        }
    }
}

TEST_F(RenderCoreTest, InitialVersionsAndAutomaticImportsKeepOldContents)
{
    CaptureVersionGraph(device);
    TestBuffer* shared    = Buffer();
    TestBuffer* source    = Buffer();
    TestBuffer* oldResult = Buffer();
    TestBuffer* newResult = Buffer();
    std::fill(source->bytes.begin(), source->bytes.end(), uint8_t(91));

    for (const bool versionInitial : {false, true})
    {
        std::fill(shared->bytes.begin(), shared->bytes.end(), uint8_t(13));
        RenderGraph graph("initial_contents_order");
        ASSERT_TRUE(graph.Begin());

        RDGResourceManager* resources = graph.GetResourceManager();
        const RDGBuffer base    = resources->ImportBuffer(shared, RDGImportContents::eDefined);
        const RDGBuffer initial = versionInitial ? resources->InitialVersion(base) : base;
        const RDGBuffer target  = versionInitial ? resources->CreateVersion(initial) : base;
        const RDGBuffer input   = resources->ImportHostWrittenBuffer(source);
        const RDGBuffer output1 = resources->ImportBuffer(oldResult);
        const RDGBuffer output2 = resources->ImportBuffer(newResult);
        const RDGBuffer future = resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));
        graph.AddTransferPass("old_read").CopyBuffer(initial, output1, {0, 0, 64});
        graph.AddTransferPass("overwrite").CopyBuffer(future, target, {0, 0, 64});
        graph.AddTransferPass("new_read").CopyBuffer(target, output2, {0, 0, 64});
        graph.AddTransferPass("late_producer").CopyBuffer(input, future, {0, 0, 64});

        ASSERT_TRUE(graph.End());

        device->GetRDGMetrics().RequestCapture(true);

        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        EXPECT_EQ(CapturedOrder(device), (std::vector<int32_t>{0, 3, 1, 2}));
        EXPECT_EQ(oldResult->bytes[0], 13);
        EXPECT_EQ(newResult->bytes[0], 91);
    }

    for (TestBuffer* buffer : {shared, source, oldResult, newResult})
    {
        device->DestroyBuffer(buffer);
    }
}

TEST_F(RenderCoreTest, VersionDeclarationsRejectBranchesMixedAccessesAndSamePassAmbiguity)
{
    CreateTestShaderProgram(device, "intent");

    for (uint32_t kind = 0; kind < 5; ++kind)
    {
        SCOPED_TRACE(kind);
        RenderGraph graph("ambiguous_versions");
        ASSERT_TRUE(graph.Begin());

        RDGResourceManager* resources = graph.GetResourceManager();
        const RDGBuffer base          = resources->CreateBuffer(LogicalBuffer());
        const RDGBuffer initial       = resources->InitialVersion(base);
        const RDGBuffer first         = resources->CreateVersion(initial);

        RDGComputePassDesc pass = IntentPass();

        if (kind == 0)
        {
            EXPECT_FALSE(resources->CreateVersion(initial));
        }
        else if (kind == 1) // Initial contents are never a writable version.
        {
            pass.BindStorageBuffer("write_buffer", initial, RDGContentGuarantee::eFullWrite);
            graph.AddComputePass(pass);
        }
        else
        {
            pass.BindStorageBuffer("write_buffer", first, RDGContentGuarantee::eFullWrite);

            if (kind == 2)
            {
                pass.BindUniformBuffer("value", base);
            }

            if (kind == 3)
            {
                pass.BindUniformBuffer("value", first); // Reads its own output.
            }

            graph.AddComputePass(pass);

            if (kind == 4)
            {
                const RDGBuffer second = resources->CreateVersion(first);
                // One pass cannot produce two values of the same allocation.
                const RDGBuffer input               = resources->CreateBuffer(LogicalBuffer());
                RDGTransferPassCmdRecorder recorder = graph.AddTransferPass("two_versions");
                recorder.CopyBuffer(input, second, {0, 32, 16});
                recorder.CopyBuffer(input, first, {0, 48, 16});
            }

            if (graph.GetResult())
            {
                ASSERT_TRUE(graph.End());
                EXPECT_FALSE(device->ExecuteRenderGraph(graph));
            }
        }

        EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eVersion);
    }
}

TEST_F(RenderCoreTest, VersionExtractionMustSelectTheFinalValueAndResourcesExpire)
{
    TestBuffer* source = Buffer();
    RenderGraph graph("version_lifetimes");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer input         = resources->ImportHostWrittenBuffer(source);
    const RDGBuffer first = resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));
    RDGExtractedBuffer extracted = resources->QueueBufferExtraction(first);
    const RDGBuffer second =
        resources->CreateVersion(first); // Also invalidates an earlier extraction request.
    graph.AddTransferPass("one").CopyBuffer(input, first, {0, 0, 64});
    graph.AddTransferPass("two").CopyBuffer(input, second, {0, 0, 64});

    EXPECT_FALSE(graph.End());
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eExport);
    EXPECT_FALSE(extracted);
    ASSERT_TRUE(graph.Begin());
    EXPECT_FALSE(resources->IsValid(second));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eLifecycle);

    RenderGraph foreign("foreign_version");
    ASSERT_TRUE(foreign.Begin());
    EXPECT_FALSE(foreign.GetResourceManager()->IsValid(first));
    EXPECT_EQ(foreign.GetResult().code, RDGErrorCode::eLifecycle);

    device->DestroyBuffer(source);
}

TEST_F(RenderCoreTest, VersionReadModifyWriteStillRequiresInitializedPredecessor)
{
    CreateTestShaderProgram(device, "intent");

    for (const bool initialize : {false, true})
    {
        RenderGraph graph("version_rmw");
        ASSERT_TRUE(graph.Begin());

        RDGResourceManager* resources = graph.GetResourceManager();
        const RDGBuffer first  = resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));
        const RDGBuffer target = initialize ? resources->CreateVersion(first) : first;

        RDGComputePassDesc read = IntentPass("consumer");
        read.BindStorageBuffer("read_buffer", target, RDGContentGuarantee::eNone);
        graph.AddComputePass(read);

        RDGComputePassDesc rmw = IntentPass("rmw");
        rmw.BindStorageBuffer("buffer", target, RDGContentGuarantee::eNone);
        graph.AddComputePass(rmw);

        if (initialize)
        {
            RDGComputePassDesc write = IntentPass("initialize");
            write.BindStorageBuffer("write_buffer", first, RDGContentGuarantee::eFullWrite);
            graph.AddComputePass(write);
        }

        ASSERT_TRUE(graph.End());
        EXPECT_EQ(device->ExecuteRenderGraph(graph), initialize);

        if (!initialize)
        {
            EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eUninitialized);
        }
    }
}

TEST_F(RenderCoreTest, VersionAliasesCannotBypassOverlappingCopyValidation)
{
    RenderGraph graph("version_copy_alias");
    ASSERT_TRUE(graph.Begin());
    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer initial = resources->InitialVersion(resources->CreateBuffer(LogicalBuffer()));
    const RDGBuffer next    = resources->CreateVersion(initial);
    graph.AddTransferPass("overlap").CopyBuffer(initial, next, {0, 0, 16});
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eRange);
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
}

TEST_F(RenderCoreTest, TextureReadersChangeLayoutsInVersionScheduledOrder)
{
    CaptureVersionGraph(device);
    CreateTestShaderProgram(device, "intent");
    RHITexture* texture = Texture();
    RHITexture* copy    = Texture();
    RenderGraph graph("version_layout_order");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGTexture image        = resources->ImportTexture(texture, RDGImportContents::eDefined);
    const RDGTexture destination  = resources->ImportTexture(copy);
    const RDGBuffer future = resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));
    RHITextureCopyRegion region{};
    region.size = {8, 8, 1};
    region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    const RDGBuffer bufferCopy = resources->CreateBuffer(LogicalBuffer());
    graph.AddTransferPass("delayed_transfer_read")
        .CopyTexture(image, destination, MakeVecView(&region, 1))
        .CopyBuffer(future, bufferCopy, {0, 0, 64});

    RDGComputePassDesc delayed = IntentPass("delayed_layout_change");
    delayed.BindSampledTexture("texture", nullptr, image);
    graph.AddComputePass(delayed);

    RDGComputePassDesc reader = IntentPass("same_layout_reader");
    reader.BindSampledTexture("texture", nullptr, image);
    graph.AddComputePass(reader);

    RDGComputePassDesc producer = IntentPass("future_producer");
    producer.BindStorageBuffer("write_buffer", future, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(producer);

    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(CapturedOrder(device), (std::vector<int32_t>{1, 2, 3, 0}));
    ASSERT_EQ(graph.GetDependencies().size(), 1u);
    EXPECT_EQ(graph.GetDependencies()[0].source, RDG_ID(3));
    EXPECT_EQ(graph.GetDependencies()[0].destination, RDG_ID(0));
    EXPECT_EQ(graph.GetDependencies()[0].version, 1);

    device->DestroyTexture(texture);
    device->DestroyTexture(copy);
}

TEST_F(RenderCoreTest, VersionSchedulesMatchValueOracleAcrossShuffledDeclarations)
{
    CaptureVersionGraph(device);
    std::mt19937 random(0x3b2026);
    std::array<TestBuffer*, 3> inputs;
    std::array<TestBuffer*, 6> outputs;

    for (uint32_t i = 0; i < inputs.size(); ++i)
    {
        inputs[i] = Buffer();
        std::fill(inputs[i]->bytes.begin(), inputs[i]->bytes.end(), uint8_t(31 * (i + 1)));
    }

    for (TestBuffer*& output : outputs)
    {
        output = Buffer();
    }

    RenderGraph graph("shuffled_versions");

    for (uint32_t trial = 0; trial < 48; ++trial)
    {
        ASSERT_TRUE(graph.Begin());

        RDGResourceManager* resources = graph.GetResourceManager();
        std::array<RDGBuffer, 3> versions;
        RDGBuffer previous = resources->CreateBuffer(LogicalBuffer());

        for (RDGBuffer& version : versions)
        {
            version  = resources->CreateVersion(previous);
            previous = version;
        }

        std::array<RDGBuffer, 3> sources;
        std::array<RDGBuffer, 6> destinations;

        for (uint32_t i = 0; i < inputs.size(); ++i)
        {
            sources[i] = resources->ImportHostWrittenBuffer(inputs[i]);
        }

        for (uint32_t i = 0; i < outputs.size(); ++i)
        {
            destinations[i] = resources->ImportBuffer(outputs[i]);
        }

        // Each value has one producer followed semantically by two independent consumers.
        std::array<uint32_t, 9> declarations;
        std::iota(declarations.begin(), declarations.end(), 0);
        std::shuffle(declarations.begin(), declarations.end(), random);

        for (uint32_t op : declarations)
        {
            const uint32_t value = op / 3;

            if (op % 3 == 0)
            {
                graph.AddTransferPass("writer").CopyBuffer(sources[value], versions[value],
                                                           {0, 0, 64});
            }
            else
            {
                graph.AddTransferPass("reader").CopyBuffer(
                    versions[value], destinations[2 * value + op % 3 - 1], {0, 0, 64});
            }
        }

        ASSERT_TRUE(graph.End());

        device->GetRDGMetrics().RequestCapture(true);

        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << "trial=" << trial;

        for (uint32_t i = 0; i < outputs.size(); ++i)
        {
            EXPECT_EQ(outputs[i]->bytes, inputs[i / 2]->bytes);
        }

        const std::vector<int32_t> order = CapturedOrder(device);
        ASSERT_EQ(order.size(), declarations.size());

        std::array<uint32_t, 9> position;

        for (uint32_t i = 0; i < order.size(); ++i)
        {
            position[declarations[order[i]]] = i;
        }

        for (uint32_t value = 0; value < versions.size(); ++value)
        {
            EXPECT_LT(position[3 * value], position[3 * value + 1]);
            EXPECT_LT(position[3 * value], position[3 * value + 2]);

            if (value != 2)
            {
                EXPECT_LT(position[3 * value + 1], position[3 * value + 3]);
                EXPECT_LT(position[3 * value + 2], position[3 * value + 3]);
            }
        }
    }

    for (TestBuffer* buffer : inputs)
    {
        device->DestroyBuffer(buffer);
    }

    for (TestBuffer* buffer : outputs)
    {
        device->DestroyBuffer(buffer);
    }
}

TEST_F(RenderCoreTest, VersionTransferPassCannotReadPredecessorAfterOverwritingIt)
{
    TestBuffer* source = Buffer();
    TestBuffer* result = Buffer();
    RenderGraph graph("version_transfer_alias");
    ASSERT_TRUE(graph.Begin());
    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer initial = resources->InitialVersion(resources->CreateBuffer(LogicalBuffer()));
    const RDGBuffer next    = resources->CreateVersion(initial);
    const RDGBuffer input   = resources->ImportHostWrittenBuffer(source);
    const RDGBuffer output  = resources->ImportBuffer(result);
    graph.AddTransferPass("overwrite_then_read_old")
        .CopyBuffer(input, next, {0, 0, 64})
        .CopyBuffer(initial, output, {0, 0, 64});
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eVersion);
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    device->DestroyBuffer(source);
    device->DestroyBuffer(result);
}

TEST_F(RenderCoreTest, VersionedCallbackFailureDoesNotPublishExtractionOrCommitContents)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer* source = Buffer();
    RenderGraph graph("version_failure_rollback");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer input         = resources->ImportHostWrittenBuffer(source);
    const RDGBuffer version = resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));

    RDGComputePassDesc read = IntentPass("failing_consumer");
    read.BindStorageBuffer("read_buffer", version, RDGContentGuarantee::eNone);
    graph.AddComputePass(read).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
        encoder.Fail(RDGErrorCode::eCallback, "version consumer failed");
    });
    graph.AddTransferPass("late_producer").CopyBuffer(input, version, {0, 0, 64});
    RDGExtractedBuffer output = resources->QueueBufferExtraction(version);
    ASSERT_TRUE(graph.End());

    const std::array<uint64_t, 3> submitted = rhi->submitted;
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eCallback);
    EXPECT_FALSE(output);
    EXPECT_EQ(rhi->submitted, submitted);
    EXPECT_EQ(DescribeResource(resources, version).physicalStableId, 0u);

    device->DestroyBuffer(source);
}

TEST_F(RenderCoreTest, CompatibleTextureReadersCanReorderForAnotherVersionsProducer)
{
    CaptureVersionGraph(device);
    CreateTestShaderProgram(device, "intent");
    RHITexture* texture = Texture();
    RHITexture* copy    = Texture();
    RenderGraph graph("compatible_version_readers");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGTexture image        = resources->ImportTexture(texture, RDGImportContents::eDefined);
    const RDGTexture destination  = resources->ImportTexture(copy);
    const RDGBuffer value = resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));
    RHITextureCopyRegion region{};
    region.size = {8, 8, 1};
    region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    graph.AddTransferPass("transfer_layout")
        .CopyTexture(image, destination, MakeVecView(&region, 1));

    RDGComputePassDesc consumer = IntentPass("sample_and_consume");
    consumer.BindSampledTexture("texture", nullptr, image);
    consumer.BindUniformBuffer("value", value);
    graph.AddComputePass(consumer);

    RDGComputePassDesc producer = IntentPass("sample_and_produce");
    producer.BindSampledTexture("texture", nullptr, image);
    producer.BindStorageBuffer("write_buffer", value, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(producer);

    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(CapturedOrder(device), (std::vector<int32_t>{0, 2, 1}));

    device->DestroyTexture(texture);
    device->DestroyTexture(copy);
}

TEST_F(RenderCoreTest, ReadyWorkDoesNotPrioritizeLowerNodeIds)
{
    CaptureVersionGraph(device);
    TestBuffer* input  = Buffer();
    TestBuffer* output = Buffer();
    std::fill(input->bytes.begin(), input->bytes.end(), uint8_t(43));
    RenderGraph graph("ready_work_queue");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer source        = resources->ImportHostWrittenBuffer(input);
    const RDGBuffer result        = resources->ImportBuffer(output);
    const RDGBuffer value = resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));
    graph.AddTransferPass("consumer").CopyBuffer(value, result, {0, 0, 64}); // ID 0
    graph.AddTransferPass("producer").CopyBuffer(source, value, {0, 0, 64}); // ID 1
    graph.AddTransferPass("independent")
        .NeverCull(); // ID 2 is already ready when ID 0 becomes ready.

    ASSERT_TRUE(graph.End());

    device->GetRDGMetrics().RequestCapture(true);

    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(CapturedOrder(device), (std::vector<int32_t>{1, 2, 0}));
    EXPECT_EQ(output->bytes, input->bytes);

    device->DestroyBuffer(input);
    device->DestroyBuffer(output);
}

TEST_F(RenderCoreTest, AutomaticAndExplicitDeclarationsProduceTheSameVersionDependencies)
{
    CaptureVersionGraph(device);
    TestBuffer* input        = Buffer();
    TestBuffer* firstResult  = Buffer();
    TestBuffer* secondResult = Buffer();
    std::fill(input->bytes.begin(), input->bytes.end(), uint8_t(62));
    std::vector<std::array<int32_t, 5>> expected;

    for (const bool explicitVersions : {false, true})
    {
        RenderGraph graph("unified_version_dependencies");
        ASSERT_TRUE(graph.Begin());

        RDGResourceManager* resources = graph.GetResourceManager();
        const RDGBuffer source        = resources->ImportHostWrittenBuffer(input);
        const RDGBuffer firstOutput   = resources->ImportBuffer(firstResult);
        const RDGBuffer secondOutput  = resources->ImportBuffer(secondResult);
        const RDGBuffer base          = resources->CreateBuffer(LogicalBuffer());
        const RDGBuffer first         = explicitVersions ? resources->CreateVersion(base) : base;
        const RDGBuffer second        = explicitVersions ? resources->CreateVersion(first) : base;
        graph.AddTransferPass("produce_first").CopyBuffer(source, first, {0, 0, 64});
        graph.AddTransferPass("read_first").CopyBuffer(first, firstOutput, {0, 0, 64});
        graph.AddTransferPass("produce_second").CopyBuffer(source, second, {0, 0, 64});
        graph.AddTransferPass("read_second").CopyBuffer(second, secondOutput, {0, 0, 64});
        RDGExtractedBuffer extracted = resources->QueueBufferExtraction(second);
        ASSERT_TRUE(graph.End());

        device->GetRDGMetrics().RequestCapture(true);

        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        EXPECT_EQ(firstResult->bytes, input->bytes);
        EXPECT_EQ(secondResult->bytes, input->bytes);
        ASSERT_TRUE(extracted);

        CapturedOrder(device);
        std::vector<std::array<int32_t, 5>> dependencies;

        for (RDGDependency const& edge : graph.GetDependencies())
        {
            EXPECT_GE(edge.version, 0);
            dependencies.push_back({edge.source, edge.destination, edge.resourceId, edge.version,
                                    int32_t(edge.reason)});
        }

        std::sort(dependencies.begin(), dependencies.end());

        if (!explicitVersions)
        {
            expected = dependencies;
        }
        else
        {
            EXPECT_EQ(dependencies, expected);
        }
    }

    for (TestBuffer* buffer : {input, firstResult, secondResult})
    {
        device->DestroyBuffer(buffer);
    }
}

TEST_F(RenderCoreTest, DifferentTextureLayoutsCannotCreateAFalseVersionCycle)
{
    CaptureVersionGraph(device);
    CreateTestShaderProgram(device, "intent");
    RHITexture* texture = Texture();
    RHITexture* copy    = Texture();

    for (const bool explicitImage : {false, true})
    {
        RenderGraph graph("read_layouts_are_not_value_dependencies");
        ASSERT_TRUE(graph.Begin());

        RDGResourceManager* resources = graph.GetResourceManager();
        const RDGTexture base  = resources->ImportTexture(texture, RDGImportContents::eDefined);
        const RDGTexture image = explicitImage ? resources->InitialVersion(base) : base;
        const RDGTexture destination = resources->ImportTexture(copy);
        const RDGBuffer future = resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));
        const RDGBuffer output = resources->CreateBuffer(LogicalBuffer());
        RHITextureCopyRegion region{};
        region.size = {8, 8, 1};
        region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        graph.AddTransferPass("transfer_consumer")
            .CopyTexture(image, destination, MakeVecView(&region, 1))
            .CopyBuffer(future, output, {0, 0, 64});

        RDGComputePassDesc producer = IntentPass("sampled_producer");
        producer.BindSampledTexture("texture", nullptr, image);
        producer.BindStorageBuffer("write_buffer", future, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(producer);

        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        EXPECT_EQ(CapturedOrder(device), (std::vector<int32_t>{1, 0}));
        ASSERT_EQ(graph.GetDependencies().size(), 1u);
        EXPECT_EQ(graph.GetDependencies()[0].reason, RDGDependencyReason::eVersionProducer);

        const RDGMetricsSnapshot& sample = device->GetRDGMetrics().GetLastSnapshot();

        for (RDGMetricIssue const issue :
             {RDGMetricIssue::eStageCoverage, RDGMetricIssue::eLayoutMismatch,
              RDGMetricIssue::eRangeCoverage})
        {
            EXPECT_EQ(sample.issues[size_t(issue)], 0u) << RDGMetrics::Format(sample);
        }

        EXPECT_TRUE(graph.GetWarnings().empty());
    }

    device->DestroyTexture(texture);
    device->DestroyTexture(copy);
}

static void AddAutomaticReader(RenderGraph& graph, RDGBuffer base)
{
    RDGComputePassDesc reader = IntentPass("automatic_reader");
    reader.BindUniformBuffer("value", base);
    graph.AddComputePass(reader);
}

TEST_F(RenderCoreTest, AutomaticDeclarationsCannotSilentlySelectAnExplicitVersion)
{
    CreateTestShaderProgram(device, "intent");

    for (const bool automaticFirst : {false, true})
    {
        RenderGraph graph("ambiguous_value_selection");
        ASSERT_TRUE(graph.Begin());

        RDGResourceManager* resources = graph.GetResourceManager();
        const RDGBuffer base          = resources->CreateBuffer(LogicalBuffer());

        if (automaticFirst)
        {
            AddAutomaticReader(graph, base);
        }

        const RDGBuffer value = resources->CreateVersion(base);

        RDGComputePassDesc writer = IntentPass("explicit_writer");
        writer.BindStorageBuffer("write_buffer", value, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(writer);

        if (!automaticFirst)
        {
            AddAutomaticReader(graph, base);
        }

        const uint32_t pipelines = rhi->pipelineCount;
        EXPECT_FALSE(graph.End());
        EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eVersion);
        EXPECT_FALSE(device->ExecuteRenderGraph(graph));
        EXPECT_EQ(rhi->pipelineCount, pipelines);
        EXPECT_EQ(DescribeResource(resources, base).physicalStableId, 0u);
    }
}

// Opt in with --gtest_also_run_disabled_tests --gtest_filter=*DependencyBuildBenchmark.
// CPU/mock measurements only; Prepare includes sorting, validity, allocation and barriers.
TEST_F(RenderCoreTest, DISABLED_DependencyBuildBenchmark)
{
    using Clock = std::chrono::steady_clock;
    CreateTestShaderProgram(device, "readers");
    RDGExecutor executor(device);
    RDGMetricsOptions options;
    options.logging.enabled = false;
    options.validate        = false;
    executor.GetMetrics().Configure(options);

    for (const char* shape : {"shared_read", "independent", "fan_out", "interleaved"})
    {
        for (const uint32_t count : {64u, 256u, 1024u, 4096u})
        {
            std::vector<TestBuffer*> buffers;

            for (uint32_t i = 0; i < count + 1; ++i)
            {
                buffers.push_back(Buffer());
            }

            RenderGraph graph("dependency_benchmark");
            std::array<std::vector<double>, 4> times;
            std::vector<RDGMetricOrderAccess> declarations;
            std::vector<int32_t> order;
            const bool independent = std::string_view(shape) == "independent";
            const bool fanOut      = std::string_view(shape) == "fan_out";
            const bool interleaved = std::string_view(shape) == "interleaved";
            int32_t sharedVersion  = 0;

            for (uint32_t i = 0; i < count; ++i)
            {
                order.push_back(i);
                const bool writer = fanOut && (i == 0 || i == count - 1);

                if (writer)
                {
                    ++sharedVersion;
                }

                declarations.push_back({int32_t(i), independent ? i + 1u : 0u, writer,
                                        independent ? 0 : sharedVersion});

                if (interleaved)
                {
                    declarations.push_back({int32_t(i), i + 1u, true, 1});
                }
                else if (writer)
                {
                    declarations.push_back({int32_t(i), count, false});
                }
            }

            for (uint32_t run = 0; run < 8; ++run)
            {
                ASSERT_TRUE(graph.Begin());

                for (TestBuffer* buffer : buffers)
                {
                    graph.GetResourceManager()->ImportBuffer(buffer, RDGImportContents::eDefined);
                }

                const std::chrono::steady_clock::time_point start = Clock::now();

                if (interleaved)
                {
                    std::vector<std::unique_ptr<RDGTransferPassCmdRecorder>> recorders;

                    for (uint32_t i = 0; i < count; ++i)
                    {
                        recorders.emplace_back(
                            new RDGTransferPassCmdRecorder(graph.AddTransferPass("interleaved")));
                    }

                    for (uint32_t i = count; i-- > 0;)
                    {
                        recorders[i]->CopyBuffer(buffers[0], buffers[i + 1], {0, 0, 4});
                    }
                }
                else
                {
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        if (fanOut && (i == 0 || i == count - 1))
                        {
                            graph.AddTransferPass("writer").CopyBuffer(buffers[count], buffers[0],
                                                                       {0, 0, 4});
                        }
                        else
                        {
                            RDGGraphicsPassDesc desc;
                            desc.SetShaderProgramName("readers");
                            desc.BindVertexBuffer(buffers[independent ? i + 1 : 0]);
                            graph.AddGraphicsPass(std::move(desc));
                        }
                    }
                }

                const std::chrono::steady_clock::time_point recorded = Clock::now();
                ASSERT_TRUE(graph.End());

                const std::chrono::steady_clock::time_point ended = Clock::now();
                ASSERT_TRUE(executor.Prepare(&graph));

                const std::chrono::steady_clock::time_point prepared = Clock::now();
                uint32_t issues                                      = 0;
                RDGBarrierValidator::CheckOrder(
                    count, order, declarations,
                    [&](RDGMetricIssue, int32_t, int32_t, uint64_t) { ++issues; });
                const std::chrono::steady_clock::time_point validated = Clock::now();
                ASSERT_EQ(issues, 0u);

                if (run != 0)
                {
                    times[0].push_back(
                        std::chrono::duration<double, std::micro>(recorded - start).count());
                    times[1].push_back(
                        std::chrono::duration<double, std::micro>(ended - recorded).count());
                    times[2].push_back(
                        std::chrono::duration<double, std::micro>(prepared - ended).count());
                    times[3].push_back(
                        std::chrono::duration<double, std::micro>(validated - prepared).count());
                }
            }

            for (std::vector<double>& samples : times)
            {
                std::sort(samples.begin(), samples.end());
            }

            std::printf(
                "RDG_BENCH %s n=%u record_us=%.2f end_us=%.2f prepare_us=%.2f order_check_us=%.2f\n",
                shape, count, times[0][3], times[1][3], times[2][3], times[3][3]);

            ASSERT_TRUE(graph.Reset());

            for (TestBuffer* buffer : buffers)
            {
                device->DestroyBuffer(buffer);
            }
        }
    }
}

TEST_F(RenderCoreTest, LogicalShaderArraysPreserveVersionsViewsAndElementIndices)
{
    TestShader* shader =
        static_cast<TestShader*>(CreateTestShaderProgram(device, "intent")->GetShader());
    shader->SetDescriptorArray("write_image", 2);
    shader->SetDescriptorArray("read_buffer", 2);
    shader->SetDescriptorArray("value", 2, 16);
    TestBuffer* input = Buffer();
    std::fill(input->bytes.begin(), input->bytes.end(), uint8_t(57));
    RenderGraph graph("logical_arrays");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    RDGBuffer source              = resources->ImportHostWrittenBuffer(input);
    std::array<RDGTexture, 2> textures;
    std::array<RDGBuffer, 2> buffers;
    std::array<RDGTextureViewDesc, 2> views;

    for (uint32_t i = 0; i < 2; ++i)
    {
        textures[i] = resources->CreateVersion(resources->CreateTexture(LogicalTexture(3)));
        buffers[i]  = resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));
        RHITextureSubResourceRange range = RHITextureSubResourceRange::Color();
        range.baseMipLevel               = i + 1;
        views[i]                         = RDGTextureViewDesc{range};
    }

    const std::array<RDGTexture, 2> savedTextures = textures;
    const std::array<RDGBuffer, 2> savedBuffers   = buffers;

    RDGComputePassDesc read = IntentPass("array_consumer");
    read.BindSampledTexture("uTextureArray", nullptr, MakeVecView(textures), MakeVecView(views));
    read.BindStorageBuffer("read_buffer", MakeVecView(buffers));
    read.BindUniformBuffer("value", MakeVecView(savedBuffers));
    graph.AddComputePass(std::move(read));

    RDGComputePassDesc write = IntentPass("array_producer");
    write.BindStorageImage("write_image", MakeVecView(textures), RDGContentGuarantee::eFullWrite,
                           MakeVecView(views));
    graph.AddComputePass(std::move(write));

    for (RDGBuffer buffer : buffers)
    {
        graph.AddTransferPass("buffer_producer").CopyBuffer(source, buffer, {0, 0, 64});
    }

    // The graph owns copies, including the individual view descriptors.
    textures.fill({});
    buffers.fill({});
    views.fill({});

    ASSERT_TRUE(graph.End());

    for (uint32_t replay = 0; replay < 2; ++replay)
    {
        rhi->graphics.boundResources.clear();
        rhi->graphics.boundArrayIndices.clear();

        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        ASSERT_EQ(rhi->graphics.boundResources.size(), 8u);

        for (uint32_t i = 0; i < 8; ++i)
        {
            EXPECT_EQ(rhi->graphics.boundArrayIndices[i], i % 2);
        }

        for (uint32_t i = 0; i < 2; ++i)
        {
            RHITextureView* writerView =
                static_cast<RHITextureView*>(rhi->graphics.boundResources[i]);
            EXPECT_EQ(writerView, rhi->graphics.boundResources[2 + i]);
            EXPECT_EQ(writerView->GetSubResourceRange().baseMipLevel, i + 1);
            EXPECT_EQ(writerView->GetTexture()->GetStableId(),
                      DescribeResource(resources, savedTextures[i]).physicalStableId);
            EXPECT_EQ(rhi->graphics.boundResources[4 + i], rhi->graphics.boundResources[6 + i]);
            EXPECT_EQ(rhi->graphics.boundResources[4 + i]->GetStableId(),
                      DescribeResource(resources, savedBuffers[i]).physicalStableId);
            EXPECT_EQ(static_cast<TestBuffer*>(rhi->graphics.boundResources[4 + i])->bytes,
                      input->bytes);
        }

        EXPECT_TRUE(graph.GetWarnings().empty());
    }

    device->DestroyBuffer(input);
}

TEST_F(RenderCoreTest, LogicalArraysAndViewDescriptorsRejectMalformedSelectionsBeforeAllocation)
{
    CreateTestShaderProgram(device, "intent");

    for (uint32_t scenario = 0; scenario < 7; ++scenario)
    {
        RenderGraph graph("invalid_resource_array");
        ASSERT_TRUE(graph.Begin());

        RDGResourceManager* resources = graph.GetResourceManager();
        std::array<RDGTexture, 2> textures{resources->CreateTexture(LogicalTexture(3)),
                                           resources->CreateTexture(LogicalTexture(3))};
        std::array<RDGTextureViewDesc, 2> views;

        RDGComputePassDesc pass = IntentPass();

        if (scenario == 0)
        {
            pass.BindStorageBuffer("read_buffer", VectorView<RDGBuffer>{});
        }
        else if (scenario == 1)
        {
            std::array<RDGBuffer, 2> buffers{resources->CreateBuffer(LogicalBuffer()),
                                             resources->CreateBuffer(LogicalBuffer())};
            pass.BindStorageBuffer("read_buffer", MakeVecView(buffers)); // Reflected count is one.
        }
        else
        {
            if (scenario == 2)
            {
                textures[1] = {};
            }

            if (scenario == 3)
            {
                views[1] = RDGTextureViewDesc{
                    RHITextureSubResourceRange{}}; // Explicitly empty is not full.
            }

            if (scenario == 4 || scenario == 5)
            {
                RHITextureSubResourceRange range = RHITextureSubResourceRange::Color();

                if (scenario == 4)
                {
                    range.baseMipLevel = 3;
                }
                else
                {
                    range.baseArrayLayer = 1;
                }

                views[1] = RDGTextureViewDesc{range};
            }

            pass.BindSampledTexture("uTextureArray", nullptr, MakeVecView(textures),
                                    MakeVecView(views.data(), scenario == 6 ? 1 : 2));
        }

        graph.AddComputePass(std::move(pass));

        EXPECT_FALSE(graph.End());
        EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    }

    EXPECT_EQ(rhi->textureCreations, 0u);
    EXPECT_EQ(rhi->finalizedLists, 0u);
}

TEST_F(RenderCoreTest, LogicalGeometryAndIndirectCommandsConsumeLaterProducedVersions)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer* input          = Buffer();
    const uint32_t arguments[] = {1, 1, 1, 0, 0};
    std::fill(input->bytes.begin(), input->bytes.end(), uint8_t(0));
    std::memcpy(input->bytes.data(), arguments, sizeof(arguments));
    RenderGraph graph("logical_geometry_indirect");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer source        = resources->ImportHostWrittenBuffer(input);
    const RDGBuffer value = resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));

    RDGGraphicsPassDesc draw;
    draw.SetShaderProgramName("intent");
    draw.BindVertexBuffer(value);
    draw.BindIndexBuffer(value, DataFormat::eR32UInt, 4);
    draw.UseIndirectBuffer(value);
    graph.AddGraphicsPass(std::move(draw)).RecordPassCommands([value](RDGPassCmdEncoder& encoder) {
        encoder.DrawIndexed(1, 1, 0, 0, 0);
        encoder.DrawIndexedIndirect(value, 0, 1, 20);
    });

    RDGComputePassDesc dispatch = IntentPass();
    dispatch.UseIndirectBuffer(value);
    graph.AddComputePass(std::move(dispatch))
        .RecordPassCommands(
            [value](RDGPassCmdEncoder& encoder) { encoder.DispatchIndirect(value, 0); });
    graph.AddTransferPass("argument_producer").CopyBuffer(source, value, {0, 0, 64});
    RDGExtractedBuffer extracted = resources->QueueBufferExtraction(value);
    ASSERT_TRUE(graph.End());

    for (uint32_t replay = 0; replay < 2; ++replay)
    {
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        ASSERT_TRUE(extracted);
        EXPECT_EQ(rhi->graphics.vertexBuffers.back(), extracted.Get());
        EXPECT_EQ(rhi->graphics.indirectDraws.back(), extracted.Get());
        EXPECT_EQ(rhi->graphics.indirectDispatches.back(), extracted.Get());
        EXPECT_EQ(static_cast<TestBuffer*>(extracted.Get())->bytes, input->bytes);
        EXPECT_TRUE(graph.GetWarnings().empty());
    }

    device->DestroyBuffer(input);
}

TEST_F(RenderCoreTest, LogicalIndirectCallbacksCannotSubstituteAnUndeclaredVersion)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer* source = Buffer();

    for (uint32_t scenario = 0; scenario < 3; ++scenario)
    {
        RenderGraph graph("indirect_version_mismatch");
        ASSERT_TRUE(graph.Begin());

        RDGResourceManager* resources = graph.GetResourceManager();
        RDGBuffer first       = resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));
        const RDGBuffer saved = first;

        if (scenario == 2)
        {
            ASSERT_TRUE(graph.Reset());
            ASSERT_TRUE(graph.Begin());
            first = resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));
        }

        const RDGBuffer second = resources->CreateVersion(first);
        const RDGBuffer input  = resources->ImportHostWrittenBuffer(source);

        RDGComputePassDesc pass = IntentPass();

        if (scenario != 0)
        {
            pass.UseIndirectBuffer(first);
        }

        graph.AddComputePass(std::move(pass))
            .RecordPassCommands(
                [selected = scenario == 2 ? saved : second](RDGPassCmdEncoder& encoder) {
                    encoder.DispatchIndirect(selected, 0);
                });
        graph.AddTransferPass("first").CopyBuffer(input, first, {0, 0, 64});
        graph.AddTransferPass("second").CopyBuffer(input, second, {0, 0, 64});

        ASSERT_TRUE(graph.End());
        EXPECT_FALSE(device->ExecuteRenderGraph(graph));
        EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eBinding);
    }

    EXPECT_EQ(rhi->finalizedLists, 0u);
    EXPECT_TRUE(rhi->graphics.indirectDispatches.empty());

    device->DestroyBuffer(source);
}

TEST_F(RenderCoreTest, LogicalUploadsAndMipGenerationPreservePredecessorContents)
{
    TestBuffer* input = Buffer(256);
    std::fill(input->bytes.begin(), input->bytes.end(), uint8_t(73));
    RenderGraph graph("logical_upload_mips");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer source        = resources->ImportHostWrittenBuffer(input);
    const RDGTexture uploaded =
        resources->CreateVersion(resources->CreateTexture(LogicalTexture(3)));
    const RDGTexture mipped = resources->CreateVersion(uploaded);
    RHIBufferTextureCopyRegion region;
    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.textureSize = {8, 8, 1};
    graph.AddTransferPass("mips").GenerateMipmaps(mipped); // Declared before its base-mip producer.
    graph.AddTransferPass("upload").CopyBufferToTexture(source, uploaded, region);
    RDGTextureDesc required;
    ASSERT_TRUE(resources->GetTextureDesc(mipped, required));
    EXPECT_TRUE(required.usageFlags.HasFlag(RHITextureUsageFlagBits::eTransferSrc));
    EXPECT_TRUE(required.usageFlags.HasFlag(RHITextureUsageFlagBits::eTransferDst));

    RDGExtractedTexture extracted = resources->QueueTextureExtraction(mipped);
    ASSERT_TRUE(graph.End());

    for (uint32_t replay = 0; replay < 2; ++replay)
    {
        rhi->graphics.blits.clear();
        rhi->graphics.textureCopies.clear();
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        ASSERT_TRUE(extracted);
        EXPECT_EQ(rhi->graphics.textureCopies.size(), 1u);
        EXPECT_EQ(rhi->graphics.blits.size(), 2u);
        EXPECT_EQ(DescribeResource(resources, uploaded).physicalStableId,
                  extracted.Get()->GetStableId());
        EXPECT_TRUE(graph.GetWarnings().empty());
    }

    device->DestroyBuffer(input);
}

TEST_F(RenderCoreTest, LogicalUploadsAndMipGenerationRejectMissingOrPartialSources)
{
    TestBuffer* input = Buffer(256);

    for (uint32_t scenario = 0; scenario < 4; ++scenario)
    {
        RenderGraph graph("invalid_logical_upload");
        ASSERT_TRUE(graph.Begin());

        RDGResourceManager* resources = graph.GetResourceManager();
        const RDGBuffer source        = scenario == 0 ?
            resources->ImportBuffer(input, RDGImportContents::eUndefined) :
            resources->ImportHostWrittenBuffer(input);
        const RDGTexture uploaded =
            resources->CreateVersion(resources->CreateTexture(LogicalTexture(3)));
        RHIBufferTextureCopyRegion region;
        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.textureSize  = {scenario == 2 ? 4 : 8, 8, 1};
        region.bufferOffset = scenario == 1 ? 4 : 0;

        if (scenario != 3)
        {
            graph.AddTransferPass("upload").CopyBufferToTexture(source, uploaded, region);
        }

        const RDGTexture mipped = scenario == 3 ? uploaded : resources->CreateVersion(uploaded);
        graph.AddTransferPass("mips").NeverCull().GenerateMipmaps(mipped);

        if (scenario == 1)
        {
            EXPECT_FALSE(graph.End());
            EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eRange);
        }
        else
        {
            ASSERT_TRUE(graph.End());
            EXPECT_FALSE(device->ExecuteRenderGraph(graph));
            EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eUninitialized);
        }
    }

    EXPECT_EQ(rhi->textureCreations, 0u);
    EXPECT_EQ(rhi->finalizedLists, 0u);

    device->DestroyBuffer(input);
}

TEST_F(RenderCoreTest, MipGenerationRejectsImportedTexturesWithoutTransferSourceUsage)
{
    RHITextureCreateInfo info{};
    info.format = DataFormat::eR8G8B8A8UNORM;
    info.width = info.height = 8;
    info.mipmaps             = 3;
    info.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);
    RHITexture* texture = rhi->CreateTexture(info);

    for (const bool logical : {false, true})
    {
        RenderGraph graph("mips_missing_source_usage");
        ASSERT_TRUE(graph.Begin());
        RDGTransferPassCmdRecorder pass = graph.AddTransferPass("mips");

        if (logical)
        {
            pass.GenerateMipmaps(graph.GetResourceManager()->ImportTexture(texture));
        }
        else
        {
            pass.GenerateMipmaps(texture);
        }

        EXPECT_FALSE(graph.End());
        EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eBinding);
        EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    }

    EXPECT_TRUE(rhi->graphics.blits.empty());
    EXPECT_EQ(rhi->finalizedLists, 0u);

    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, ContentGuaranteesCannotContradictReflectedAccess)
{
    CreateTestShaderProgram(device, "intent");

    for (const std::pair<NameID, RDGContentGuarantee> bindingGuarantee :
         {std::pair{"read_buffer", RDGContentGuarantee::eFullWrite},
          std::pair{"read_buffer", RDGContentGuarantee::eDiscard},
          std::pair{"read_buffer", RDGContentGuarantee::eProducedElements},
          std::pair{"write_buffer", RDGContentGuarantee::eConsumeProducedElements},
          std::pair{"buffer", RDGContentGuarantee::eConsumeProducedElements},
          std::pair{"buffer", static_cast<RDGContentGuarantee>(255)}})
    {
        RenderGraph graph("invalid_content_guarantee");
        ASSERT_TRUE(graph.Begin());
        RDGBuffer buffer = graph.GetResourceManager()->CreateBuffer(LogicalBuffer());

        RDGComputePassDesc pass = IntentPass();
        pass.BindStorageBuffer(bindingGuarantee.first, buffer, bindingGuarantee.second);
        graph.AddComputePass(std::move(pass));
        EXPECT_FALSE(graph.End());
        EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eBinding);
        EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    }

    EXPECT_EQ(rhi->finalizedLists, 0u);
}

TEST_F(RenderCoreTest, ViewDescriptorsPreserveVersionSchedulingAndReuseNativeViews)
{
    CreateTestShaderProgram(device, "intent");
    RenderGraph graph("versioned_view_descriptors");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGTexture first  = resources->CreateVersion(resources->CreateTexture(LogicalTexture(3)));
    const RDGTexture second = resources->CreateVersion(first);
    RHITextureSubResourceRange range = RHITextureSubResourceRange::Color();
    range.baseMipLevel               = 1;
    const RDGTextureViewDesc view{range};

    for (RDGTexture const texture : {first, second})
    {
        RDGComputePassDesc read = IntentPass();
        read.BindSampledTexture("texture", nullptr, texture, view);
        graph.AddComputePass(std::move(read));
    }

    for (RDGTexture const texture : {second, first})
    {
        RDGComputePassDesc write = IntentPass();
        write.BindStorageImage("write_image", texture, view, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(write));
    }

    ASSERT_TRUE(graph.End());

    RHIResource* previousView = nullptr;

    for (uint32_t replay = 0; replay < 2; ++replay)
    {
        rhi->graphics.boundResources.clear();

        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        ASSERT_EQ(rhi->graphics.boundResources.size(), 4u);

        if (previousView != nullptr)
        {
            EXPECT_EQ(previousView, rhi->graphics.boundResources[0]);
        }

        previousView = rhi->graphics.boundResources[0];

        for (RHIResource* bound : rhi->graphics.boundResources)
        {
            EXPECT_EQ(bound, previousView);
        }

        EXPECT_TRUE(graph.GetWarnings().empty());
    }
}

TEST_F(RenderCoreTest, PendingExtractionTicketsCanMoveOrBeDroppedBeforePublication)
{
    TestBuffer* source = Buffer();
    std::fill(source->bytes.begin(), source->bytes.end(), uint8_t(61));

    for (const bool texture : {false, true})
    {
        for (const bool dropTicket : {false, true})
        {
            SCOPED_TRACE(texture);
            SCOPED_TRACE(dropTicket);
            RenderGraph graph("pending_extraction_owner");
            ASSERT_TRUE(graph.Begin());

            RDGResourceManager* resources = graph.GetResourceManager();
            RDGResource value;
            RDGExtractedBuffer bufferOwner;
            RDGExtractedTexture textureOwner;

            if (texture)
            {
                const RDGTexture output =
                    resources->CreateVersion(resources->CreateTexture(LogicalTexture()));
                value = output;
                graph.AddTransferPass("clear").ClearTexture(output, Color(0.f));
                RDGExtractedTexture pending = resources->QueueTextureExtraction(output);
                textureOwner                = std::move(pending);
                EXPECT_FALSE(pending);
            }
            else
            {
                const RDGBuffer input = resources->ImportHostWrittenBuffer(source);
                const RDGBuffer output =
                    resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));
                value = output;
                graph.AddTransferPass("copy").CopyBuffer(input, output, {0, 0, 64});
                RDGExtractedBuffer pending = resources->QueueBufferExtraction(output);
                bufferOwner                = std::move(pending);
                EXPECT_FALSE(pending);
            }

            ASSERT_TRUE(graph.End());

            RDGExecutor executor(device);
            ASSERT_TRUE(executor.Prepare(&graph));
            EXPECT_FALSE(bufferOwner);
            EXPECT_FALSE(textureOwner); // Preparation must not publish either moved ticket.

            if (dropTicket)
            {
                bufferOwner.Reset();
                textureOwner.Reset();
            }

            ASSERT_TRUE(device->ExecuteRenderGraph(graph));

            const uint64_t id = DescribeResource(resources, value).physicalStableId;
            ASSERT_NE(id, 0u);

            if (!dropTicket)
            {
                if (texture)
                {
                    ASSERT_TRUE(textureOwner);
                    EXPECT_EQ(textureOwner.Get()->GetStableId(), id);
                }
                else
                {
                    ASSERT_TRUE(bufferOwner);
                    EXPECT_EQ(static_cast<TestBuffer*>(bufferOwner.Get())->bytes, source->bytes);
                }
            }

            bufferOwner.Reset();
            textureOwner.Reset();
            rhi->completed = rhi->submitted;
            device->CollectCompletedResources();

            EXPECT_FALSE(destroyed.contains(id)); // The recorded graph remains a replay owner.
            ASSERT_TRUE(device->ExecuteRenderGraph(graph));
            EXPECT_EQ(DescribeResource(resources, value).physicalStableId, id);
            ASSERT_TRUE(graph.Reset());

            rhi->completed = rhi->submitted;
            device->CollectCompletedResources();

            EXPECT_TRUE(destroyed.contains(id));
        }
    }

    device->DestroyBuffer(source);
}

TEST_F(RenderCoreTest, ImportedExtractionsSurviveOriginalGraphAndOwnerRelease)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer* source = Buffer();
    std::fill(source->bytes.begin(), source->bytes.end(), uint8_t(37));
    RDGExtractedBuffer bufferOwner;
    RDGExtractedTexture textureOwner;

    {
        RenderGraph producer("extraction_producer");
        ASSERT_TRUE(producer.Begin());
        RDGResourceManager* resources = producer.GetResourceManager();
        const RDGBuffer input         = resources->ImportHostWrittenBuffer(source);
        const RDGBuffer buffer = resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));
        const RDGTexture texture =
            resources->CreateVersion(resources->CreateTexture(LogicalTexture(2)));
        producer.AddTransferPass("produce_buffer").CopyBuffer(input, buffer, {0, 0, 64});
        producer.AddTransferPass("produce_texture").ClearTexture(texture, Color(0.f));
        bufferOwner = resources->QueueBufferExtraction(
            buffer, BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferSrcBuffer));
        textureOwner = resources->QueueTextureExtraction(texture);
        ASSERT_TRUE(producer.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(producer));
    }

    ASSERT_TRUE(bufferOwner);
    ASSERT_TRUE(textureOwner);

    const uint64_t bufferId  = bufferOwner.Get()->GetStableId();
    const uint64_t textureId = textureOwner.Get()->GetStableId();
    TestBuffer* result       = Buffer();
    RenderGraph consumer("extraction_consumer");
    ASSERT_TRUE(consumer.Begin());

    RDGResourceManager* resources = consumer.GetResourceManager();
    const RDGBuffer buffer        = resources->ImportBuffer(bufferOwner.Get());
    const RDGTexture texture      = resources->ImportTexture(textureOwner.Get());
    EXPECT_EQ(buffer, resources->ImportBuffer(bufferOwner.Get()));
    EXPECT_EQ(texture, resources->ImportTexture(textureOwner.Get()));

    consumer.AddTransferPass("read_buffer")
        .CopyBuffer(buffer, resources->ImportBuffer(result), {0, 0, 64});

    RDGComputePassDesc pass          = IntentPass();
    RHITextureSubResourceRange range = RHITextureSubResourceRange::Color();
    range.baseMipLevel               = 1;
    pass.BindSampledTexture("texture", nullptr, texture, RDGTextureViewDesc{range});
    consumer.AddComputePass(std::move(pass));

    ASSERT_TRUE(consumer.End());

    bufferOwner.Reset();
    textureOwner.Reset();
    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    EXPECT_FALSE(destroyed.contains(bufferId));
    EXPECT_FALSE(destroyed.contains(textureId));

    RHIResource* nativeView = nullptr;

    for (uint32_t replay = 0; replay < 2; ++replay)
    {
        std::fill(result->bytes.begin(), result->bytes.end(), uint8_t(0));
        rhi->graphics.boundResources.clear();
        ASSERT_TRUE(device->ExecuteRenderGraph(consumer));
        EXPECT_EQ(result->bytes, source->bytes);
        ASSERT_EQ(rhi->graphics.boundResources.size(), 1u);

        if (nativeView)
        {
            EXPECT_EQ(rhi->graphics.boundResources[0], nativeView);
        }

        nativeView = rhi->graphics.boundResources[0];
        EXPECT_EQ(static_cast<RHITextureView*>(nativeView)->GetSubResourceRange().baseMipLevel, 1u);
        EXPECT_TRUE(consumer.GetWarnings().empty());
    }

    ASSERT_TRUE(consumer.Reset());

    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    EXPECT_TRUE(destroyed.contains(bufferId));
    EXPECT_TRUE(destroyed.contains(textureId));

    device->DestroyBuffer(result);
    device->DestroyBuffer(source);
}

TEST_F(RenderCoreTest, FailedReplayPreservesPreviouslyPublishedExtractionAndContents)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer* source = Buffer();
    std::fill(source->bytes.begin(), source->bytes.end(), uint8_t(23));
    RenderGraph graph("published_extraction_failed_replay");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer input         = resources->ImportHostWrittenBuffer(source);
    const RDGBuffer value = resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));
    graph.AddTransferPass("producer").CopyBuffer(input, value, {0, 0, 64});
    bool failReplay = false;

    RDGComputePassDesc reader = IntentPass();
    reader.BindStorageBuffer("read_buffer", value);
    graph.AddComputePass(std::move(reader)).RecordPassCommands([&](RDGPassCmdEncoder& encoder) {
        if (failReplay)
        {
            encoder.Fail(RDGErrorCode::eCallback, "reject replay");
        }
    });
    RDGExtractedBuffer owner = resources->QueueBufferExtraction(
        value, BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferSrcBuffer));
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    ASSERT_TRUE(owner);

    TestBuffer* published                   = static_cast<TestBuffer*>(owner.Get());
    const uint64_t id                       = published->GetStableId();
    const std::vector<uint8_t> oldBytes     = published->bytes;
    const std::array<uint64_t, 3> submitted = rhi->submitted;
    std::fill(source->bytes.begin(), source->bytes.end(), uint8_t(89));
    failReplay = true;

    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eCallback);
    EXPECT_EQ(rhi->submitted, submitted);
    EXPECT_EQ(owner.Get(), published);
    EXPECT_EQ(published->bytes, oldBytes);
    ASSERT_TRUE(graph.Reset());

    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    EXPECT_FALSE(destroyed.contains(id));

    RenderGraph readerGraph("read_last_successful_result");
    ASSERT_TRUE(readerGraph.Begin());

    TestBuffer* result = Buffer();
    readerGraph.AddTransferPass("copy").CopyBuffer(owner.Get(), result, {0, 0, 64});

    ASSERT_TRUE(readerGraph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(readerGraph));
    EXPECT_EQ(result->bytes, oldBytes);
    EXPECT_TRUE(readerGraph.GetWarnings().empty());
    ASSERT_TRUE(readerGraph.Reset());

    owner.Reset();
    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    EXPECT_TRUE(destroyed.contains(id));

    device->DestroyBuffer(source);
    device->DestroyBuffer(result);
}

TEST_F(RenderCoreTest, DeadPassesDoNotAllocateRunCallbacksOrKeepUnusedReadersAlive)
{
    CaptureVersionGraph(device);
    CreateTestShaderProgram(device, "intent");
    RenderGraph graph("dead_passes");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    TestBuffer* source            = Buffer();
    TestBuffer* output            = Buffer();
    std::fill(source->bytes.begin(), source->bytes.end(), uint8_t(17));
    const RDGBuffer input  = resources->ImportHostWrittenBuffer(source);
    const RDGBuffer value  = resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));
    const RDGBuffer second = resources->CreateVersion(value);
    const RDGBuffer unused = resources->CreateBuffer(LogicalBuffer());
    const RDGTexture deadTexture = resources->CreateTexture(LogicalTexture());
    graph.AddTransferPass("first").CopyBuffer(input, value, {0, 0, 64});
    graph.AddTransferPass("dead_reader").CopyBuffer(value, unused, {0, 0, 64});
    graph.AddTransferPass("second").CopyBuffer(input, second, {0, 0, 64});
    graph.AddTransferPass("observable")
        .CopyBuffer(second, resources->ImportBuffer(output), {0, 0, 64});
    graph.AddTransferPass("dead_clear").ClearTexture(deadTexture, Color(0.f));
    bool deadCallback = false;

    RDGComputePassDesc dead = IntentPass("dead_shader");
    dead.allowCulling       = true;
    dead.BindStorageImage("write_image", deadTexture, {}, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(dead).RecordPassCommands([&](RDGPassCmdEncoder&) { deadCallback = true; });
    bool retainedCallback = false;
    graph.AddComputePass(IntentPass("side_effect")).RecordPassCommands([&](RDGPassCmdEncoder&) {
        retainedCallback = true;
    });

    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_FALSE(deadCallback);
    EXPECT_TRUE(retainedCallback);
    EXPECT_EQ(output->bytes, source->bytes);
    EXPECT_EQ(graph.GetCompileStats().culledPassCount, 3u);
    EXPECT_EQ(graph.GetCompileStats().passCount, 4u);
    EXPECT_EQ(graph.GetCompileStats().liveResourceCount, 3u);
    EXPECT_EQ(DescribeResource(resources, unused).physicalStableId, 0u);
    EXPECT_EQ(DescribeResource(resources, deadTexture).physicalStableId, 0u);
    EXPECT_EQ(rhi->textureCreations, 0u);

    for (RDGDependency const& edge : graph.GetDependencies())
    {
        EXPECT_TRUE(edge.source != RDG_ID(1) && edge.destination != RDG_ID(1));
    }

    EXPECT_TRUE(graph.GetWarnings().empty());

    for (const uint32_t issue : device->GetRDGMetrics().GetLastSnapshot().issues)
    {
        EXPECT_EQ(issue, 0u);
    }

    device->DestroyBuffer(source);
    device->DestroyBuffer(output);
}

TEST_F(RenderCoreTest, NonOverlappingBuffersReuseStorageAndReplayWithIndependentLogicalContents)
{
    CaptureVersionGraph(device);
    TestBuffer* input  = Buffer();
    TestBuffer* tail   = Buffer();
    TestBuffer* middle = Buffer();
    TestBuffer* output = Buffer();
    std::fill(input->bytes.begin(), input->bytes.end(), uint8_t(17));
    std::fill(tail->bytes.begin(), tail->bytes.end(), uint8_t(29));

    for (const bool reuse : {false, true})
    {
        RenderGraph graph("reuse_buffers");
        ASSERT_TRUE(graph.SetOptimizations(true, reuse));
        ASSERT_TRUE(graph.Begin());

        RDGResourceManager* resources = graph.GetResourceManager();
        // Creation order deliberately differs from lifetime order.
        const RDGBuffer b      = resources->CreateBuffer(LogicalBuffer());
        const RDGBuffer a      = resources->CreateBuffer(LogicalBuffer());
        const RDGBuffer source = resources->ImportHostWrittenBuffer(input);
        const RDGBuffer extra  = resources->ImportHostWrittenBuffer(tail);
        const RDGBuffer bridge = resources->ImportBuffer(middle);
        const RDGBuffer result = resources->ImportBuffer(output);
        graph.AddTransferPass("a_write").CopyBuffer(source, a, {0, 0, 64});
        graph.AddTransferPass("a_read").CopyBuffer(a, bridge, {0, 0, 64});
        graph.AddTransferPass("b_write")
            .CopyBuffer(bridge, b, {0, 0, 32})
            .CopyBuffer(extra, b, {32, 32, 32});
        graph.AddTransferPass("b_read").CopyBuffer(b, result, {0, 0, 64});

        ASSERT_TRUE(graph.End());

        uint64_t id = 0;

        for (uint32_t replay = 0; replay < 3; ++replay)
        {
            device->GetRDGMetrics().RequestCapture(true);

            ASSERT_TRUE(device->ExecuteRenderGraph(graph));

            const uint64_t aId = DescribeResource(resources, a).physicalStableId;
            const uint64_t bId = DescribeResource(resources, b).physicalStableId;
            EXPECT_EQ(aId == bId, reuse);

            if (id)
            {
                EXPECT_EQ(aId, id);
            }

            id = aId;

            EXPECT_EQ(middle->bytes, input->bytes);

            for (uint32_t i = 0; i < 64; ++i)
            {
                EXPECT_EQ(output->bytes[i], i < 32 ? 17 : 29);
            }

            EXPECT_EQ(graph.GetCompileStats().reusedAllocationCount, reuse ? 1u : 0u);
            EXPECT_EQ(resources->GetPoolStats().assignedCount, reuse ? 1u : 2u);
            EXPECT_EQ(resources->GetPoolStats().assignedBytes, reuse ? 64u : 128u);
            EXPECT_TRUE(graph.GetWarnings().empty());

            for (const uint32_t issue : device->GetRDGMetrics().GetLastSnapshot().issues)
            {
                EXPECT_EQ(issue, 0u);
            }
        }

        ASSERT_TRUE(graph.Reset());
        EXPECT_EQ(resources->GetPoolStats().availableCount, reuse ? 1u : 2u);
        ASSERT_TRUE(resources->TrimPool(true));

        rhi->completed = rhi->submitted;
        device->CollectCompletedResources();

        EXPECT_TRUE(destroyed.contains(id));
    }

    for (TestBuffer* buffer : {input, tail, middle, output})
    {
        device->DestroyBuffer(buffer);
    }
}

TEST_F(RenderCoreTest, PoolBudgetEvictsIdleAllocationsThroughBothQueueCompletionGates)
{
    CreateTestShaderProgram(device, "intent");
    RenderGraph graph("bounded_pool");
    RDGResourceManager* resources = graph.GetResourceManager();
    ASSERT_TRUE(resources->SetPoolConfig({64, 120}));

    uint64_t firstId = 0;

    for (const uint64_t size : {64u, 32u, 32u})
    {
        ASSERT_TRUE(graph.Begin());

        RDGBufferDesc desc     = LogicalBuffer();
        desc.size              = size;
        const RDGBuffer buffer = resources->CreateBuffer(desc);

        RDGComputePassDesc pass = IntentPass();
        pass.BindStorageBuffer("write_buffer", buffer, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(pass);

        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));

        const uint64_t id = DescribeResource(resources, buffer).physicalStableId;

        if (size == 64)
        {
            firstId = id;
        }

        // Conservatively protect retirement against both submitted queues.
        ++rhi->submitted[rhi->Index(RHICommandContextType::eTransfer)];

        ASSERT_TRUE(graph.Reset());

        const RDGPoolStats stats = resources->GetPoolStats();
        EXPECT_LE(stats.availableBytes, 64u);
        EXPECT_EQ(stats.assignedCount, 0u);
        EXPECT_FALSE(destroyed.contains(firstId));
    }

    RDGPoolStats stats = resources->GetPoolStats();
    EXPECT_EQ(stats.hits, 1u);
    EXPECT_EQ(stats.misses, 2u);
    EXPECT_EQ(stats.evictions, 1u);
    EXPECT_EQ(stats.availableBytes, 32u);
    EXPECT_EQ(stats.retiringBytes, 64u);
    ASSERT_EQ(resources->GetPoolBuckets().size(), 1u);
    EXPECT_EQ(resources->GetPoolBuckets()[0].bufferSize, 32u);

    rhi->completed[rhi->Index(RHICommandContextType::eGraphics)] =
        rhi->submitted[rhi->Index(RHICommandContextType::eGraphics)];
    device->CollectCompletedResources();

    EXPECT_FALSE(destroyed.contains(firstId));

    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    EXPECT_TRUE(destroyed.contains(firstId));
    EXPECT_EQ(resources->GetPoolStats().retiringBytes, 0u);
    ASSERT_TRUE(resources->SetPoolConfig({64, 0}));
    ASSERT_TRUE(graph.Begin());
    ASSERT_TRUE(graph.Reset());
    EXPECT_EQ(resources->GetPoolStats().availableCount, 0u);
}

TEST_F(RenderCoreTest, TexturePoolAccountsMipsLayersSamplesAndBoundsResizeFamilies)
{
    RenderGraph graph("texture_budget");
    RDGResourceManager* resources = graph.GetResourceManager();
    ASSERT_TRUE(resources->SetPoolConfig({4096, 120}));

    for (uint32_t extent = 8; extent <= 96; extent += 8)
    {
        ASSERT_TRUE(graph.Begin());

        RDGTextureDesc desc  = LogicalTexture(3);
        desc.texFormat.width = desc.texFormat.height = extent;
        const RDGTexture texture                     = resources->CreateTexture(desc);
        graph.AddTransferPass("observable_clear").NeverCull().ClearTexture(texture, Color(0.f));

        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));

        if (extent == 8)
        {
            EXPECT_EQ(resources->GetPoolStats().assignedBytes, 336u);
        }

        ASSERT_TRUE(graph.Reset());

        rhi->completed = rhi->submitted;
        device->CollectCompletedResources();

        EXPECT_LE(resources->GetPoolStats().availableBytes, 4096u);
        EXPECT_LE(resources->GetPoolStats().descriptorCount, 3u);
    }

    ASSERT_TRUE(resources->TrimPool(true));
    EXPECT_TRUE(resources->GetPoolBuckets().empty());
    ASSERT_TRUE(graph.Begin());

    RDGTextureDesc desc        = LogicalTexture();
    desc.texFormat.arrayLayers = 6;
    desc.texFormat.sampleCount = SampleCount::e4;
    RDGTexture texture         = resources->CreateTexture(desc);
    graph.AddTransferPass("account_layers_samples").NeverCull().ClearTexture(texture, Color(0.f));

    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(resources->GetPoolStats().assignedBytes, 8u * 8u * 4u * 6u * 4u);
    ASSERT_TRUE(resources->TrimPool(true));
    EXPECT_NE(DescribeResource(resources, texture).physicalStableId,
              0u); // Active is never trimmed.
}

TEST_F(RenderCoreTest, OverlapDescriptorMismatchAndExtractionPreventStorageReuse)
{
    TestBuffer* input        = Buffer();
    TestBuffer* bridgeBuffer = Buffer();
    TestBuffer* output       = Buffer();
    std::fill(input->bytes.begin(), input->bytes.end(), uint8_t(47));

    for (uint32_t scenario = 0; scenario < 3; ++scenario)
    {
        RenderGraph graph("reuse_exclusions");
        ASSERT_TRUE(graph.Begin());

        RDGResourceManager* resources = graph.GetResourceManager();
        RDGBufferDesc desc            = LogicalBuffer();
        const RDGBuffer a             = resources->CreateBuffer(desc);

        if (scenario == 1)
        {
            desc.usageFlags.SetFlag(RHIBufferUsageFlagBits::eUniformBuffer);
        }

        const RDGBuffer b      = resources->CreateBuffer(desc);
        const RDGBuffer source = resources->ImportHostWrittenBuffer(input);
        const RDGBuffer bridge = resources->ImportBuffer(bridgeBuffer);
        graph.AddTransferPass("a_write").CopyBuffer(source, a, {0, 0, 64});

        if (scenario == 0)
        {
            graph.AddTransferPass("same_pass_overlap").CopyBuffer(a, b, {0, 0, 64});
        }
        else
        {
            graph.AddTransferPass("a_read").CopyBuffer(a, bridge, {0, 0, 64});
            graph.AddTransferPass("b_write").CopyBuffer(bridge, b, {0, 0, 64});
        }

        graph.AddTransferPass("b_read").CopyBuffer(b, resources->ImportBuffer(output), {0, 0, 64});
        RDGExtractedBuffer owner;

        if (scenario == 2)
        {
            owner = resources->QueueBufferExtraction(
                b, BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferSrcBuffer));
        }

        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        EXPECT_NE(DescribeResource(resources, a).physicalStableId,
                  DescribeResource(resources, b).physicalStableId);
        EXPECT_EQ(graph.GetCompileStats().reusedAllocationCount, 0u);
        EXPECT_EQ(resources->GetPoolStats().assignedCount, 2u);
        EXPECT_EQ(output->bytes, input->bytes);
        ASSERT_TRUE(graph.Reset());
        EXPECT_EQ(resources->GetPoolStats().availableCount, scenario == 2 ? 1u : 2u);
        ASSERT_TRUE(resources->TrimPool(true));

        rhi->completed = rhi->submitted;
        device->CollectCompletedResources();

        if (scenario == 2)
        {
            ASSERT_TRUE(owner);
            EXPECT_EQ(static_cast<TestBuffer*>(owner.Get())->bytes, input->bytes);
        }
    }

    for (TestBuffer* buffer : {input, bridgeBuffer, output})
    {
        device->DestroyBuffer(buffer);
    }
}

TEST_F(RenderCoreTest, TextureReusePreservesPhysicalBarriersViewsAndFailureRetirement)
{
    CaptureVersionGraph(device);
    CreateTestShaderProgram(device, "intent");
    RHITexture* bridgeTexture = Texture(2);
    RenderGraph graph("reuse_texture");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    RDGTextureDesc desc           = LogicalTexture(2);
    desc.usageFlags.SetFlags(RHITextureUsageFlagBits::eTransferSrc,
                             RHITextureUsageFlagBits::eTransferDst);
    const RDGTexture b      = resources->CreateTexture(desc);
    const RDGTexture a      = resources->CreateTexture(desc);
    const RDGTexture bridge = resources->ImportTexture(bridgeTexture);
    RHITextureCopyRegion regions[2]{};

    for (uint32_t mip = 0; mip < 2; ++mip)
    {
        RHITextureCopyRegion& region = regions[mip];
        region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.srcSubresources.mipmap = region.dstSubresources.mipmap = mip;
        region.size = {int32_t(8 >> mip), int32_t(8 >> mip), 1};
    }

    RHITextureSubResourceRange range = RHITextureSubResourceRange::Color();
    range.baseMipLevel               = 1;
    graph.AddTransferPass("a_write").ClearTexture(a, Color(0.f));

    RDGComputePassDesc readA = IntentPass("a_view");
    readA.BindSampledTexture("texture", nullptr, a, RDGTextureViewDesc{range});
    graph.AddComputePass(readA);
    graph.AddTransferPass("a_read").CopyTexture(a, bridge, MakeVecView(regions, 2));
    graph.AddTransferPass("b_write").CopyTexture(bridge, b, MakeVecView(regions, 2));

    RDGComputePassDesc readB = IntentPass("b_view");
    readB.BindSampledTexture("texture", nullptr, b, RDGTextureViewDesc{range});
    bool fail = false;
    graph.AddComputePass(readB).RecordPassCommands([&](RDGPassCmdEncoder& encoder) {
        if (fail)
        {
            encoder.Fail(RDGErrorCode::eCallback, "reject reused texture replay");
        }
    });

    ASSERT_TRUE(graph.End());

    uint64_t id       = 0;
    RHIResource* view = nullptr;

    for (uint32_t replay = 0; replay < 2; ++replay)
    {
        rhi->graphics.boundResources.clear();
        rhi->graphics.barrierBatches.clear();

        ASSERT_TRUE(device->ExecuteRenderGraph(graph));

        id = DescribeResource(resources, a).physicalStableId;

        EXPECT_EQ(id, DescribeResource(resources, b).physicalStableId);
        EXPECT_EQ(graph.GetCompileStats().reusedAllocationCount, 1u);
        ASSERT_EQ(rhi->graphics.boundResources.size(), 2u);
        EXPECT_EQ(rhi->graphics.boundResources[0], rhi->graphics.boundResources[1]);

        if (view)
        {
            EXPECT_EQ(rhi->graphics.boundResources[0], view);
        }

        view              = rhi->graphics.boundResources[0];
        bool reuseBarrier = false;

        for (TestContext::BarrierBatch const& batch : rhi->graphics.barrierBatches)
        {
            for (const RHITextureTransition& transition : batch.textures)
            {
                reuseBarrier |= transition.pTexture->GetStableId() == id &&
                    transition.oldUsage == RHITextureUsage::eTransferSrc &&
                    transition.newUsage == RHITextureUsage::eTransferDst;
            }
        }

        EXPECT_TRUE(reuseBarrier);
        EXPECT_TRUE(graph.GetWarnings().empty());

        const std::array<uint32_t, static_cast<size_t>(RDGMetricIssue::eCount)>& issues =
            device->GetRDGMetrics().GetLastSnapshot().issues;

        for (size_t i = 0; i < issues.size(); ++i)
        {
            if (i != size_t(RDGMetricIssue::eBroadTextureRange))
            {
                EXPECT_EQ(issues[i], 0u);
            }
        }
    }

    const std::array<uint64_t, 3> submitted = rhi->submitted;
    fail                                    = true;

    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eCallback);
    EXPECT_EQ(rhi->submitted, submitted);
    EXPECT_EQ(resources->GetPoolStats().availableCount, 1u);
    EXPECT_EQ(resources->GetPoolStats().assignedCount, 0u);
    ASSERT_TRUE(graph.Reset());
    ASSERT_TRUE(resources->TrimPool(true));

    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    EXPECT_TRUE(destroyed.contains(id));

    device->DestroyTexture(bridgeTexture);
}

TEST_F(RenderCoreTest,
       NewLogicalOccupantsCannotInheritInitializationAndGraphsKeepOneExecutionTracker)
{
    TestBuffer* input  = Buffer();
    TestBuffer* output = Buffer();
    RenderGraph graph("logical_contents_and_tracker");
    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();
    RDGBuffer value               = resources->CreateBuffer(LogicalBuffer());
    graph.AddTransferPass("initialize")
        .CopyBuffer(resources->ImportHostWrittenBuffer(input), value, {0, 0, 64});
    graph.AddTransferPass("read").CopyBuffer(value, resources->ImportBuffer(output), {0, 0, 64});

    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    ASSERT_TRUE(graph.Reset());
    EXPECT_EQ(resources->GetPoolStats().availableCount, 1u);
    ASSERT_TRUE(graph.Begin());

    value = resources->CreateBuffer(LogicalBuffer());
    graph.AddTransferPass("uninitialized_read")
        .CopyBuffer(value, resources->ImportBuffer(output), {0, 0, 64});

    ASSERT_TRUE(graph.End());

    const std::array<uint64_t, 3> submitted = rhi->submitted;
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eUninitialized);
    EXPECT_EQ(rhi->submitted, submitted);
    ASSERT_TRUE(graph.Reset());
    ASSERT_TRUE(graph.Begin());

    value = resources->CreateBuffer(LogicalBuffer());
    graph.AddTransferPass("initialize")
        .CopyBuffer(resources->ImportHostWrittenBuffer(input), value, {0, 0, 64});
    graph.AddTransferPass("read").CopyBuffer(value, resources->ImportBuffer(output), {0, 0, 64});

    ASSERT_TRUE(graph.End());

    RDGExecutor foreign(device);
    RHICommandList list;
    EXPECT_FALSE(foreign.Execute(&graph, &list));
    EXPECT_EQ(list.GetCommandCount(), 0u);
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eLifecycle);
    EXPECT_EQ(rhi->submitted, submitted);

    device->DestroyBuffer(input);
    device->DestroyBuffer(output);
}

TEST_F(RenderCoreTest, ShuffledLiveChainsMatchByteOracleWithCullingAndReuseEnabledOrDisabled)
{
    constexpr uint32_t steps = 12;
    TestBuffer* input        = Buffer();
    TestBuffer* bridgeBuffer = Buffer();
    std::fill(input->bytes.begin(), input->bytes.end(), uint8_t(73));
    std::array<TestBuffer*, steps> outputs;

    for (TestBuffer*& output : outputs)
    {
        output = Buffer();
    }

    for (uint32_t seed = 0; seed < 16; ++seed)
    {
        for (const bool optimize : {false, true})
        {
            SCOPED_TRACE(seed);
            SCOPED_TRACE(optimize);
            std::fill(bridgeBuffer->bytes.begin(), bridgeBuffer->bytes.end(), uint8_t(19));
            RenderGraph graph("shuffled_lifetimes");
            ASSERT_TRUE(graph.SetOptimizations(optimize, optimize));
            ASSERT_TRUE(graph.Begin());

            RDGResourceManager* resources = graph.GetResourceManager();
            const RDGBuffer source        = resources->ImportHostWrittenBuffer(input);
            RDGBuffer previous =
                resources->InitialVersion(resources->ImportHostWrittenBuffer(bridgeBuffer));
            std::array<RDGBuffer, steps> scratch, bridge, predecessors;

            for (uint32_t step = 0; step < steps; ++step)
            {
                scratch[step] = resources->CreateVersion(resources->CreateBuffer(LogicalBuffer()));
                predecessors[step] = previous;
                bridge[step]       = resources->CreateVersion(previous);
                previous           = bridge[step];
            }

            std::vector<uint32_t> declarations(3 * steps);
            std::iota(declarations.begin(), declarations.end(), 0u);
            std::mt19937 random(seed);
            std::shuffle(declarations.begin(), declarations.end(), random);

            for (uint32_t const declaration : declarations)
            {
                const uint32_t step = declaration / 3;

                if (declaration % 3 == 0)
                {
                    graph.AddTransferPass("produce")
                        .CopyBuffer(predecessors[step], scratch[step], {0, 0, 32})
                        .CopyBuffer(source, scratch[step], {32, 32, 32});
                }
                else if (declaration % 3 == 1)
                {
                    graph.AddTransferPass("consume")
                        .CopyBuffer(scratch[step], bridge[step], {0, 0, 64})
                        .CopyBuffer(scratch[step], resources->ImportBuffer(outputs[step]),
                                    {0, 0, 64});
                }
                else
                {
                    graph.AddTransferPass("dead").CopyBuffer(
                        source, resources->CreateBuffer(LogicalBuffer()), {0, 0, 64});
                }
            }

            ASSERT_TRUE(graph.End());

            for (uint32_t replay = 0; replay < 2; ++replay)
            {
                ASSERT_TRUE(device->ExecuteRenderGraph(graph));

                for (const TestBuffer* output : outputs)
                {
                    for (uint32_t i = 0; i < 64; ++i)
                    {
                        EXPECT_EQ(output->bytes[i], i < 32 ? 19 : 73);
                    }
                }

                EXPECT_EQ(graph.GetCompileStats().culledPassCount, optimize ? steps : 0u);
                EXPECT_EQ(resources->GetPoolStats().assignedCount, optimize ? 1u : 2u * steps);
                EXPECT_TRUE(graph.GetWarnings().empty());
            }
        }
    }

    for (TestBuffer* output : outputs)
    {
        device->DestroyBuffer(output);
    }

    device->DestroyBuffer(input);
    device->DestroyBuffer(bridgeBuffer);
}

class RDGPoolFrameCountTest : public RenderCoreTest, public testing::WithParamInterface<uint32_t>
{
    void SetUp() override
    {
        InitializeDevice(nullptr, GetParam());
    }
};

TEST_P(RDGPoolFrameCountTest, ZeroBudgetRetirementProtectsEveryFrameSlot)
{
    CreateTestShaderProgram(device, "intent");
    RenderGraph graph("pool_frame_slots");
    RDGResourceManager* resources = graph.GetResourceManager();
    ASSERT_TRUE(resources->SetPoolConfig({0, 0}));

    std::vector<uint64_t> ids;

    for (uint32_t frame = 0; frame < GetParam(); ++frame)
    {
        ASSERT_TRUE(graph.Begin());

        const RDGTexture texture = resources->CreateTexture(LogicalTexture());
        graph.AddTransferPass("keep").NeverCull().ClearTexture(texture, Color(0.f));
        // Keep execution on graphics, so dedicated-transfer fallback waits do not complete
        // the deliberately outstanding serials whose retirement gates this test exercises.
        graph.AddComputePass(IntentPass("graphics_queue"));

        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));

        ids.push_back(DescribeResource(resources, texture).physicalStableId);
        ++rhi->submitted[rhi->Index(RHICommandContextType::eTransfer)];

        ASSERT_TRUE(graph.Reset());
        EXPECT_EQ(resources->GetPoolStats().availableBytes, 0u);

        if (frame + 1 < GetParam())
        {
            device->NextFrame(); // Stop before wrapping into an occupied slot and waiting for it.
        }
    }

    device->CollectCompletedResources();

    for (uint64_t id : ids)
    {
        EXPECT_FALSE(destroyed.contains(id));
    }

    rhi->completed[rhi->Index(RHICommandContextType::eGraphics)] =
        rhi->submitted[rhi->Index(RHICommandContextType::eGraphics)];
    device->CollectCompletedResources();

    for (uint64_t id : ids)
    {
        EXPECT_FALSE(destroyed.contains(id));
    }

    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    for (uint64_t id : ids)
    {
        EXPECT_TRUE(destroyed.contains(id));
    }
}

INSTANTIATE_TEST_SUITE_P(FramesInFlight, RDGPoolFrameCountTest, testing::Values(2u, 3u, 4u));

TEST_P(RDGPoolFrameCountTest, DISABLED_FrameSlotWaitScopeBenchmark)
{
    for (uint32_t frame = 0; frame < GetParam(); ++frame)
    {
        rhi->submitted = {uint64_t(frame + 1), 100, uint64_t(frame + 1)};
        device->NextFrame();
    }

    std::cout << "frame_wait_scope frames=" << GetParam() << " device_idle=" << rhi->deviceIdleWaits
              << " completed_graphics=" << rhi->completed[0]
              << " completed_compute=" << rhi->completed[1]
              << " completed_transfer=" << rhi->completed[2] << '\n';
}

TEST_P(RDGPoolFrameCountTest, FrameSlotWaitLeavesNewerSubmissionsAndComputePending)
{
    TestBuffer* oldest      = Buffer();
    TestBuffer* newer       = Buffer();
    const uint64_t oldestId = oldest->GetStableId();
    const uint64_t newerId  = newer->GetStableId();

    for (uint32_t frame = 0; frame < GetParam(); ++frame)
    {
        rhi->submitted = {uint64_t(frame + 1), 100, uint64_t(frame + 1)};

        if (frame == 0)
        {
            device->DestroyBuffer(oldest);
        }

        if (frame == 1)
        {
            device->DestroyBuffer(newer);
        }

        device->NextFrame();
    }

    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
    EXPECT_EQ(rhi->completed, (std::array<uint64_t, 3>{1, 0, 1}));
    ASSERT_EQ(rhi->submissionWaits.size(), 2u);
    EXPECT_EQ(rhi->submissionWaits[0], (std::pair{RHICommandContextType::eGraphics, uint64_t(1)}));
    EXPECT_EQ(rhi->submissionWaits[1], (std::pair{RHICommandContextType::eTransfer, uint64_t(1)}));
    EXPECT_TRUE(destroyed.contains(oldestId));
    EXPECT_FALSE(destroyed.contains(newerId));
}

TEST_P(RDGPoolFrameCountTest, SharedQueueWaitUsesTheOldestSlotsHighestSerial)
{
    rhi->shared             = true;
    constexpr uint64_t base = uint64_t(1) << 32;

    for (uint32_t frame = 0; frame < GetParam(); ++frame)
    {
        rhi->submitted[0] = base + frame + 1;
        device->NextFrame();
    }

    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
    EXPECT_EQ(rhi->completed[0], base + 1);
    ASSERT_EQ(rhi->submissionWaits.size(), 1u);
    EXPECT_EQ(rhi->submissionWaits[0].second, base + 1);
}

TEST_P(RDGPoolFrameCountTest, FailedFrameWaitDoesNotBeginOrRetireTheSlot)
{
    TestBuffer* buffer = Buffer();
    const uint64_t id  = buffer->GetStableId();
    rhi->submitted     = {1, 0, 1};
    device->DestroyBuffer(buffer);
    rhi->failSubmissionWait = true;

    for (uint32_t frame = 0; frame < GetParam(); ++frame)
    {
        device->NextFrame();
    }

    EXPECT_EQ(rhi->frameBegins, GetParam());
    EXPECT_FALSE(destroyed.contains(id));
    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
    EXPECT_EQ(rhi->completed, (std::array<uint64_t, 3>{}));

    RenderGraph graph("blocked_frame");
    ASSERT_TRUE(graph.Begin());
    ASSERT_TRUE(graph.End());
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(rhi->finalizedLists, 0u);

    rhi->failSubmissionWait     = false;
    const RenderFrameSlot frame = GRenderFrameState.GetFrameSlot();
    device->NextFrame();

    EXPECT_EQ(GRenderFrameState.GetFrameSlot(), frame);
    EXPECT_EQ(rhi->frameBegins, GetParam() + 1);
    EXPECT_TRUE(destroyed.contains(id));
    EXPECT_TRUE(device->ExecuteRenderGraph(graph));
}

TEST_F(RenderCoreTest, StagingWaitsForItsOwnBlocksAndKeepsUnsubmittedAllocations)
{
    StagingBufferManager manager(16, 32);
    StagingAllocation submitted, unsubmitted, reuse;
    ASSERT_EQ(manager.Allocate(16, 4, &submitted), StagingFlushAction::eNone);
    ASSERT_EQ(manager.Allocate(16, 4, &unsubmitted), StagingFlushAction::eNone);
    manager.Release(submitted, {3, 2});
    rhi->submitted = {10, 100, 20};
    ASSERT_TRUE(device->ResolveStagingFlushAction(StagingFlushAction::eFlush, &manager));
    EXPECT_EQ(rhi->completed, (std::array<uint64_t, 3>{2, 0, 3}));
    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
    ASSERT_EQ(manager.Allocate(16, 4, &reuse), StagingFlushAction::eNone);
    EXPECT_EQ(reuse.pBuffer, submitted.pBuffer);
    StagingAllocation blocked;
    EXPECT_EQ(manager.Allocate(16, 4, &blocked), StagingFlushAction::eFlush);
    manager.Release(unsubmitted, {});
    manager.Release(reuse, {});
    manager.Destroy();
}

TEST_F(RenderCoreTest, FailedStagingWaitDoesNotReuseTheBlock)
{
    StagingBufferManager manager(16, 16);
    StagingAllocation allocation, blocked;
    ASSERT_EQ(manager.Allocate(16, 4, &allocation), StagingFlushAction::eNone);
    manager.Release(allocation, {1, 1});
    rhi->submitted          = {3, 0, 3};
    rhi->failSubmissionWait = true;
    EXPECT_FALSE(device->ResolveStagingFlushAction(StagingFlushAction::eFlush, &manager));
    EXPECT_EQ(manager.Allocate(16, 4, &blocked), StagingFlushAction::eFlush);
    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
    rhi->failSubmissionWait = false;
    ASSERT_TRUE(device->ResolveStagingFlushAction(StagingFlushAction::eFlush, &manager));
    EXPECT_EQ(manager.Allocate(16, 4, &blocked), StagingFlushAction::eNone);
    EXPECT_EQ(blocked.pBuffer, allocation.pBuffer);
    manager.Release(blocked, {});
    manager.Destroy();
}

TEST_F(RenderCoreTest, SharedQueueUploadPressureWaitsWithoutIdlingTheDevice)
{
    rhi->shared = true;
    StagingBufferManager manager(16, 16);
    StagingUploadQueue queue(device, &manager);
    TestBuffer* buffer = Buffer();
    std::array<uint8_t, 40> bytes{};

    for (uint32_t i = 0; i < bytes.size(); ++i)
    {
        bytes[i] = uint8_t(i + 1);
    }

    queue.EnqueueBuffer(buffer, 0, bytes.size(), bytes.data());
    queue.Flush();

    EXPECT_FALSE(queue.HasPending());
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), buffer->bytes.begin()));
    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
    EXPECT_EQ(rhi->submitted[0], 3u);
    EXPECT_EQ(rhi->completed[0], 2u);
    ASSERT_EQ(rhi->submissionWaits.size(), 2u);
    EXPECT_EQ(rhi->submissionWaits[0].second, 1u);
    EXPECT_EQ(rhi->submissionWaits[1].second, 2u);

    queue.Destroy();
    manager.Destroy();
    device->DestroyBuffer(buffer);
}

TEST_F(RenderCoreTest, ImportedShaderWritesStayLiveAndResizeDropsTheFrameGraphsIdlePool)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer* output            = Buffer();
    RenderGraph* graph            = device->GetCurrentFrameRDG();
    RDGResourceManager* resources = graph->GetResourceManager();
    ASSERT_TRUE(graph->Begin());

    const RDGTexture texture = resources->CreateTexture(LogicalTexture());
    graph->AddTransferPass("keep_texture").NeverCull().ClearTexture(texture, Color(0.f));

    RDGComputePassDesc write = IntentPass("observable_imported_write");
    write.allowCulling       = true;
    write.BindStorageBuffer("write_buffer", resources->ImportBuffer(output),
                            RDGContentGuarantee::eFullWrite);
    bool executed = false;
    graph->AddComputePass(write).RecordPassCommands([&](RDGPassCmdEncoder&) { executed = true; });

    ASSERT_TRUE(graph->End());
    ASSERT_TRUE(device->ExecuteRenderGraph(*graph));
    EXPECT_TRUE(executed);
    EXPECT_EQ(graph->GetCompileStats().culledPassCount, 0u);

    const uint64_t id = DescribeResource(resources, texture).physicalStableId;
    EXPECT_EQ(resources->GetPoolStats().assignedCount, 1u);

    device->InvalidateRDGPassCompilerForResize();

    EXPECT_EQ(resources->GetPoolStats().assignedCount, 0u);
    EXPECT_EQ(resources->GetPoolStats().availableCount, 0u);
    EXPECT_TRUE(resources->GetPoolBuckets().empty());

    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();

    EXPECT_TRUE(destroyed.contains(id));

    device->DestroyBuffer(output);
}

// Fixed end-to-end CPU/mock baseline; GPU work and presentation are not measured here.
TEST_F(RenderCoreTest, DISABLED_ExecutionPreparationBenchmark)
{
    using Clock = std::chrono::steady_clock;
    CreateTestShaderProgram(device, "readers");

    for (bool validate : {false, true})
    {
        RDGMetricsOptions options   = device->GetRDGMetrics().GetOptions();
        options.logging.enabled     = true;
        options.logging.sampleEvery = 1;
        options.logging.minInterval = std::chrono::milliseconds(0);
        options.validate            = validate;
        device->GetRDGMetrics().Configure(options);
        device->GetRDGMetrics().SetSink({});

        for (uint32_t count : {64u, 512u, 2048u})
        {
            TestBuffer* source = Buffer();
            RenderGraph graph("execution_preparation_benchmark");

            for (bool replay : {false, true})
            {
                std::vector<double> elapsed;

                for (uint32_t run = 0; run < 9; ++run)
                {
                    if (!replay || run == 0)
                    {
                        ASSERT_TRUE(graph.Begin());
                        graph.GetResourceManager()->ImportBuffer(source,
                                                                 RDGImportContents::eDefined);

                        for (uint32_t i = 0; i < count; ++i)
                        {
                            RDGGraphicsPassDesc pass;
                            pass.SetShaderProgramName("readers");
                            pass.BindVertexBuffer(source);
                            graph.AddGraphicsPass(std::move(pass));
                        }

                        ASSERT_TRUE(graph.End());
                    }

                    const std::chrono::steady_clock::time_point start = Clock::now();
                    ASSERT_TRUE(device->ExecuteRenderGraph(graph));

                    const std::chrono::steady_clock::time_point end = Clock::now();

                    if (run != 0)
                    {
                        elapsed.push_back(
                            std::chrono::duration<double, std::micro>(end - start).count());
                    }

                    rhi->completed = rhi->submitted;
                }

                std::sort(elapsed.begin(), elapsed.end());
                std::printf("RDG_EXEC_BENCH validate=%d replay=%d n=%u execute_us=%.2f\n", validate,
                            replay, count, elapsed[elapsed.size() / 2]);
            }

            device->DestroyBuffer(source);
            device->CollectCompletedResources();
        }
    }
}

namespace zen::rc
{
// Exercise invalidation boundaries without exposing execution plans to renderer callers.
struct RDGExecutionPlanTestAccess
{
    using Plan = RDGExecutor::ExecutionPlan;

    static bool Prepare(RDGExecutor& executor, RenderGraph& graph, Plan& plan)
    {
        return executor.PrepareExecution(&graph, plan);
    }

    static bool Refresh(RDGExecutor& executor, Plan& plan)
    {
        return executor.RefreshExecution(plan);
    }

    static bool Execute(RDGExecutor& executor, Plan& plan, RHICommandList& commands)
    {
        return executor.ExecutePrepared(plan, &commands);
    }

    static RDGGraphicsPass* Graphics(RenderGraph& graph)
    {
        return graph.m_compiledGfxPasses.empty() ? nullptr : graph.m_compiledGfxPasses[0];
    }

    static RDGComputePass* Compute(RenderGraph& graph)
    {
        return graph.m_compiledComputePasses.empty() ? nullptr : graph.m_compiledComputePasses[0];
    }

    static size_t IdleCount(const RenderGraph& graph)
    {
        return graph.m_idleGfxPasses.size() + graph.m_idleComputePasses.size();
    }

    static size_t IdleBytes(const RenderGraph& graph)
    {
        return graph.m_idlePassBytes;
    }

    template <typename Pass> static void CheckIdlePass(Pass* pass, size_t& bytes)
    {
        EXPECT_EQ(pass->pPipeline, nullptr);
        EXPECT_TRUE(pass->passTag.ToString().empty());
        EXPECT_FALSE(pass->shaderParameters.HasAnyParameter());
        EXPECT_TRUE(pass->indirectBindings.empty());
        bytes += pass->GetStorageBytes();
    }

    static void CheckIdle(const RenderGraph& graph)
    {
        size_t bytes = 0;

        for (RDGGraphicsPass* pass : graph.m_idleGfxPasses)
        {
            CheckIdlePass(pass, bytes);
            EXPECT_EQ(pass->pRenderingLayout, nullptr);
            EXPECT_TRUE(pass->geometryBuffer.vertexBuffers.empty());
            EXPECT_EQ(pass->geometryBuffer.pIndexBuffer, nullptr);
            EXPECT_EQ(pass->geometryBuffer.indexBufferFormat, DataFormat::eR32UInt);
            EXPECT_EQ(pass->geometryBuffer.indexBufferOffset, 0u);
        }

        for (RDGComputePass* pass : graph.m_idleComputePasses)
        {
            CheckIdlePass(pass, bytes);
        }

        EXPECT_EQ(bytes, IdleBytes(graph));
        EXPECT_LE(bytes, graph.cMaxIdlePassBytes);
        EXPECT_LE(IdleCount(graph), graph.cMaxIdlePassCount);
    }
};
} // namespace zen::rc

TEST_F(RenderCoreTest, DeviceExecutionPreparesOnceForFirstBuildReplayAndViewport)
{
    CreateTestShaderProgram(device, "intent");
    CaptureVersionGraph(device);
    RenderGraph graph("single_preparation");
    ASSERT_TRUE(graph.Begin());

    graph.AddComputePass(IntentPass());

    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(device->GetRDGMetrics().GetLastSnapshot().preparationPasses, 1u);
    EXPECT_FALSE(device->GetRDGMetrics().GetLastSnapshot().precompiled);
    EXPECT_GT(device->GetRDGMetrics().GetLastSnapshot().compileCPUUs, 0);
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(device->GetRDGMetrics().GetLastSnapshot().preparationPasses, 1u);
    EXPECT_TRUE(device->GetRDGMetrics().GetLastSnapshot().precompiled);

    TestViewport viewport;
    RenderGraph* frame = device->GetCurrentFrameRDG();
    ASSERT_TRUE(frame->Begin());

    frame->AddComputePass(IntentPass());

    ASSERT_TRUE(frame->End());
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    EXPECT_EQ(device->GetRDGMetrics().GetLastSnapshot().preparationPasses, 1u);
    EXPECT_FALSE(device->GetRDGMetrics().GetLastSnapshot().precompiled);
}

TEST_F(RenderCoreTest, UploadFlushRefreshesPlanContentsBarriersAndQueueChoice)
{
    CreateTestShaderProgram(device, "intent");
    CaptureVersionGraph(device);

    for (bool priorShaderRead : {false, true})
    {
        TestBuffer* source = Buffer();
        TestBuffer* output = Buffer();

        if (priorShaderRead)
        {
            RenderGraph reader("prior_reader");
            ASSERT_TRUE(reader.Begin());
            RDGBuffer logical =
                reader.GetResourceManager()->ImportBuffer(source, RDGImportContents::eDefined);

            RDGComputePassDesc pass = IntentPass();
            pass.BindStorageBuffer("read_buffer", logical);
            reader.AddComputePass(pass);
            ASSERT_TRUE(reader.End());
            ASSERT_TRUE(device->ExecuteRenderGraph(reader));
        }

        std::array<uint8_t, 64> bytes{};
        std::iota(bytes.begin(), bytes.end(), uint8_t(11));
        device->UpdateBuffer(source, bytes.size(), bytes.data());
        RenderGraph copy("after_upload");
        ASSERT_TRUE(copy.Begin());

        copy.AddTransferPass("copy").CopyBuffer(source, output, {0, 0, 64});

        ASSERT_TRUE(copy.End());

        const size_t transferCopies = rhi->transfer.bufferCopies.size();
        const size_t graphicsCopies = rhi->graphics.bufferCopies.size();
        ASSERT_TRUE(device->ExecuteRenderGraph(copy));
        EXPECT_EQ(device->GetRDGMetrics().GetLastSnapshot().preparationPasses, 2u);
        EXPECT_FALSE(device->GetRDGMetrics().GetLastSnapshot().precompiled);
        EXPECT_TRUE(copy.GetWarnings().empty());
        EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), output->bytes.begin()));
        // A prior compute read puts the upload on graphics. Its final transfer state permits
        // the consumer on transfer; using the pre-upload queue choice would be stale.
        EXPECT_EQ(rhi->graphics.bufferCopies.size() - graphicsCopies, priorShaderRead ? 1u : 0u);
        EXPECT_EQ(rhi->transfer.bufferCopies.size() - transferCopies, priorShaderRead ? 1u : 2u);

        bool hasTransferReadBarrier = false;

        for (const TestContext::BarrierBatch& batch : rhi->transfer.barrierBatches)
        {
            for (const RHIBufferTransition& transition : batch.buffers)
            {
                if (transition.pBuffer == source &&
                    transition.oldUsage == RHIBufferUsage::eTransferDst &&
                    transition.newUsage == RHIBufferUsage::eTransferSrc)
                {
                    hasTransferReadBarrier = true;
                    break;
                }
            }

            if (hasTransferReadBarrier)
            {
                break;
            }
        }

        EXPECT_TRUE(hasTransferReadBarrier);

        device->DestroyBuffer(source);
        device->DestroyBuffer(output);
    }
}

TEST_F(RenderCoreTest, ConsumedResetForeignAndUnrefreshedPlansRecordNoCommands)
{
    using Access = RDGExecutionPlanTestAccess;

    for (int scenario = 0; scenario < 4; ++scenario)
    {
        RDGExecutor executor(device), other(device);
        RenderGraph graph("invalid_plan");
        ASSERT_TRUE(graph.Begin());
        ASSERT_TRUE(graph.End());

        Access::Plan plan;
        ASSERT_TRUE(Access::Prepare(executor, graph, plan));

        RHICommandList commands;

        if (scenario == 0)
        {
            ASSERT_TRUE(Access::Execute(executor, plan, commands));
        }
        else if (scenario == 1)
        {
            ASSERT_TRUE(graph.Begin());
            ASSERT_TRUE(graph.End());
        }
        else if (scenario == 3)
        {
            TestBuffer* resource = Buffer();
            executor.GetResourceStateTracker().SetContents(resource, {});
            device->DestroyBuffer(resource);
        }

        const uint32_t count = commands.GetCommandCount();
        EXPECT_FALSE(Access::Execute(scenario == 2 ? other : executor, plan, commands));
        EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eLifecycle);
        EXPECT_EQ(commands.GetCommandCount(), count);
    }
}

TEST_F(RenderCoreTest, PlanRefreshReconcilesForeignPreparationAndContentInvalidation)
{
    using Access       = RDGExecutionPlanTestAccess;
    TestBuffer* source = Buffer();
    TestBuffer* output = Buffer();
    RDGExecutor executor(device), other(device);
    RenderGraph graph("foreign_preparation");
    ASSERT_TRUE(graph.Begin());

    graph.AddTransferPass("copy").CopyBuffer(source, output, {0, 0, 64});

    ASSERT_TRUE(graph.End());

    ResourceStateTracker& tracker = executor.GetResourceStateTracker();
    RDGResourceContent defined;
    defined.status = RDGContentStatus::eDefined;
    tracker.SetContents(source, defined);
    Access::Plan plan;
    ASSERT_TRUE(Access::Prepare(executor, graph, plan));
    EXPECT_TRUE(plan.transfer);
    ASSERT_TRUE(other.Prepare(&graph));
    ASSERT_TRUE(Access::Refresh(executor, plan));
    EXPECT_EQ(plan.preparationPasses, 2u);

    tracker.UpdateBufferState(
        source, RHIAccessMode::eReadWrite,
        BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eStorageBuffer),
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eComputeShader));
    tracker.SetContents(source, defined);

    ASSERT_TRUE(Access::Refresh(executor, plan));
    EXPECT_FALSE(plan.transfer);
    EXPECT_EQ(plan.preparationPasses, 3u);

    RHICommandList commands;
    ASSERT_TRUE(Access::Execute(executor, plan, commands));

    commands.Reset();
    Access::Plan next;
    ASSERT_TRUE(Access::Prepare(executor, graph, next));

    RDGResourceContent undefined;
    undefined.status = RDGContentStatus::eUndefined;
    tracker.SetContents(source, undefined);

    EXPECT_FALSE(Access::Refresh(executor, next));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eUninitialized);
    EXPECT_TRUE(next.consumed);

    device->DestroyBuffer(source);
    device->DestroyBuffer(output);
}

TEST_F(RenderCoreTest, ShaderReplacementAfterPlanPreparationRejectsWithoutPublishingExtraction)
{
    using Access = RDGExecutionPlanTestAccess;

    for (bool reinitialize : {false, true})
    {
        ShaderProgram* shader = CreateTestShaderProgram(device, "intent");
        RDGExecutor executor(device);
        RenderGraph graph("plan_shader_reload");
        ASSERT_TRUE(graph.Begin());

        RDGBuffer buffer   = graph.GetResourceManager()->CreateBuffer(LogicalBuffer());
        TestBuffer* source = Buffer();
        RDGBuffer input    = graph.GetResourceManager()->ImportHostWrittenBuffer(source);
        graph.AddTransferPass("fill").CopyBuffer(input, buffer, {0, 0, 64});
        device->DestroyBuffer(source);
        graph.AddComputePass(IntentPass());
        RDGExtractedBuffer extracted = graph.GetResourceManager()->QueueBufferExtraction(buffer);
        ASSERT_TRUE(graph.End());

        Access::Plan plan;
        ASSERT_TRUE(Access::Prepare(executor, graph, plan));

        if (reinitialize)
        {
            ASSERT_TRUE(shader->Init());
        }
        else
        {
            CreateTestShaderProgram(device, "intent");
        }

        RHICommandList commands;
        EXPECT_FALSE(Access::Execute(executor, plan, commands));
        EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eShader);
        EXPECT_EQ(commands.GetCommandCount(), 0u);
        EXPECT_FALSE(extracted);
        EXPECT_TRUE(plan.consumed);
    }
}

TEST_F(RenderCoreTest, TrackerAssignmentCannotReuseAPlanWithACollidingRevision)
{
    using Access       = RDGExecutionPlanTestAccess;
    TestBuffer* source = Buffer();

    for (bool move : {false, true})
    {
        RDGExecutor executor(device);
        ResourceStateTracker& tracker = executor.GetResourceStateTracker();
        RDGResourceContent defined, undefined;
        defined.status   = RDGContentStatus::eDefined;
        undefined.status = RDGContentStatus::eUndefined;
        tracker.SetContents(source, defined);
        ResourceStateTracker replacement;
        replacement.SetContents(source, undefined);

        ASSERT_EQ(tracker.GetRevision(), replacement.GetRevision());

        RenderGraph graph("assigned_tracker");
        ASSERT_TRUE(graph.Begin());
        ASSERT_TRUE(graph.End());

        Access::Plan plan;
        ASSERT_TRUE(Access::Prepare(executor, graph, plan));

        if (move)
        {
            tracker = std::move(replacement);
        }
        else
        {
            tracker = replacement;
        }

        RHICommandList commands;
        EXPECT_FALSE(Access::Execute(executor, plan, commands));
        EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eLifecycle);
        EXPECT_EQ(commands.GetCommandCount(), 0u);
        EXPECT_EQ(tracker.GetContents(source).status, RDGContentStatus::eUndefined);
    }

    device->DestroyBuffer(source);
}

TEST_F(RenderCoreTest, CompiledPassStorageRebuildsBindingsAndClearsFailedRecordings)
{
    using Access                = RDGExecutionPlanTestAccess;
    ShaderProgram* firstShader  = CreateTestShaderProgram(device, "storage_first");
    ShaderProgram* secondShader = CreateTestShaderProgram(device, "storage_second");
    RHIBuffer* buffers[]        = {Buffer(), Buffer()};
    RHITexture* textures[]      = {Texture(), Texture()};
    RenderGraph graph("storage_rebuild");
    RDGGraphicsPass* firstGraphics = nullptr;
    RDGComputePass* firstCompute   = nullptr;

    for (uint32_t run = 0; run < 5; ++run)
    {
        ASSERT_TRUE(graph.Begin());

        Access::CheckIdle(graph);

        if (run != 0)
        {
            EXPECT_EQ(Access::IdleCount(graph), 2u);
        }

        const bool empty  = run == 3;
        const bool fail   = run == 2;
        RHIBuffer* buffer = buffers[run % 2];
        RDGBuffer logical =
            graph.GetResourceManager()->ImportBuffer(buffer, RDGImportContents::eDefined);

        for (RHITexture* texture : textures)
        {
            graph.GetResourceManager()->ImportTexture(texture, RDGImportContents::eDefined);
        }

        const NameID shader = run % 2 ? "storage_second" : "storage_first";
        const size_t count  = empty ? 0 : (run == 0 ? 32 : 1);
        std::vector<RHITextureView*> views(count, textures[run % 2]->GetDefaultView());
        std::vector<uint32_t> values(run == 0 ? 128 : 1, 0x12340000u + run);

        RDGGraphicsPassDesc graphics;
        graphics.SetShaderProgramName(shader);

        RDGComputePassDesc compute;
        compute.SetShaderProgramName(shader);

        if (!empty)
        {
            graphics.BindValue("value", values.data(), uint32_t(values.size() * sizeof(uint32_t)));
            graphics.BindSampledTexture("uTextureArray", nullptr,
                                        MakeVecView(views.data(), views.size()));

            if (run % 2 == 0)
            {
                graphics.BindVertexBuffer(logical);
                graphics.BindIndexBuffer(logical, DataFormat::eR16UInt, 4);
            }
            else
            {
                graphics.BindVertexBuffer(buffer);
                graphics.BindIndexBuffer(buffer, DataFormat::eR32UInt, 8);
            }

            compute.BindStorageBuffer("read_buffer", logical);
            compute.UseIndirectBuffer(logical);
        }

        graph.AddGraphicsPass(std::move(graphics))
            .RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Draw(3, 1); });
        graph.AddComputePass(std::move(compute))
            .RecordPassCommands([=](RDGPassCmdEncoder& encoder) {
                if (fail)
                {
                    encoder.Fail(RDGErrorCode::eCallback, "storage failure probe");
                }
                else if (!empty)
                {
                    encoder.DispatchIndirect(logical, 0);
                }
            });

        ASSERT_TRUE(graph.End());

        rhi->graphics.values.clear();
        rhi->graphics.boundResources.clear();
        rhi->graphics.vertexBuffers.clear();

        if (fail)
        {
            EXPECT_FALSE(device->ExecuteRenderGraph(graph));
            EXPECT_TRUE(rhi->graphics.values.empty());
            EXPECT_TRUE(rhi->graphics.boundResources.empty());
            EXPECT_EQ(Access::Graphics(graph), nullptr);
            EXPECT_EQ(Access::Compute(graph), nullptr);
            Access::CheckIdle(graph);
            continue;
        }

        ASSERT_TRUE(device->ExecuteRenderGraph(graph));

        RDGGraphicsPass* compiledGraphics = Access::Graphics(graph);
        RDGComputePass* compiledCompute   = Access::Compute(graph);
        ASSERT_NE(compiledGraphics, nullptr);
        ASSERT_NE(compiledCompute, nullptr);

        if (run == 0)
        {
            firstGraphics = compiledGraphics;
            firstCompute  = compiledCompute;
        }

        EXPECT_EQ(compiledGraphics, firstGraphics);
        EXPECT_EQ(compiledCompute, firstCompute);

        ShaderProgram* expectedShader = run % 2 ? secondShader : firstShader;
        EXPECT_EQ(compiledGraphics->pPipeline->GetShader(), expectedShader->GetShader());
        EXPECT_EQ(compiledCompute->pPipeline->GetShader(), expectedShader->GetShader());
        EXPECT_EQ(Access::IdleBytes(graph), 0u);
        EXPECT_EQ(rhi->graphics.values.size(), empty ? 0u : 1u);
        EXPECT_EQ(rhi->graphics.boundResources.size(), empty ? 0u : count + 1);
        EXPECT_EQ(rhi->graphics.vertexBuffers.size(), empty ? 0u : 1u);
        EXPECT_EQ(compiledGraphics->geometryBuffer.pIndexBuffer, empty ? nullptr : buffer);
        EXPECT_EQ(compiledGraphics->geometryBuffer.indexBufferOffset,
                  empty ? 0u : (run % 2 ? 8u : 4u));
        EXPECT_EQ(compiledGraphics->geometryBuffer.indexBufferFormat,
                  empty || run % 2 ? DataFormat::eR32UInt : DataFormat::eR16UInt);
        EXPECT_EQ(compiledCompute->indirectBindings.size(), empty ? 0u : 1u);

        if (!empty)
        {
            const std::vector<uint8_t>& bytes = rhi->graphics.values.back();
            ASSERT_EQ(bytes.size(), values.size() * sizeof(uint32_t));
            EXPECT_EQ(std::memcmp(bytes.data(), values.data(), bytes.size()), 0);
            EXPECT_EQ(rhi->graphics.vertexBuffers.back(), buffer);

            for (size_t i = 0; i < count; ++i)
            {
                EXPECT_EQ(rhi->graphics.boundResources[i], textures[run % 2]->GetDefaultView());
            }

            EXPECT_EQ(rhi->graphics.boundResources.back(), buffer);
            EXPECT_EQ(compiledCompute->indirectBindings[0].resource, logical);
            EXPECT_EQ(compiledCompute->indirectBindings[0].buffer, buffer);
        }
    }

    ASSERT_TRUE(graph.Reset());

    Access::CheckIdle(graph);

    for (RHIBuffer* buffer : buffers)
    {
        device->DestroyBuffer(buffer);
    }

    for (RHITexture* texture : textures)
    {
        device->DestroyTexture(texture);
    }
}

TEST_F(RenderCoreTest, CompiledPassStorageBoundsIdleCountBytesAndTrimsOnResize)
{
    using Access = RDGExecutionPlanTestAccess;
    CreateTestShaderProgram(device, "storage_budget");

    for (const std::pair<uint32_t, uint32_t> workload :
         {std::pair{300u, 0u}, {48u, 32768u}, {1u, 1048576u}})
    {
        RenderGraph graph("storage_budget");
        ASSERT_TRUE(graph.Begin());

        std::vector<uint8_t> values(workload.second, 0x51);

        for (uint32_t i = 0; i < workload.first; ++i)
        {
            RDGGraphicsPassDesc pass;
            pass.SetShaderProgramName("storage_budget");

            if (workload.second)
            {
                pass.BindValue("value", values.data(), workload.second);
            }

            if (i % 2)
            {
                graph.AddGraphicsPass(std::move(pass));
            }
            else
            {
                RDGComputePassDesc compute;
                static_cast<RDGPassDescBase&>(compute) =
                    std::move(static_cast<RDGPassDescBase&>(pass));
                graph.AddComputePass(std::move(compute));
            }
        }

        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        ASSERT_TRUE(graph.Reset());

        Access::CheckIdle(graph);

        if (workload.second == 0)
        {
            EXPECT_EQ(Access::IdleCount(graph), 256u);
        }
        else if (workload.first == 1)
        {
            EXPECT_EQ(Access::IdleCount(graph), 0u); // One oversize object cannot bypass the cap.
        }
        else
        {
            EXPECT_GT(Access::IdleCount(graph), 0u);
            EXPECT_LT(Access::IdleCount(graph), workload.first);
        }
    }

    RenderGraph* frame = device->GetCurrentFrameRDG();
    ASSERT_TRUE(frame->Begin());

    RDGComputePassDesc compute;
    compute.SetShaderProgramName("storage_budget");
    frame->AddComputePass(std::move(compute));

    ASSERT_TRUE(frame->End());
    ASSERT_TRUE(device->ExecuteRenderGraph(*frame));
    ASSERT_TRUE(frame->Reset());
    ASSERT_GT(Access::IdleBytes(*frame), 0u);

    device->InvalidateRDGPassCompilerForResize();

    EXPECT_EQ(Access::IdleCount(*frame), 0u);
    EXPECT_EQ(Access::IdleBytes(*frame), 0u);
}

static void RebuildTimedGraph(RenderGraph& graph, RenderDevice* device)
{
    EXPECT_TRUE(graph.Begin());

    RDGComputePassDesc pass;
    pass.SetShaderProgramName("setup_timing");
    pass.BindValue("value", uint32_t(42));
    graph.AddComputePass(std::move(pass));
    EXPECT_TRUE(graph.End());
    EXPECT_TRUE(device->ExecuteRenderGraph(graph));
}

TEST_F(RenderCoreTest, PassSetupTimingsDistinguishDisabledRebuildAndReplay)
{
    CreateTestShaderProgram(device, "setup_timing");
    RDGMetrics& metrics = device->GetRDGMetrics();
    CaptureVersionGraph(device);
    RDGMetricsOptions options  = metrics.GetOptions();
    options.preparationTimings = true;
    metrics.Configure(options);
    RenderGraph graph("setup_timing");

    RebuildTimedGraph(graph, device);
    const RDGMetricsSnapshot measured   = metrics.GetLastSnapshot();
    const RDGPassCompileTimings& timing = measured.passCompileTimings;
    EXPECT_TRUE(timing.enabled);
    EXPECT_GT(timing.bindingCPUUs, 0);
    EXPECT_GT(timing.pipelineCPUUs, 0);
    EXPECT_GE(timing.totalCPUUs, timing.bindingCPUUs + timing.pipelineCPUUs);
    EXPECT_GE(measured.compileCPUUs, timing.totalCPUUs);
    EXPECT_EQ(RDGMetrics::Format(measured).find("pass_setup_cpu_us=disabled"), std::string::npos);
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_TRUE(metrics.GetLastSnapshot().passCompileTimings.enabled);
    EXPECT_EQ(metrics.GetLastSnapshot().passCompileTimings.totalCPUUs, 0); // No recompilation.

    RebuildTimedGraph(graph, device);

    EXPECT_GT(metrics.GetLastSnapshot().passCompileTimings.totalCPUUs, 0);

    options.preparationTimings = false;
    metrics.Configure(options);
    RebuildTimedGraph(graph, device);

    EXPECT_FALSE(metrics.GetLastSnapshot().passCompileTimings.enabled);
    EXPECT_EQ(metrics.GetLastSnapshot().passCompileTimings.totalCPUUs, 0);
    EXPECT_NE(RDGMetrics::Format(metrics.GetLastSnapshot()).find("pass_setup_cpu_us=disabled"),
              std::string::npos);
    EXPECT_EQ(RDGMetrics::Format(measured).find("pass_setup_cpu_us=disabled"), std::string::npos);
}

TEST_F(RenderCoreTest, ParameterSnapshotsPreserveOffsetsArrayMetadataAndBindlessResources)
{
    RHITexture* texture = Texture();
    TestBuffer* buffer  = Buffer();
    RHISampler* sampler = rhi->CreateSampler({});
    RHIBatchedShaderParameters source;
    RHIShaderResourceDescriptor value{};
    value.set     = 2;
    value.binding = 5;
    source.AddValueParam(value, std::array<uint32_t, 2>{7, 11});
    value.set     = 4;
    value.binding = 9;
    source.AddValueParam(value, uint8_t(0xEF));
    RHIShaderResourceDescriptor resource{};
    resource.set     = 3;
    resource.binding = 6;
    resource.type    = RHIShaderResourceType::eSamplerWithTexture;
    source.AddResourceParam(resource, texture->GetDefaultView(), sampler, 17);
    resource.bindless = true;
    resource.type     = RHIShaderResourceType::eStorageBuffer;
    source.AddResourceParam(resource, buffer, nullptr, 23);
    RHICommandSetShaderParameters command(source);
    source.Reset();
    source.AddValueParam(value, uint64_t(0)); // Reuse and overwrite the original byte storage.
    const RHIBatchedShaderParameters& snapshot       = command.parameters;
    VectorView<const RHIShaderValueParameter> values = snapshot.GetValueParams();
    ASSERT_EQ(values.size(), 2u);
    EXPECT_EQ(values[0].set, 2u);
    EXPECT_EQ(values[0].binding, 5u);
    EXPECT_EQ(values[0].byteOffset, 0u);
    EXPECT_EQ(values[1].set, 4u);
    EXPECT_EQ(values[1].binding, 9u);
    EXPECT_EQ(values[1].byteOffset, 8u);

    std::array<uint32_t, 2> result;
    VectorView<const uint8_t> bytes = snapshot.GetValueBytes(values[0]);
    ASSERT_EQ(bytes.size(), sizeof(result));

    std::memcpy(result.data(), bytes.data(), bytes.size());

    EXPECT_EQ(result, (std::array<uint32_t, 2>{7, 11}));
    ASSERT_EQ(snapshot.GetValueBytes(values[1]).size(), 1u);
    EXPECT_EQ(snapshot.GetValueBytes(values[1])[0], 0xEF);

    VectorView<const RHIShaderResourceParameter> resources = snapshot.GetResourceParams();
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_EQ(resources[0].set, 3u);
    EXPECT_EQ(resources[0].binding, 6u);
    EXPECT_EQ(resources[0].arrayIndex, 17u);
    EXPECT_EQ(resources[0].resourceType, RHIShaderResourceType::eSamplerWithTexture);
    EXPECT_EQ(resources[0].pResource, texture->GetDefaultView());
    EXPECT_EQ(resources[0].pAuxResource, sampler);

    VectorView<const RHIShaderResourceParameter> bindless = snapshot.GetBindlessParams();
    ASSERT_EQ(bindless.size(), 1u);
    EXPECT_EQ(bindless[0].set, 3u);
    EXPECT_EQ(bindless[0].binding, 6u);
    EXPECT_EQ(bindless[0].arrayIndex, 23u);
    EXPECT_EQ(bindless[0].resourceType, RHIShaderResourceType::eStorageBuffer);
    EXPECT_EQ(bindless[0].pResource, buffer);
    EXPECT_EQ(bindless[0].pAuxResource, nullptr);

    source.Reset();
    command.parameters.CopyFrom(source);

    EXPECT_FALSE(command.parameters.HasAnyParameter());

    device->DestroyTexture(texture);
    device->DestroyBuffer(buffer);
    sampler->ReleaseReference();
}

namespace zen::rc
{
struct PipelineCacheTestAccess
{
    using KeyType = RenderDevice::PipelineKey;

    static KeyType Key(RHIShader* shader,
                       const RHIGfxPipelineStates& states,
                       const RHIRenderingLayout& layout,
                       bool dynamic                            = true,
                       const HashMap<uint32_t, int>& constants = {})
    {
        return RenderDevice::MakePipelineKey(shader, &states, &layout, constants, dynamic);
    }

    static void CheckCollisions(RHIShader* shader, const RHIRenderingLayout& layout)
    {
        RHIGfxPipelineStates states;
        PipelineCacheTestAccess::KeyType first  = Key(shader, states, layout);
        states.rasterizationState.lineWidth     = 2;
        PipelineCacheTestAccess::KeyType second = Key(shader, states, layout);
        ASSERT_FALSE(first == second);
        // Force identical hashes while using the production key equality and LRU container.
        first.hash = second.hash = 0;
        LRUCache<PipelineCacheTestAccess::KeyType, int, RenderDevice::PipelineKeyHasher> cache(2);
        cache.try_emplace(first, 11);
        cache.try_emplace(second, 29);
        ASSERT_EQ(cache.size(), 2u);
        EXPECT_EQ(cache.find(first)->second, 11);
        EXPECT_EQ(cache.find(second)->second, 29);
    }

    static size_t KeySize()
    {
        return sizeof(PipelineCacheTestAccess::KeyType);
    }
};
} // namespace zen::rc

TEST_F(RenderCoreTest, PipelineKeysCompareDescriptorsAndSurviveHashCollisions)
{
    using Access      = PipelineCacheTestAccess;
    RHIShader* shader = CreateTestShaderProgram(device, "pipeline_keys")->GetShader();
    RHIRenderingLayout layout;

    RHIGfxPipelineStates base;
    base.colorBlendState.AddAttachment();
    const PipelineCacheTestAccess::KeyType key = Access::Key(shader, base, layout);
    std::array<RHIGfxPipelineStates, 12> changes;
    changes.fill(base);
    changes[0].primitiveType                     = RHIDrawPrimitiveType::eLineList;
    changes[1].rasterizationState.lineWidth      = 2;
    changes[2].rasterizationState.depthBiasClamp = 0.25f;
    changes[3].multiSampleState.sampleCount      = SampleCount::e4;
    changes[4].multiSampleState.sampleMasks ^= 4;
    changes[5].depthStencilState.enableDepthWrite                 = true;
    changes[6].depthStencilState.frontOp.reference                = 7;
    changes[7].depthStencilState.backOp.writeMask                 = 0x80000000;
    changes[8].colorBlendState.attachments[0].srcColorBlendFactor = RHIBlendFactor::eOne;
    changes[9].colorBlendState.attachments[0].srcAlphaBlendFactor = RHIBlendFactor::eOne;
    changes[10].colorBlendState.blendConstants.r                  = 0.25f;
    changes[11].dynamicStates.Enable(RHIDynamicState::eLineWidth);

    for (size_t i = 0; i < changes.size(); ++i)
    {
        SCOPED_TRACE(i);
        EXPECT_FALSE(key == Access::Key(shader, changes[i], layout));
    }

    Access::CheckCollisions(shader, layout);
    // A 64-sample pipeline must retain mask bits above the first Vulkan mask word.
    RHIGfxPipelineStates fullMask         = base;
    fullMask.multiSampleState.sampleCount = SampleCount::e64;
    RHIGfxPipelineStates partialMask      = fullMask;
    partialMask.multiSampleState.sampleMasks ^= uint64_t{1} << 40;
    EXPECT_FALSE(Access::Key(shader, fullMask, layout) == Access::Key(shader, partialMask, layout));
    // Bitwise float identity remains reflexive, including unusual payloads.
    base.rasterizationState.lineWidth = std::numeric_limits<float>::quiet_NaN();

    EXPECT_TRUE(Access::Key(shader, base, layout) == Access::Key(shader, base, layout));
}

TEST_F(RenderCoreTest, PipelineCacheReusesDynamicAttachmentsAndKeepsLegacyKeysSeparate)
{
    using Access              = PipelineCacheTestAccess;
    RHIShader* shader         = CreateTestShaderProgram(device, "pipeline_layouts")->GetShader();
    RHITexture* firstTexture  = Texture();
    RHITexture* secondTexture = Texture();
    RHIRenderingLayout first;
    first.AddColorRenderTarget(firstTexture->GetFormat(), firstTexture,
                               RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore);
    RHIRenderingLayout second             = first;
    second.colorRenderTargets[0].pTexture = secondTexture;
    second.colorRenderTargets[0].loadOp   = RHIRenderTargetLoadOp::eLoad;
    second.colorRenderTargets[0].storeOp  = RHIRenderTargetStoreOp::eNone;
    second.SetRenderArea(3, 5, 13, 17);

    RHIGfxPipelineStates states;
    states.colorBlendState.AddAttachment();

    EXPECT_TRUE(Access::Key(shader, states, first) == Access::Key(shader, states, second));
    EXPECT_FALSE(Access::Key(shader, states, first, false) ==
                 Access::Key(shader, states, second, false));
    EXPECT_FALSE(Access::Key(shader, states, first, false) == Access::Key(shader, states, first));

    RHIPipeline* pipeline = device->GetOrCreateGfxPipeline(states, shader, &first, {});
    ASSERT_NE(pipeline, nullptr);
    EXPECT_EQ(pipeline, device->GetOrCreateGfxPipeline(states, shader, &second, {}));

    second.colorRenderTargets[0].format = DataFormat::eR16G16B16A16SFloat;

    EXPECT_NE(pipeline, device->GetOrCreateGfxPipeline(states, shader, &second, {}));

    second                                  = first;
    second.colorRenderTargets[0].numSamples = SampleCount::e4;

    EXPECT_FALSE(Access::Key(shader, states, first) == Access::Key(shader, states, second));

    second                                 = first;
    second.hasDepthStencilRT               = true;
    second.depthStencilRenderTarget.format = DataFormat::eD32SFloat;

    EXPECT_FALSE(Access::Key(shader, states, first) == Access::Key(shader, states, second));

    first                                      = second;
    second.depthStencilRenderTarget.numSamples = SampleCount::e4;

    EXPECT_FALSE(Access::Key(shader, states, first) == Access::Key(shader, states, second));

    device->DestroyTexture(firstTexture);
    device->DestroyTexture(secondTexture);
}

TEST_F(RenderCoreTest,
       PipelineCacheDistinguishesExplicitDepthStencilAspectsAndReusesEquivalentViews)
{
    RHIShader* shader = CreateTestShaderProgram(device, "pipeline_attachment_aspects")->GetShader();
    RHITextureCreateInfo textureInfo{};
    textureInfo.format = DataFormat::eD32SFloatS8UInt;
    textureInfo.type   = RHITextureType::e2D;
    textureInfo.width = textureInfo.height = 8;
    textureInfo.mipmaps                    = 2;
    textureInfo.arrayLayers                = 2;
    textureInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eDepthStencilAttachment);
    RHITexture* texture = rhi->CreateTexture(textureInfo);
    RHIGfxPipelineStates states{};
    RHIRenderingLayout combined{};
    combined.AddDepthStencilRenderTarget(texture->GetFormat(), texture,
                                         RHIRenderTargetLoadOp::eClear,
                                         RHIRenderTargetStoreOp::eStore);
    RHIPipeline* both = device->GetOrCreateGfxPipeline(states, shader, &combined, {});
    RHITextureViewCreateInfo viewInfo{};
    viewInfo.format = textureInfo.format;
    viewInfo.type   = textureInfo.type;
    viewInfo.aspect.SetFlag(RHITextureAspectFlagBits::eDepth);
    RHIRenderingLayout depth{};
    depth.AddDepthStencilRenderTarget(texture->CreateView(viewInfo), RHIRenderTargetLoadOp::eClear,
                                      RHIRenderTargetStoreOp::eStore);
    RHIPipeline* depthOnly = device->GetOrCreateGfxPipeline(states, shader, &depth, {});
    EXPECT_NE(depthOnly, both);
    viewInfo.baseArrayLayer = viewInfo.baseMipLevel = 1;
    RHIRenderingLayout otherMip{};
    otherMip.AddDepthStencilRenderTarget(texture->CreateView(viewInfo),
                                         RHIRenderTargetLoadOp::eLoad,
                                         RHIRenderTargetStoreOp::eStore);
    EXPECT_EQ(depthOnly, device->GetOrCreateGfxPipeline(states, shader, &otherMip, {}));
    viewInfo.aspect.Clear();
    viewInfo.aspect.SetFlag(RHITextureAspectFlagBits::eStencil);
    RHIRenderingLayout stencil{};
    stencil.AddDepthStencilRenderTarget(texture->CreateView(viewInfo),
                                        RHIRenderTargetLoadOp::eClear,
                                        RHIRenderTargetStoreOp::eStore);
    RHIPipeline* stencilOnly = device->GetOrCreateGfxPipeline(states, shader, &stencil, {});
    EXPECT_NE(stencilOnly, both);
    EXPECT_NE(stencilOnly, depthOnly);
    device->DestroyTexture(texture);
}

TEST_F(RenderCoreTest, PipelineCachePreservesLargeSpecializationsAndCanonicalOrder)
{
    RHIShader* shader = CreateTestShaderProgram(device, "pipeline_constants")->GetShader();

    RHIGfxPipelineStates states;
    RHIRenderingLayout layout;
    HashMap<uint32_t, int> forward, reverse;

    for (uint32_t i = 0; i < 256; ++i)
    {
        forward[i * 65537] = int(i) - 128;
    }

    for (uint32_t i = 256; i > 0; --i)
    {
        reverse[(i - 1) * 65537] = int(i - 1) - 128;
    }

    RHIPipeline* first = device->GetOrCreateGfxPipeline(states, shader, &layout, forward);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, device->GetOrCreateGfxPipeline(states, shader, &layout, reverse));
    EXPECT_EQ(first->GetShader()->GetCreateInfo().specializationConstants, forward);
    EXPECT_TRUE(shader->GetCreateInfo().specializationConstants.empty());

    reverse[0] += 65536;

    EXPECT_NE(first, device->GetOrCreateGfxPipeline(states, shader, &layout, reverse));
}

TEST_F(RenderCoreTest, PipelineCacheTracksIdentityFailuresEvictionAndResize)
{
    ShaderProgram* program = CreateTestShaderProgram(device, "pipeline_lifecycle");
    RHIShader* shader      = program->GetShader();

    RHIGfxPipelineStates states;
    RHIRenderingLayout layout;
    const PipelineCacheMetrics initial = device->GetPipelineCacheMetrics();
    RHIPipeline* first                 = device->GetOrCreateComputePipeline(shader);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, device->GetOrCreateComputePipeline(shader));

    ASSERT_TRUE(program->Init());

    shader                   = program->GetShader();
    RHIPipeline* replacement = device->GetOrCreateComputePipeline(shader);
    EXPECT_NE(first, replacement);

    rhi->failPipelineCreationAt = rhi->pipelineCount + 1;

    EXPECT_EQ(device->GetOrCreateGfxPipeline(states, shader, &layout, {}), nullptr);
    ASSERT_NE(device->GetOrCreateGfxPipeline(states, shader, &layout, {}), nullptr);

    rhi->failShaderCreation = true;

    EXPECT_EQ(device->GetOrCreateGfxPipeline(states, shader, &layout, {{1, 7}}), nullptr);

    rhi->failShaderCreation = false;

    ASSERT_NE(device->GetOrCreateGfxPipeline(states, shader, &layout, {{1, 7}}), nullptr);

    PipelineCacheMetrics measured = device->GetPipelineCacheMetrics().Since(initial);
    EXPECT_EQ(measured.requests, 7u);
    EXPECT_EQ(measured.hits, 1u);
    EXPECT_EQ(measured.misses, 6u);
    EXPECT_EQ(measured.creations, 4u);
    EXPECT_EQ(measured.failures, 2u);
    EXPECT_EQ(measured.timedRequests, 0u);
    EXPECT_EQ(measured.keyCPUUs + measured.lookupCPUUs + measured.creationCPUUs, 0);

    for (uint32_t i = 0; i < 260; ++i)
    {
        states.rasterizationState.lineWidth = float(i + 2);
        ASSERT_NE(device->GetOrCreateGfxPipeline(states, shader, &layout, {}), nullptr);
    }

    measured = device->GetPipelineCacheMetrics().Since(initial);

    EXPECT_EQ(measured.evictions, measured.creations - 256);

    const uint64_t replacementId = replacement->GetStableId();
    EXPECT_FALSE(destroyed.contains(replacementId));

    device->InvalidateRDGPassCompilerForResize();
    measured = device->GetPipelineCacheMetrics().Since(initial);

    EXPECT_EQ(measured.invalidations, 1u);
    EXPECT_EQ(measured.invalidatedEntries, 256u);
    ASSERT_NE(device->GetOrCreateComputePipeline(shader), nullptr);

    device->NextFrame();
    device->NextFrame();

    EXPECT_TRUE(destroyed.contains(replacementId));

    layout.numColorRenderTargets = MAX_NUM_COLOR_ATTACHMENTS + 1;

    EXPECT_EQ(device->GetOrCreateGfxPipeline(states, shader, &layout, {}), nullptr);
}

static void RecordPipelineMetrics(RenderGraph& graph,
                                  RDGExecutor& executor,
                                  RHICommandList& commands)
{
    EXPECT_TRUE(graph.Begin());

    RDGComputePassDesc pass;
    pass.SetShaderProgramName("pipeline_metrics");
    graph.AddComputePass(std::move(pass));
    EXPECT_TRUE(graph.End());
    EXPECT_TRUE(executor.Execute(&graph, &commands));
    commands.Reset();
}

TEST_F(RenderCoreTest, PipelineMetricsUseTheExecutingGraphsTimingOptionAndResetOnReplay)
{
    CreateTestShaderProgram(device, "pipeline_metrics");
    RDGExecutor executor(device);
    RDGMetricsOptions options;
    options.logging.sampleEvery = 1;
    options.logging.minInterval = std::chrono::milliseconds(0);
    options.preparationTimings  = true;
    executor.GetMetrics().Configure(options);
    executor.GetMetrics().SetSink({});

    EXPECT_FALSE(device->GetRDGMetrics().GetOptions().preparationTimings);

    RenderGraph graph("pipeline_metrics");
    RHICommandList commands;

    RecordPipelineMetrics(graph, executor, commands);
    const RDGMetricsSnapshot first      = executor.GetMetrics().GetLastSnapshot();
    const PipelineCacheMetrics& metrics = first.passCompileTimings.pipelines;
    EXPECT_EQ(metrics.requests, 1u);
    EXPECT_EQ(metrics.misses, 1u);
    EXPECT_EQ(metrics.creations, 1u);
    EXPECT_EQ(metrics.timedRequests, 1u);
    // Short key/empty-cache lookups can fall within one clock tick on faster builds.
    EXPECT_GE(metrics.keyCPUUs, 0);
    EXPECT_GE(metrics.lookupCPUUs, 0);
    EXPECT_GT(metrics.creationCPUUs, 0);
    EXPECT_GE(first.passCompileTimings.pipelineCPUUs,
              metrics.keyCPUUs + metrics.lookupCPUUs + metrics.creationCPUUs);
    ASSERT_TRUE(executor.Execute(&graph, &commands));

    commands.Reset();

    EXPECT_EQ(executor.GetMetrics().GetLastSnapshot().passCompileTimings.pipelines.requests, 0u);

    RecordPipelineMetrics(graph, executor, commands);
    const PipelineCacheMetrics& hit =
        executor.GetMetrics().GetLastSnapshot().passCompileTimings.pipelines;
    EXPECT_EQ(hit.hits, 1u);
    EXPECT_EQ(hit.creations, 0u);
    EXPECT_EQ(hit.creationCPUUs, 0);

    options.preparationTimings = false;
    executor.GetMetrics().Configure(options);
    RDGMetricsOptions deviceOptions  = device->GetRDGMetrics().GetOptions();
    deviceOptions.preparationTimings = true;
    device->GetRDGMetrics().Configure(deviceOptions);
    RecordPipelineMetrics(graph, executor, commands);
    const RDGMetricsSnapshot disabled = executor.GetMetrics().GetLastSnapshot();
    EXPECT_EQ(disabled.passCompileTimings.pipelines.hits, 1u);
    EXPECT_EQ(disabled.passCompileTimings.pipelines.timedRequests, 0u);
    EXPECT_EQ(disabled.passCompileTimings.pipelines.keyCPUUs, 0);
    EXPECT_NE(RDGMetrics::Format(disabled).find("pipeline_cpu_us=disabled"), std::string::npos);
    EXPECT_EQ(RDGMetrics::Format(first).find("pipeline_cpu_us=disabled"), std::string::npos);
}

// Measures the CPU cache path with synthetic shader bytes; never executes them on a GPU.
static RHIPipeline* RequestBenchmarkPipeline(RenderDevice* device,
                                             bool compute,
                                             RHIShader* shader,
                                             const RHIGfxPipelineStates& states,
                                             RHIRenderingLayout& layout)
{
    return compute ? device->GetOrCreateComputePipeline(shader) :
                     device->GetOrCreateGfxPipeline(states, shader, &layout, {});
}

TEST_F(RenderCoreTest, DISABLED_PipelineCacheBenchmark)
{
    using Clock = std::chrono::steady_clock;
    std::printf("PIPELINE_KEY_INLINE_BYTES %zu\n", PipelineCacheTestAccess::KeySize());
    RDGMetricsOptions options  = device->GetRDGMetrics().GetOptions();
    options.preparationTimings = true;
    device->GetRDGMetrics().Configure(options);
    device->GetRDGMetrics().SetSink({});
    ShaderProgram* program = CreateTestShaderProgram(device, "pipeline_benchmark");
    RHIRenderingLayout layout;
    RHITexture* target = Texture();
    layout.AddColorRenderTarget(target->GetFormat(), target, RHIRenderTargetLoadOp::eClear,
                                RHIRenderTargetStoreOp::eStore);

    RHIGfxPipelineStates states;
    states.colorBlendState.AddAttachment();

    for (size_t shaderBytes : {size_t(0), size_t(65536)})
    {
        for (bool compute : {false, true})
        {
            ASSERT_TRUE(program->Init());
            TestShader* shader = static_cast<TestShader*>(program->GetShader());
            shader->SetBenchmarkSPIRV(shaderBytes);

            RHIPipeline* expected =
                RequestBenchmarkPipeline(device, compute, shader, states, layout);
            ASSERT_NE(expected, nullptr);

            HeapVector<HeapVector<double>> samples(5);
            constexpr uint32_t requests = 256;

            for (uint32_t run = 0; run < 9; ++run)
            {
                const PipelineCacheMetrics before = device->GetPipelineCacheMetrics();
                const size_t allocated            = DefaultAllocator::GetTrackedAllocationEvents();
                const std::chrono::steady_clock::time_point start = Clock::now();

                for (uint32_t i = 0; i < requests; ++i)
                {
                    ASSERT_EQ(RequestBenchmarkPipeline(device, compute, shader, states, layout),
                              expected);
                }

                const std::chrono::steady_clock::time_point stop = Clock::now();
                const size_t events = DefaultAllocator::GetTrackedAllocationEvents() - allocated;
                const PipelineCacheMetrics metrics =
                    device->GetPipelineCacheMetrics().Since(before);
                EXPECT_EQ(metrics.hits, requests);
                EXPECT_EQ(metrics.misses, 0u);
                EXPECT_EQ(metrics.creations, 0u);

                if (run >= 3)
                {
                    samples[0].push_back(
                        std::chrono::duration<double, std::micro>(stop - start).count());
                    samples[1].push_back(metrics.keyCPUUs);
                    samples[2].push_back(metrics.lookupCPUUs);
                    samples[3].push_back(metrics.creationCPUUs);
                    samples[4].push_back(double(events));
                }
            }

            for (HeapVector<double>& sample : samples)
            {
                std::sort(sample.begin(), sample.end());
            }

            std::printf(
                "PIPELINE_BENCH compute=%d shader_bytes=%zu requests=%u total_us=%.2f key_us=%.2f lookup_us=%.2f create_us=%.2f alloc_events=%.0f\n",
                compute, shaderBytes, requests, samples[0][3], samples[1][3], samples[2][3],
                samples[3][3], samples[4][3]);
        }
    }

    device->DestroyTexture(target);
}

// End-to-end Debug/CPU benchmark. Allocation counters cover the engine allocator only.
TEST_F(RenderCoreTest, DISABLED_PassSetupBenchmark)
{
    using Clock = std::chrono::steady_clock;
    CreateTestShaderProgram(device, "setup_benchmark");
    RDGMetricsOptions options   = device->GetRDGMetrics().GetOptions();
    options.logging.sampleEvery = 1;
    options.logging.minInterval = std::chrono::milliseconds(0);
    options.validate            = false;
    options.preparationTimings  = true;
    device->GetRDGMetrics().Configure(options);
    device->GetRDGMetrics().SetSink({});
    std::vector<RHITexture*> textures;
    std::vector<RHITextureView*> views;

    for (uint32_t i = 0; i < 32; ++i)
    {
        textures.push_back(Texture());
        views.push_back(textures.back()->GetDefaultView());
    }

    TestBuffer* vertices = Buffer();

    for (bool compute : {false, true})
    {
        for (uint32_t arrays : {1u, 32u})
        {
            constexpr uint32_t passes = 256;
            RenderGraph graph("pass_setup_benchmark");
            std::array<std::vector<double>, 6> samples;

            for (uint32_t run = 0; run < 11; ++run)
            {
                ASSERT_TRUE(graph.Begin());

                graph.GetResourceManager()->ImportBuffer(vertices, RDGImportContents::eDefined);

                for (RHITexture* texture : textures)
                {
                    graph.GetResourceManager()->ImportTexture(texture, RDGImportContents::eDefined);
                }

                for (uint32_t i = 0; i < passes; ++i)
                {
                    RDGGraphicsPassDesc graphics;
                    graphics.SetShaderProgramName("setup_benchmark");
                    std::array<uint32_t, 64> value{};
                    value.fill(run * passes + i);
                    graphics.BindValue("value", value);
                    graphics.BindSampledTexture("uTextureArray", nullptr,
                                                MakeVecView(views.data(), arrays));

                    if (compute)
                    {
                        RDGComputePassDesc pass;
                        static_cast<RDGPassDescBase&>(pass) =
                            std::move(static_cast<RDGPassDescBase&>(graphics));
                        graph.AddComputePass(std::move(pass));
                    }
                    else
                    {
                        graphics.BindVertexBuffer(vertices);
                        graph.AddGraphicsPass(std::move(graphics));
                    }
                }

                ASSERT_TRUE(graph.End());

                const size_t allocations = DefaultAllocator::GetTrackedAllocationEvents();
                const std::chrono::steady_clock::time_point begin = Clock::now();
                ASSERT_TRUE(device->ExecuteRenderGraph(graph));

                const std::chrono::steady_clock::time_point end = Clock::now();
                const size_t allocated =
                    DefaultAllocator::GetTrackedAllocationEvents() - allocations;
                const RDGMetricsSnapshot& snapshot = device->GetRDGMetrics().GetLastSnapshot();
                ASSERT_TRUE(snapshot.passCompileTimings.enabled);

                const RDGPassCompileTimings& timing = snapshot.passCompileTimings;

                if (run >= 3)
                {
                    samples[0].push_back(
                        std::chrono::duration<double, std::micro>(end - begin).count());
                    samples[1].push_back(snapshot.compileCPUUs);
                    samples[2].push_back(timing.totalCPUUs);
                    samples[3].push_back(timing.bindingCPUUs);
                    samples[4].push_back(timing.pipelineCPUUs);
                    samples[5].push_back(double(allocated));
                }

                rhi->completed = rhi->submitted;
                device->CollectCompletedResources();
            }

            for (std::vector<double>& sample : samples)
            {
                std::sort(sample.begin(), sample.end());
            }

            ASSERT_TRUE(graph.Reset());

            std::printf(
                "RDG_SETUP_BENCH compute=%d array=%u passes=%u execute_us=%.2f prepare_us=%.2f setup_us=%.2f binding_us=%.2f pipeline_us=%.2f alloc_events=%.0f idle_cpu_bytes=%zu\n",
                compute, arrays, passes, samples[0][4], samples[1][4], samples[2][4], samples[3][4],
                samples[4][4], samples[5][4], RDGExecutionPlanTestAccess::IdleBytes(graph));
        }
    }

    device->DestroyBuffer(vertices);

    for (RHITexture* texture : textures)
    {
        device->DestroyTexture(texture);
    }
}

#include "RHIThreadingTests.inl"
