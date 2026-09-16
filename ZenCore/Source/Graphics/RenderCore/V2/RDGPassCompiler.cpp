#include "Utils/MetricsLogger.h"
#include "Graphics/RHI/RHICommon.h"
#include "Graphics/RHI/RHIResource.h"
#include "Graphics/RHI/RHIShaderParameters.h"
#include "Graphics/RenderCore/V2/RenderGraph/RDGResourceManager.h"
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/RenderGraph/RDGPassCompiler.h"

#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "RDGCopyValidation.h"
#include "Math/Math.h"
#include "Templates/BitField.h"
#include "Templates/HeapVector.h"
#include "Templates/NameID.h"
#include "Utils/Errors.h"

namespace zen::rc
{
static void FillRenderingLayout(RHIRenderingLayout* pLayout, const RDGGraphicsPassDesc& desc)
{
    uint32_t attachmentIdx = 0;

    while (attachmentIdx < MAX_NUM_COLOR_ATTACHMENTS && desc.HasColorOutput(attachmentIdx))
    {
        const RDGColorOutputDesc& co = desc.colorOutputs[attachmentIdx];
        pLayout->AddColorRenderTarget(co.format, co.pTexture, co.loadOp, co.storeOp,
                                      DEFAULT_COLOR_CLEAR_VALUE,
                                      co.pTexture->GetBaseInfo().samples);
        attachmentIdx++;
    }

    if (desc.HasDepthStencilOutput())
    {
        const RDGDepthStencilOutputDesc& ds = desc.depthStencilOutput;
        pLayout->AddDepthStencilRenderTarget(ds.format, ds.pTexture, ds.loadOp, ds.storeOp);
    }

    pLayout->numLayers = 1;
    pLayout->SetRenderArea(desc.renderArea.minX, desc.renderArea.minY, desc.renderArea.maxX,
                           desc.renderArea.maxY);
}

static bool ValidateTextureCopyBox(RDGResult& result,
                                   RHITexture* texture,
                                   const RHITextureSubresourceLayers& layers,
                                   const Vec3i& offset,
                                   const Vec3i& size)
{
    return result.Check(texture != nullptr, RDGErrorCode::eRange, "Null texture in copy") &&
        ValidateTextureCopyBox(result, texture->GetBaseInfo(), texture->GetResourceTag(), layers,
                               offset, size);
}

template <typename Allocation>
static RHITextureCreateInfo LogicalTextureInfo(const Allocation* resource)
{
    RHITextureCreateInfo info{};
    const TextureFormat& format = resource->texFormat;
    info.format                 = format.format;
    info.type                   = static_cast<RHITextureType>(format.dimension);
    info.width                  = format.width;
    info.height                 = format.height;
    info.depth                  = format.depth;
    info.mipmaps                = format.mipmaps;
    info.arrayLayers            = format.arrayLayers;
    info.samples                = format.sampleCount;

    return info;
}

static bool ValidateBufferTextureFootprint(RDGResult& result,
                                           RHIBuffer* buffer,
                                           RHITexture* texture,
                                           const RHIBufferTextureCopyRegion& region,
                                           uint64_t* footprint = nullptr)
{
    return result.Check(buffer != nullptr && texture != nullptr, RDGErrorCode::eRange,
                        "Null buffer-to-texture resource") &&
        ValidateBufferTextureFootprint(result, buffer->GetRequiredSize(), texture->GetBaseInfo(),
                                       region, footprint);
}

static RHITextureSubResourceRange CopyRange(const RHITextureSubresourceLayers& layers)
{
    RHITextureSubResourceRange range{};
    range.aspect         = layers.aspect;
    range.baseMipLevel   = layers.mipmap;
    range.levelCount     = 1;
    range.baseArrayLayer = layers.baseArrayLayer;
    range.layerCount     = layers.layerCount;

    return range;
}

static bool CoversMip(const RHITextureCreateInfo& texture,
                      uint32_t mip,
                      const Vec3i& offset,
                      const Vec3i& size)
{
    return offset.x == 0 && offset.y == 0 && offset.z == 0 &&
        size.x == int32_t(std::max(1u, texture.width >> mip)) &&
        size.y == int32_t(std::max(1u, texture.height >> mip)) &&
        size.z == int32_t(std::max(1u, texture.depth >> mip));
}

static bool CoversMip(RHITexture* texture, uint32_t mip, const Vec3i& offset, const Vec3i& size)
{
    return CoversMip(texture->GetBaseInfo(), mip, offset, size);
}

static uint32_t MipExtent(uint32_t extent, uint32_t mipLevel)
{
    return std::max(1u, extent >> mipLevel);
}

static Vec3i MipExtent3D(const RHITextureCreateInfo& info, uint32_t mipLevel)
{
    return Vec3i{static_cast<int32_t>(MipExtent(info.width, mipLevel)),
                 static_cast<int32_t>(MipExtent(info.height, mipLevel)),
                 static_cast<int32_t>(MipExtent(info.depth, mipLevel))};
}

bool RDGPassCompiler::Check(bool condition, RDGErrorCode code, const std::string& message)
{
    bool result{};

    if (m_pRDG == nullptr)
    {
        LOGE("RDG compiler [{}]: {}", static_cast<uint32_t>(code), message);

        result = false;
    }
    else
    {
        result = m_pRDG->Check(condition, code, message);
    }

    return result;
}

RDGGraphicsPass* RDGPassCompiler::CompileGraphicsPass(const RDGGraphicsPassDesc& desc)
{
    RDGGraphicsPass* pResult = nullptr;
    bool valid               = true;

    valid = Check(m_pRenderDevice != nullptr && m_pRDG != nullptr, RDGErrorCode::eLifecycle,
                  "Pass compilation requires a graph and device");

    if (valid)
    {
        RDGPassCompileTimings& timings = m_pRDG->m_passCompileTimings;
        ScopedMetricsTimer setupTimer(timings.enabled, timings.totalCPUUs);
        ShaderProgram* pSP =
            ShaderProgramManager::GetInstance().RequestShaderProgram(desc.shaderProgramName);

        valid = Check(pSP != nullptr && pSP->GetShader() != nullptr, RDGErrorCode::eShader,
                      "Invalid shader program '" + desc.shaderProgramName.ToString() + "'");

        if (valid)
        {
            RDGGraphicsPass* pGfxPass = m_pRDG->AcquireGraphicsPass();
            m_pRDG->m_compiledGfxPasses.push_back(pGfxPass);
            pGfxPass->passTag = desc.passTag;

            valid = BuildShaderParameters(pSP, &desc, pGfxPass->shaderParameters);

            if (valid)
            {
                pGfxPass->pRenderingLayout = m_pRenderDevice->AcquireRenderingLayout();
                valid = Check(pGfxPass->pRenderingLayout != nullptr, RDGErrorCode::eAllocation,
                              "Failed to acquire rendering layout for '" + desc.passTag.ToString() +
                                  "'");
            }

            if (valid)
            {
                FillRenderingLayout(pGfxPass->pRenderingLayout, desc);

                {
                    ScopedMetricsTimer pipelineTimer(timings.enabled, timings.pipelineCPUUs);
                    pGfxPass->pPipeline = m_pRenderDevice->GetOrCreateGfxPipeline(
                        desc.pipelineStates, pSP->GetShader(), pGfxPass->pRenderingLayout, {},
                        timings.enabled);
                }

                valid = Check(pGfxPass->pPipeline != nullptr, RDGErrorCode::eAllocation,
                              "Failed to create graphics pipeline for '" + desc.passTag.ToString() +
                                  "'");
            }

            if (valid)
            {
                pGfxPass->pPipeline->AddReference();
                RHIGeometryBuffer& geometry = pGfxPass->geometryBuffer;
                geometry.vertexBuffers.reserve(desc.geometryBuffer.vertexBuffers.size() +
                                               desc.vertexBuffers.size());

                for (RHIBuffer* buffer : desc.geometryBuffer.vertexBuffers)
                {
                    geometry.vertexBuffers.push_back(buffer);
                }

                geometry.pIndexBuffer      = desc.geometryBuffer.pIndexBuffer;
                geometry.indexBufferFormat = desc.geometryBuffer.indexBufferFormat;
                geometry.indexBufferOffset = desc.geometryBuffer.indexBufferOffset;

                for (RDGBuffer const buffer : desc.vertexBuffers)
                {
                    const RDGResourceManager::Allocation* allocation =
                        m_pRDG->m_resourceManager.Resolve(buffer);
                    valid = Check(allocation != nullptr && allocation->pBuffer != nullptr,
                                  RDGErrorCode::eAllocation, "Missing logical vertex buffer");

                    if (valid)
                    {
                        pGfxPass->geometryBuffer.vertexBuffers.push_back(allocation->pBuffer);
                    }

                    if (!valid)
                    {
                        break;
                    }
                }

                if (valid)
                {
                    if (desc.indexBuffer)
                    {
                        const RDGResourceManager::Allocation* allocation =
                            m_pRDG->m_resourceManager.Resolve(desc.indexBuffer);
                        valid = Check(allocation != nullptr && allocation->pBuffer != nullptr,
                                      RDGErrorCode::eAllocation, "Missing logical index buffer");

                        if (valid)
                        {
                            pGfxPass->geometryBuffer.pIndexBuffer      = allocation->pBuffer;
                            pGfxPass->geometryBuffer.indexBufferFormat = desc.indexBufferFormat;
                            pGfxPass->geometryBuffer.indexBufferOffset = desc.indexBufferOffset;
                        }
                    }

                    valid = valid && (BuildIndirectBindings(desc, *pGfxPass));
                }

                if (valid)
                {
                    pResult = pGfxPass;
                }
            }
        }
    }

    return pResult;
}

RDGComputePass* RDGPassCompiler::CompileComputePass(const RDGComputePassDesc& desc)
{
    RDGComputePass* result{};

    if (Check(m_pRenderDevice != nullptr && m_pRDG != nullptr, RDGErrorCode::eLifecycle,
              "Pass compilation requires a graph and device"))
    {
        RDGPassCompileTimings& timings = m_pRDG->m_passCompileTimings;
        ScopedMetricsTimer setupTimer(timings.enabled, timings.totalCPUUs);
        ShaderProgram* pSP =
            ShaderProgramManager::GetInstance().RequestShaderProgram(desc.shaderProgramName);

        if (Check(pSP != nullptr && pSP->GetShader() != nullptr, RDGErrorCode::eShader,
                  "Invalid shader program '" + desc.shaderProgramName.ToString() + "'"))
        {
            RDGComputePass* pComputePass = m_pRDG->AcquireComputePass();
            m_pRDG->m_compiledComputePasses.push_back(pComputePass);
            pComputePass->passTag = desc.passTag;

            if (BuildShaderParameters(pSP, &desc, pComputePass->shaderParameters))
            {
                {
                    ScopedMetricsTimer pipelineTimer(timings.enabled, timings.pipelineCPUUs);
                    pComputePass->pPipeline = m_pRenderDevice->GetOrCreateComputePipeline(
                        pSP->GetShader(), timings.enabled);
                }

                if (Check(pComputePass->pPipeline != nullptr, RDGErrorCode::eAllocation,
                          "Failed to create compute pipeline for '" + desc.passTag.ToString() +
                              "'"))
                {
                    pComputePass->pPipeline->AddReference();

                    if (BuildIndirectBindings(desc, *pComputePass))
                    {
                        result = pComputePass;
                    }
                }
            }
        }
    }

    return result;
}

bool RDGPassCompiler::BuildIndirectBindings(const RDGPassDescBase& desc, RDGShaderPass& pass)
{
    bool valid = true;

    for (RDGBuffer const resource : desc.logicalIndirectBuffers)
    {
        const RDGResourceManager::Allocation* allocation =
            m_pRDG->m_resourceManager.Resolve(resource);
        valid = Check(allocation != nullptr && allocation->pBuffer != nullptr,
                      RDGErrorCode::eAllocation, "Missing logical indirect buffer");

        if (valid)
        {
            pass.indirectBindings.push_back({resource, allocation->pBuffer});
        }

        if (!valid)
        {
            break;
        }
    }

    return valid;
}

struct RDGPassCompiler::ShaderParameterBuilder
{
    RDGPassCompiler& compiler;
    RenderGraph* m_pRDG;
    ShaderProgram* pShaderProgram;
    const RDGPassDescBase* pDesc;
    RHIBatchedShaderParameters& parameters;

