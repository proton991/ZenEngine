#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/VulkanRHI/VulkanPlatformCommandList.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanDescriptorPool.h"
#include "Graphics/VulkanRHI/VulkanDescriptorState.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"
#include "Platform/Timer.h"
#include "Templates/HeapVector.h"
#include "Utils/Errors.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace zen
{
#if ZEN_VK_RHI_DEBUG
static const char* VulkanCommandBufferTypeToString(VulkanCommandBufferType type)
{
    const char* pName = "Unknown";

    switch (type)
    {
        case VulkanCommandBufferType::ePrimary: pName = "Primary"; break;
        default: break;
    }

    return pName;
}
#endif

template <typename T> static bool SameValues(const HeapVector<T>& left, const HeapVector<T>& right)
{
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

void FVulkanCommandBuffer::InvalidateCachedState()
{
    for (BoundPipelineState& state : m_boundStates)
    {
        state.pipeline         = VK_NULL_HANDLE;

        state.descriptorLayout = VK_NULL_HANDLE;

        state.firstSet         = 0;

        state.descriptorSets.clear();

        state.dynamicOffsets.clear();
    }

    m_validDynamicStates.Reset();

    m_boundVertexBuffers.clear();

    m_boundVertexOffsets.clear();

    m_boundIndexBuffer = VK_NULL_HANDLE;
}

void FVulkanCommandBuffer::BindIndexBuffer(VkBuffer buffer, uint64_t offset, VkIndexType type)
{
    if (m_boundIndexBuffer != buffer || m_boundIndexOffset != offset || m_boundIndexType != type)
    {
        vkCmdBindIndexBuffer(m_vkHandle, buffer, offset, type);

        m_boundIndexBuffer = buffer;

        m_boundIndexOffset = offset;

        m_boundIndexType   = type;
    }
}

void FVulkanCommandBuffer::BindPipelineAndDescriptorSets(VulkanPipeline*                    pipeline,
                                                         const HeapVector<VkDescriptorSet>& sets,
                                                         uint32_t                           firstSet,
                                                         const HeapVector<uint32_t>&        offsets)
{
    if (pipeline == nullptr)
    {
        m_error = {RHIErrorCode::eInvalidArgument, 0, "Bind pipeline and descriptors"};
    }
    else if (!m_error.IsFailure())
    {
        const VkPipelineBindPoint bindPoint = pipeline->GetVkPipelineBindPoint();

        BoundPipelineState& state           = m_boundStates[bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS ? 0 : 1];

        if (state.pipeline != pipeline->GetVkPipeline())
        {
            vkCmdBindPipeline(m_vkHandle, bindPoint, pipeline->GetVkPipeline());

            state.pipeline = pipeline->GetVkPipeline();

            if (bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS)
            {
                // A static pipeline can invalidate dynamic values. Conservatively re-emit
                // the next pipeline's dynamic state after every actual graphics pipeline bind.
                m_validDynamicStates.Reset();
            }
        }

        const VkPipelineLayout layout = pipeline->GetVkPipelineLayout();

        if (!sets.empty()
            && (state.descriptorLayout != layout || state.firstSet != firstSet || !SameValues(state.descriptorSets, sets)
                || !SameValues(state.dynamicOffsets, offsets)))
        {
            vkCmdBindDescriptorSets(m_vkHandle, bindPoint, layout, firstSet, static_cast<uint32_t>(sets.size()), sets.data(),
                                    static_cast<uint32_t>(offsets.size()), offsets.empty() ? nullptr : offsets.data());

            state.descriptorLayout = layout;

            state.firstSet         = firstSet;

            state.descriptorSets   = sets;

            state.dynamicOffsets   = offsets;
        }
    }
}

void FVulkanCommandBuffer::SetViewport(const VkViewport& viewport)
{
    const uint32_t bit = ToUnderlying(RHIDynamicState::eViewPort);

    if (!m_validDynamicStates.Test(bit) || m_viewport.x != viewport.x || m_viewport.y != viewport.y
        || m_viewport.width != viewport.width || m_viewport.height != viewport.height
        || m_viewport.minDepth != viewport.minDepth || m_viewport.maxDepth != viewport.maxDepth)
    {
        vkCmdSetViewport(m_vkHandle, 0, 1, &viewport);

        m_viewport = viewport;

        m_validDynamicStates.Set(bit);
    }
}

void FVulkanCommandBuffer::SetScissor(const VkRect2D& scissor)
{
    const uint32_t bit = ToUnderlying(RHIDynamicState::eScissor);

    if (!m_validDynamicStates.Test(bit) || m_scissor.offset.x != scissor.offset.x || m_scissor.offset.y != scissor.offset.y
        || m_scissor.extent.width != scissor.extent.width || m_scissor.extent.height != scissor.extent.height)
    {
        vkCmdSetScissor(m_vkHandle, 0, 1, &scissor);

        m_scissor = scissor;

        m_validDynamicStates.Set(bit);
    }
}

void FVulkanCommandBuffer::SetDepthBias(float constantFactor, float clamp, float slopeFactor)
{
    const uint32_t bit = ToUnderlying(RHIDynamicState::eDepthBias);

    if (!m_validDynamicStates.Test(bit) || m_depthBias[0] != constantFactor || m_depthBias[1] != clamp
        || m_depthBias[2] != slopeFactor)
    {
        vkCmdSetDepthBias(m_vkHandle, constantFactor, clamp, slopeFactor);

        m_depthBias[0] = constantFactor;

        m_depthBias[1] = clamp;

        m_depthBias[2] = slopeFactor;

        m_validDynamicStates.Set(bit);
    }
}

void FVulkanCommandBuffer::SetLineWidth(float width)
{
    const uint32_t bit = ToUnderlying(RHIDynamicState::eLineWidth);

    if (!m_validDynamicStates.Test(bit) || m_lineWidth != width)
    {
        vkCmdSetLineWidth(m_vkHandle, width);

        m_lineWidth = width;

        m_validDynamicStates.Set(bit);
    }
}

void FVulkanCommandBuffer::BindVertexBuffers(const HeapVector<VkBuffer>& buffers, const HeapVector<uint64_t>& offsets)
{
    if (!buffers.empty() && (!SameValues(m_boundVertexBuffers, buffers) || !SameValues(m_boundVertexOffsets, offsets)))
    {
        vkCmdBindVertexBuffers(m_vkHandle, 0, static_cast<uint32_t>(buffers.size()), buffers.data(), offsets.data());

        m_boundVertexBuffers = buffers;

        m_boundVertexOffsets = offsets;
    }
}

bool FVulkanCommandBuffer::Begin()
{
    DiscardGPUTimings();

    m_timestampsReset = false;

    InvalidateCachedState();

    m_breadcrumbNames.clear();

    m_openBreadcrumbs.clear();

    if (m_breadcrumbBuffer != VK_NULL_HANDLE)
    {
        std::memset(m_breadcrumbAllocation.info.pMappedData, 0, kMaxBreadcrumbs * 2 * sizeof(uint32_t));
    }

    VkResult result = VK_SUCCESS;

    if (!m_error.IsFailure() && m_state == State::eNeedReset)
    {
        result = vkResetCommandBuffer(m_vkHandle, 0);

        if (result == VK_SUCCESS)
        {
            m_state = State::eReadyForBegin;
        }
        else
        {
            m_error = MakeVulkanError(result, "vkResetCommandBuffer", __FILE__, __LINE__);
        }
    }

    if (!m_error.IsFailure() && m_state == State::eReadyForBegin)
    {
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        result          = vkBeginCommandBuffer(m_vkHandle, &beginInfo);

        if (result == VK_SUCCESS)
        {
            m_state = State::eIsInsideBegin;

            const VkQueueFamilyProperties& family =
                GVulkanRHI->GetDevice()->GetQueueFamilyProperties(m_pCmdBufferPool->GetQueue()->GetFamilyIndex());

            m_frameTimingInterval = GVulkanRHI->RegisterNativeGPUFrameRecording(
                (family.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0);

            m_nativeTimingRecording = true;

            BeginGPUTiming(m_frameTimingInterval);
        }
        else
        {
            m_error = MakeVulkanError(result, "vkBeginCommandBuffer", __FILE__, __LINE__);
        }
    }

    return !m_error.IsFailure() && HasBegun();
}

bool FVulkanCommandBuffer::End()
{
    if (!m_error.IsFailure() && IsOutsideRenderPass())
    {
        EndGPUTiming(m_frameTimingInterval);

        m_frameTimingInterval.Reset();

        if (m_breadcrumbBuffer != VK_NULL_HANDLE)
        {
            VkMemoryBarrier visibility{VK_STRUCTURE_TYPE_MEMORY_BARRIER};

            visibility.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

            visibility.dstAccessMask = VK_ACCESS_HOST_READ_BIT;

            vkCmdPipelineBarrier(m_vkHandle, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &visibility,
                                 0, nullptr, 0, nullptr);
        }

        const VkResult result = vkEndCommandBuffer(m_vkHandle);

        if (result == VK_SUCCESS)
        {
            m_state = State::eHasEnded;
        }
        else
        {
            m_error = MakeVulkanError(result, "vkEndCommandBuffer", __FILE__, __LINE__);
        }
    }

    return !m_error.IsFailure() && HasEnded();
}

void FVulkanCommandBuffer::BeginRendering(const VkRenderingInfo* pRenderingInfo)
{
    vkCmdBeginRenderingKHR(m_vkHandle, pRenderingInfo);

    m_state              = State::eIsInsideRenderPass;

    m_lastRenderingFlags = pRenderingInfo->flags;
}

void FVulkanCommandBuffer::EndRendering()
{
    vkCmdEndRenderingKHR(m_vkHandle);

    m_state = State::eIsInsideBegin;
}

void FVulkanCommandBuffer::SetSubmitted()
{
    LockAuto lock(m_pCmdBufferPool->GetMutex());

    // Native acceptance, rather than vkEndCommandBuffer, closes the capture boundary.
    if (m_nativeTimingRecording)
    {
        GVulkanRHI->ReleaseNativeGPUFrameRecording();

        m_nativeTimingRecording = false;
    }

    m_state      = State::eSubmitted;

    m_submitTime = platform::Timer::Now<>();
}

void FVulkanCommandBuffer::SetCompleted()
{
    LockAuto lock(m_pCmdBufferPool->GetMutex());

    if (m_state == State::eSubmitted)
    {
        ResolveGPUTimings();

        m_state = State::eNeedReset;
    }
}

void FVulkanCommandBuffer::Discard()
{
    LockAuto lock(m_pCmdBufferPool->GetMutex());

    VERIFY_EXPR_MSG(m_state != State::eSubmitted, "Cannot discard a submitted command buffer");

    DiscardGPUTimings();

    InvalidateCachedState();

    if (m_vkHandle != VK_NULL_HANDLE)
    {
        m_state = State::eNeedReset;
    }

    m_error = {};
}

VulkanCommandBufferType FVulkanCommandBuffer::GetCommandBufferType() const
{
    return m_pCmdBufferPool->GetCommandBufferType();
}

FVulkanCommandBuffer::FVulkanCommandBuffer(FVulkanCommandBufferPool* pPool) : m_pCmdBufferPool(pPool)
{
    AllocMemory();
}

FVulkanCommandBuffer::~FVulkanCommandBuffer()
{
    if (m_breadcrumbBuffer != VK_NULL_HANDLE)
    {
        GVulkanRHI->GetDevice()->UnregisterDiagnosticBuffer(this);

        GVkMemAllocator->FreeBuffer(m_breadcrumbBuffer, m_breadcrumbAllocation);
    }

    if (m_state != State::eNotAllocated)
    {
        FreeMemory();
    }
}

void FVulkanCommandBuffer::BeginBreadcrumb(NameID name)
{
    if (RHIOptions::GetInstance().DeviceLossDiagnostics())
    {
        uint32_t index = UINT32_MAX;

        if (IsOutsideRenderPass() && m_breadcrumbNames.size() < kMaxBreadcrumbs)
        {
            if (m_breadcrumbBuffer == VK_NULL_HANDLE)
            {
                VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};

                info.size  = kMaxBreadcrumbs * 2 * sizeof(uint32_t);

                info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

                if (GVkMemAllocator->AllocBuffer(info.size, &info, RHIBufferAllocateType::eCPURead, &m_breadcrumbBuffer,
                                                 &m_breadcrumbAllocation))
                {
                    std::memset(m_breadcrumbAllocation.info.pMappedData, 0, size_t(info.size));

                    GVulkanRHI->GetDevice()->RegisterDiagnosticBuffer(this);
                }
            }

            if (m_breadcrumbBuffer != VK_NULL_HANDLE)
            {
                index = static_cast<uint32_t>(m_breadcrumbNames.size());

                m_breadcrumbNames.push_back(name);

                WriteBreadcrumb(index, 1);
            }
        }

        m_openBreadcrumbs.push_back(index);
    }
}

void FVulkanCommandBuffer::EndBreadcrumb()
{
    if (!m_openBreadcrumbs.empty())
    {
        const uint32_t index = m_openBreadcrumbs.back();

        m_openBreadcrumbs.pop_back();

        if (index != UINT32_MAX && IsOutsideRenderPass())
        {
            WriteBreadcrumb(index, 2);
        }
    }
}

void FVulkanCommandBuffer::WriteBreadcrumb(uint32_t index, uint32_t value)
{
    if (GVulkanRHI->GetDevice()->GetExtensionFlags().hasBufferMarker && vkCmdWriteBufferMarkerAMD != nullptr)
    {
        vkCmdWriteBufferMarkerAMD(m_vkHandle,
                                  value == 1 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                  m_breadcrumbBuffer, (uint64_t(index) * 2 + value - 1) * sizeof(uint32_t), 1);
    }
    else
    {
        // The fill waits for all earlier work, so a marker means everything recorded before it
        // has finished. Later work does not wait for the fill: blocking it would serialize every
        // pass boundary. End() makes the fills visible to the host.
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};

        barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;

        barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;

        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

        barrier.buffer              = m_breadcrumbBuffer;

        barrier.offset              = (uint64_t(index) * 2 + value - 1) * sizeof(uint32_t);

        barrier.size                = sizeof(uint32_t);

        vkCmdPipelineBarrier(m_vkHandle, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                             &barrier, 0, nullptr);

        vkCmdFillBuffer(m_vkHandle, m_breadcrumbBuffer, barrier.offset, barrier.size, value);
    }
}

void FVulkanCommandBuffer::ReportBreadcrumbs() const
{
    const volatile uint32_t* values = static_cast<const volatile uint32_t*>(m_breadcrumbAllocation.info.pMappedData);

    const VulkanQueue* queue        = m_pCmdBufferPool->GetQueue();

    const char* started             = "none observed";

    const char* completed           = "none observed";

    for (uint32_t i = 0; values != nullptr && i < m_breadcrumbNames.size(); ++i)
    {
        const uint32_t startedValue   = values[i * 2];

        const uint32_t completedValue = values[i * 2 + 1];

        if (startedValue != 0 || completedValue != 0)
        {
            started = m_breadcrumbNames[i].CStr();
        }

        if (completedValue != 0)
        {
            completed = m_breadcrumbNames[i].CStr();
        }
    }

    std::fprintf(stderr, "RHI GPU breadcrumb queue=%u:%u buffer=%p last_started=%s last_completed=%s\n",
                 queue->GetFamilyIndex(), queue->GetQueueIndex(), reinterpret_cast<void*>(m_vkHandle), started, completed);
}

void FVulkanCommandBuffer::AllocMemory()
{
    VkCommandBufferAllocateInfo allocInfo;

    InitVkStruct(allocInfo, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);

    allocInfo.commandPool        = m_pCmdBufferPool->GetVkHandle();

    allocInfo.commandBufferCount = 1;

    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

    const VkResult result        = allocInfo.commandPool != VK_NULL_HANDLE
                                     ? vkAllocateCommandBuffers(GVulkanRHI->GetVkDevice(), &allocInfo, &m_vkHandle)
                                     : VK_ERROR_INITIALIZATION_FAILED;

    if (result == VK_SUCCESS && m_vkHandle != VK_NULL_HANDLE)
    {
        m_state = State::eReadyForBegin;

        m_error = {};
    }
    else
    {
        m_error = MakeVulkanError(result, "vkAllocateCommandBuffers", __FILE__, __LINE__);
    }
}

void FVulkanCommandBuffer::FreeMemory()
{
    DiscardGPUTimings();

    ReleaseGPUTimingPool();

    vkFreeCommandBuffers(GVulkanRHI->GetVkDevice(), m_pCmdBufferPool->GetVkHandle(), 1, &m_vkHandle);

    m_state    = State::eNotAllocated;

    m_vkHandle = VK_NULL_HANDLE;
}

void FVulkanCommandBuffer::BeginGPUTiming(const RHIGPUTimingPtr& result)
{
    if (result != nullptr && result->GetStatus() == RHIGPUTimingStatus::ePending)
    {
        RHIGPUTimingStatus status = RHIGPUTimingStatus::ePending;

        if (!IsOutsideRenderPass())
        {
            status = RHIGPUTimingStatus::eError;
        }
        else if (m_timingScopes.size() >= kMaxGPUTimingScopes)
        {
            status = RHIGPUTimingStatus::eDropped;
        }
        else if (std::any_of(m_timingScopes.begin(), m_timingScopes.end(),
                             [&result](const GPUTimingScope& scope) { return scope.result == result; }))
        {
            status = RHIGPUTimingStatus::eError;
        }
        else
        {
            status = PrepareGPUTimingPool();
        }

        if (status == RHIGPUTimingStatus::ePending)
        {
            if (!m_timestampsReset)
            {
                vkCmdResetQueryPool(m_vkHandle, m_timestampPool, 0, 2 * kMaxGPUTimingScopes);

                m_timestampsReset = true;
            }

            const uint32_t query = static_cast<uint32_t>(m_timingScopes.size()) * 2;

            m_timingScopes.push_back({result, false});

            vkCmdWriteTimestamp(m_vkHandle, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_timestampPool, query);
        }
        else
        {
            result->Publish(status);
        }
    }
}

RHIGPUTimingStatus FVulkanCommandBuffer::PrepareGPUTimingPool()
{
    RHIGPUTimingStatus status = RHIGPUTimingStatus::ePending;

    if (m_timestampPool == VK_NULL_HANDLE)
    {
        VulkanDevice* device = GVulkanRHI->GetDevice();

        const VkQueueFamilyProperties& family =
            device->GetQueueFamilyProperties(m_pCmdBufferPool->GetQueue()->GetFamilyIndex());

        m_timestampValidBits = family.timestampValidBits;

        m_timestampPeriod    = device->GetPhysicalDeviceProperties().limits.timestampPeriod;

        // Command-buffer query resets require graphics or compute capability.
        // Dedicated transfer timing would need the optional hostQueryReset feature.
        if ((family.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0 || m_timestampValidBits == 0
            || m_timestampValidBits > 64 || !std::isfinite(m_timestampPeriod) || m_timestampPeriod <= 0
            || vkCreateQueryPool == nullptr || vkDestroyQueryPool == nullptr || vkCmdResetQueryPool == nullptr
            || vkCmdWriteTimestamp == nullptr || vkGetQueryPoolResults == nullptr)
        {
            status = RHIGPUTimingStatus::eUnsupported;
        }
        else
        {
            const VkResult acquired = device->AcquireGPUTimingPool(m_timestampPool);

            if (acquired != VK_SUCCESS)
            {
                m_timestampPool = VK_NULL_HANDLE;

                status = acquired == VK_ERROR_TOO_MANY_OBJECTS ? RHIGPUTimingStatus::eDropped : RHIGPUTimingStatus::eError;
            }
        }
    }

    return status;
}

void FVulkanCommandBuffer::EndGPUTiming(const RHIGPUTimingPtr& result)
{
    if (result != nullptr && result->GetStatus() == RHIGPUTimingStatus::ePending)
    {
        bool ended = false;

        for (size_t index = m_timingScopes.size(); index > 0 && !ended; --index)
        {
            GPUTimingScope& scope = m_timingScopes[index - 1];

            if (scope.result == result && !scope.ended && IsOutsideRenderPass())
            {
                vkCmdWriteTimestamp(m_vkHandle, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_timestampPool,
                                    static_cast<uint32_t>(index - 1) * 2 + 1);

                scope.ended = true;

                ended       = true;
            }
        }

        if (!ended)
        {
            result->Publish(RHIGPUTimingStatus::eError);
        }
    }
}

void FVulkanCommandBuffer::ResolveGPUTimings()
{
    if (!m_timingScopes.empty())
    {
        struct TimestampValue
        {
            uint64_t ticks{0};

            uint64_t available{0};
        };

        std::array<TimestampValue, 2 * kMaxGPUTimingScopes> values{};

        const uint32_t count = static_cast<uint32_t>(m_timingScopes.size()) * 2;

        // The queue's fence/timeline proves completion. Never wait for a profiler query.
        const VkResult status = vkGetQueryPoolResults(GVulkanRHI->GetVkDevice(), m_timestampPool, 0, count,
                                                      count * sizeof(TimestampValue), values.data(), sizeof(TimestampValue),
                                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

        for (uint32_t index = 0; index < m_timingScopes.size(); ++index)
        {
            GPUTimingScope& scope = m_timingScopes[index];

            const bool available  = scope.ended && (status == VK_SUCCESS || status == VK_NOT_READY)
                                && values[2 * index].available != 0 && values[2 * index + 1].available != 0;

            if (available)
            {
                scope.result->PublishTimestamps(
                    {values[2 * index].ticks, values[2 * index + 1].ticks, m_timestampValidBits, m_timestampPeriod});
            }
            else
            {
                scope.result->Publish(scope.ended ? RHIGPUTimingStatus::eError : RHIGPUTimingStatus::eDiscarded);
            }
        }

        m_timingScopes.clear();

        ReleaseGPUTimingPool();
    }
}

void FVulkanCommandBuffer::DiscardGPUTimings()
{
    if (m_nativeTimingRecording)
    {
        GVulkanRHI->ReleaseNativeGPUFrameRecording();

        m_nativeTimingRecording = false;
    }

    m_frameTimingInterval.Reset();

    for (GPUTimingScope& scope : m_timingScopes)
    {
        scope.result->Publish(RHIGPUTimingStatus::eDiscarded);
    }

    m_timingScopes.clear();
}

void FVulkanCommandBuffer::ReleaseGPUTimingPool()
{
    if (m_timestampPool != VK_NULL_HANDLE)
    {
        GVulkanRHI->GetDevice()->ReleaseGPUTimingPool(m_timestampPool);

        m_timestampPool = VK_NULL_HANDLE;
    }
}

FVulkanCommandBufferPool::FVulkanCommandBufferPool(VulkanQueue* pQueue, VulkanCommandBufferType type) :
    m_pQueue(pQueue), m_type(type)
{
    VkCommandPoolCreateInfo cmdPoolCI;

    InitVkStruct(cmdPoolCI, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);

    cmdPoolCI.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // reset cmd buffer

    cmdPoolCI.queueFamilyIndex = pQueue->GetFamilyIndex();

    const VkResult result      = vkCreateCommandPool(GVulkanRHI->GetVkDevice(), &cmdPoolCI, nullptr, &m_vkHandle);

    if (result != VK_SUCCESS)
    {
        LOGE("vkCreateCommandPool failed: {}", GetResultString(result));
    }
}

FVulkanCommandBufferPool::~FVulkanCommandBufferPool()
{
#if ZEN_VK_RHI_DEBUG
    LOGI("[VulkanCmdBufferPool] destroy type={} requests={} readyReuses={} freeReuses={} "
         "allocations={} inUse={} free={}",
         VulkanCommandBufferTypeToString(m_type), m_numCmdBufferRequests, m_numReadyCmdBufferReuses, m_numFreeCmdBufferReuses,
         m_numCmdBufferAllocations, m_cmdBuffersInUse.size(), m_cmdBuffersFree.size());
#endif

    for (uint32_t i = 0; i < m_cmdBuffersInUse.size(); i++)
    {
        FVulkanCommandBuffer* pCmdBuffer = m_cmdBuffersInUse[i];

        pCmdBuffer->~FVulkanCommandBuffer();

        ZEN_MEM_FREE(pCmdBuffer);
    }

    for (uint32_t i = 0; i < m_cmdBuffersFree.size(); i++)
    {
        FVulkanCommandBuffer* pCmdBuffer = m_cmdBuffersFree[i];

        pCmdBuffer->~FVulkanCommandBuffer();

        ZEN_MEM_FREE(pCmdBuffer);
    }

    vkDestroyCommandPool(GVulkanRHI->GetVkDevice(), m_vkHandle, nullptr);
}

void FVulkanCommandBufferPool::FreeUnusedCommandBuffers()
{
    LockAuto lock(&m_mutex);

    const double currentTime                       = platform::Timer::Now<>();

    HeapVector<FVulkanCommandBuffer*>::iterator it = m_cmdBuffersInUse.end();

    while (it != m_cmdBuffersInUse.begin())
    {
        --it;

        FVulkanCommandBuffer* pCmdBuffer = *it;

        if ((pCmdBuffer->m_state == FVulkanCommandBuffer::State::eReadyForBegin
             || pCmdBuffer->m_state == FVulkanCommandBuffer::State::eNeedReset)
            && (currentTime - pCmdBuffer->m_submitTime) > 10.0f)
        {
            pCmdBuffer->FreeMemory();

            it = m_cmdBuffersInUse.erase(it);

            m_cmdBuffersFree.push_back(pCmdBuffer);
        }
    }
}

FVulkanCommandBuffer* FVulkanCommandBufferPool::CreateCmdBuffer()
{
    FVulkanCommandBuffer* result{};

    if (!m_cmdBuffersFree.empty())
    {
        FVulkanCommandBuffer* pCmdBuffer = m_cmdBuffersFree[0];

        m_cmdBuffersFree.remove(0);

        pCmdBuffer->AllocMemory();

        m_cmdBuffersInUse.emplace_back(pCmdBuffer);

        result = pCmdBuffer;
    }
    else
    {
        FVulkanCommandBuffer* pCmdBuffer = static_cast<FVulkanCommandBuffer*>(ZEN_MEM_ALLOC(sizeof(FVulkanCommandBuffer)));

        new (pCmdBuffer) FVulkanCommandBuffer(this);

        m_cmdBuffersInUse.emplace_back(pCmdBuffer);

        result = pCmdBuffer;
    }

    return result;
}

VulkanWorkload::~VulkanWorkload() {}

FVulkanCommandBuffer* VulkanWorkload::GetLastCommandBuffer() const
{
    return m_commandBuffers.empty() ? nullptr : m_commandBuffers.back();
}

void VulkanWorkload::AddCommandBuffer(FVulkanCommandBuffer* pCmdBuffer)
{
    VERIFY_EXPR(pCmdBuffer != nullptr);

    m_commandBuffers.push_back(pCmdBuffer);
}

void VulkanWorkload::Merge(VulkanWorkload* pOtherWorkload)
{
    VERIFY_EXPR(pOtherWorkload != nullptr);

    VERIFY_EXPR(pOtherWorkload->m_pQueue == m_pQueue);

    VERIFY_EXPR(m_signalSemaphoreInfos.empty());

    VERIFY_EXPR(pOtherWorkload->m_waitSemaphoreInfos.empty());

    m_commandBuffers.push_back(pOtherWorkload->m_commandBuffers);

    pOtherWorkload->m_commandBuffers.clear();

    m_signalSemaphoreInfos.push_back(pOtherWorkload->m_signalSemaphoreInfos);

    pOtherWorkload->m_signalSemaphoreInfos.clear();

    // Keep duplicate IDs: each source recording contributed its own pending count.
    m_lifetimeIds.push_back(pOtherWorkload->m_lifetimeIds);

    pOtherWorkload->m_lifetimeIds.clear();

    pOtherWorkload->m_pMergedInto = this;

    m_mergedWorkloads.push_back(pOtherWorkload);
}

VulkanCommandContextBase::~VulkanCommandContextBase()
{
    // CollectWorkloads transfers ownership out of these lists before queue submission.
    VulkanCommandContextBase::DiscardRecording();

    m_pQueue->RecycleCommandBufferPool(m_pCmdBufferPool);
}

bool VulkanCommandContextBase::HasWorkloadData(const VulkanWorkload* pWorkload) const
{
    return pWorkload != nullptr
        && (pWorkload->HasCommandBuffers() || !pWorkload->m_waitSemaphoreInfos.empty()
            || !pWorkload->m_signalSemaphoreInfos.empty() || !pWorkload->m_lifetimeIds.empty()
            || pWorkload->m_resources.GetCount() != 0);
}

VulkanWorkload* VulkanCommandContextBase::GetWorkload(WorkloadPhase phase)
{
    if (m_pCurrentWorkload != nullptr && phase < m_currentWorkloadPhase)
    {
        FinalizePendingWorkload();
    }

    if (m_pCurrentWorkload == nullptr)
    {
        StartWorkload();
    }

    m_currentWorkloadPhase = phase;

    return m_pCurrentWorkload;
}

void VulkanCommandContextBase::CollectWorkloads(HeapVector<VulkanWorkload*>& outWorkloads)
{
    FinalizePendingWorkload();

    if (!m_finalizedWorkloads.empty())
    {
        outWorkloads.push_back(m_finalizedWorkloads);

        m_finalizedWorkloads.clear();
    }
}

void VulkanCommandContextBase::FinalizePendingWorkload()
{
    if (m_pCurrentWorkload != nullptr)
    {
        VERIFY_EXPR(m_pCurrentWorkload->m_pQueue == m_pQueue);

        if (HasWorkloadData(m_pCurrentWorkload))
        {
            EndWorkload();

            m_finalizedWorkloads.push_back(m_pCurrentWorkload);
        }
        else
        {
            m_pQueue->ReleaseWorkload(m_pCurrentWorkload);
        }

        m_pCurrentWorkload     = nullptr;

        m_currentWorkloadPhase = WorkloadPhase::eWait;
    }
}

RHISubmissionResult VulkanCommandContextBase::SubmitRecordedWorkloads()
{
    RHISubmissionResult result = RHISubmissionResult::eSuccess;

    HeapVector<VulkanWorkload*> workloads;

    CollectWorkloads(workloads);

    const uint64_t transaction = DetachRecordingTransaction();

    if (m_recordingError.IsFailure())
    {
        result =
            m_recordingError.code == RHIErrorCode::eDeviceLost ? RHISubmissionResult::eFatal : RHISubmissionResult::eRejected;

        for (VulkanWorkload* workload : workloads)
        {
            m_pQueue->DiscardWorkload(workload);
        }

        DiscardRecording();
    }
    else
    {
        for (VulkanWorkload* workload : workloads)
        {
            m_pQueue->EnqueueWorkload(workload);
        }

        if (!workloads.empty())
        {
            uint64_t serial = 0;

            result =
                GVulkanRHI->AreSubmissionsBlocked() ? RHISubmissionResult::eFatal : m_pQueue->SubmitPendingWorkloads(serial);

            SetLastSubmittedSerial(serial);

            if (result != RHISubmissionResult::eSuccess)
            {
                m_pQueue->DiscardPendingWorkloads(result == RHISubmissionResult::eFatal);

                GVulkanRHI->BlockSubmissions();
            }
            else
            {
                m_pQueue->ProcessPendingWorkloads(0);
            }
        }
    }

    GVulkanRHI->GetBindlessDescriptorPoolManager()->ResolveTransaction(
        transaction, result == RHISubmissionResult::eSuccess || result == RHISubmissionResult::eFatal);

    return result;
}

void VulkanCommandContextBase::RecordLifetime(uint64_t id)
{
    if (id != 0)
    {
        HeapVector<uint64_t>& ids = GetWorkload(WorkloadPhase::eExecute)->m_lifetimeIds;

        if ((ids.empty() || ids.back() != id) && std::find(ids.begin(), ids.end(), id) == ids.end())
        {
            ids.push_back(id);

            GVulkanRHI->GetLifetimeTracker().RetainRecording(id);
        }
    }
}

void VulkanCommandContextBase::RecordResource(RHIResource* resource)
{
    if (resource != nullptr && !m_recordingError.IsFailure())
    {
        GetWorkload(WorkloadPhase::eExecute)->m_resources.Retain(resource);
    }
}

void VulkanCommandContextBase::LatchError(RHIError error)
{
    if (error.IsFailure())
    {
        if (!m_recordingError.IsFailure())
        {
            m_recordingError = error;

            LOGE("RHI recording failed in {} (native {})", error.operation, error.nativeCode);
        }

        if (error.code == RHIErrorCode::eDeviceLost)
        {
            GVulkanRHI->BlockSubmissions();
        }
    }
}

bool VulkanCommandContextBase::EnsureRecording()
{
    if (!m_recordingError.IsFailure())
    {
        FVulkanCommandBuffer* buffer = GetCommandBuffer();

        LatchError(buffer->GetError());
    }

    return !m_recordingError.IsFailure();
}

void VulkanCommandContextBase::DiscardRecording()
{
    if (m_pCurrentWorkload != nullptr)
    {
        m_pQueue->DiscardWorkload(m_pCurrentWorkload);

        m_pCurrentWorkload = nullptr;
    }

    for (VulkanWorkload* workload : m_finalizedWorkloads)
    {
        m_pQueue->DiscardWorkload(workload);
    }

    m_finalizedWorkloads.clear();

    GVulkanRHI->GetBindlessDescriptorPoolManager()->ResolveTransaction(m_recordingTransaction, false);

    m_recordingTransaction = 0;

    m_currentWorkloadPhase = WorkloadPhase::eWait;

    m_recordingError       = {};
}

uint64_t VulkanCommandContextBase::GetRecordingTransaction()
{
    if (m_recordingTransaction == 0)
    {
        m_recordingTransaction = GVulkanRHI->GetBindlessDescriptorPoolManager()->BeginTransaction();
    }

    return m_recordingTransaction;
}

uint64_t VulkanCommandContextBase::DetachRecordingTransaction()
{
    const uint64_t transaction = m_recordingTransaction;

    m_recordingTransaction     = 0;

    return transaction;
}

void VulkanCommandContextBase::SetRecordingTransaction(uint64_t transaction)
{
    if (m_recordingTransaction == 0)
    {
        m_recordingTransaction = transaction;
    }
}

void VulkanCommandContextBase::RecordDescriptorPool(VulkanDescriptorPoolSetContainer* pContainer)
{
    if (pContainer != nullptr)
    {
        RecordLifetime(pContainer->GetLifetimeId());
    }
}

void VulkanCommandContextBase::RecordUniformBufferBlock(uint64_t blockId)
{
    RecordLifetime(GVulkanRHI->GetUniformBufferAllocator()->GetBlockLifetime(blockId));
}

void VulkanCommandContextBase::WaitForLastSubmittedWork(uint64_t timeToWaitNS)
{
    VERIFY_EXPR(!(m_lastSubmittedSerial == 0 && m_hasPendingFlushWorkload));

    if (m_lastSubmittedSerial != 0)
    {
        if (m_pQueue->WaitForCompletion(m_lastSubmittedSerial, timeToWaitNS))
        {
            m_lastSubmittedSerial = 0;
        }
        else
        {
            LOGE("Vulkan command context: submission {} did not complete", m_lastSubmittedSerial);
        }
    }
}

void VulkanCommandContextBase::SetupNewCommandBuffer()
{
    LockAuto lock(&m_pCmdBufferPool->m_mutex);

#if ZEN_VK_RHI_DEBUG
    ++m_pCmdBufferPool->m_numCmdBufferRequests;
#endif
    FVulkanCommandBuffer* pCmdBuffer = nullptr;

    for (uint32_t i = 0; i < m_pCmdBufferPool->m_cmdBuffersInUse.size(); i++)
    {
        FVulkanCommandBuffer* pCurrent = m_pCmdBufferPool->m_cmdBuffersInUse[i];

        if (pCurrent->m_state == FVulkanCommandBuffer::State::eReadyForBegin
            || pCurrent->m_state == FVulkanCommandBuffer::State::eNeedReset)
        {
            pCmdBuffer = pCurrent;
        }
        else if (pCurrent->m_state == FVulkanCommandBuffer::State::eNotAllocated)
        {
            pCurrent->AllocMemory();

            pCmdBuffer = pCurrent;
        }
        else
        {
            VERIFY_EXPR(pCurrent->IsSubmitted() || pCurrent->HasEnded());
        }
    }

    if (!pCmdBuffer)
    {
#if ZEN_VK_RHI_DEBUG
        const bool reuseFreeCmdBuffer = !m_pCmdBufferPool->m_cmdBuffersFree.empty();
#endif
        pCmdBuffer = m_pCmdBufferPool->CreateCmdBuffer();
#if ZEN_VK_RHI_DEBUG
        if (reuseFreeCmdBuffer)
        {
            ++m_pCmdBufferPool->m_numFreeCmdBufferReuses;
        }
        else
        {
            ++m_pCmdBufferPool->m_numCmdBufferAllocations;
        }
#endif
    }

#if ZEN_VK_RHI_DEBUG
    else
    {
        ++m_pCmdBufferPool->m_numReadyCmdBufferReuses;
    }
#endif

    m_pCurrentWorkload->AddCommandBuffer(pCmdBuffer);

    if (!pCmdBuffer->Begin())
    {
        LatchError(pCmdBuffer->GetError());
    }
}

void VulkanCommandContextBase::StartWorkload()
{
    VERIFY_EXPR(m_pCurrentWorkload == nullptr);

    ++m_workloadGeneration;

    m_pCurrentWorkload     = m_pQueue->AcquireWorkload();

    m_currentWorkloadPhase = WorkloadPhase::eWait;
}

void VulkanCommandContextBase::EndWorkload()
{
    if (m_pCurrentWorkload != nullptr)
    {
        FVulkanCommandBuffer* pCommandBuffer = m_pCurrentWorkload->GetLastCommandBuffer();

        if (pCommandBuffer != nullptr && !pCommandBuffer->HasEnded())
        {
            if (pCommandBuffer->IsInsideRenderPass()
                && pCommandBuffer->GetCommandBufferType() == VulkanCommandBufferType::ePrimary)
            {
                pCommandBuffer->EndRendering();
            }

            if (!pCommandBuffer->End())
            {
                LatchError(pCommandBuffer->GetError());
            }
        }
    }
}

void VulkanGfxState::SetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    m_viewports.resize(1);

    m_viewports[0]          = {};

    m_viewports[0].x        = minX;

    m_viewports[0].y        = minY;

    m_viewports[0].width    = maxX - minX;

    m_viewports[0].height   = maxY - minY;

    m_viewports[0].minDepth = 0.0f;

    m_viewports[0].maxDepth = 1.0f;
}

void VulkanGfxState::SetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    m_scissors.resize(1);

    m_scissors[0].offset.x      = minX;

    m_scissors[0].offset.y      = minY;

    m_scissors[0].extent.width  = maxX - minX;

    m_scissors[0].extent.height = maxY - minY;
}

void VulkanGfxState::SetBlendConstants(float r, float g, float b, float a)
{
    m_blendConstants[0] = r;

    m_blendConstants[1] = g;

    m_blendConstants[2] = b;

    m_blendConstants[3] = a;
}

void VulkanGfxState::SetLineWidth(float lineWidth)
{
    m_rasterizationState.lineWidth = lineWidth;
}

void VulkanGfxState::SetDepthBias(float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor)
{
    m_rasterizationState.depthBiasConstantFactor = depthBiasConstantFactor;

    m_rasterizationState.depthBiasClamp          = depthBiasClamp;

    m_rasterizationState.depthBiasSlopeFactor    = depthBiasSlopeFactor;
}

void VulkanGfxState::SetVertexBuffers(uint32_t numVertexBuffers, RHIBuffer* const* ppVertexBuffers, const uint64_t* pOffsets)
{
    m_vertexBuffers.resize(numVertexBuffers);

    m_vertexBufferOffsets.resize(numVertexBuffers);

    for (uint32_t i = 0; i < numVertexBuffers; i++)
    {
        m_vertexBuffers[i]       = TO_VK_BUFFER(ppVertexBuffers[i])->GetVkBuffer();

        m_vertexBufferOffsets[i] = pOffsets[i];
    }
}

VulkanGfxState::VulkanGfxState()
{
    m_pDescriptorSetState = ZEN_NEW() VulkanDescriptorSetState();
}

VulkanGfxState::~VulkanGfxState()
{
    ZEN_DELETE(m_pDescriptorSetState);

    m_pDescriptorSetState = nullptr;
}

void VulkanGfxState::SetPipelineState(RHIPipeline* pPipeline)
{
    m_pCurrentPipeline = TO_VK_PIPELINE(pPipeline);

    m_pDescriptorSetState->SetPipeline(m_pCurrentPipeline);
}

bool VulkanGfxState::SetShaderParameters(RHIShaderParameterView parameters, uint64_t recordedEpoch, uint64_t transaction)
{
    return m_pCurrentPipeline != nullptr && m_pDescriptorSetState->SetShaderParameters(parameters, recordedEpoch, transaction);
}

bool VulkanGfxState::PreDraw(FVulkanCommandListContext* pContext)
{
    bool ready = m_pCurrentPipeline != nullptr && pContext->EnsureRecording();

    if (ready)
    {
        FVulkanCommandBuffer* commandBuffer = pContext->GetCommandBuffer();

        uint32_t firstSet                   = 0;

        // Always resolve/retain descriptor pools: cache eviction and a new workload can
        // require work even when the native binding commands themselves are unchanged.
        ready = m_pDescriptorSetState->FlushPendingDescriptorWrites(pContext, m_descriptorSets, firstSet, m_dynamicOffsets);

        if (ready)
        {
            commandBuffer->BindPipelineAndDescriptorSets(m_pCurrentPipeline, m_descriptorSets, firstSet, m_dynamicOffsets);

            // Dynamic commands must follow the last static pipeline that invalidated them.
            // Emit only states declared dynamic by the active pipeline.
            if (m_pCurrentPipeline->UsesDynamicState(RHIDynamicState::eViewPort) && !m_viewports.empty())
            {
                commandBuffer->SetViewport(m_viewports[0]);
            }

            if (m_pCurrentPipeline->UsesDynamicState(RHIDynamicState::eScissor) && !m_scissors.empty())
            {
                commandBuffer->SetScissor(m_scissors[0]);
            }

            if (m_pCurrentPipeline->UsesDynamicState(RHIDynamicState::eDepthBias))
            {
                commandBuffer->SetDepthBias(m_rasterizationState.depthBiasConstantFactor, m_rasterizationState.depthBiasClamp,
                                            m_rasterizationState.depthBiasSlopeFactor);
            }

            if (m_pCurrentPipeline->UsesDynamicState(RHIDynamicState::eLineWidth))
            {
                commandBuffer->SetLineWidth(m_rasterizationState.lineWidth);
            }

            // Blend constants are static pipeline state in the current RHI dynamic-state enum.
            // Procedural draws declare no vertex inputs. Rebinding a previous draw's
            // cached handles would introduce an undeclared use after their owner retires.
            const VulkanShader* shader = TO_VK_SHADER(m_pCurrentPipeline->GetShader());

            if (shader->GetVertexInputStateCreateInfoData()->vertexBindingDescriptionCount != 0)
            {
                commandBuffer->BindVertexBuffers(m_vertexBuffers, m_vertexBufferOffsets);
            }
        }
    }

    if (!ready)
    {
        pContext->LatchError({RHIErrorCode::eInvalidArgument, 0, "Prepare shader descriptors"});
    }

    return ready;
}

VulkanComputeState::VulkanComputeState()
{
    m_pDescriptorSetState = ZEN_NEW() VulkanDescriptorSetState();
}

VulkanComputeState::~VulkanComputeState()
{
    ZEN_DELETE(m_pDescriptorSetState);

    m_pDescriptorSetState = nullptr;
}

void VulkanComputeState::SetPipelineState(RHIPipeline* pPipeline)
{
    m_pCurrentPipeline = TO_VK_PIPELINE(pPipeline);

    m_pDescriptorSetState->SetPipeline(m_pCurrentPipeline);
}

bool VulkanComputeState::SetShaderParameters(RHIShaderParameterView parameters, uint64_t recordedEpoch, uint64_t transaction)
{
    return m_pCurrentPipeline != nullptr && m_pDescriptorSetState->SetShaderParameters(parameters, recordedEpoch, transaction);
}

bool VulkanComputeState::PreDispatch(FVulkanCommandListContext* pContext)
{
    bool ready = m_pCurrentPipeline != nullptr && pContext->EnsureRecording();

    if (ready)
    {
        FVulkanCommandBuffer* commandBuffer = pContext->GetCommandBuffer();

        uint32_t firstSet                   = 0;

        ready = m_pDescriptorSetState->FlushPendingDescriptorWrites(pContext, m_descriptorSets, firstSet, m_dynamicOffsets);

        if (ready)
        {
            commandBuffer->BindPipelineAndDescriptorSets(m_pCurrentPipeline, m_descriptorSets, firstSet, m_dynamicOffsets);
        }
    }

    if (!ready)
    {
        pContext->LatchError({RHIErrorCode::eInvalidArgument, 0, "Prepare shader descriptors"});
    }

    return ready;
}

FVulkanCommandListContext::FVulkanCommandListContext(RHICommandContextType contextType, VulkanDevice* pDevice) :
    VulkanCommandContextBase(pDevice->GetQueue(contextType), VulkanCommandBufferType::ePrimary),
    m_contextType(contextType),
    m_pDevice(pDevice)
{
    m_pGfxState     = ZEN_NEW() VulkanGfxState();

    m_pComputeState = ZEN_NEW() VulkanComputeState();

    BindRecordingError(&GetRecordingError());
}

FVulkanCommandListContext::~FVulkanCommandListContext()
{
    ZEN_DELETE(m_pComputeState);

    m_pComputeState = nullptr;

    ZEN_DELETE(m_pGfxState);

    m_pGfxState = nullptr;
}

void FVulkanCommandListContext::RHIDiscardRecording()
{
    DiscardRecording();
}

void FVulkanCommandListContext::DiscardRecording()
{
    VulkanCommandContextBase::DiscardRecording();

    m_pCurrentPipeline = nullptr;

    m_pGfxState->SetPipelineState(nullptr);

    m_pComputeState->SetPipelineState(nullptr);
}

RHICommandContextType FVulkanCommandListContext::GetContextType()
{
    return m_contextType;
}

static RHITextureView* ResolveRenderingAttachment(const RHIRenderTarget&    target,
                                                  const RHIRenderingLayout& layout,
                                                  bool                      depthStencil,
                                                  SampleCount&              samples,
                                                  bool&                     hasSamples)
{
    RHITextureView* view = nullptr;

    bool valid           = target.pTexture != nullptr;

    if (valid)
    {
        view = target.pTextureView != nullptr ? target.pTextureView : TO_VK_TEXTURE(target.pTexture)->GetAttachmentView();

        const bool depthFormat =
            FormatIsDepthOnly(target.format) || FormatIsStencilOnly(target.format) || FormatIsDepthStencil(target.format);

        const RHITextureUsageFlagBits usage =
            depthStencil ? RHITextureUsageFlagBits::eDepthStencilAttachment : RHITextureUsageFlagBits::eColorAttachment;

        valid = view != nullptr && view->GetTexture() == target.pTexture && view->GetFormat() == target.format
             && depthStencil == depthFormat && target.pTexture->GetBaseInfo().usageFlags.HasFlag(usage);

        if (valid)
        {
            const RHITextureSubResourceRange& range = view->GetSubResourceRange();

            const uint32_t width                    = std::max(1u, target.pTexture->GetWidth() >> range.baseMipLevel);

            const uint32_t height                   = std::max(1u, target.pTexture->GetHeight() >> range.baseMipLevel);

            const SampleCount actualSamples         = target.pTexture->GetBaseInfo().samples;

            valid                                   = view->GetTextureType() != RHITextureType::e3D && range.levelCount == 1
                 && layout.numLayers <= range.layerCount && uint32_t(layout.renderArea.maxX) <= width
                 && uint32_t(layout.renderArea.maxY) <= height && target.numSamples == actualSamples
                 && (!hasSamples || samples == actualSamples);

            if (valid)
            {
                samples    = actualSamples;

                hasSamples = true;
            }
        }
    }

    if (!valid)
    {
        view = nullptr;
    }

    return view;
}

void FVulkanCommandListContext::RHIBeginRendering(const RHIRenderingLayout* layout)
{
    bool valid = !GetRecordingError().IsFailure() && layout != nullptr;

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};

    VkRenderingAttachmentInfo colors[MAX_NUM_COLOR_ATTACHMENTS]{};

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};

    SampleCount samples{};

    bool hasSamples = false;

    if (valid)
    {
        const Rect2<int>& area               = layout->renderArea;

        const VkPhysicalDeviceLimits& limits = GVulkanRHI->GetDevice()->GetPhysicalDeviceProperties().limits;

        valid = area.minX >= 0 && area.minY >= 0 && area.maxX > area.minX && area.maxY > area.minY
             && uint32_t(area.maxX) <= limits.maxFramebufferWidth && uint32_t(area.maxY) <= limits.maxFramebufferHeight
             && layout->numLayers > 0 && layout->numLayers <= limits.maxFramebufferLayers
             && layout->numColorRenderTargets <= std::min<uint32_t>(MAX_NUM_COLOR_ATTACHMENTS, limits.maxColorAttachments);

        if (valid)
        {
            rendering.layerCount           = layout->numLayers;

            rendering.renderArea           = {{area.minX, area.minY}, {uint32_t(area.Width()), uint32_t(area.Height())}};

            rendering.colorAttachmentCount = layout->numColorRenderTargets;

            rendering.pColorAttachments    = colors;

            for (uint32_t i = 0; valid && i < layout->numColorRenderTargets; ++i)
            {
                const RHIRenderTarget& target = layout->colorRenderTargets[i];

                RHITextureView* view          = ResolveRenderingAttachment(target, *layout, false, samples, hasSamples);

                valid                         = view != nullptr;

                if (valid)
                {
                    colors[i]                  = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};

                    colors[i].imageView        = TO_VK_TEXTURE_VIEW(view)->GetVkImageView();

                    colors[i].imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

                    colors[i].loadOp           = ToVkAttachmentLoadOp(target.loadOp);

                    colors[i].storeOp          = ToVkAttachmentStoreOp(target.storeOp);

                    colors[i].clearValue.color = ToVkClearColor(target.clearValue);

                    RecordResource(view);
                }
            }

            if (valid && layout->hasDepthStencilRT)
            {
                const RHIRenderTarget& target = layout->depthStencilRenderTarget;

                RHITextureView* view          = ResolveRenderingAttachment(target, *layout, true, samples, hasSamples);

                valid                         = view != nullptr;

                if (valid)
                {
                    depth.imageView                                  = TO_VK_TEXTURE_VIEW(view)->GetVkImageView();

                    depth.imageLayout                                = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

                    depth.loadOp                                     = ToVkAttachmentLoadOp(target.loadOp);

                    depth.storeOp                                    = ToVkAttachmentStoreOp(target.storeOp);

                    depth.clearValue.depthStencil                    = ToVkClearDepthStencil(target.clearValue);

                    const BitField<RHITextureAspectFlagBits> aspects = target.GetAspects();

                    rendering.pDepthAttachment   = aspects.HasFlag(RHITextureAspectFlagBits::eDepth) ? &depth : nullptr;

                    rendering.pStencilAttachment = aspects.HasFlag(RHITextureAspectFlagBits::eStencil) ? &depth : nullptr;

                    RecordResource(view);
                }
            }
        }
    }

    if (valid && EnsureRecording())
    {
        GetCommandBuffer()->BeginRendering(&rendering);
    }
    else if (!valid)
    {
        LatchError({RHIErrorCode::eInvalidArgument, 0, "Begin rendering attachments"});
    }
}

