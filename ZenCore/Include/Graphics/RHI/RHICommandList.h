#pragma once
#include "RHICommon.h"
#include "Memory/Memory.h"
#define ALLOC_CMD(...) new (AllocateCmd(sizeof(__VA_ARGS__), alignof(__VA_ARGS__))) __VA_ARGS__
namespace zen
{
class RHIDescriptorSet;
class RHIPipeline;

struct RHICommandBase
{
    RHICommandBase* nextCmd{nullptr};
};


class IRHICommandContext
{
public:
    virtual ~IRHICommandContext() {}

    virtual void RHISetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) = 0;

    virtual void RHISetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) = 0;

    virtual void RHIBindPipeline(RHIPipelineType pipelineType,
                                 RHIPipeline* pPipeline,
                                 uint32_t numDescriptorSets,
                                 RHIDescriptorSet* const* pDescriptorSets) = 0;

    virtual void RHIBindVertexBuffer(RHIBuffer* buffer, uint64_t offset) = 0;

    virtual void RHIBindIndexBuffer(RHIBuffer* buffer, DataFormat format, uint64_t offset) = 0;

    virtual void RHIDrawIndexed(RHIBuffer* buffer,
                                uint32_t indexCount,
                                uint32_t instanceCount,
                                uint32_t firstIndex,
                                int32_t vertexOffset,
                                uint32_t firstInstance) = 0;

    virtual void RHIDrawIndexedIndirect(RHIBuffer* indirectBuffer,
                                        RHIBuffer* indexBuffer,
                                        uint32_t offset,
                                        uint32_t drawCount,
                                        uint32_t stride) = 0;
};

class RHICommandListBase
{
public:
    virtual ~RHICommandListBase()
    {
        RHICommandBase* cmd = m_cmdHead;
        while (cmd)
        {
            RHICommandBase* next = cmd->nextCmd; // Save next before freeing
            DefaultAllocator::Free(cmd);
            cmd = next;
        }

        m_cmdHead         = nullptr;
        m_cmdPtr          = nullptr;
        m_numCommands     = 0;
        m_graphicsContext = nullptr;
        m_computeContext  = nullptr;
    }

    void* AllocateCmd(uint32_t size, uint32_t alignment)
    {
        RHICommandBase* cmd =
            static_cast<RHICommandBase*>(DefaultAllocator::Alloc(size, alignment));
        *m_cmdPtr = cmd;
        m_cmdPtr  = &cmd->nextCmd;
        ++m_numCommands;
        return cmd;
    }

    IRHICommandContext* GetContext() const
    {
        return m_graphicsContext;
    }

protected:
    RHICommandListBase()
    {
        m_cmdPtr = &m_cmdHead;
    }

    RHICommandBase* m_cmdHead{nullptr};
    RHICommandBase** m_cmdPtr{nullptr};

    IRHICommandContext* m_graphicsContext{nullptr};
    IRHICommandContext* m_computeContext{nullptr};

    uint32_t m_numCommands{0};
};

struct RHICommand : public RHICommandBase
{
    virtual ~RHICommand() {};

    virtual void Execute(RHICommandListBase& cmdList) = 0;
};

struct RHICommandBeginRendering final : public RHICommand
{};

struct RHICommandBindPipeline final : public RHICommand
{
    RHIPipelineType pipelineType;
    RHIPipeline* pPipeline;
    uint32_t numDescriptorSets;
    RHIDescriptorSet* const* pDescriptorSets;

    explicit RHICommandBindPipeline(RHIPipelineType pipelineType,
                                    RHIPipeline* pPipeline,
                                    uint32_t numDescriptorSets,
                                    RHIDescriptorSet* const* pDescriptorSets) :
        pipelineType(pipelineType),
        pPipeline(pPipeline),
        numDescriptorSets(numDescriptorSets),
        pDescriptorSets(pDescriptorSets)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIBindPipeline(pipelineType, pPipeline, numDescriptorSets,
                                              pDescriptorSets);
    }
};

struct RHICommandSetScissor final : public RHICommand
{
    uint32_t minX;
    uint32_t minY;
    uint32_t maxX;
    uint32_t maxY;

    RHICommandSetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) :
        minX(minX), minY(minY), maxX(maxX), maxY(maxY)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHISetScissor(minX, minY, maxX, maxY);
    }
};

struct RHICommandSetViewport final : public RHICommand
{
    uint32_t minX;
    uint32_t minY;
    uint32_t maxX;
    uint32_t maxY;

    RHICommandSetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) :
        minX(minX), minY(minY), maxX(maxX), maxY(maxY)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHISetViewport(minX, minY, maxX, maxY);
    }
};

struct RHICommandBindIndexBuffer final : public RHICommand
{
    RHIBuffer* indexBuffer;
    DataFormat format;
    uint64_t offset;

    RHICommandBindIndexBuffer(RHIBuffer* buffer, DataFormat format, uint64_t offset) :
        indexBuffer(buffer), format(format), offset(offset)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIBindIndexBuffer(indexBuffer, format, offset);
    }
};

struct RHICommandBindVertexBuffer final : public RHICommand
{
    RHIBuffer* vertexBuffer;
    uint64_t offset;

    RHICommandBindVertexBuffer(RHIBuffer* buffer, uint64_t offset) :
        vertexBuffer(buffer), offset(offset)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIBindVertexBuffer(vertexBuffer, offset);
    }
};

struct RHICommandDrawParam
{
    RHIBuffer* indexBuffer;
    uint32_t offset;
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t vertexOffset;
    uint32_t firstInstance;
};

struct RHICommandDrawIndexed final : public RHICommand
{
    // note: For now index buffer format is UINT32 only
    RHIBuffer* indexBuffer;
    uint32_t offset;
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t vertexOffset;
    uint32_t firstInstance;

    explicit RHICommandDrawIndexed(const RHICommandDrawParam& param) :
        indexBuffer(param.indexBuffer),
        offset(param.offset),
        indexCount(param.indexCount),
        instanceCount(param.instanceCount),
        firstIndex(param.firstIndex),
        vertexOffset(param.vertexOffset),
        firstInstance(param.firstInstance)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIDrawIndexed(indexBuffer, indexCount, instanceCount, firstIndex,
                                             vertexOffset, firstInstance);
    }
};

struct RHICommandDrawIndexedIndirect final : public RHICommand
{
    RHIBuffer* indirectBuffer;
    RHIBuffer* indexBuffer;
    uint32_t offset;
    uint32_t drawCount;
    uint32_t stride;

    RHICommandDrawIndexedIndirect(RHIBuffer* indirectBuffer,
                                  RHIBuffer* indexBuffer,
                                  uint32_t offset,
                                  uint32_t drawCount,
                                  uint32_t stride) :
        indirectBuffer(indirectBuffer),
        indexBuffer(indexBuffer),
        offset(offset),
        drawCount(drawCount),
        stride(stride)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIDrawIndexedIndirect(indirectBuffer, indexBuffer, offset, drawCount,
                                                     stride);
    }
};

class FRHICommandList : public RHICommandListBase
{
public:
    virtual void SetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY);

    virtual void SetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY);

    virtual void BindPipeline(RHIPipelineType pipelineType,
                              RHIPipeline* pPipeline,
                              uint32_t numDescriptorSets,
                              RHIDescriptorSet* const* pDescriptorSets);

    // All vertex attributes packed in 1 buffer
    virtual void BindVertexBuffer(RHIBuffer* buffer, uint64_t offset);

    virtual void DrawIndexed(const RHICommandDrawParam& param);
};
} // namespace zen