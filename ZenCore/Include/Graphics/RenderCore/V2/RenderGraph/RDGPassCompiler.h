#pragma once
#include "Graphics/RHI/RHICommon.h"
#include "RDGDefs.h"
#include "Graphics/RHI/RHIResource.h"
#include "Templates/HeapVector.h"
// #include "Graphics/RHI/RHIResource.h"
// #include "Graphics/RHI/RHICommon.h"
#include "Graphics/RHI/RHIShaderParameters.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Templates/NameID.h"
#include "Templates/VectorView.h"
#include <cstring>

// todo: do not use dynamic vector in structure/classes, use flat linked list
namespace zen
{
class RHICommandList;
}

namespace zen::rc
{
class RenderGraph;
class RenderDevice;
struct RDGPassNode;
struct RDGNodeMetrics;

struct RDGBindingSlice
{
    uint32_t offset{0};
    uint32_t count{0};
};

struct RDGBufferBinding
{
    NameID glslName;
    RDGBindingSlice buffers;
    RDGContentGuarantee contents{RDGContentGuarantee::eNone};
};

struct RDGTextureBinding
{
    NameID glslName;
    RHISampler* pSampler{nullptr};
    RDGBindingSlice views;
    RDGContentGuarantee contents{RDGContentGuarantee::eNone};
};

enum class RDGBindingType
{
    eNone           = 0,
    eSampledTexture = 1,
    eStorageImage   = 2,
    eStorageBuffer  = 3,
    eUniformBuffer  = 4,
    eMax            = 5
};

struct RDGBoundResource
{
    RDGResource resource;
    RDGTextureViewDesc view;
};

struct RDGResourceBinding
{
    RDGBindingType resourceType{RDGBindingType::eNone};
    NameID glslName;
    NameID
        producerOutputTag; // Transitional preceding-output adapter; removed with caller migration.
    RHISampler* pSampler{nullptr};
    RDGBindingSlice resources;
    RDGContentGuarantee contents{RDGContentGuarantee::eNone};
};

struct RDGValueBinding
{
    NameID glslName;
    RDGBindingSlice bytes;
};

struct RDGPassDescBase
{
    RDGResult validationResult;
    uint64_t shaderIdentity{0}; // Set by AddPass; detects replacement before compile/replay.

    // Opt in only when multiple dispatches in this pass have no inter-dispatch dependencies.
    bool independentDispatches{false};

    // Opt in only if the callback has no effects beyond its declared GPU resource writes.
    // Existing renderer callbacks are retained unless their caller supplies this contract.
    bool allowCulling{false};

    void Reject(RDGErrorCode code, const std::string& message)
    {
        if (validationResult)
        {
            validationResult = {code, message};
        }
    }

    // external bindings
    HeapVector<RDGBufferBinding> UAVBufferBindings;
    HeapVector<RDGTextureBinding> sampledTexBindings;
    HeapVector<RDGTextureBinding> UAVTexBindings;
    HeapVector<RDGValueBinding> valueBindings;

    HeapVector<RDGResourceBinding> resourceBindings;
    HeapVector<RDGBoundResource> resourceStorage;

    HeapVector<RHIBuffer*> bufferStorage;
    HeapVector<RHITextureView*> textureViewStorage;
    HeapVector<uint8_t> valueByteStorage;
    HeapVector<RHIBuffer*> indirectBuffers; // Transitional raw adapter.
    HeapVector<RDGBuffer> logicalIndirectBuffers;

    NameID shaderProgramName;
    NameID passTag;

    void SetShaderProgramName(NameID name)
    {
        shaderProgramName = name;
    }

    void SetPassTag(NameID tag)
    {
        passTag = tag;
    }