    bool Check(bool condition, RDGErrorCode code, const std::string& message)
    {
        return compiler.Check(condition, code, message);
    }

    const RHIShaderResourceDescriptor* Descriptor(NameID name, RHIShaderResourceType type)
    {
        const RHIShaderResourceDescriptor* pSRD = pShaderProgram->GetShaderResourceDescriptor(name);

        if (pSRD == nullptr || pSRD->type != type)
        {
            m_pRDG->Fail(RDGErrorCode::eBinding, "Invalid binding '" + name.ToString() + "'");
            pSRD = nullptr;
        }

        return pSRD;
    }

    bool BindValues()
    {
        bool allValid = true;

        for (const RDGValueBinding& binding : pDesc->valueBindings)
        {
            const RHIShaderResourceDescriptor* pSRD =
                Descriptor(binding.glslName, RHIShaderResourceType::eUniformBuffer);

            if (pSRD != nullptr && binding.bytes.count != 0)
            {
                parameters.AddValueParam(*pSRD,
                                         pDesc->valueByteStorage.data() + binding.bytes.offset,
                                         binding.bytes.count);
            }

            allValid = pSRD != nullptr;

            if (!allValid)
            {
                break;
            }
        }

        return allValid;
    }

    bool BindBuffers()
    {
        bool allValid = true;

        for (const RDGBufferBinding& binding : pDesc->UAVBufferBindings)
        {
            const RHIShaderResourceDescriptor* pSRD =
                Descriptor(binding.glslName, RHIShaderResourceType::eStorageBuffer);

            if (pSRD != nullptr)
            {
                for (uint32_t i = 0; i < binding.buffers.count; ++i)
                {
                    parameters.AddResourceParam(
                        *pSRD, pDesc->bufferStorage[binding.buffers.offset + i], nullptr, i);
                }
            }

            allValid = pSRD != nullptr;

            if (!allValid)
            {
                break;
            }
        }

        return allValid;
    }

    bool BindTextures(const HeapVector<RDGTextureBinding>& bindings, RHIShaderResourceType type)
    {
        bool allValid = true;

        for (const RDGTextureBinding& binding : bindings)
        {
            const RHIShaderResourceDescriptor* pSRD = Descriptor(binding.glslName, type);

            if (pSRD != nullptr)
            {
                RHISampler* pSampler =
                    type == RHIShaderResourceType::eSamplerWithTexture ? binding.pSampler : nullptr;

                for (uint32_t i = 0; i < binding.views.count; ++i)
                {
                    parameters.AddResourceParam(
                        *pSRD, pDesc->textureViewStorage[binding.views.offset + i], pSampler, i);
                }
            }

            allValid = pSRD != nullptr;

            if (!allValid)
            {
                break;
            }
        }

        return allValid;
    }

    bool BindSamplers()
    {
        bool allValid = true;

        for (const RDGSamplerBinding& binding : pDesc->samplerBindings)
        {
            const RHIShaderResourceDescriptor* pSRD =
                Descriptor(binding.glslName, RHIShaderResourceType::eSampler);

            if (pSRD != nullptr)
            {
                parameters.AddResourceParam(*pSRD, binding.pSampler, nullptr, 0);
            }
            else
            {
                allValid = false;
                break;
            }
        }

        return allValid;
    }