void FVulkanCommandListContext::RHIEndRendering()
{
    if (EnsureRecording())
    {
        GetCommandBuffer()->EndRendering();
    }
}

void FVulkanCommandListContext::RHIBeginDebugLabel(NameID name)
{
    if (EnsureRecording())
    {
        GetCommandBuffer()->BeginBreadcrumb(name);

        if (GVulkanRHI->GetInstanceExtensionFlags().hasDebugUtils && vkCmdBeginDebugUtilsLabelEXT != nullptr
            && vkCmdEndDebugUtilsLabelEXT != nullptr)
        {
            VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};

            label.pLabelName = name.CStr();

            vkCmdBeginDebugUtilsLabelEXT(GetCommandBuffer()->GetVkHandle(), &label);
        }
    }
}

void FVulkanCommandListContext::RHIEndDebugLabel()
{
    if (EnsureRecording())
    {
        GetCommandBuffer()->EndBreadcrumb();

        if (GVulkanRHI->GetInstanceExtensionFlags().hasDebugUtils && vkCmdBeginDebugUtilsLabelEXT != nullptr
            && vkCmdEndDebugUtilsLabelEXT != nullptr)
        {
            vkCmdEndDebugUtilsLabelEXT(GetCommandBuffer()->GetVkHandle());
        }
    }
}