    void BindValue(NameID glslName, const void* pData, uint32_t size)
    {
        if (pData == nullptr || size == 0 || valueByteStorage.size() > UINT32_MAX - size)
        {
            Reject(RDGErrorCode::eBinding, "Invalid value binding data or size");
        }
        else
        {
            RDGValueBinding* pBinding = static_cast<decltype(valueBindings)::value_type*>(nullptr);

            for (RDGValueBinding& binding : valueBindings)
            {
                if (binding.glslName == glslName)
                {
                    pBinding = &binding;
                    break;
                }
            }

            RDGBindingSlice bytes{static_cast<uint32_t>(valueByteStorage.size()), size};

            if (pBinding != nullptr && pBinding->bytes.count == size)
            {
                bytes = pBinding->bytes;
            }
            else
            {
                valueByteStorage.resize(size_t(bytes.offset) + size);
            }

            std::memcpy(valueByteStorage.data() + bytes.offset, pData, size);

            if (pBinding != nullptr)
            {
                pBinding->bytes = bytes;
            }
            else
            {
                valueBindings.push_back({glslName, bytes});
            }
        }
    }

    template <typename T> void BindValue(NameID glslName, const T& value)
    {
        BindValue(glslName, &value, static_cast<uint32_t>(sizeof(T)));
    }

    void UseIndirectBuffer(RDGBuffer buffer)
    {
        if (!buffer)
        {
            Reject(RDGErrorCode::eBinding, "Empty indirect resource");
        }
        else
        {
            logicalIndirectBuffers.push_back(buffer);
        }
    }

    void UseIndirectBuffer(RHIBuffer* pBuffer)
    {
        indirectBuffers.push_back(pBuffer);
    }

    void BindStorageBuffer(NameID glslName,
                           RHIBuffer* pBuffer,
                           RDGContentGuarantee contents = RDGContentGuarantee::eNone)
    {
        RDGBufferBinding& binding = UAVBufferBindings.emplace_back();
        binding.contents          = contents;
        binding.glslName          = glslName;
        binding.buffers.count     = 1;
        binding.buffers.offset    = bufferStorage.size();
        bufferStorage.push_back(pBuffer);
    }

    void BindStorageBuffer(NameID glslName,
                           VectorView<RHIBuffer*> buffers,
                           RDGContentGuarantee contents = RDGContentGuarantee::eNone)
    {
        RDGBufferBinding& binding = UAVBufferBindings.emplace_back();
        binding.contents          = contents;
        binding.glslName          = glslName;
        binding.buffers.count     = buffers.size();
        binding.buffers.offset    = bufferStorage.size();
        bufferStorage.push_back(buffers);
    }

    void BindSampledTexture(NameID glslName, RHISampler* pSampler, RHITextureView* pTextureView)
    {
        RDGTextureBinding& binding = sampledTexBindings.emplace_back();
        binding.glslName           = glslName;
        binding.pSampler           = pSampler;
        binding.views.count        = 1;
        binding.views.offset       = textureViewStorage.size();

        textureViewStorage.push_back(pTextureView);
    }

    void BindSampledTexture(NameID glslName,
                            RHISampler* pSampler,
                            VectorView<RHITextureView*> textureViews)
    {
        RDGTextureBinding& binding = sampledTexBindings.emplace_back();
        binding.glslName           = glslName;
        binding.pSampler           = pSampler;
        binding.views.count        = textureViews.size();
        binding.views.offset       = textureViewStorage.size();

        textureViewStorage.push_back(textureViews);
    }

    void BindSampledTexture(NameID glslName, RHISampler* pSampler, NameID producerOutputTag)
    {
        RDGResourceBinding& binding = resourceBindings.emplace_back();
        binding.resourceType        = RDGBindingType::eSampledTexture;
        binding.glslName            = glslName;
        binding.pSampler            = pSampler;
        binding.producerOutputTag   = producerOutputTag;
        binding.resources           = {uint32_t(resourceStorage.size()), 1};
        resourceStorage.emplace_back();
    }

    void BindSampledTexture(NameID name,
                            RHISampler* sampler,
                            RDGTexture texture,
                            const RDGTextureViewDesc& view = {})
    {
        RDGTextureViewDesc selectedView = view;
        BindResources(name, RDGBindingType::eSampledTexture, MakeVecView(&texture, 1),
                      RDGContentGuarantee::eNone, sampler, MakeVecView(&selectedView, 1));
    }