    bool BindResources()
    {
        bool allValid = true;

        for (const RDGResourceBinding& binding : pDesc->resourceBindings)
        {
            const RHIShaderResourceDescriptor* pSRD =
                pShaderProgram->GetShaderResourceDescriptor(binding.glslName);
            bool bound = Check(pSRD != nullptr, RDGErrorCode::eBinding, "Unknown logical binding");

            for (uint32_t i = 0; bound && i < binding.resources.count; ++i)
            {
                RDGBoundResource const& element =
                    pDesc->resourceStorage[binding.resources.offset + i];
                const RDGResourceManager::Allocation* resource =
                    m_pRDG->m_resourceManager.Resolve(element.resource);
                bound = resource != nullptr;

                if (bound)
                {
                    if (resource->type == RDGResourceType::eBuffer)
                    {
                        bound = Check(resource->pBuffer != nullptr, RDGErrorCode::eAllocation,
                                      "Missing logical buffer");

                        if (bound)
                        {
                            parameters.AddResourceParam(*pSRD, resource->pBuffer, nullptr, i);
                        }
                    }
                    else
                    {
                        RHITextureView* view = m_pRDG->m_resourceManager.MaterializeView(
                            element.resource, element.view);
                        bound = Check(view != nullptr, RDGErrorCode::eAllocation,
                                      "Failed to materialize texture view");

                        if (bound)
                        {
                            parameters.AddResourceParam(*pSRD, view, binding.pSampler, i);
                        }
                    }
                }
            }

            allValid = bound;

            if (!allValid)
            {
                break;
            }
        }

        return allValid;
    }
};

bool RDGPassCompiler::BuildShaderParameters(ShaderProgram* pShaderProgram,
                                            const RDGPassDescBase* pDesc,
                                            RHIBatchedShaderParameters& parameters)
{
    bool valid = Check(pShaderProgram != nullptr && pShaderProgram->GetShader() != nullptr &&
                           pDesc != nullptr,
                       RDGErrorCode::eShader, "Shader parameters require a shader and description");

    if (valid)
    {
        RDGPassCompileTimings& timings = m_pRDG->m_passCompileTimings;
        ScopedMetricsTimer bindingTimer(timings.enabled, timings.bindingCPUUs);
        ShaderParameterBuilder builder{*this, m_pRDG, pShaderProgram, pDesc, parameters};
        valid = builder.BindValues() && builder.BindBuffers() &&
            builder.BindTextures(pDesc->sampledTexBindings,
                                 RHIShaderResourceType::eSamplerWithTexture) &&
            builder.BindTextures(pDesc->UAVTexBindings, RHIShaderResourceType::eImage) &&
            builder.BindTextures(pDesc->separateTexBindings, RHIShaderResourceType::eTexture) &&
            builder.BindSamplers() && builder.BindResources();
    }

    return valid;
}

RDGPassCmdEncoder::RDGPassCmdEncoder(RHICommandList* pCmdList,
                                     RDGShaderPass* pPass,
                                     RDGNodeMetrics* pMetrics,
                                     const RDGPassDescBase* pDescription) :
    m_pDescription(pDescription), m_pCmdList(pCmdList), m_pPass(pPass), m_pMetrics(pMetrics)
{
    Check(m_pCmdList != nullptr, RDGErrorCode::eLifecycle,
          "Command encoder requires a command list");
}

bool RDGPassCmdEncoder::RequirePass(RDGCompiledPassType type, const char* command)
{
    bool result{};

    const RDGCompiledPassType actual =
        m_pPass != nullptr ? m_pPass->type : RDGCompiledPassType::eTransfer;

    if (Check(m_pCmdList != nullptr && actual == type, RDGErrorCode::eUnsupportedCommand,
              std::string(command) + " is not supported in this pass"))
    {
        result = true;
    }

    return result;
}

RHIBuffer* RDGPassCmdEncoder::ResolveIndirectBuffer(RDGBuffer resource)
{
    RHIBuffer* pBuffer = nullptr;

    if (m_result)
    {
        bool found = false;

        if (m_pPass != nullptr)
        {
            for (RDGShaderPass::IndirectBinding const& binding : m_pPass->indirectBindings)
            {
                if (binding.resource == resource)
                {
                    pBuffer = binding.buffer;
                    found   = true;
                    break;
                }
            }
        }

        if (!found)
        {
            Fail(RDGErrorCode::eBinding,
                 "Indirect command requires UseIndirectBuffer for this exact resource version");
        }
    }

    return pBuffer;
}

void RDGPassCmdEncoder::DrawIndexedIndirect(RDGBuffer resource,
                                            uint32_t offset,
                                            uint32_t drawCount,
                                            uint32_t stride)
{
    if (!RequirePass(RDGCompiledPassType::eGraphics, "DrawIndexedIndirect"))
    {
        return;
    }

    if (RHIBuffer* buffer = ResolveIndirectBuffer(resource))
    {
        DrawIndexedIndirect(buffer, offset, drawCount, stride);
    }
}

void RDGPassCmdEncoder::DispatchIndirect(RDGBuffer resource, uint32_t offset)
{
    if (!RequirePass(RDGCompiledPassType::eCompute, "DispatchIndirect"))
    {
        return;
    }

    if (RHIBuffer* buffer = ResolveIndirectBuffer(resource))
    {
        DispatchIndirect(buffer, offset);
    }
}

bool RDGPassCmdEncoder::RequireIndirect(RHIBuffer* buffer,
                                        uint64_t offset,
                                        uint64_t size,
                                        const char* command)
{
    bool result{};

    bool declared = m_pDescription != nullptr &&
        std::find(m_pDescription->indirectBuffers.begin(), m_pDescription->indirectBuffers.end(),
                  buffer) != m_pDescription->indirectBuffers.end();

    if (m_pPass != nullptr)
    {
        for (RDGShaderPass::IndirectBinding const& binding : m_pPass->indirectBindings)
        {
            declared |= binding.buffer == buffer;
        }
    }

    if (((Check(buffer != nullptr && declared, RDGErrorCode::eBinding,
                std::string(command) + " requires UseIndirectBuffer for its argument buffer"))) &&
        ((Check(offset % 4 == 0 && offset <= buffer->GetRequiredSize() &&
                    size <= buffer->GetRequiredSize() - offset,
                RDGErrorCode::eRange,
                std::string(command) + " argument range is out of bounds or misaligned"))))
    {
        result = true;
    }

    return result;
}

bool RDGPassCmdEncoder::ValidateDispatch()
{
    bool result{};

    if (((RequirePass(RDGCompiledPassType::eCompute, "Dispatch"))) &&
        ((Check(
            m_dispatchCount == 0 ||
                (m_pDescription != nullptr && m_pDescription->independentDispatches),
            RDGErrorCode::eUnsupportedCommand,
            "Dependent dispatches require separate passes; assert independentDispatches only for independent work"))))
    {
        ++m_dispatchCount;
        result = true;
    }

    return result;
}

void RDGPassCmdEncoder::Draw(uint32_t vertexCount,
                             uint32_t instanceCount,
                             uint32_t firstVertex,
                             uint32_t firstInstance)
{
    if (((RequirePass(RDGCompiledPassType::eGraphics, "Draw"))) &&
        ((Check(m_pCmdList != nullptr, RDGErrorCode::eLifecycle,
                "Command encoder requires a command list"))))
    {
        m_pCmdList->Draw(vertexCount, instanceCount, firstVertex, firstInstance);
    }
}

void RDGPassCmdEncoder::DrawIndexed(uint32_t indexCount,
                                    uint32_t instanceCount,
                                    uint32_t firstIndex,
                                    uint32_t vertexOffset,
                                    uint32_t firstInstance)
{
    if (((RequirePass(RDGCompiledPassType::eGraphics, "DrawIndexed"))) &&
        (((Check(static_cast<RDGGraphicsPass*>(m_pPass)->geometryBuffer.pIndexBuffer != nullptr,
                 RDGErrorCode::eBinding, "DrawIndexed requires an index buffer"))) &&
         ((Check(m_pCmdList != nullptr, RDGErrorCode::eLifecycle,
                 "Command encoder requires a command list")))))
    {
        RDGGraphicsPass* pGfxPass = static_cast<RDGGraphicsPass*>(m_pPass);

        RHICommandDrawIndexed::Param params{};
        params.pIndexBuffer      = pGfxPass->geometryBuffer.pIndexBuffer;
        params.indexFormat       = pGfxPass->geometryBuffer.indexBufferFormat;
        params.indexBufferOffset = pGfxPass->geometryBuffer.indexBufferOffset;
        params.indexCount        = indexCount;
        params.instanceCount     = instanceCount;
        params.firstIndex        = firstIndex;
        params.vertexOffset      = vertexOffset;
        params.firstInstance     = firstInstance;

        m_pCmdList->DrawIndexed(params);
    }
}

void RDGPassCmdEncoder::DrawIndexedIndirect(RHIBuffer* indirectBuffer,
                                            uint32_t offset,
                                            uint32_t drawCount,
                                            uint32_t stride)
{
    if (((RequirePass(RDGCompiledPassType::eGraphics, "DrawIndexedIndirect"))) &&
        (((Check(static_cast<RDGGraphicsPass*>(m_pPass)->geometryBuffer.pIndexBuffer != nullptr,
                 RDGErrorCode::eBinding, "DrawIndexedIndirect requires an index buffer"))) &&
         (((Check(drawCount <= 1 || (stride >= 20 && stride % 4 == 0), RDGErrorCode::eRange,
                  "Invalid indexed indirect command stride"))) &&
          (((RequireIndirect(indirectBuffer, offset,
                             drawCount == 0 ? 0 : uint64_t(drawCount - 1) * stride + 20,
                             "DrawIndexedIndirect"))) &&
           ((Check(m_pCmdList != nullptr, RDGErrorCode::eLifecycle,
                   "Command encoder requires a command list")))))))
    {
        RDGGraphicsPass* pGfxPass = static_cast<RDGGraphicsPass*>(m_pPass);

        RHICommandDrawIndexedIndirect::Param params{};
        params.pIndexBuffer      = pGfxPass->geometryBuffer.pIndexBuffer;
        params.indexFormat       = pGfxPass->geometryBuffer.indexBufferFormat;
        params.indexBufferOffset = pGfxPass->geometryBuffer.indexBufferOffset;
        params.offset            = offset;
        params.pIndirectBuffer   = indirectBuffer;
        params.drawCount         = drawCount;
        params.stride            = stride;

        m_pCmdList->DrawIndexedIndirect(params);
    }
}

void RDGPassCmdEncoder::Dispatch(uint32_t groupCountX, int32_t groupCountY, int32_t groupCountZ)
{
    if (((ValidateDispatch())) &&
        (((Check(groupCountY >= 0 && groupCountZ >= 0, RDGErrorCode::eRange,
                 "Negative dispatch group count"))) &&
         ((Check(m_pCmdList != nullptr, RDGErrorCode::eLifecycle,
                 "Command encoder requires a command list")))))
    {
        m_pCmdList->Dispatch(groupCountX, groupCountY, groupCountZ);
    }
}

void RDGPassCmdEncoder::DispatchIndirect(RHIBuffer* indirectBuffer, uint32_t offset)
{
    if (((ValidateDispatch())) &&
        (((RequireIndirect(indirectBuffer, offset, 12, "DispatchIndirect"))) &&
         ((Check(m_pCmdList != nullptr, RDGErrorCode::eLifecycle,
                 "Command encoder requires a command list")))))
    {
        m_pCmdList->DispatchIndirect(indirectBuffer, offset);
    }
}

void RDGPassCmdEncoder::SetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    if (((RequirePass(RDGCompiledPassType::eGraphics, "SetViewport"))) &&
        ((Check(m_pCmdList != nullptr, RDGErrorCode::eLifecycle,
                "Command encoder requires a command list"))))
    {
        m_pCmdList->SetViewport(minX, minY, maxX, maxY);
    }
}