void FVulkanCommandListContext::RHIBeginGPUTiming(const RHIGPUTimingPtr& result)
{
    if (EnsureRecording())
    {
        if (result != nullptr && result->GetStatus() == RHIGPUTimingStatus::ePending)
        {
            GetCommandBuffer()->BeginGPUTiming(result);
        }
    }
}

void FVulkanCommandListContext::RHIEndGPUTiming(const RHIGPUTimingPtr& result)
{
    if (EnsureRecording())
    {
        if (result != nullptr && result->GetStatus() == RHIGPUTimingStatus::ePending)
        {
            GetCommandBuffer()->EndGPUTiming(result);
        }
    }
}

void FVulkanCommandListContext::RHISetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    m_pGfxState->SetScissor(minX, minY, maxX, maxY);
}

void FVulkanCommandListContext::RHISetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    m_pGfxState->SetViewport(minX, minY, maxX, maxY);
}

void FVulkanCommandListContext::RHISetDepthBias(float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor)
{
    m_pGfxState->SetDepthBias(depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
}

void FVulkanCommandListContext::RHISetLineWidth(float lineWidth)
{
    m_pGfxState->SetLineWidth(lineWidth);
}

void FVulkanCommandListContext::RHISetBlendConstants(const Color& blendConstants)
{
    m_pGfxState->SetBlendConstants(blendConstants.r, blendConstants.g, blendConstants.b, blendConstants.a);
}

void FVulkanCommandListContext::RHIBindPipeline(RHIPipeline* pPipeline)
{
    if (pPipeline == nullptr)
    {
        LatchError({RHIErrorCode::eInvalidArgument, 0, "Bind pipeline"});
    }
    else if (!GetRecordingError().IsFailure())
    {
        VulkanPipeline* pVkPipeline = TO_VK_PIPELINE(pPipeline);

        m_pCurrentPipeline          = pVkPipeline;

        if (pVkPipeline->GetVkPipelineBindPoint() == VK_PIPELINE_BIND_POINT_COMPUTE)
        {
            m_pComputeState->SetPipelineState(pPipeline);
        }
        else
        {
            m_pGfxState->SetPipelineState(pPipeline);
        }
    }
}

uint64_t FVulkanCommandListContext::RHIGetCurrentBindlessEpoch() const
{
    return GVulkanRHI->GetBindlessDescriptorPoolManager()->GetCurrentEpoch();
}

uint64_t FVulkanCommandListContext::RHICaptureBindlessEpoch()
{
    return GVulkanRHI->GetBindlessDescriptorPoolManager()->CaptureEpoch();
}

void FVulkanCommandListContext::RHIReleaseBindlessEpoch(uint64_t epoch)
{
    GVulkanRHI->GetBindlessDescriptorPoolManager()->ReleaseEpoch(epoch);
}

void FVulkanCommandListContext::RecordCurrentBindlessEpoch()
{
    if (m_recordedBindlessEpoch != 0)
    {
        RecordLifetime(m_recordedBindlessEpoch);
    }
    else
    {
        const uint64_t epoch = RHICaptureBindlessEpoch();

        RecordLifetime(epoch);

        RHIReleaseBindlessEpoch(epoch);
    }
}

void FVulkanCommandListContext::RHISetShaderParameters(RHIShaderParameterView parameters)
{
    bool ready = false;

    if (!GetRecordingError().IsFailure() && m_pCurrentPipeline != nullptr)
    {
        const uint64_t transaction = GetRecordingTransaction();

        ready                      = m_pCurrentPipeline->GetVkPipelineBindPoint() == VK_PIPELINE_BIND_POINT_COMPUTE
                                       ? m_pComputeState->SetShaderParameters(parameters, m_recordedBindlessEpoch, transaction)
                                       : m_pGfxState->SetShaderParameters(parameters, m_recordedBindlessEpoch, transaction);
    }

    if (!ready)
    {
        LatchError({RHIErrorCode::eInvalidArgument, 0, "Set shader parameters"});
    }
}

void FVulkanCommandListContext::RHIBindVertexBuffers(VectorView<RHIBuffer*> pBuffers, VectorView<uint64_t> offsets)
{
    m_pGfxState->SetVertexBuffers(pBuffers.size(), pBuffers.data(), offsets.data());
}

void FVulkanCommandListContext::RHIBindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset)
{
    m_pGfxState->SetVertexBuffers(1, &pBuffer, &offset);
}