    void BindSampledTexture(NameID name,
                            RHISampler* sampler,
                            VectorView<const RDGTexture> textures,
                            VectorView<const RDGTextureViewDesc> views = {})
    {
        BindResources(name, RDGBindingType::eSampledTexture, textures, RDGContentGuarantee::eNone,
                      sampler, views);
    }

    void BindStorageImage(NameID name,
                          RDGTexture texture,
                          RDGContentGuarantee contents = RDGContentGuarantee::eNone)
    {
        BindResources(name, RDGBindingType::eStorageImage, MakeVecView(&texture, 1), contents);
    }

    void BindStorageImage(NameID name,
                          RDGTexture texture,
                          const RDGTextureViewDesc& view,
                          RDGContentGuarantee contents = RDGContentGuarantee::eNone)
    {
        RDGTextureViewDesc selectedView = view;
        BindResources(name, RDGBindingType::eStorageImage, MakeVecView(&texture, 1), contents,
                      nullptr, MakeVecView(&selectedView, 1));
    }

    void BindStorageImage(NameID name,
                          VectorView<const RDGTexture> textures,
                          RDGContentGuarantee contents               = RDGContentGuarantee::eNone,
                          VectorView<const RDGTextureViewDesc> views = {})
    {
        BindResources(name, RDGBindingType::eStorageImage, textures, contents, nullptr, views);
    }

    void BindStorageImage(NameID glslName,
                          RHITextureView* pTextureView,
                          RDGContentGuarantee contents = RDGContentGuarantee::eNone)
    {
        RDGTextureBinding& binding = UAVTexBindings.emplace_back();
        binding.contents           = contents;
        binding.glslName           = glslName;
        binding.views.count        = 1;
        binding.views.offset       = textureViewStorage.size();

        textureViewStorage.push_back(pTextureView);
    }

    void BindStorageImage(NameID glslName,
                          VectorView<RHITextureView*> textureViews,
                          RDGContentGuarantee contents = RDGContentGuarantee::eNone)
    {
        RDGTextureBinding& binding = UAVTexBindings.emplace_back();
        binding.contents           = contents;
        binding.glslName           = glslName;
        binding.views.count        = textureViews.size();
        binding.views.offset       = textureViewStorage.size();

        textureViewStorage.push_back(textureViews);
    }

    void BindStorageImage(NameID glslName,
                          NameID producerOutputTag,
                          RDGContentGuarantee contents = RDGContentGuarantee::eNone)
    {
        RDGResourceBinding& binding = resourceBindings.emplace_back();
        binding.resourceType        = RDGBindingType::eStorageImage;
        binding.contents            = contents;
        binding.glslName            = glslName;
        binding.producerOutputTag   = producerOutputTag;
        binding.resources           = {uint32_t(resourceStorage.size()), 1};
        resourceStorage.emplace_back();
    }

    void BindStorageBuffer(NameID name,
                           RDGBuffer buffer,
                           RDGContentGuarantee contents = RDGContentGuarantee::eNone)
    {
        BindResources(name, RDGBindingType::eStorageBuffer, MakeVecView(&buffer, 1), contents);
    }

    void BindStorageBuffer(NameID name,
                           VectorView<const RDGBuffer> buffers,
                           RDGContentGuarantee contents = RDGContentGuarantee::eNone)
    {
        BindResources(name, RDGBindingType::eStorageBuffer, buffers, contents);
    }

    void BindUniformBuffer(NameID name, RDGBuffer buffer)
    {
        BindResources(name, RDGBindingType::eUniformBuffer, MakeVecView(&buffer, 1));
    }

