#include "Graphics/RHI/RHICommandList.h"

namespace zen
{
void FRHICommandList::SetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    ALLOC_CMD(RHICommandSetViewport)(minX, minY, maxX, maxY);
}

void FRHICommandList::SetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    ALLOC_CMD(RHICommandSetScissor)(minX, minY, maxX, maxY);
}

void FRHICommandList::BindGraphicsPipeline(RHIPipeline* pipeline)
{
    ALLOC_CMD(RHICommandBindGraphicsPipeline)(pipeline);
}

void FRHICommandList::BindVertexBuffer(RHIBuffer* buffer, uint64_t offset)
{
    ALLOC_CMD(RHICommandBindVertexBuffer)(buffer, offset);
}

void FRHICommandList::DrawIndexed(const RHICommandDrawParam& param)
{
    ALLOC_CMD(RHICommandDrawIndexed)(param);
}

} // namespace zen