void FVulkanCommandListContext::RHIDraw(uint32_t vertexCount,
                                        uint32_t instanceCount,
                                        uint32_t firstVertex,
                                        uint32_t firstInstance)
{
    if (m_pGfxState->PreDraw(this))
    {
        GVulkanRHI->GetExecutionCounterStorage().Increment(GVulkanRHI->GetExecutionCounterStorage().draws);

        vkCmdDraw(GetCommandBuffer()->GetVkHandle(), vertexCount, instanceCount, firstVertex, firstInstance);
    }
}

void FVulkanCommandListContext::RHIDrawIndexed(RHIBuffer* pIndexBuffer,
                                               DataFormat indexFormat,
                                               uint64_t   indexBufferOffset,
                                               uint32_t   indexCount,
                                               uint32_t   instanceCount,
                                               uint32_t   firstIndex,
                                               int32_t    vertexOffset,
                                               uint32_t   firstInstance)
{
    if (m_pGfxState->PreDraw(this))
    {
        FVulkanCommandBuffer* pCmdBuffer = GetCommandBuffer();

        VulkanBuffer* pVkBuffer          = TO_VK_BUFFER(pIndexBuffer);

        VkIndexType vkIndexType          = indexFormat == DataFormat::eR16UInt ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

        pCmdBuffer->BindIndexBuffer(pVkBuffer->GetVkBuffer(), indexBufferOffset, vkIndexType);

        GVulkanRHI->GetExecutionCounterStorage().Increment(GVulkanRHI->GetExecutionCounterStorage().draws);

        vkCmdDrawIndexed(pCmdBuffer->GetVkHandle(), indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }
}

void FVulkanCommandListContext::RHIDrawIndexedIndirect(RHIBuffer* pIndirectBuffer,
                                                       RHIBuffer* pIndexBuffer,
                                                       DataFormat indexFormat,
                                                       uint64_t   indexBufferOffset,
                                                       uint64_t   offset,
                                                       uint32_t   drawCount,
                                                       uint32_t   stride)
{
    if (m_pGfxState->PreDraw(this))
    {
        FVulkanCommandBuffer* pCmdBuffer = GetCommandBuffer();

        VkIndexType vkIndexType          = indexFormat == DataFormat::eR16UInt ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

        pCmdBuffer->BindIndexBuffer(TO_VK_BUFFER(pIndexBuffer)->GetVkBuffer(), indexBufferOffset, vkIndexType);

        GVulkanRHI->GetExecutionCounterStorage().Increment(GVulkanRHI->GetExecutionCounterStorage().draws);

        vkCmdDrawIndexedIndirect(pCmdBuffer->GetVkHandle(), TO_VK_BUFFER(pIndirectBuffer)->GetVkBuffer(), offset, drawCount,
                                 stride);
    }
}

void FVulkanCommandListContext::RHIDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
    if (m_pComputeState->PreDispatch(this))
    {
        GVulkanRHI->GetExecutionCounterStorage().Increment(GVulkanRHI->GetExecutionCounterStorage().dispatches);

        vkCmdDispatch(GetCommandBuffer()->GetVkHandle(), groupCountX, groupCountY, groupCountZ);
    }
}