    void BindUniformBuffer(NameID name, VectorView<const RDGBuffer> buffers)
    {
        BindResources(name, RDGBindingType::eUniformBuffer, buffers);
    }

private:
    template <typename Resource>
    void BindResources(NameID name,
                       RDGBindingType type,
                       VectorView<Resource> resources,
                       RDGContentGuarantee contents               = RDGContentGuarantee::eNone,
                       RHISampler* sampler                        = nullptr,
                       VectorView<const RDGTextureViewDesc> views = {})
    {
        if (resources.empty() || resources.data() == nullptr ||
            resourceStorage.size() > UINT32_MAX ||
            resources.size() > UINT32_MAX - resourceStorage.size() ||
            (!views.empty() && (views.data() == nullptr || views.size() != resources.size())))
        {
            Reject(RDGErrorCode::eBinding, "Invalid logical resource or view array");
            return;
        }

        bool valid = true;

        for (const Resource& resource : resources)
        {
            if (!resource)
            {
                Reject(RDGErrorCode::eBinding, "Empty logical resource in binding");
                valid = false;
                break;
            }
        }

        if (valid)
        {
            const RDGBindingSlice slice{uint32_t(resourceStorage.size()),
                                        uint32_t(resources.size())};

            for (uint32_t i = 0; i < resources.size(); ++i)
            {
                resourceStorage.push_back(
                    {resources[i], views.empty() ? RDGTextureViewDesc{} : views[i]});
            }

            resourceBindings.push_back({type, name, {}, sampler, slice, contents});
        }
    }
};

struct RDGColorOutputDesc
{
    uint32_t slot{0};
    RHIRenderTargetLoadOp loadOp{RHIRenderTargetLoadOp::eClear};
    RHIRenderTargetStoreOp storeOp{RHIRenderTargetStoreOp::eStore};
    RHITexture* pTexture{nullptr};
    DataFormat format{DataFormat::eUndefined};
    uint32_t width{0};
    uint32_t height{0};
    SampleCount samples{SampleCount::e1};
    NameID tag;
    RDGTexture texture;
    bool fullWrite{false}; // Assert full render-area coverage when Load=None.
};

struct RDGDepthStencilOutputDesc
{
    RHIRenderTargetLoadOp loadOp{RHIRenderTargetLoadOp::eClear};
    RHIRenderTargetStoreOp storeOp{RHIRenderTargetStoreOp::eStore};
    RHITexture* pTexture{nullptr};
    DataFormat format{DataFormat::eUndefined};
    uint32_t width{0};
    uint32_t height{0};
    NameID tag;
    RDGTexture texture;
    bool fullWrite{false}; // Assert full render-area coverage when Load=None.
};

struct RDGGraphicsPassDesc : RDGPassDescBase
{
    RHIGeometryBuffer geometryBuffer; // Transitional raw adapter.
    HeapVector<RDGBuffer> vertexBuffers;
    RDGBuffer indexBuffer;
    DataFormat indexBufferFormat{DataFormat::eR32UInt};
    uint32_t indexBufferOffset{0};
    RHIGfxPipelineStates pipelineStates;

    Rect2<int> renderArea;

    RDGColorOutputDesc colorOutputs[MAX_NUM_COLOR_ATTACHMENTS];
    RDGDepthStencilOutputDesc depthStencilOutput;
    uint32_t colorOutputCount{0};
    BitMask<MAX_NUM_COLOR_ATTACHMENTS + 1> outputMask; // last bit for depth stencils

    void SetPipelineStates(const RHIGfxPipelineStates& pso)
    {
        pipelineStates = pso;
    }

    void SetRenderArea(int32_t offsetX, int32_t offsetY, int32_t width, int32_t height)
    {
        renderArea.minX = offsetX;
        renderArea.minY = offsetY;
        renderArea.maxX = width;
        renderArea.maxY = height;
    }

    void BindVertexBuffer(RDGBuffer buffer)
    {
        if (!buffer)
        {
            Reject(RDGErrorCode::eBinding, "Empty vertex resource");
        }
        else
        {
            vertexBuffers.push_back(buffer);
        }
    }

    void BindIndexBuffer(RDGBuffer buffer,
                         DataFormat format = DataFormat::eR32UInt,
                         uint32_t offset   = 0)
    {
        if (!buffer)
        {
            Reject(RDGErrorCode::eBinding, "Empty index resource");
            return;
        }

        indexBuffer       = buffer;
        indexBufferFormat = format;
        indexBufferOffset = offset;
    }