void RDGPassCmdEncoder::SetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    if (((RequirePass(RDGCompiledPassType::eGraphics, "SetScissor"))) &&
        ((Check(m_pCmdList != nullptr, RDGErrorCode::eLifecycle,
                "Command encoder requires a command list"))))
    {
        m_pCmdList->SetScissor(minX, minY, maxX, maxY);
    }
}

void RDGPassCmdEncoder::SetPushConstants(const void* pData, uint32_t dataSize, uint32_t offset)
{
    if (!Check(m_pPass != nullptr && m_pPass->pPipeline != nullptr && pData != nullptr &&
                   dataSize > 0 && dataSize % 4 == 0 && offset == 0,
               RDGErrorCode::eBinding,
               "Invalid push constant data; nonzero offsets are not supported by this RHI path"))
    {
        return;
    }

    if (m_pPass != nullptr && m_pPass->pPipeline != nullptr && pData != nullptr && dataSize > 0)
    {
        m_pCmdList->SetPushConstants(m_pPass->pPipeline, static_cast<const uint8_t*>(pData),
                                     dataSize, offset);
    }
}

void RDGPassCmdEncoder::SetShaderValue(NameID glslName, const void* pData, uint32_t dataSize)
{
    if (Check(m_pPass != nullptr && m_pPass->pPipeline != nullptr && pData != nullptr &&
                  dataSize > 0,
              RDGErrorCode::eBinding, "Invalid shader value command"))
    {
        RHIShader* pShader                      = m_pPass->pPipeline->GetShader();
        const RHIShaderResourceDescriptor* pSRD = pShader->GetSRDByName(glslName);

        if (Check(pSRD != nullptr && pSRD->type == RHIShaderResourceType::eUniformBuffer &&
                      (pSRD->blockSize == 0 || dataSize <= pSRD->blockSize),
                  RDGErrorCode::eBinding,
                  "Invalid shader value binding '" + glslName.ToString() + "'"))
        {
            RHIBatchedShaderParameters params;
            params.AddValueParam(*pSRD, pData, dataSize);
            m_pCmdList->SetShaderParameters(params);
        }
    }
}

void RDGPassCmdEncoder::GenerateMipmaps(RHITexture* pTexture)
{
    if (!RequirePass(RDGCompiledPassType::eTransfer, "GenerateMipmaps"))
    {
        return;
    }

    if (pTexture != nullptr)
    {
        const RHITextureCreateInfo& info        = pTexture->GetBaseInfo();
        const RHITextureSubResourceRange& range = pTexture->GetSubResourceRange();

        EmitTextureTransition(pTexture, range.GetMipRange(0), RHIAccessMode::eReadWrite,
                              RHIAccessMode::eRead, RHITextureUsage::eTransferDst,
                              RHITextureUsage::eTransferSrc);

        for (uint32_t mipLevel = 1; mipLevel < info.mipmaps; mipLevel++)
        {
            RHITextureBlitRegion region{};
            region.srcOffset0 = {0, 0, 0};
            region.srcOffset1 = MipExtent3D(info, mipLevel - 1);
            region.dstOffset0 = {0, 0, 0};
            region.dstOffset1 = MipExtent3D(info, mipLevel);
            region.srcSubresources =
                RHITextureSubresourceLayers::MakeMipLayers(range, mipLevel - 1);
            region.dstSubresources = RHITextureSubresourceLayers::MakeMipLayers(range, mipLevel);

            m_pCmdList->BlitTexture(pTexture, pTexture, region, RHISamplerFilter::eLinear);

            EmitTextureTransition(pTexture, range.GetMipRange(mipLevel), RHIAccessMode::eReadWrite,
                                  RHIAccessMode::eRead, RHITextureUsage::eTransferDst,
                                  RHITextureUsage::eTransferSrc);
        }

        EmitTextureTransition(pTexture, range, RHIAccessMode::eRead, RHIAccessMode::eReadWrite,
                              RHITextureUsage::eTransferSrc, RHITextureUsage::eTransferDst);
    }
}

void RDGPassCmdEncoder::CopyTexture(RHITexture* pSrcTexture,
                                    RHITexture* pDstTexture,
                                    VectorView<const RHITextureCopyRegion> regions)
{
    if (((RequirePass(RDGCompiledPassType::eTransfer, "CopyTexture"))) &&
        ((Check(m_pCmdList != nullptr, RDGErrorCode::eLifecycle,
                "Command encoder requires a command list"))))
    {
        m_pCmdList->CopyTexture(pSrcTexture, pDstTexture, regions);
    }
}

void RDGPassCmdEncoder::CopyBuffer(RHIBuffer* pSrcBuffer,
                                   RHIBuffer* pDstBuffer,
                                   const RHIBufferCopyRegion& region)
{
    if (((RequirePass(RDGCompiledPassType::eTransfer, "CopyBuffer"))) &&
        ((Check(m_pCmdList != nullptr, RDGErrorCode::eLifecycle,
                "Command encoder requires a command list"))))
    {
        m_pCmdList->CopyBuffer(pSrcBuffer, pDstBuffer, region);
    }
}