void FVulkanCommandListContext::RHIDispatchIndirect(RHIBuffer* pIndirectBuffer, uint64_t offset)
{
    if (m_pComputeState->PreDispatch(this))
    {
        VulkanBuffer* pVkBuffer = TO_VK_BUFFER(pIndirectBuffer);

        GVulkanRHI->GetExecutionCounterStorage().Increment(GVulkanRHI->GetExecutionCounterStorage().dispatches);

        vkCmdDispatchIndirect(GetCommandBuffer()->GetVkHandle(), pVkBuffer->GetVkBuffer(), offset);
    }
}

void FVulkanCommandListContext::RHISetPushConstants(RHIPipeline* pPipeline, VectorView<const uint8_t> data, uint32_t offset)
{
    if (EnsureRecording())
    {
        VulkanPipeline* pVkPipeline = TO_VK_PIPELINE(pPipeline);

        vkCmdPushConstants(GetCommandBuffer()->GetVkHandle(), pVkPipeline->GetVkPipelineLayout(),
                           pVkPipeline->GetPushConstantsStageFlags(), offset, data.size(), data.data());
    }
}

void FVulkanCommandListContext::RHIAddTransitions(BitField<RHIPipelineStageFlagBits> srcStages,
                                                  BitField<RHIPipelineStageFlagBits> dstStages,
                                                  VectorView<RHIMemoryTransition>    memoryTransitions,
                                                  VectorView<RHIBufferTransition>    bufferTransitions,
                                                  VectorView<RHITextureTransition>   textureTransitions)
{
    if (EnsureRecording())
    {
        VulkanPipelineBarrier barrier;

        bool hasBarrier = false;

        for (RHIMemoryTransition const& memoryTransition : memoryTransitions)
        {
            barrier.AddMemoryBarrier(memoryTransition.srcAccess, memoryTransition.dstAccess);

            hasBarrier = true;
        }

        for (RHIBufferTransition const& bufferTransition : bufferTransitions)
        {
            VulkanBuffer* pVulkanBuffer = TO_VK_BUFFER(bufferTransition.pBuffer);

            VkAccessFlags srcAccess =
                RHIBufferUsageToAccessFlagBits(bufferTransition.oldUsage, bufferTransition.oldAccessMode, srcStages);

            VkAccessFlags dstAccess =
                RHIBufferUsageToAccessFlagBits(bufferTransition.newUsage, bufferTransition.newAccessMode, dstStages);

            srcAccess |= bufferTransition.additionalSrcAccess;

            barrier.AddBufferBarrier(pVulkanBuffer->GetVkBuffer(), bufferTransition.offset, bufferTransition.size, srcAccess,
                                     dstAccess);

            hasBarrier = true;
        }

        for (RHITextureTransition const& textureTransition : textureTransitions)
        {
            VulkanTexture* pVulkanTexture = TO_VK_TEXTURE(textureTransition.pTexture);

            VkAccessFlags srcAccess       = textureTransition.GetSourceAccess();

            VkAccessFlags dstAccess =
                RHITextureUsageToAccessFlagBits(textureTransition.newUsage, textureTransition.newAccessMode);

            VkImageLayout oldLayout = ToVkImageLayout(RHITextureUsageToLayout(textureTransition.oldUsage));

            VkImageLayout newLayout = ToVkImageLayout(RHITextureUsageToLayout(textureTransition.newUsage));

            VkImageSubresourceRange subresourceRange{};

            ToVkImageSubresourceRange(textureTransition.subResourceRange, &subresourceRange);

            barrier.AddImageBarrier(pVulkanTexture->GetVkImage(), oldLayout, newLayout, subresourceRange, srcAccess, dstAccess);

            hasBarrier = true;
        }

        if (hasBarrier)
        {
            barrier.Execute(GetCommandBuffer()->GetVkHandle(), srcStages, dstStages);
        }
    }
}