    void BindVertexBuffer(RHIBuffer* pBuffer)
    {
        geometryBuffer.vertexBuffers.push_back(pBuffer);
    }

    void BindIndexBuffer(RHIBuffer* pBuffer,
                         DataFormat format = DataFormat::eR32UInt,
                         uint32_t offset   = 0)
    {
        geometryBuffer.pIndexBuffer      = pBuffer;
        geometryBuffer.indexBufferFormat = format;
        geometryBuffer.indexBufferOffset = offset;
    }

    void AddColorOutput(DataFormat format,
                        uint32_t width,
                        uint32_t height,
                        NameID tag,
                        RHIRenderTargetLoadOp loadOp   = RHIRenderTargetLoadOp::eClear,
                        RHIRenderTargetStoreOp storeOp = RHIRenderTargetStoreOp::eStore)
    {
        if (colorOutputCount >= MAX_NUM_COLOR_ATTACHMENTS)
        {
            Reject(RDGErrorCode::eAttachment, "Too many color attachments");
            return;
        }

        RDGColorOutputDesc colorOutput{};
        colorOutput.slot    = colorOutputCount;
        colorOutput.loadOp  = loadOp;
        colorOutput.storeOp = storeOp;
        colorOutput.format  = format;
        colorOutput.width   = width;
        colorOutput.height  = height;
        colorOutput.tag     = tag;

        colorOutputs[colorOutputCount] = std::move(colorOutput);
        outputMask.Set(colorOutputCount);
        colorOutputCount++;
    }

    void AddColorOutput(RHITexture* pTexture,
                        RHIRenderTargetLoadOp loadOp   = RHIRenderTargetLoadOp::eClear,
                        RHIRenderTargetStoreOp storeOp = RHIRenderTargetStoreOp::eStore)
    {
        if (colorOutputCount >= MAX_NUM_COLOR_ATTACHMENTS)
        {
            Reject(RDGErrorCode::eAttachment, "Too many color attachments");
        }
        else if (pTexture == nullptr)
        {
            Reject(RDGErrorCode::eAttachment, "Null color attachment");
        }
        else
        {
            RDGColorOutputDesc colorOutput{};
            colorOutput.pTexture = pTexture;
            colorOutput.slot     = colorOutputCount;
            colorOutput.loadOp   = loadOp;
            colorOutput.storeOp  = storeOp;
            colorOutput.format   = pTexture->GetFormat();
            colorOutput.width    = pTexture->GetWidth();
            colorOutput.height   = pTexture->GetHeight();
            colorOutput.tag      = pTexture->GetResourceTag();

            colorOutputs[colorOutputCount] = std::move(colorOutput);
            outputMask.Set(colorOutputCount);
            colorOutputCount++;
        }
    }

    void AddDepthStencilOutput(DataFormat format,
                               uint32_t width,
                               uint32_t height,
                               NameID tag,
                               RHIRenderTargetLoadOp loadOp   = RHIRenderTargetLoadOp::eClear,
                               RHIRenderTargetStoreOp storeOp = RHIRenderTargetStoreOp::eStore)
    {
        depthStencilOutput.format  = format;
        depthStencilOutput.loadOp  = loadOp;
        depthStencilOutput.storeOp = storeOp;
        depthStencilOutput.width   = width;
        depthStencilOutput.height  = height;
        depthStencilOutput.tag     = tag;

        outputMask.Set(MAX_NUM_COLOR_ATTACHMENTS);
    }

    void AddDepthStencilOutput(RHITexture* pTexture,
                               RHIRenderTargetLoadOp loadOp   = RHIRenderTargetLoadOp::eClear,
                               RHIRenderTargetStoreOp storeOp = RHIRenderTargetStoreOp::eStore)
    {
        if (pTexture == nullptr)
        {
            Reject(RDGErrorCode::eAttachment, "Null depth attachment");
            return;
        }

        depthStencilOutput.pTexture = pTexture;
        depthStencilOutput.format   = pTexture->GetFormat();
        depthStencilOutput.loadOp   = loadOp;
        depthStencilOutput.storeOp  = storeOp;
        depthStencilOutput.width    = pTexture->GetWidth();
        depthStencilOutput.height   = pTexture->GetHeight();
        depthStencilOutput.tag      = pTexture->GetResourceTag();

        outputMask.Set(MAX_NUM_COLOR_ATTACHMENTS);
    }