void RDGPassCmdEncoder::CopyBufferToTexture(RHIBuffer* pSrcBuffer,
                                            RHITexture* pDstTexture,
                                            const RHIBufferTextureCopyRegion& region)
{
    if (((RequirePass(RDGCompiledPassType::eTransfer, "CopyBufferToTexture"))) &&
        ((Check(m_pCmdList != nullptr, RDGErrorCode::eLifecycle,
                "Command encoder requires a command list"))))
    {
        m_pCmdList->CopyBufferToTexture(pSrcBuffer, pDstTexture, region);
    }
}

void RDGPassCmdEncoder::ClearTexture(RHITexture* pTexture,
                                     const Color& clearColor,
                                     const RHITextureSubResourceRange& range)
{
    if (((RequirePass(RDGCompiledPassType::eTransfer, "ClearTexture"))) &&
        ((Check(m_pCmdList != nullptr, RDGErrorCode::eLifecycle,
                "Command encoder requires a command list"))))
    {
        m_pCmdList->ClearTexture(pTexture, clearColor, range);
    }
}

void RDGPassCmdEncoder::EmitTextureTransition(RHITexture* pTexture,
                                              const RHITextureSubResourceRange& range,
                                              RHIAccessMode oldAccess,
                                              RHIAccessMode newAccess,
                                              RHITextureUsage oldUsage,
                                              RHITextureUsage newUsage)
{
    RHITextureTransition transition{};
    transition.pTexture         = pTexture;
    transition.oldAccessMode    = oldAccess;
    transition.newAccessMode    = newAccess;
    transition.oldUsage         = oldUsage;
    transition.newUsage         = newUsage;
    transition.subResourceRange = range;

    BitField<RHIPipelineStageFlagBits> stage(RHIPipelineStageFlagBits::eTransfer);

    m_pCmdList->AddTransitions(stage, stage, {}, {}, transition);

    if (m_pMetrics != nullptr)
    {
        ++m_pMetrics->barrierCalls;
        ++m_pMetrics->internalTextureTransitions;
    }
}

RDGShaderPassCmdRecorder::RDGShaderPassCmdRecorder(RenderGraph* graph, RDGPassNode* node) :
    m_pRDG(graph), m_pNode(node), m_generation(graph->m_buildGeneration)
{}

RDGTransferPassCmdRecorder::RDGTransferPassCmdRecorder(RenderGraph* graph, RDGPassNode* node) :
    m_pRDG(graph), m_pNode(node), m_generation(graph->m_buildGeneration)
{
    if (node != nullptr)
    {
        ++graph->m_openTransferRecorders;
    }
}

void RDGShaderPassCmdRecorder::RecordPassCommands(std::function<void(RDGPassCmdEncoder&)> lambda)
{
    if (m_pRDG->CheckRecorder(m_pNode, m_generation))
    {
        if (!lambda || m_pNode->cmdLambdaIdx >= 0)
        {
            m_pRDG->Fail(RDGErrorCode::eBinding,
                         "A pass requires at most one nonempty command callback");
        }
        else
        {
            m_pNode->cmdLambdaIdx = m_pRDG->AddPassCmdLambda(std::move(lambda));
        }
    }
}

RDGTransferPassCmdRecorder& RDGTransferPassCmdRecorder::NeverCull()
{
    if (m_pRDG->CheckRecorder(m_pNode, m_generation))
    {
        m_pNode->neverCull = true;
    }

    return *this;
}

RDGTransferPassCmdRecorder::~RDGTransferPassCmdRecorder()
{
    if (m_pNode != nullptr && m_generation == m_pRDG->m_buildGeneration)
    {
        --m_pRDG->m_openTransferRecorders;
    }

    if (!m_ops.empty() && m_pRDG->CheckRecorder(m_pNode, m_generation))
    {
        m_pNode->cmdLambdaIdx =
            m_pRDG->AddPassCmdLambda([ops = std::move(m_ops)](RDGPassCmdEncoder& encoder) {
                for (size_t i = 0; i < ops.size(); ++i)
                {
                    if (!encoder.GetResult())
                    {
                        return;
                    }

                    // A pass can contain dependent copies (including overlapping writes).
                    if (i != 0)
                    {
                        RHIMemoryTransition barrier{};
                        barrier.srcAccess.SetFlags(RHIAccessFlagBits::eTransferRead,
                                                   RHIAccessFlagBits::eTransferWrite);
                        barrier.dstAccess = barrier.srcAccess;
                        const BitField<RHIPipelineStageFlagBits> stages(
                            RHIPipelineStageFlagBits::eTransfer);
                        encoder.m_pCmdList->AddTransitions(stages, stages, MakeVecView(&barrier, 1),
                                                           {}, {});

                        if (encoder.m_pMetrics != nullptr)
                        {
                            ++encoder.m_pMetrics->barrierCalls;
                            ++encoder.m_pMetrics->internalMemoryTransitions;
                        }
                    }

                    ops[i](encoder);
                }
            });
    }
}

bool RDGTransferPassCmdRecorder::Check(bool condition,
                                       RDGErrorCode code,
                                       const std::string& message)
{
    return m_pRDG->Check(condition, code, "Pass '" + m_pNode->tag.ToString() + "': " + message);
}

RDGTransferPassCmdRecorder& RDGTransferPassCmdRecorder::GenerateMipmaps(RHITexture* pTexture)
{
    bool valid = true;

    valid = m_pRDG->CheckRecorder(m_pNode, m_generation);
    valid = valid &&
        (Check(pTexture != nullptr && pTexture->GetNumMipmaps() > 0 &&
                   pTexture->GetNumMipmaps() <= 32,
               RDGErrorCode::eRange, "Invalid mipmap texture"));
    valid = valid &&
        (Check(pTexture->GetBaseInfo().usageFlags.HasFlag(RHITextureUsageFlagBits::eTransferSrc),
               RDGErrorCode::eBinding, "Mipmap texture lacks transfer-source creation usage"));

    if (valid)
    {
        RDGResult result;

        if (!ValidateMipmapCapabilities(result, pTexture->GetBaseInfo()))
        {
            m_pRDG->Fail(result.code, "Pass '" + m_pNode->tag.ToString() + "': " + result.message);
            valid = false;
        }

        if ((valid) && (pTexture != nullptr))
        {
            // Image blits require graphics capability even though they execute at transfer stage.
            static_cast<RDGTransferPass*>(m_pNode->pCompiledPass)->requiresGraphicsQueue = true;
            const RDGResourceManager::Allocation* pResource =
                m_pRDG->GetResourceManager()->ImportTextureAllocation(pTexture);
            RHITextureSubResourceRange baseRange = pTexture->GetSubResourceRange();
            baseRange.levelCount                 = 1;
            valid = m_pRDG->DeclareContentAccess(m_pNode, pResource, RDGContentEffect::eRead,
                                                 baseRange);
            valid = valid &&
                (m_pRDG->DeclareTextureAccessForPass(
                    m_pNode, pResource, RHITextureUsage::eTransferDst,
                    pTexture->GetSubResourceRange(), RHIAccessMode::eReadWrite, {},
                    RDGContentEffect::eFullWrite));

            if (valid)
            {
                m_pNode->contentAccesses.back().sourceResourceId = pResource->id;
                m_pNode->contentAccesses.back().sourceRange      = baseRange;
                m_ops.push_back(
                    [pTexture](RDGPassCmdEncoder& encoder) { encoder.GenerateMipmaps(pTexture); });
            }
        }
    }

    return *this;
}