void FVulkanCommandListContext::RHIClearBuffer(RHIBuffer* pBuffer, uint64_t offset, uint64_t size)
{
    if (EnsureRecording())
    {
        vkCmdFillBuffer(GetCommandBuffer()->GetVkHandle(), TO_VK_BUFFER(pBuffer)->GetVkBuffer(), offset, size, 0);
    }
}

void FVulkanCommandListContext::RHICopyBuffer(RHIBuffer* pSrcBuffer, RHIBuffer* pDstBuffer, const RHIBufferCopyRegion& region)
{
    if (EnsureRecording())
    {
        VkBufferCopy bufferCopy;

        bufferCopy.srcOffset = region.srcOffset;

        bufferCopy.dstOffset = region.dstOffset;

        bufferCopy.size      = region.size;

        vkCmdCopyBuffer(GetCommandBuffer()->GetVkHandle(), TO_VK_BUFFER(pSrcBuffer)->GetVkBuffer(),
                        TO_VK_BUFFER(pDstBuffer)->GetVkBuffer(), 1, &bufferCopy);
    }
}

void FVulkanCommandListContext::RHIClearTexture(RHITexture*                       pTexture,
                                                const Color&                      color,
                                                const RHITextureSubResourceRange& range)
{
    if (EnsureRecording())
    {
        VkImageSubresourceRange vkRange;

        ToVkImageSubresourceRange(range, &vkRange);

        VkClearColorValue colorValue;

        ToVkClearColor(color, &colorValue);

        vkCmdClearColorImage(GetCommandBuffer()->GetVkHandle(), TO_VK_TEXTURE(pTexture)->GetVkImage(),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colorValue, 1, &vkRange);
    }
}