    void AddColorOutput(RDGTexture texture,
                        RHIRenderTargetLoadOp loadOp   = RHIRenderTargetLoadOp::eClear,
                        RHIRenderTargetStoreOp storeOp = RHIRenderTargetStoreOp::eStore,
                        bool fullWrite                 = false)
    {
        if (!texture || colorOutputCount >= MAX_NUM_COLOR_ATTACHMENTS)
        {
            Reject(RDGErrorCode::eAttachment, "Invalid logical color attachment");
            return;
        }

        RDGColorOutputDesc& output = colorOutputs[colorOutputCount];
        output.texture             = texture;
        output.slot                = colorOutputCount;
        output.loadOp              = loadOp;
        output.storeOp             = storeOp;
        output.fullWrite           = fullWrite;
        outputMask.Set(colorOutputCount++);
    }

    void AddDepthStencilOutput(RDGTexture texture,
                               RHIRenderTargetLoadOp loadOp   = RHIRenderTargetLoadOp::eClear,
                               RHIRenderTargetStoreOp storeOp = RHIRenderTargetStoreOp::eStore,
                               bool fullWrite                 = false)
    {
        if (!texture)
        {
            Reject(RDGErrorCode::eAttachment, "Empty depth handle");
            return;
        }

        depthStencilOutput.texture   = texture;
        depthStencilOutput.loadOp    = loadOp;
        depthStencilOutput.storeOp   = storeOp;
        depthStencilOutput.fullWrite = fullWrite;
        outputMask.Set(MAX_NUM_COLOR_ATTACHMENTS);
    }

    bool HasColorOutput(uint32_t attachmentIdx) const
    {
        return outputMask.Test(attachmentIdx) != false;
    }

    bool HasDepthStencilOutput() const
    {
        return outputMask.Test(MAX_NUM_COLOR_ATTACHMENTS) != false;
    }
};

struct RDGComputePassDesc : RDGPassDescBase
{};

struct RDGTransferPassDesc
{
    NameID passTag;
};

enum class RDGCompiledPassType : uint8_t
{
    eNone     = 0,
    eGraphics = 1,
    eCompute  = 2,
    eTransfer = 3,
    eMax      = 4
};

struct RDGCompiledPass
{
    RDGCompiledPass() = default;

    RDGCompiledPass(RDGCompiledPassType t) : type(t) {}

    RDGCompiledPassType type{RDGCompiledPassType::eNone};
    NameID passTag;
};

struct RDGShaderPass : RDGCompiledPass
{
    RDGShaderPass(RDGCompiledPassType type) : RDGCompiledPass(type) {}

    RHIPipeline* pPipeline{nullptr};
    RHIBatchedShaderParameters shaderParameters{};
    struct IndirectBinding
    {
        RDGBuffer resource;
        RHIBuffer* buffer;
    };
    HeapVector<IndirectBinding> indirectBindings;

    size_t GetDynamicStorageBytes() const
    {
        return shaderParameters.GetStorageBytes() +
            indirectBindings.capacity() * sizeof(IndirectBinding);
    }
};

struct RDGGraphicsPass : RDGShaderPass
{
    RHIRenderingLayout* pRenderingLayout{nullptr};
    RHIGeometryBuffer geometryBuffer;

    RDGGraphicsPass() : RDGShaderPass(RDGCompiledPassType::eGraphics) {}

    size_t GetStorageBytes() const
    {
        return sizeof(*this) + GetDynamicStorageBytes() +
            geometryBuffer.vertexBuffers.capacity() * sizeof(RHIBuffer*);
    }
};

struct RDGComputePass : RDGShaderPass
{
    RDGComputePass() : RDGShaderPass(RDGCompiledPassType::eCompute) {}