RDGTransferPassCmdRecorder& RDGTransferPassCmdRecorder::CopyTexture(
    RHITexture* pSrcTexture,
    RHITexture* pDstTexture,
    VectorView<RHITextureCopyRegion> regions)
{
    bool valid = true;

    RDGResult result;
    valid = m_pRDG->CheckRecorder(m_pNode, m_generation);
    valid = valid &&
        (Check(pSrcTexture != nullptr && pDstTexture != nullptr && !regions.empty() &&
                   regions.data() != nullptr,
               RDGErrorCode::eRange, "Texture copy requires two textures and regions"));

    if (valid)
    {
        for (RHITextureCopyRegion const& region : regions)
        {
            if (!ValidateTextureCopyBox(result, pSrcTexture, region.srcSubresources,
                                        region.srcOffset, region.size))
            {
                m_pRDG->Fail(result.code,
                             "Pass '" + m_pNode->tag.ToString() + "': " + result.message);
                valid = false;
            }

            if (valid)
            {
                if (!ValidateTextureCopyBox(result, pDstTexture, region.dstSubresources,
                                            region.dstOffset, region.size) ||
                    !ValidateTextureCopyCapabilities(
                        result, pSrcTexture->GetBaseInfo(), pDstTexture->GetBaseInfo(), region,
                        static_cast<RDGTransferPass*>(m_pNode->pCompiledPass)
                            ->requiresGraphicsQueue))
                {
                    m_pRDG->Fail(result.code,
                                 "Pass '" + m_pNode->tag.ToString() + "': " + result.message);
                    valid = false;
                }

                valid = valid &&
                    (Check(region.srcSubresources.layerCount == region.dstSubresources.layerCount &&
                               int64_t(region.srcSubresources.aspect) ==
                                   int64_t(region.dstSubresources.aspect),
                           RDGErrorCode::eRange,
                           "Copy source and destination subresources do not match"));
            }

            if (!valid)
            {
                break;
            }
        }

        if ((valid) && (pSrcTexture != nullptr && pDstTexture != nullptr))
        {
            const RDGResourceManager::Allocation* pSrcResource =
                m_pRDG->GetResourceManager()->ImportTextureAllocation(pSrcTexture);
            const RDGResourceManager::Allocation* pDstResource =
                m_pRDG->GetResourceManager()->ImportTextureAllocation(pDstTexture);

            for (RHITextureCopyRegion const& region : regions)
            {
                valid = m_pRDG->DeclareTextureAccessForPass(
                    m_pNode, pSrcResource, RHITextureUsage::eTransferSrc,
                    CopyRange(region.srcSubresources), RHIAccessMode::eRead);

                if (valid)
                {
                    const bool full = CoversMip(pDstTexture, region.dstSubresources.mipmap,
                                                region.dstOffset, region.size);
                    valid           = m_pRDG->DeclareTextureAccessForPass(
                        m_pNode, pDstResource, RHITextureUsage::eTransferDst,
                        CopyRange(region.dstSubresources), RHIAccessMode::eReadWrite, {},
                        full ? RDGContentEffect::eFullWrite : RDGContentEffect::eWrite);

                    if (valid)
                    {
                        m_pNode->contentAccesses.back().sourceResourceId = pSrcResource->id;
                        m_pNode->contentAccesses.back().sourceRange =
                            CopyRange(region.srcSubresources);
                    }
                }

                if (!valid)
                {
                    break;
                }
            }

            if (valid)
            {
                HeapVector<RHITextureCopyRegion> copy(regions);
                m_ops.push_back(
                    [pSrcTexture, pDstTexture, copy = std::move(copy)](RDGPassCmdEncoder& encoder) {
                        encoder.CopyTexture(pSrcTexture, pDstTexture, copy);
                    });
            }
        }
    }

    return *this;
}

RDGTransferPassCmdRecorder& RDGTransferPassCmdRecorder::CopyBuffer(
    RHIBuffer* pSrcBuffer,
    RHIBuffer* pDstBuffer,
    const RHIBufferCopyRegion& region)
{
    bool valid = true;

    valid = m_pRDG->CheckRecorder(m_pNode, m_generation);
    valid = valid &&
        (Check(pSrcBuffer != nullptr && pDstBuffer != nullptr && region.size > 0,
               RDGErrorCode::eRange, "Buffer copy requires two buffers and a nonempty region"));
    valid = valid &&
        (Check(region.srcOffset <= pSrcBuffer->GetRequiredSize() &&
                   region.size <= pSrcBuffer->GetRequiredSize() - region.srcOffset &&
                   region.dstOffset <= pDstBuffer->GetRequiredSize() &&
                   region.size <= pDstBuffer->GetRequiredSize() - region.dstOffset,
               RDGErrorCode::eRange, "Buffer copy region is out of bounds"));
    valid = valid &&
        (Check(pSrcBuffer != pDstBuffer || region.srcOffset + region.size <= region.dstOffset ||
                   region.dstOffset + region.size <= region.srcOffset,
               RDGErrorCode::eRange, "Overlapping regions in a buffer copy"));

    if ((valid) && (pSrcBuffer != nullptr && pDstBuffer != nullptr))
    {
        const RDGResourceManager::Allocation* pSrcResource =
            m_pRDG->GetResourceManager()->ImportBufferAllocation(pSrcBuffer);
        const RDGResourceManager::Allocation* pDstResource =
            m_pRDG->GetResourceManager()->ImportBufferAllocation(pDstBuffer);

        BitField<RHIBufferUsageFlagBits> srcUsage(RHIBufferUsageFlagBits::eTransferSrcBuffer);
        BitField<RHIBufferUsageFlagBits> dstUsage(RHIBufferUsageFlagBits::eTransferDstBuffer);

        valid = m_pRDG->DeclareBufferAccessForPass(m_pNode, pSrcResource, srcUsage,
                                                   RHIAccessMode::eRead);

        if (valid)
        {
            m_pNode->contentAccesses.back().bufferOffset = region.srcOffset;
            m_pNode->contentAccesses.back().bufferSize   = region.size;
            valid                                        = m_pRDG->DeclareBufferAccessForPass(
                m_pNode, pDstResource, dstUsage, RHIAccessMode::eReadWrite, {},
                region.dstOffset == 0 && region.size == pDstBuffer->GetRequiredSize() ?
                    RDGContentEffect::eFullWrite :
                    RDGContentEffect::eWrite);
        }

        if (valid)
        {
            RDGContentAccess& destinationAccess  = m_pNode->contentAccesses.back();
            destinationAccess.sourceResourceId   = pSrcResource->id;
            destinationAccess.bufferOffset       = region.dstOffset;
            destinationAccess.bufferSize         = region.size;
            destinationAccess.sourceBufferOffset = region.srcOffset;
            destinationAccess.sourceBufferSize   = region.size;
            m_ops.push_back([pSrcBuffer, pDstBuffer, region](RDGPassCmdEncoder& encoder) {
                encoder.CopyBuffer(pSrcBuffer, pDstBuffer, region);
            });
        }
    }

    return *this;
}

RDGTransferPassCmdRecorder& RDGTransferPassCmdRecorder::CopyBufferToTexture(
    RHIBuffer* pSrcBuffer,
    RHITexture* pDstTexture,
    const RHIBufferTextureCopyRegion& region)
{
    bool valid = true;

    RDGResult result;
    valid = m_pRDG->CheckRecorder(m_pNode, m_generation);
    valid = valid &&
        (Check(pSrcBuffer != nullptr && pDstTexture != nullptr &&
                   region.bufferOffset < pSrcBuffer->GetRequiredSize(),
               RDGErrorCode::eRange, "Invalid buffer-to-texture source offset"));

    if (valid)
    {
        if (!ValidateTextureCopyBox(result, pDstTexture, region.textureSubresources,
                                    region.textureOffset, region.textureSize))
        {
            m_pRDG->Fail(result.code, "Pass '" + m_pNode->tag.ToString() + "': " + result.message);
            valid = false;
        }
    }

    if (valid)
    {
        uint64_t footprint = 0;

        if (!ValidateBufferTextureFootprint(result, pSrcBuffer, pDstTexture, region, &footprint) ||
            !ValidateBufferTextureCopyCapabilities(
                result, pDstTexture->GetBaseInfo(), region,
                static_cast<RDGTransferPass*>(m_pNode->pCompiledPass)->requiresGraphicsQueue))
        {
            m_pRDG->Fail(result.code, "Pass '" + m_pNode->tag.ToString() + "': " + result.message);
            valid = false;
        }

        if ((valid) && (pSrcBuffer != nullptr && pDstTexture != nullptr))
        {
            const RDGResourceManager::Allocation* pSrcResource =
                m_pRDG->GetResourceManager()->ImportBufferAllocation(pSrcBuffer);
            const RDGResourceManager::Allocation* pDstResource =
                m_pRDG->GetResourceManager()->ImportTextureAllocation(pDstTexture);

            BitField<RHIBufferUsageFlagBits> srcUsage(RHIBufferUsageFlagBits::eTransferSrcBuffer);
            valid = m_pRDG->DeclareBufferAccessForPass(m_pNode, pSrcResource, srcUsage,
                                                       RHIAccessMode::eRead);

            if (valid)
            {
                m_pNode->contentAccesses.back().bufferOffset = region.bufferOffset;
                m_pNode->contentAccesses.back().bufferSize   = footprint;
                valid                                        = m_pRDG->DeclareTextureAccessForPass(
                    m_pNode, pDstResource, RHITextureUsage::eTransferDst,
                    CopyRange(region.textureSubresources), RHIAccessMode::eReadWrite, {},
                    CoversMip(pDstTexture, region.textureSubresources.mipmap, region.textureOffset,
                              region.textureSize) ?
                        RDGContentEffect::eFullWrite :
                        RDGContentEffect::eWrite);
            }

            if (valid)
            {
                m_pNode->contentAccesses.back().sourceResourceId   = pSrcResource->id;
                m_pNode->contentAccesses.back().sourceBufferOffset = region.bufferOffset;
                m_pNode->contentAccesses.back().sourceBufferSize   = footprint;
                m_ops.push_back([pSrcBuffer, pDstTexture, region](RDGPassCmdEncoder& encoder) {
                    encoder.CopyBufferToTexture(pSrcBuffer, pDstTexture, region);
                });
            }
        }
    }

    return *this;
}