void FVulkanCommandListContext::RHICopyTexture(RHITexture*                      pSrcTexture,
                                               RHITexture*                      pDstTexture,
                                               VectorView<RHITextureCopyRegion> regions)
{
    if (EnsureRecording())
    {
        HeapVector<VkImageCopy> copies(regions.size());

        for (uint32_t i = 0; i < regions.size(); i++)
        {
            ToVkImageCopy(regions[i], &copies[i]);
        }

        vkCmdCopyImage(GetCommandBuffer()->GetVkHandle(), TO_VK_TEXTURE(pSrcTexture)->GetVkImage(),
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TO_VK_TEXTURE(pDstTexture)->GetVkImage(),
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copies.size(), copies.data());
    }
}

void FVulkanCommandListContext::RHIBlitTexture(RHITexture*                      pSrcTexture,
                                               RHITexture*                      pDstTexture,
                                               VectorView<RHITextureBlitRegion> regions,
                                               RHISamplerFilter                 filter)
{
    if (EnsureRecording())
    {
        HeapVector<VkImageBlit> blits(regions.size());

        for (uint32_t i = 0; i < regions.size(); i++)
        {
            ToVkImageBlit(regions[i], &blits[i]);
        }

        vkCmdBlitImage(GetCommandBuffer()->GetVkHandle(), TO_VK_TEXTURE(pSrcTexture)->GetVkImage(),
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TO_VK_TEXTURE(pDstTexture)->GetVkImage(),
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, blits.size(), blits.data(), ToVkFilter(filter));
    }
}