    size_t GetStorageBytes() const
    {
        return sizeof(*this) + GetDynamicStorageBytes();
    }
};

struct RDGTransferPass : RDGCompiledPass
{
    bool requiresGraphicsQueue{false};

    RDGTransferPass() : RDGCompiledPass(RDGCompiledPassType::eTransfer) {}
};

// Create RDGPass
// Create RDGResources && declare resource accesses in RDG
class RDGPassCompiler
{
public:
    explicit RDGPassCompiler(RenderDevice* pRenderDevice, RenderGraph* pRDG) :
        m_pRenderDevice(pRenderDevice), m_pRDG(pRDG)
    {}

    RDGGraphicsPass* CompileGraphicsPass(const RDGGraphicsPassDesc& desc);

    RDGComputePass* CompileComputePass(const RDGComputePassDesc& desc);

    void SetRenderGraph(RenderGraph* pRDG)
    {
        m_pRDG = pRDG;
    }

private:
    bool Check(bool condition, RDGErrorCode code, const std::string& message);

    bool BuildIndirectBindings(const RDGPassDescBase& desc, RDGShaderPass& pass);

    struct ShaderParameterBuilder;

    bool BuildShaderParameters(ShaderProgram* pShaderProgram,
                               const RDGPassDescBase* pDesc,
                               RHIBatchedShaderParameters& parameters);

    RenderDevice* m_pRenderDevice{nullptr};
    RenderGraph* m_pRDG{nullptr};
};

class RDGPassCmdEncoder
{
public:
    RDGPassCmdEncoder(RHICommandList* pCmdList,
                      RDGShaderPass* pPass,
                      RDGNodeMetrics* pMetrics            = nullptr,
                      const RDGPassDescBase* pDescription = nullptr);

    // Keep the first failure; subsequent encoder commands are ignored.
    const RDGResult& GetResult() const
    {
        return m_result;
    }

    // Callbacks report application errors here; the graph logs the result and rolls back recording.
    bool Fail(RDGErrorCode code, const std::string& message)
    {
        return m_result.Fail(code, message);
    }

    void Draw(uint32_t vertexCount,
              uint32_t instanceCount,
              uint32_t firstVertex   = 0,
              uint32_t firstInstance = 0);

    void DrawIndexed(uint32_t indexCount,
                     uint32_t instanceCount,
                     uint32_t firstIndex,
                     uint32_t vertexOffset,
                     uint32_t firstInstance);

    void DrawIndexedIndirect(RDGBuffer indirectBuffer,
                             uint32_t offset,
                             uint32_t drawCount,
                             uint32_t stride);

    void DispatchIndirect(RDGBuffer indirectBuffer, uint32_t offset);

    void DrawIndexedIndirect(RHIBuffer* indirectBuffer,
                             uint32_t offset,
                             uint32_t drawCount,
                             uint32_t stride);

    void Dispatch(uint32_t groupCountX, int32_t groupCountY, int32_t groupCountZ);

    void DispatchIndirect(RHIBuffer* indirectBuffer, uint32_t offset);

    void SetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY);

    void SetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY);

    void SetPushConstants(const void* pData, uint32_t dataSize, uint32_t offset = 0);

    template <typename T> void SetPushConstants(const T& data, uint32_t offset = 0)
    {
        SetPushConstants(&data, static_cast<uint32_t>(sizeof(T)), offset);
    }

    void SetShaderValue(NameID glslName, const void* pData, uint32_t dataSize);

    template <typename T> void SetShaderValue(NameID glslName, const T& value)
    {
        SetShaderValue(glslName, &value, static_cast<uint32_t>(sizeof(T)));
    }

    void GenerateMipmaps(RHITexture* pTexture);

    void CopyTexture(RHITexture* pSrcTexture,
                     RHITexture* pDstTexure,
                     VectorView<const RHITextureCopyRegion> regions);

    void CopyBuffer(RHIBuffer* pSrcBuffer,
                    RHIBuffer* pDstBuffer,
                    const RHIBufferCopyRegion& region);

    void CopyBufferToTexture(RHIBuffer* pSrcBuffer,
                             RHITexture* pDstTexure,
                             const RHIBufferTextureCopyRegion& region);

    void ClearTexture(RHITexture* pTexture,
                      const Color& clearColor,
                      const RHITextureSubResourceRange& range);