RDGTransferPassCmdRecorder& RDGTransferPassCmdRecorder::ClearTexture(RHITexture* pTexture,
                                                                     const Color& color)
{
    bool valid = true;

    valid = m_pRDG->CheckRecorder(m_pNode, m_generation);
    valid = valid &&
        (Check(pTexture != nullptr &&
                   pTexture->GetSubResourceRange().aspect.HasFlag(RHITextureAspectFlagBits::eColor),
               RDGErrorCode::eRange, "ClearTexture requires a color texture"));

    if ((valid) && (pTexture != nullptr))
    {
        // vkCmdClearColorImage requires graphics or compute capability, not a transfer-only queue.
        static_cast<RDGTransferPass*>(m_pNode->pCompiledPass)->requiresGraphicsQueue = true;
        const RHITextureSubResourceRange range = pTexture->GetSubResourceRange();
        const RDGResourceManager::Allocation* pResource =
            m_pRDG->GetResourceManager()->ImportTextureAllocation(pTexture);
        valid = m_pRDG->DeclareTextureAccessForPass(
            m_pNode, pResource, RHITextureUsage::eTransferDst, range, RHIAccessMode::eReadWrite, {},
            RDGContentEffect::eFullWrite);

        if (valid)
        {
            m_ops.push_back([pTexture, color, range](RDGPassCmdEncoder& encoder) {
                encoder.ClearTexture(pTexture, color, range);
            });
        }
    }

    return *this;
}

RDGTransferPassCmdRecorder& RDGTransferPassCmdRecorder::GenerateMipmaps(RDGTexture output)
{
    bool valid = true;

    valid = m_pRDG->CheckRecorder(m_pNode, m_generation);

    if (valid)
    {
        const RDGResourceManager::Allocation* resource = m_pRDG->m_resourceManager.Resolve(output);
        valid                                          = (resource != nullptr);
        valid                                          = valid &&
            (Check(resource->texFormat.mipmaps > 0 && resource->texFormat.mipmaps <= 32,
                   RDGErrorCode::eRange, "Invalid logical mipmap texture"));

        if (valid)
        {
            RDGResult result;

            if (!ValidateMipmapCapabilities(result, LogicalTextureInfo(resource)))
            {
                m_pRDG->Fail(result.code,
                             "Pass '" + m_pNode->tag.ToString() + "': " + result.message);
                valid = false;
            }

            if (valid)
            {
                const uint32_t transferSrc =
                    static_cast<uint32_t>(RHITextureUsageFlagBits::eTransferSrc);
                valid = Check(!resource->imported || resource->usageFlags.HasFlag(transferSrc),
                              RDGErrorCode::eBinding,
                              "Mipmap texture lacks transfer-source creation usage");

                // Blits transition individual mips internally; require the capability without adding
                // a conflicting transfer-source layout to the pass's whole-allocation access.
                if (valid)
                {
                    if (!resource->imported)
                    {
                        const_cast<RDGResourceManager::Allocation*>(resource)->usageFlags.SetFlag(
                            transferSrc);
                    }

                    const RHITextureSubResourceRange range =
                        m_pRDG->m_resourceManager.ViewRange(*resource, {});
                    RHITextureSubResourceRange base = range;
                    base.levelCount                 = 1;
                    // This fixed-function operation reads the predecessor's base mip and defines the output
                    // version, including that preserved base. It never declares a separate read of its output.
                    valid = !(!m_pRDG->DeclareContentAccess(m_pNode, resource,
                                                            RDGContentEffect::eRead, base) ||
                              !m_pRDG->DeclareTextureAccessForPass(
                                  m_pNode, resource, RHITextureUsage::eTransferDst, range,
                                  RHIAccessMode::eReadWrite, {}, RDGContentEffect::eFullWrite, true,
                                  false, output));

                    if (valid)
                    {
                        m_pNode->contentAccesses.back().sourceResourceId = resource->id;
                        m_pNode->contentAccesses.back().sourceRange      = base;
                        static_cast<RDGTransferPass*>(m_pNode->pCompiledPass)
                            ->requiresGraphicsQueue = true;
                        m_ops.push_back([resource](RDGPassCmdEncoder& encoder) {
                            encoder.GenerateMipmaps(resource->pTexture);
                        });
                    }
                }
            }
        }
    }

    return *this;
}

RDGTransferPassCmdRecorder& RDGTransferPassCmdRecorder::CopyBufferToTexture(
    RDGBuffer source,
    RDGTexture destination,
    const RHIBufferTextureCopyRegion& region)
{
    bool valid = true;

    valid = m_pRDG->CheckRecorder(m_pNode, m_generation);

    if (valid)
    {
        const RDGResourceManager::Allocation* src = m_pRDG->m_resourceManager.Resolve(source);
        const RDGResourceManager::Allocation* dst = m_pRDG->m_resourceManager.Resolve(destination);
        valid                                     = !(src == nullptr || dst == nullptr);

        if (valid)
        {
            const RHITextureCreateInfo info = LogicalTextureInfo(dst);
            RDGResult result;
            uint64_t footprint = 0;

            if (!ValidateTextureCopyBox(result, info, dst->name, region.textureSubresources,
                                        region.textureOffset, region.textureSize) ||
                !ValidateBufferTextureFootprint(result, src->bufferSize, info, region,
                                                &footprint) ||
                !ValidateBufferTextureCopyCapabilities(
                    result, info, region,
                    static_cast<RDGTransferPass*>(m_pNode->pCompiledPass)->requiresGraphicsQueue))
            {
                m_pRDG->Fail(result.code,
                             "Pass '" + m_pNode->tag.ToString() + "': " + result.message);
                valid = false;
            }

            valid = valid &&
                (m_pRDG->DeclareBufferAccessForPass(
                    m_pNode, src,
                    BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferSrcBuffer),
                    RHIAccessMode::eRead, {}, RDGContentEffect::eRead, true, false, source));

            if (valid)
            {
                m_pNode->contentAccesses.back().bufferOffset = region.bufferOffset;
                m_pNode->contentAccesses.back().bufferSize   = footprint;
                valid                                        = m_pRDG->DeclareTextureAccessForPass(
                    m_pNode, dst, RHITextureUsage::eTransferDst,
                    CopyRange(region.textureSubresources), RHIAccessMode::eReadWrite, {},
                    CoversMip(info, region.textureSubresources.mipmap, region.textureOffset,
                              region.textureSize) ?
                        RDGContentEffect::eFullWrite :
                        RDGContentEffect::eWrite,
                    true, false, destination);
            }

            if (valid)
            {
                RDGContentAccess& access  = m_pNode->contentAccesses.back();
                access.sourceResourceId   = src->id;
                access.sourceBufferOffset = region.bufferOffset;
                access.sourceBufferSize   = footprint;
                m_ops.push_back([src, dst, region](RDGPassCmdEncoder& encoder) {
                    encoder.CopyBufferToTexture(src->pBuffer, dst->pTexture, region);
                });
            }
        }
    }

    return *this;
}