void FVulkanCommandListContext::RHICopyTextureToBuffer(RHITexture*                            pSrcTex,
                                                       RHIBuffer*                             pDstBuffer,
                                                       VectorView<RHIBufferTextureCopyRegion> regions)
{
    if (EnsureRecording())
    {
        HeapVector<VkBufferImageCopy> copies(regions.size());

        for (uint32_t i = 0; i < regions.size(); i++)
        {
            ToVkBufferImageCopy(regions[i], &copies[i]);
        }

        vkCmdCopyImageToBuffer(GetCommandBuffer()->GetVkHandle(), TO_VK_TEXTURE(pSrcTex)->GetVkImage(),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TO_VK_BUFFER(pDstBuffer)->GetVkBuffer(), copies.size(),
                               copies.data());
    }
}

void FVulkanCommandListContext::RHICopyBufferToTexture(RHIBuffer*                             pSrcBuffer,
                                                       RHITexture*                            pDstTexture,
                                                       VectorView<RHIBufferTextureCopyRegion> regions)
{
    if (EnsureRecording())
    {
        HeapVector<VkBufferImageCopy> copies(regions.size());

        for (uint32_t i = 0; i < copies.size(); i++)
        {
            ToVkBufferImageCopy(regions[i], &copies[i]);
        }

        vkCmdCopyBufferToImage(GetCommandBuffer()->GetVkHandle(), TO_VK_BUFFER(pSrcBuffer)->GetVkBuffer(),
                               TO_VK_TEXTURE(pDstTexture)->GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copies.size(),
                               copies.data());
    }
}

void FVulkanCommandListContext::RHIResolveTexture(RHITexture* pSrcTexture,
                                                  RHITexture* pDstTexture,
                                                  uint32_t    srcLayer,
                                                  uint32_t    srcMipmap,
                                                  uint32_t    dstLayer,
                                                  uint32_t    dstMipmap)
{
    if (EnsureRecording())
    {
        VkImageResolve region{};

        region.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;

        region.srcSubresource.mipLevel       = srcMipmap;

        region.srcSubresource.baseArrayLayer = srcLayer;

        region.srcSubresource.layerCount     = 1;

        region.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;

        region.dstSubresource.mipLevel       = dstMipmap;

        region.dstSubresource.baseArrayLayer = dstLayer;

        region.dstSubresource.layerCount     = 1;

        region.extent.width                  = std::max(1u, pSrcTexture->GetBaseInfo().width >> srcMipmap);

        region.extent.height                 = std::max(1u, pSrcTexture->GetBaseInfo().height >> srcMipmap);

        region.extent.depth                  = std::max(1u, pSrcTexture->GetBaseInfo().depth >> srcMipmap);

        vkCmdResolveImage(GetCommandBuffer()->GetVkHandle(), TO_VK_TEXTURE(pSrcTexture)->GetVkImage(),
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TO_VK_TEXTURE(pDstTexture)->GetVkImage(),
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
}

void FVulkanCommandListContext::RHIWaitUntilCompleted()
{
    WaitForLastSubmittedWork(UINT64_MAX);
}

VulkanPlatformCommandList* VulkanRHI::AcquirePlatformCommandList()
{
    return m_platformCommandListPool.Acquire();
}

void VulkanRHI::ReleasePlatformCommandList(VulkanPlatformCommandList* pCommandList)
{
    m_platformCommandListPool.Release(pCommandList);
}

void VulkanRHI::DestroyPlatformCommandListPool()
{
    m_platformCommandListPool.Destroy();
}

RHIStatus VulkanRHI::FinalizeCommandLists(VectorView<RHICommandList*>          cmdLists,
                                          HeapVector<RHIPlatformCommandList*>& outCommandLists)
{
    GetRHIThread().CheckOwnership();

    RHIStatus status;

    const size_t firstOutput   = outCommandLists.size();

    const uint64_t transaction = m_pBindlessDescriptorPoolManager->BeginTransaction();

    if (m_submissionBlocked || !PrepareCommandListDependencies(cmdLists))
    {
        status.error = {RHIErrorCode::eBackendFailure, 0, "Prepare command dependencies"};

        BlockSubmissions();
    }

    for (RHICommandList* commands : cmdLists)
    {
        if (status)
        {
            FVulkanCommandListContext* context = static_cast<FVulkanCommandListContext*>(commands->GetContext());

            context->SetRecordingTransaction(transaction);

            VulkanPlatformCommandList* platform = AcquirePlatformCommandList();

            commands->Execute();

            context->CollectWorkloads(platform->m_workloads);

            status.error = context->GetRecordingError();

            platform->m_transactions.push_back(context->DetachRecordingTransaction());

            platform->m_contextWorkloadRanges.push_back({context, 0, static_cast<uint32_t>(platform->m_workloads.size())});

            outCommandLists.push_back(platform);
        }
    }

    if (!status)
    {
        for (size_t i = firstOutput; i < outCommandLists.size(); ++i)
        {
            VulkanPlatformCommandList* platform = static_cast<VulkanPlatformCommandList*>(outCommandLists[i]);

            for (VulkanWorkload* workload : platform->m_workloads)
            {
                workload->m_pQueue->DiscardWorkload(workload);
            }

            for (uint64_t owner : platform->m_transactions)
            {
                m_pBindlessDescriptorPoolManager->ResolveTransaction(owner, false);
            }

            ReleasePlatformCommandList(platform);
        }

        outCommandLists.resize(firstOutput);

        for (RHICommandList* commands : cmdLists)
        {
            if (commands != nullptr && commands->GetContext() != nullptr)
            {
                commands->GetContext()->RHIDiscardRecording();
            }
        }
    }

    return status;
}

void VulkanRHI::SubmitPlatformCommandLists(VectorView<RHIPlatformCommandList*> commandLists)
{
    for (RHIPlatformCommandList* pCommandList : commandLists)
    {
        VulkanPlatformCommandList* pPlatformCmdList = static_cast<VulkanPlatformCommandList*>(pCommandList);

        for (VulkanWorkload* pWorkload : pPlatformCmdList->m_workloads)
        {
            pWorkload->m_pQueue->EnqueueWorkload(pWorkload);
        }

        for (const VulkanPlatformCommandList::ContextWorkloadRange& contextWorkloadRange :
             pPlatformCmdList->m_contextWorkloadRanges)
        {
            contextWorkloadRange.pContext->MarkWorkloadPendingFlush();
        }

        m_pendingPlatformCmdLists.push_back(pPlatformCmdList);
    }
}

RHISubmissionResult VulkanRHI::FlushAllGPUCommands()
{
    GetRHIThread().CheckOwnership();

    SetSubmissionError({});

    HeapVector<VulkanQueue*> queues;

    for (uint32_t i = 0; i < ToUnderlying(RHICommandContextType::eMax); ++i)
    {
        VulkanQueue* queue = m_pDevice->GetQueue(static_cast<RHICommandContextType>(i));

        if (std::find(queues.begin(), queues.end(), queue) == queues.end())
        {
            queues.push_back(queue);
        }
    }

    RHISubmissionResult result = m_submissionBlocked ? RHISubmissionResult::eFatal : RHISubmissionResult::eSuccess;

    bool submitted             = false;

    for (VulkanQueue* queue : queues)
    {
        if (result != RHISubmissionResult::eSuccess)
        {
            break;
        }

        uint64_t serial  = 0;

        result           = queue->SubmitPendingWorkloads(serial);

        submitted       |= serial != 0;

        if (result == RHISubmissionResult::eRejected && submitted)
        {
            result = RHISubmissionResult::eFatal;
        }
    }

    if (result == RHISubmissionResult::eFatal)
    {
        BlockSubmissions(GetLastSubmissionError().IsFailure()
                             ? GetLastSubmissionError()
                             : MakeRHIError(RHIErrorCode::eBackendFailure, "Vulkan submission", __FILE__, __LINE__));
    }

    // Workload objects stay alive until every context has read the actual accepted serials.
    // Shared physical queues must not be polled again before these references are consumed.
    for (VulkanPlatformCommandList* platform : m_pendingPlatformCmdLists)
    {
        for (VulkanPlatformCommandList::ContextWorkloadRange const& range : platform->m_contextWorkloadRanges)
        {
            uint64_t serial = 0;

            for (uint32_t i = 0; i < range.workloadCount; ++i)
            {
                serial = std::max(serial, platform->m_workloads[range.firstWorkloadIndex + i]->m_submissionSerial);
            }

            range.pContext->SetLastSubmittedSerial(serial);
        }

        for (uint64_t owner : platform->m_transactions)
        {
            m_pBindlessDescriptorPoolManager->ResolveTransaction(owner, result == RHISubmissionResult::eSuccess
                                                                            || result == RHISubmissionResult::eFatal);
        }

        if (result == RHISubmissionResult::eRejected)
        {
            for (const VulkanPlatformCommandList::ContextWorkloadRange& range : platform->m_contextWorkloadRanges)
            {
                range.pContext->RHIDiscardRecording();
            }
        }

        ReleasePlatformCommandList(platform);
    }

    m_pendingPlatformCmdLists.clear();

    for (VulkanQueue* queue : queues)
    {
        queue->DiscardPendingWorkloads(result == RHISubmissionResult::eFatal);
    }

    return result;
}

} // namespace zen