private:
    void EmitTextureTransition(RHITexture* pTexture,
                               const RHITextureSubResourceRange& range,
                               RHIAccessMode oldAccess,
                               RHIAccessMode newAccess,
                               RHITextureUsage oldUsage,
                               RHITextureUsage newUsage);

    bool RequirePass(RDGCompiledPassType type, const char* command);

    RHIBuffer* ResolveIndirectBuffer(RDGBuffer buffer);

    bool RequireIndirect(RHIBuffer* buffer, uint64_t offset, uint64_t size, const char* command);

    const RDGPassDescBase* m_pDescription{nullptr};
    uint32_t m_dispatchCount{0};

    bool ValidateDispatch();

    RDGResult m_result;

    bool Check(bool condition, RDGErrorCode code, const std::string& message)
    {
        return m_result.Check(condition, code, message);
    }

    RHICommandList* m_pCmdList{nullptr};
    RDGShaderPass* m_pPass{nullptr};
    RDGNodeMetrics* m_pMetrics{nullptr};
    friend class RDGTransferPassCmdRecorder;
};

class RDGShaderPassCmdRecorder
{
public:
    RDGShaderPassCmdRecorder(RenderGraph* pRDG, RDGPassNode* pNode);

    void RecordPassCommands(std::function<void(RDGPassCmdEncoder&)> lambda);

private:
    RenderGraph* m_pRDG{nullptr};
    RDGPassNode* m_pNode{nullptr};
    uint64_t m_generation{0};
};

class RDGTransferPassCmdRecorder
{
public:
    RDGTransferPassCmdRecorder(RenderGraph* pRDG, RDGPassNode* pNode);

    ~RDGTransferPassCmdRecorder();

    RDGTransferPassCmdRecorder(const RDGTransferPassCmdRecorder&) = delete;

    RDGTransferPassCmdRecorder& operator=(const RDGTransferPassCmdRecorder&) = delete;

    RDGTransferPassCmdRecorder& GenerateMipmaps(RHITexture* pTexture);

    RDGTransferPassCmdRecorder& NeverCull();

    RDGTransferPassCmdRecorder& CopyTexture(RHITexture* pSrcTexture,
                                            RHITexture* pDstTexture,
                                            VectorView<RHITextureCopyRegion> regions);

    RDGTransferPassCmdRecorder& CopyBuffer(RHIBuffer* pSrcBuffer,
                                           RHIBuffer* pDstBuffer,
                                           const RHIBufferCopyRegion& region);

    RDGTransferPassCmdRecorder& CopyBufferToTexture(RHIBuffer* pSrcBuffer,
                                                    RHITexture* pDstTexture,
                                                    const RHIBufferTextureCopyRegion& region);

    RDGTransferPassCmdRecorder& ClearTexture(RHITexture* pTexture, const Color& color);

    RDGTransferPassCmdRecorder& GenerateMipmaps(RDGTexture output);
    RDGTransferPassCmdRecorder& CopyBufferToTexture(RDGBuffer source,
                                                    RDGTexture destination,
                                                    const RHIBufferTextureCopyRegion& region);
    RDGTransferPassCmdRecorder& ClearTexture(RDGTexture texture, const Color& color);
    RDGTransferPassCmdRecorder& CopyTexture(RDGTexture source,
                                            RDGTexture destination,
                                            VectorView<RHITextureCopyRegion> regions);
    RDGTransferPassCmdRecorder& CopyBuffer(RDGBuffer source,
                                           RDGBuffer destination,
                                           const RHIBufferCopyRegion& region);

private:
    bool Check(bool condition, RDGErrorCode code, const std::string& message);

    RenderGraph* m_pRDG{nullptr};
    RDGPassNode* m_pNode{nullptr};
    uint64_t m_generation{0};
    HeapVector<std::function<void(RDGPassCmdEncoder&)>> m_ops;
};
} // namespace zen::rc