RDGTransferPassCmdRecorder& RDGTransferPassCmdRecorder::ClearTexture(RDGTexture texture,
                                                                     const Color& color)
{
    bool valid = true;

    valid = m_pRDG->CheckRecorder(m_pNode, m_generation);

    if (valid)
    {
        const RDGResourceManager::Allocation* resource = m_pRDG->m_resourceManager.Resolve(texture);
        valid                                          = (resource != nullptr);
        valid                                          = valid &&
            (Check(!FormatIsDepthOnly(resource->texFormat.format) &&
                       !FormatIsDepthStencil(resource->texFormat.format) &&
                       !FormatIsStencilOnly(resource->texFormat.format),
                   RDGErrorCode::eRange, "ClearTexture requires a color texture"));

        if (valid)
        {
            RHITextureSubResourceRange range{};
            range.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
            range.levelCount = resource->texFormat.mipmaps;
            range.layerCount = resource->texFormat.arrayLayers;
            valid            = m_pRDG->DeclareTextureAccessForPass(
                m_pNode, resource, RHITextureUsage::eTransferDst, range, RHIAccessMode::eReadWrite,
                {}, RDGContentEffect::eFullWrite, true, false, texture);

            if (valid)
            {
                static_cast<RDGTransferPass*>(m_pNode->pCompiledPass)->requiresGraphicsQueue = true;
                m_ops.push_back([resource, color, range](RDGPassCmdEncoder& encoder) {
                    encoder.ClearTexture(resource->pTexture, color, range);
                });
            }
        }
    }

    return *this;
}

RDGTransferPassCmdRecorder& RDGTransferPassCmdRecorder::CopyBuffer(
    RDGBuffer source,
    RDGBuffer destination,
    const RHIBufferCopyRegion& region)
{
    bool valid = true;

    valid = m_pRDG->CheckRecorder(m_pNode, m_generation);

    if (valid)
    {
        const RDGResourceManager::Allocation* src = m_pRDG->m_resourceManager.Resolve(source);
        const RDGResourceManager::Allocation* dst = m_pRDG->m_resourceManager.Resolve(destination);
        valid                                     = !(src == nullptr || dst == nullptr);
        valid                                     = valid &&
            (Check(region.size > 0 && region.srcOffset <= src->bufferSize &&
                       region.size <= src->bufferSize - region.srcOffset &&
                       region.dstOffset <= dst->bufferSize &&
                       region.size <= dst->bufferSize - region.dstOffset &&
                       (src != dst || region.srcOffset + region.size <= region.dstOffset ||
                        region.dstOffset + region.size <= region.srcOffset),
                   RDGErrorCode::eRange, "Invalid logical buffer copy range"));
        valid = valid &&
            (m_pRDG->DeclareBufferAccessForPass(
                m_pNode, src,
                BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferSrcBuffer),
                RHIAccessMode::eRead, {}, RDGContentEffect::eRead, true, false, source));

        if (valid)
        {
            m_pNode->contentAccesses.back().bufferOffset = region.srcOffset;
            m_pNode->contentAccesses.back().bufferSize   = region.size;
            valid                                        = m_pRDG->DeclareBufferAccessForPass(
                m_pNode, dst,
                BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer),
                RHIAccessMode::eReadWrite, {},
                region.dstOffset == 0 && region.size == dst->bufferSize ?
                    RDGContentEffect::eFullWrite :
                    RDGContentEffect::eWrite,
                true, false, destination);
        }

        if (valid)
        {
            RDGContentAccess& destinationAccess  = m_pNode->contentAccesses.back();
            destinationAccess.sourceResourceId   = src->id;
            destinationAccess.bufferOffset       = region.dstOffset;
            destinationAccess.bufferSize         = region.size;
            destinationAccess.sourceBufferOffset = region.srcOffset;
            destinationAccess.sourceBufferSize   = region.size;
            m_ops.push_back([src, dst, region](RDGPassCmdEncoder& encoder) {
                encoder.CopyBuffer(src->pBuffer, dst->pBuffer, region);
            });
        }
    }

    return *this;
}

RDGTransferPassCmdRecorder& RDGTransferPassCmdRecorder::CopyTexture(
    RDGTexture source,
    RDGTexture destination,
    VectorView<RHITextureCopyRegion> regions)
{
    bool valid = true;

    valid = m_pRDG->CheckRecorder(m_pNode, m_generation);

    if (valid)
    {
        const RDGResourceManager::Allocation* src = m_pRDG->m_resourceManager.Resolve(source);
        const RDGResourceManager::Allocation* dst = m_pRDG->m_resourceManager.Resolve(destination);
        valid                                     = !(src == nullptr || dst == nullptr);
        valid                                     = valid &&
            (Check(!regions.empty() && regions.data() != nullptr &&
                       src->texFormat.format == dst->texFormat.format &&
                       src->texFormat.sampleCount == dst->texFormat.sampleCount,
                   RDGErrorCode::eRange,
                   "Logical texture copy requires matching formats/samples and regions"));

        if (valid)
        {
            RDGResult result;
            const RHITextureCreateInfo srcInfo = LogicalTextureInfo(src);
            const RHITextureCreateInfo dstInfo = LogicalTextureInfo(dst);

            for (RHITextureCopyRegion const& region : regions)
            {
                if (!ValidateTextureCopyBox(result, srcInfo, src->name, region.srcSubresources,
                                            region.srcOffset, region.size) ||
                    !ValidateTextureCopyBox(result, dstInfo, dst->name, region.dstSubresources,
                                            region.dstOffset, region.size) ||
                    !ValidateTextureCopyCapabilities(
                        result, srcInfo, dstInfo, region,
                        static_cast<RDGTransferPass*>(m_pNode->pCompiledPass)
                            ->requiresGraphicsQueue))
                {
                    m_pRDG->Fail(result.code,
                                 "Pass '" + m_pNode->tag.ToString() + "': " + result.message);
                    valid = false;
                }

                valid = valid &&
                    (Check(region.srcSubresources.layerCount == region.dstSubresources.layerCount &&
                               int64_t(region.srcSubresources.aspect) ==
                                   int64_t(region.dstSubresources.aspect),
                           RDGErrorCode::eRange,
                           "Copy source and destination subresources do not match"));

                if (valid)
                {
                    const Vec3i extent = MipExtent3D(dstInfo, region.dstSubresources.mipmap);
                    const bool full    = region.dstOffset == Vec3i(0) && region.size == extent;
                    valid = !((!m_pRDG->DeclareTextureAccessForPass(
                                   m_pNode, src, RHITextureUsage::eTransferSrc,
                                   CopyRange(region.srcSubresources), RHIAccessMode::eRead, {},
                                   RDGContentEffect::eRead, true, false, source) ||
                               !m_pRDG->DeclareTextureAccessForPass(
                                   m_pNode, dst, RHITextureUsage::eTransferDst,
                                   CopyRange(region.dstSubresources), RHIAccessMode::eReadWrite, {},
                                   full ? RDGContentEffect::eFullWrite : RDGContentEffect::eWrite,
                                   true, false, destination)));

                    if (valid)
                    {
                        m_pNode->contentAccesses.back().sourceResourceId = src->id;
                        m_pNode->contentAccesses.back().sourceRange =
                            CopyRange(region.srcSubresources);
                    }
                }

                if (!valid)
                {
                    break;
                }
            }

            if (valid)
            {
                HeapVector<RHITextureCopyRegion> copy(regions);
                m_ops.push_back([src, dst, copy = std::move(copy)](RDGPassCmdEncoder& encoder) {
                    encoder.CopyTexture(src->pTexture, dst->pTexture, copy);
                });
            }
        }
    }

    return *this;
}

} // namespace zen::rc
