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
        case VulkanCommandBufferType::eSecondary: pName = "Secondary"; break;
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
    for (auto& state : m_boundStates)
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
}

void FVulkanCommandBuffer::BindPipelineAndDescriptorSets(VulkanPipeline* pipeline,
                                                         const HeapVector<VkDescriptorSet>& sets,
                                                         uint32_t firstSet,
                                                         const HeapVector<uint32_t>& offsets)
{
    if (pipeline == nullptr)
    {
        LOG_ERROR_AND_THROW("Draw or dispatch requires a pipeline");
    }
    const VkPipelineBindPoint bindPoint = pipeline->GetVkPipelineBindPoint();
    auto& state = m_boundStates[bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS ? 0 : 1];
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
    if (!sets.empty() &&
        (state.descriptorLayout != layout || state.firstSet != firstSet ||
         !SameValues(state.descriptorSets, sets) || !SameValues(state.dynamicOffsets, offsets)))
    {
        vkCmdBindDescriptorSets(m_vkHandle, bindPoint, layout, firstSet,
                                static_cast<uint32_t>(sets.size()), sets.data(),
                                static_cast<uint32_t>(offsets.size()),
                                offsets.empty() ? nullptr : offsets.data());
        state.descriptorLayout = layout;
        state.firstSet         = firstSet;
        state.descriptorSets   = sets;
        state.dynamicOffsets   = offsets;
    }
}

void FVulkanCommandBuffer::SetViewport(const VkViewport& viewport)
{
    const uint32_t bit = ToUnderlying(RHIDynamicState::eViewPort);
    if (!m_validDynamicStates.Test(bit) || m_viewport.x != viewport.x ||
        m_viewport.y != viewport.y || m_viewport.width != viewport.width ||
        m_viewport.height != viewport.height || m_viewport.minDepth != viewport.minDepth ||
        m_viewport.maxDepth != viewport.maxDepth)
    {
        vkCmdSetViewport(m_vkHandle, 0, 1, &viewport);
        m_viewport = viewport;
        m_validDynamicStates.Set(bit);
    }
}

void FVulkanCommandBuffer::SetScissor(const VkRect2D& scissor)
{
    const uint32_t bit = ToUnderlying(RHIDynamicState::eScissor);
    if (!m_validDynamicStates.Test(bit) || m_scissor.offset.x != scissor.offset.x ||
        m_scissor.offset.y != scissor.offset.y || m_scissor.extent.width != scissor.extent.width ||
        m_scissor.extent.height != scissor.extent.height)
    {
        vkCmdSetScissor(m_vkHandle, 0, 1, &scissor);
        m_scissor = scissor;
        m_validDynamicStates.Set(bit);
    }
}

void FVulkanCommandBuffer::SetDepthBias(float constantFactor, float clamp, float slopeFactor)
{
    const uint32_t bit = ToUnderlying(RHIDynamicState::eDepthBias);
    if (!m_validDynamicStates.Test(bit) || m_depthBias[0] != constantFactor ||
        m_depthBias[1] != clamp || m_depthBias[2] != slopeFactor)
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

void FVulkanCommandBuffer::BindVertexBuffers(const HeapVector<VkBuffer>& buffers,
                                             const HeapVector<uint64_t>& offsets)
{
    if (!buffers.empty() &&
        (!SameValues(m_boundVertexBuffers, buffers) || !SameValues(m_boundVertexOffsets, offsets)))
    {
        vkCmdBindVertexBuffers(m_vkHandle, 0, static_cast<uint32_t>(buffers.size()), buffers.data(),
                               offsets.data());
        m_boundVertexBuffers = buffers;
        m_boundVertexOffsets = offsets;
    }
}

void FVulkanCommandBuffer::Begin()
{
    InvalidateCachedState();
    if (m_state == State::eNeedReset)
    {
        vkResetCommandBuffer(m_vkHandle, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
    }
    else
    {
        VERIFY_EXPR_MSG(m_state == State::eReadyForBegin,
                        "Incorrect command buffer state when beginning command buffer");
    }

    m_state = State::eIsInsideBegin;
    VkCommandBufferBeginInfo beginInfo;
    InitVkStruct(beginInfo, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(m_vkHandle, &beginInfo));
}

void FVulkanCommandBuffer::End()
{
    VERIFY_EXPR_MSG(IsOutsideRenderPass(), "Can't end command buffer inside a render pass");
    vkEndCommandBuffer(m_vkHandle);
    m_state = State::eHasEnded;
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

void FVulkanCommandBuffer::BeginRenderPass(const VkRenderPassBeginInfo* pBeginInfo)
{
    vkCmdBeginRenderPass(m_vkHandle, pBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
    m_state = State::eIsInsideRenderPass;
}

void FVulkanCommandBuffer::EndRenderPass()
{
    vkCmdEndRenderPass(m_vkHandle);
    m_state = State::eIsInsideBegin;
}

void FVulkanCommandBuffer::SetSubmitted()
{
    LockAuto lock(m_pCmdBufferPool->GetMutex());
    m_state      = State::eSubmitted;
    m_submitTime = platform::Timer::Now<>();
}

void FVulkanCommandBuffer::SetCompleted()
{
    LockAuto lock(m_pCmdBufferPool->GetMutex());

    if (m_state == State::eSubmitted)
    {
        m_state = State::eNeedReset;
    }
}

void FVulkanCommandBuffer::Discard()
{
    LockAuto lock(m_pCmdBufferPool->GetMutex());
    VERIFY_EXPR(m_state == State::eHasEnded);
    m_state = State::eNeedReset;
}

VulkanCommandBufferType FVulkanCommandBuffer::GetCommandBufferType() const
{
    return m_pCmdBufferPool->GetCommandBufferType();
}

FVulkanCommandBuffer::FVulkanCommandBuffer(FVulkanCommandBufferPool* pPool) :
    m_pCmdBufferPool(pPool)
{
    AllocMemory();
}

FVulkanCommandBuffer::~FVulkanCommandBuffer()
{
    if (m_state != State::eNotAllocated)
    {
        FreeMemory();
    }
}

void FVulkanCommandBuffer::AllocMemory()
{
    VkCommandBufferAllocateInfo allocInfo;
    InitVkStruct(allocInfo, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    allocInfo.commandPool        = m_pCmdBufferPool->GetVkHandle();
    allocInfo.commandBufferCount = 1;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; // todo: support secondary commandbuffer

    VKCHECK(vkAllocateCommandBuffers(GVulkanRHI->GetVkDevice(), &allocInfo, &m_vkHandle))

    m_state = State::eReadyForBegin;
}

void FVulkanCommandBuffer::FreeMemory()
{
    vkFreeCommandBuffers(GVulkanRHI->GetVkDevice(), m_pCmdBufferPool->GetVkHandle(), 1,
                         &m_vkHandle);
    m_state    = State::eNotAllocated;
    m_vkHandle = VK_NULL_HANDLE;
}

FVulkanCommandBufferPool::FVulkanCommandBufferPool(VulkanQueue* pQueue,
                                                   VulkanCommandBufferType type) :
    m_pQueue(pQueue), m_type(type)
{
    VkCommandPoolCreateInfo cmdPoolCI;
    InitVkStruct(cmdPoolCI, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    cmdPoolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // reset cmd buffer
    cmdPoolCI.queueFamilyIndex = pQueue->GetFamilyIndex();
    VKCHECK(vkCreateCommandPool(GVulkanRHI->GetVkDevice(), &cmdPoolCI, nullptr, &m_vkHandle));
}

FVulkanCommandBufferPool::~FVulkanCommandBufferPool()
{
#if ZEN_VK_RHI_DEBUG
    LOGI("[VulkanCmdBufferPool] destroy type={} requests={} readyReuses={} freeReuses={} "
         "allocations={} inUse={} free={}",
         VulkanCommandBufferTypeToString(m_type), m_numCmdBufferRequests, m_numReadyCmdBufferReuses,
         m_numFreeCmdBufferReuses, m_numCmdBufferAllocations, m_cmdBuffersInUse.size(),
         m_cmdBuffersFree.size());
#endif

    for (uint32_t i = 0; i < m_cmdBuffersInUse.size(); i++)
    {
        FVulkanCommandBuffer* pCmdBuffer = m_cmdBuffersInUse[i];
        pCmdBuffer->~FVulkanCommandBuffer();
        ZEN_MEM_FREE(pCmdBuffer);
        //ZEN_DELETE(pCmdBuffer);
    }

    for (uint32_t i = 0; i < m_cmdBuffersFree.size(); i++)
    {
        FVulkanCommandBuffer* pCmdBuffer = m_cmdBuffersFree[i];
        pCmdBuffer->~FVulkanCommandBuffer();
        ZEN_MEM_FREE(pCmdBuffer);
        //ZEN_DELETE(pCmdBuffer);
    }

    vkDestroyCommandPool(GVulkanRHI->GetVkDevice(), m_vkHandle, nullptr);
}

void FVulkanCommandBufferPool::FreeUnusedCommandBuffers()
{
    LockAuto lock(&m_mutex);

    const double currentTime = platform::Timer::Now<>();

    HeapVector<FVulkanCommandBuffer*>::iterator it = m_cmdBuffersInUse.end();

    while (it != m_cmdBuffersInUse.begin())
    {
        --it;
        FVulkanCommandBuffer* pCmdBuffer = *it;

        if ((pCmdBuffer->m_state == FVulkanCommandBuffer::State::eReadyForBegin ||
             pCmdBuffer->m_state == FVulkanCommandBuffer::State::eNeedReset) &&
            (currentTime - pCmdBuffer->m_submitTime) > 10.0f)
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
        FVulkanCommandBuffer* pCmdBuffer =
            static_cast<FVulkanCommandBuffer*>(ZEN_MEM_ALLOC(sizeof(FVulkanCommandBuffer)));

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
    if (m_pCurrentWorkload != nullptr)
    {
        m_pQueue->ReleaseWorkload(m_pCurrentWorkload);
        m_pCurrentWorkload = nullptr;
    }

    for (VulkanWorkload* pWorkload : m_finalizedWorkloads)
    {
        m_pQueue->ReleaseWorkload(pWorkload);
    }

    m_finalizedWorkloads.clear();

    m_pQueue->RecycleCommandBufferPool(m_pCmdBufferPool);
}

bool VulkanCommandContextBase::HasWorkloadData(const VulkanWorkload* pWorkload) const
{
    return pWorkload != nullptr &&
        (pWorkload->HasCommandBuffers() || !pWorkload->m_waitSemaphoreInfos.empty() ||
         !pWorkload->m_signalSemaphoreInfos.empty() || !pWorkload->m_lifetimeIds.empty());
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
    if (m_pCurrentWorkload == nullptr)
    {
        return;
    }

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

RHISubmissionResult VulkanCommandContextBase::SubmitRecordedWorkloads()
{
    RHISubmissionResult submissionResult = RHISubmissionResult::eSuccess;
    HeapVector<VulkanWorkload*> workloadsToSubmit;
    CollectWorkloads(workloadsToSubmit);

    for (VulkanWorkload* pWorkload : workloadsToSubmit)
    {
        m_pQueue->EnqueueWorkload(pWorkload);
    }

    if (!workloadsToSubmit.empty())
    {
        uint64_t submissionSerial        = 0;
        const RHISubmissionResult result = GVulkanRHI->AreSubmissionsBlocked() ?
            RHISubmissionResult::eFatal :
            m_pQueue->SubmitPendingWorkloads(submissionSerial);
        SetLastSubmittedSerial(submissionSerial);

        if (result != RHISubmissionResult::eSuccess)
        {
            m_pQueue->DiscardPendingWorkloads(result == RHISubmissionResult::eFatal);
            // This direct path initializes viewport layouts; callers cannot replay its state.
            GVulkanRHI->BlockSubmissions();
            submissionResult = RHISubmissionResult::eFatal;
        }
        else
        {
            m_pQueue->ProcessPendingWorkloads(0);
        }
    }

    return submissionResult;
}

void VulkanCommandContextBase::RecordLifetime(uint64_t id)
{
    if (id != 0)
    {
        auto& ids = GetWorkload(WorkloadPhase::eExecute)->m_lifetimeIds;
        if (std::find(ids.begin(), ids.end(), id) == ids.end())
        {
            ids.push_back(id);
            GVulkanRHI->GetLifetimeTracker().RetainRecording(id);
        }
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
        if (m_pQueue->WaitForSubmission(m_lastSubmittedSerial, timeToWaitNS))
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

        if (pCurrent->m_state == FVulkanCommandBuffer::State::eReadyForBegin ||
            pCurrent->m_state == FVulkanCommandBuffer::State::eNeedReset)
        {
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
    pCmdBuffer->Begin();
}

void VulkanCommandContextBase::StartWorkload()
{
    VERIFY_EXPR(m_pCurrentWorkload == nullptr);
    m_pCurrentWorkload     = m_pQueue->AcquireWorkload();
    m_currentWorkloadPhase = WorkloadPhase::eWait;
}

void VulkanCommandContextBase::EndWorkload()
{
    if (m_pCurrentWorkload != nullptr)
    {
        FVulkanCommandBuffer* pCommandBuffer = m_pCurrentWorkload->GetLastCommandBuffer();

        if (pCommandBuffer != nullptr)
        {
            if (pCommandBuffer->HasEnded())
            {
                return;
            }

            if (pCommandBuffer->IsInsideRenderPass() &&
                pCommandBuffer->GetCommandBufferType() == VulkanCommandBufferType::ePrimary)
            {
                if (RHIOptions::GetInstance().UseDynamicRendering())
                {
                    pCommandBuffer->EndRendering();
                }
                else
                {
                    pCommandBuffer->EndRenderPass();
                }
            }

            pCommandBuffer->End();
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

void VulkanGfxState::SetDepthBias(float depthBiasConstantFactor,
                                  float depthBiasClamp,
                                  float depthBiasSlopeFactor)
{
    m_rasterizationState.depthBiasConstantFactor = depthBiasConstantFactor;
    m_rasterizationState.depthBiasClamp          = depthBiasClamp;
    m_rasterizationState.depthBiasSlopeFactor    = depthBiasSlopeFactor;
}

void VulkanGfxState::SetVertexBuffers(uint32_t numVertexBuffers,
                                      RHIBuffer* const* ppVertexBuffers,
                                      const uint64_t* pOffsets)
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

void VulkanGfxState::SetShaderParameters(const RHIBatchedShaderParameters& parameters,
                                         uint64_t recordedEpoch)
{
    VERIFY_EXPR(m_pCurrentPipeline != nullptr);
    VERIFY_EXPR(m_pDescriptorSetState != nullptr);

    m_pDescriptorSetState->SetShaderParameters(parameters, recordedEpoch);
}

void VulkanGfxState::PreDraw(FVulkanCommandListContext* pContext)
{
    FVulkanCommandBuffer* commandBuffer = pContext->GetCommandBuffer();
    uint32_t firstSet                   = 0;
    // Always resolve/retain descriptor pools: cache eviction and a new workload can
    // require work even when the native binding commands themselves are unchanged.
    m_pDescriptorSetState->FlushPendingDescriptorWrites(pContext, m_descriptorSets, firstSet,
                                                        m_dynamicOffsets);
    commandBuffer->BindPipelineAndDescriptorSets(m_pCurrentPipeline, m_descriptorSets, firstSet,
                                                 m_dynamicOffsets);
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
        commandBuffer->SetDepthBias(m_rasterizationState.depthBiasConstantFactor,
                                    m_rasterizationState.depthBiasClamp,
                                    m_rasterizationState.depthBiasSlopeFactor);
    }
    if (m_pCurrentPipeline->UsesDynamicState(RHIDynamicState::eLineWidth))
    {
        commandBuffer->SetLineWidth(m_rasterizationState.lineWidth);
    }
    // Blend constants are static pipeline state in the current RHI dynamic-state enum.
    commandBuffer->BindVertexBuffers(m_vertexBuffers, m_vertexBufferOffsets);
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

void VulkanComputeState::SetShaderParameters(const RHIBatchedShaderParameters& parameters,
                                             uint64_t recordedEpoch)
{
    VERIFY_EXPR(m_pCurrentPipeline != nullptr);
    VERIFY_EXPR(m_pDescriptorSetState != nullptr);
    m_pDescriptorSetState->SetShaderParameters(parameters, recordedEpoch);
}

void VulkanComputeState::PreDispatch(FVulkanCommandListContext* pContext)
{
    FVulkanCommandBuffer* commandBuffer = pContext->GetCommandBuffer();
    uint32_t firstSet                   = 0;
    m_pDescriptorSetState->FlushPendingDescriptorWrites(pContext, m_descriptorSets, firstSet,
                                                        m_dynamicOffsets);
    commandBuffer->BindPipelineAndDescriptorSets(m_pCurrentPipeline, m_descriptorSets, firstSet,
                                                 m_dynamicOffsets);
}

FVulkanCommandListContext::FVulkanCommandListContext(RHICommandContextType contextType,
                                                     VulkanDevice* pDevice) :
    VulkanCommandContextBase(pDevice->GetQueue(contextType), VulkanCommandBufferType::ePrimary),
    m_contextType(contextType),
    m_pDevice(pDevice)
{
    m_pGfxState     = ZEN_NEW() VulkanGfxState();
    m_pComputeState = ZEN_NEW() VulkanComputeState();
}

FVulkanCommandListContext::~FVulkanCommandListContext()
{
    ZEN_DELETE(m_pComputeState);
    m_pComputeState = nullptr;

    ZEN_DELETE(m_pGfxState);
    m_pGfxState = nullptr;
}

RHICommandContextType FVulkanCommandListContext::GetContextType()
{
    return m_contextType;
}

static RHITextureView* ResolveRenderingAttachment(const RHIRenderTarget& target,
                                                  const RHIRenderingLayout& layout,
                                                  bool depthStencil,
                                                  SampleCount& samples,
                                                  bool& hasSamples)
{
    if (target.pTexture == nullptr)
    {
        LOG_ERROR_AND_THROW("Rendering attachment has no texture");
    }
    RHITextureView* view = target.pTextureView != nullptr ?
        target.pTextureView :
        TO_VK_TEXTURE(target.pTexture)->GetAttachmentView();
    if (view->GetTexture() != target.pTexture || view->GetFormat() != target.format)
    {
        LOG_ERROR_AND_THROW("Rendering attachment texture or format does not match its view");
    }
    const bool hasDepthStencilFormat = FormatIsDepthOnly(target.format) ||
        FormatIsStencilOnly(target.format) || FormatIsDepthStencil(target.format);
    const auto usage = depthStencil ? RHITextureUsageFlagBits::eDepthStencilAttachment :
                                      RHITextureUsageFlagBits::eColorAttachment;
    if (depthStencil != hasDepthStencilFormat ||
        !target.pTexture->GetBaseInfo().usageFlags.HasFlag(usage))
    {
        LOG_ERROR_AND_THROW(
            "Rendering attachment format or image usage is incompatible with its role");
    }
    const auto& range = view->GetSubResourceRange();
    if (view->GetTextureType() == RHITextureType::e3D || range.levelCount != 1 ||
        layout.numLayers > range.layerCount)
    {
        LOG_ERROR_AND_THROW(
            "Rendering requires a single-mip view with enough array layers; 3D views are unsupported");
    }
    const uint32_t width  = std::max(1u, target.pTexture->GetWidth() >> range.baseMipLevel);
    const uint32_t height = std::max(1u, target.pTexture->GetHeight() >> range.baseMipLevel);
    if (uint32_t(layout.renderArea.maxX) > width || uint32_t(layout.renderArea.maxY) > height)
    {
        LOG_ERROR_AND_THROW("Render area exceeds the selected attachment mip extent");
    }
    const SampleCount actualSamples = target.pTexture->GetBaseInfo().samples;
    if (target.numSamples != actualSamples || (hasSamples && samples != actualSamples))
    {
        LOG_ERROR_AND_THROW("Rendering attachment sample counts do not match");
    }
    samples    = actualSamples;
    hasSamples = true;
    return view;
}

void FVulkanCommandListContext::RHIBeginRendering(const RHIRenderingLayout* pRenderingLayout)
{
    if (pRenderingLayout == nullptr ||
        pRenderingLayout->numColorRenderTargets > MAX_NUM_COLOR_ATTACHMENTS)
    {
        LOG_ERROR_AND_THROW("Invalid rendering layout or color attachment count");
    }
    if (RHIOptions::GetInstance().UseDynamicRendering())
    {
        VkRenderingInfoKHR renderingInfo{};

        const Rect2<int>& area = pRenderingLayout->renderArea;
        const auto& limits     = GVulkanRHI->GetDevice()->GetPhysicalDeviceProperties().limits;
        if (area.minX < 0 || area.minY < 0 || area.maxX <= area.minX || area.maxY <= area.minY ||
            uint32_t(area.maxX) > limits.maxFramebufferWidth ||
            uint32_t(area.maxY) > limits.maxFramebufferHeight || pRenderingLayout->numLayers == 0 ||
            pRenderingLayout->numLayers > limits.maxFramebufferLayers ||
            pRenderingLayout->numColorRenderTargets > limits.maxColorAttachments)
        {
            LOG_ERROR_AND_THROW(
                "Rendering area, layer count or attachment count exceeds device limits");
        }
        SampleCount samples{};
        bool hasSamples = false;

        InitVkStruct(renderingInfo, VK_STRUCTURE_TYPE_RENDERING_INFO_KHR);
        renderingInfo.layerCount               = pRenderingLayout->numLayers;
        renderingInfo.viewMask                 = 0;
        renderingInfo.flags                    = 0;
        renderingInfo.renderArea.offset.x      = area.minX;
        renderingInfo.renderArea.offset.y      = area.minY;
        renderingInfo.renderArea.extent.width  = area.Width();
        renderingInfo.renderArea.extent.height = area.Height();

        HeapVector<VkRenderingAttachmentInfoKHR> colorAttachments;
        colorAttachments.reserve(pRenderingLayout->numColorRenderTargets);

        for (uint32_t i = 0; i < pRenderingLayout->numColorRenderTargets; i++)
        {
            const RHIRenderTarget& colorRT = pRenderingLayout->colorRenderTargets[i];
            RHITextureView* view =
                ResolveRenderingAttachment(colorRT, *pRenderingLayout, false, samples, hasSamples);
            VkRenderingAttachmentInfoKHR colorAttachment{};
            InitVkStruct(colorAttachment, VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR);
            colorAttachment.imageView        = TO_VK_TEXTURE_VIEW(view)->GetVkImageView();
            colorAttachment.imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp           = ToVkAttachmentLoadOp(colorRT.loadOp);
            colorAttachment.storeOp          = ToVkAttachmentStoreOp(colorRT.storeOp);
            colorAttachment.clearValue.color = ToVkClearColor(colorRT.clearValue);
            colorAttachments.emplace_back(colorAttachment);
        }

        renderingInfo.colorAttachmentCount = pRenderingLayout->numColorRenderTargets;
        renderingInfo.pColorAttachments    = colorAttachments.data();

        VkRenderingAttachmentInfoKHR depthStencilAttachment;
        InitVkStruct(depthStencilAttachment, VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR);

        if (pRenderingLayout->hasDepthStencilRT)
        {
            const RHIRenderTarget& depthStencilRT = pRenderingLayout->depthStencilRenderTarget;
            RHITextureView* view = ResolveRenderingAttachment(depthStencilRT, *pRenderingLayout,
                                                              true, samples, hasSamples);
            depthStencilAttachment.imageView   = TO_VK_TEXTURE_VIEW(view)->GetVkImageView();
            depthStencilAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthStencilAttachment.loadOp      = ToVkAttachmentLoadOp(depthStencilRT.loadOp);
            depthStencilAttachment.storeOp     = ToVkAttachmentStoreOp(depthStencilRT.storeOp);
            depthStencilAttachment.clearValue.depthStencil =
                ToVkClearDepthStencil(depthStencilRT.clearValue);

            // Vulkan ignores the view's aspectMask for rendering. Select the intended
            // depth/stencil operations through these attachment pointers instead.
            const auto aspects = depthStencilRT.GetAspects();
            if (aspects.HasFlag(RHITextureAspectFlagBits::eDepth))
            {
                renderingInfo.pDepthAttachment = &depthStencilAttachment;
            }
            if (aspects.HasFlag(RHITextureAspectFlagBits::eStencil))
            {
                renderingInfo.pStencilAttachment = &depthStencilAttachment;
            }
        }

        GetCommandBuffer()->BeginRendering(&renderingInfo);
    }
    else
    {
        // Explicit attachment selection is implemented only by dynamic rendering.
        if (pRenderingLayout->numLayers != 1 ||
            (pRenderingLayout->hasDepthStencilRT &&
             pRenderingLayout->depthStencilRenderTarget.pTextureView != nullptr))
        {
            LOG_ERROR_AND_THROW(
                "Explicit attachment views and layered rendering require dynamic rendering");
        }
        for (uint32_t i = 0; i < pRenderingLayout->numColorRenderTargets; ++i)
        {
            if (pRenderingLayout->colorRenderTargets[i].pTextureView != nullptr)
            {
                LOG_ERROR_AND_THROW("Explicit attachment views require dynamic rendering");
            }
        }
        uint32_t numAttachments = pRenderingLayout->GetTotalNumRenderTargets();
        HeapVector<VkClearValue> clearValues;
        clearValues.resize(numAttachments);
        HeapVector<RHIRenderTargetClearValue> clearValuesRHI;
        clearValuesRHI.resize(numAttachments);
        pRenderingLayout->GetRHIRenderTargetClearValueData(clearValuesRHI.data());

        for (uint32_t i = 0; i < pRenderingLayout->numColorRenderTargets; i++)
        {
            clearValues[i].color = ToVkClearColor(clearValuesRHI[i]);
        }

        if (pRenderingLayout->hasDepthStencilRT)
        {
            clearValues[numAttachments - 1].depthStencil =
                ToVkClearDepthStencil(clearValuesRHI[numAttachments - 1]);
        }

        VkRenderPassBeginInfo rpBeginInfo;
        InitVkStruct(rpBeginInfo, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        rpBeginInfo.renderPass = GVulkanRHI->GetOrCreateRenderPass(pRenderingLayout);
        rpBeginInfo.framebuffer =
            GVulkanRHI->GetOrCreateFramebuffer(pRenderingLayout, rpBeginInfo.renderPass);
        rpBeginInfo.renderArea.offset.x      = pRenderingLayout->renderArea.minX;
        rpBeginInfo.renderArea.offset.y      = pRenderingLayout->renderArea.minY;
        rpBeginInfo.renderArea.extent.width  = pRenderingLayout->renderArea.Width();
        rpBeginInfo.renderArea.extent.height = pRenderingLayout->renderArea.Height();
        rpBeginInfo.clearValueCount          = numAttachments;
        rpBeginInfo.pClearValues             = clearValues.data();

        GetCommandBuffer()->BeginRenderPass(&rpBeginInfo);
    }
}

void FVulkanCommandListContext::RHIEndRendering()
{
    if (RHIOptions::GetInstance().UseDynamicRendering())
    {
        GetCommandBuffer()->EndRendering();
    }
    else
    {
        GetCommandBuffer()->EndRenderPass();
    }
}

void FVulkanCommandListContext::RHISetScissor(uint32_t minX,
                                              uint32_t minY,
                                              uint32_t maxX,
                                              uint32_t maxY)
{
    m_pGfxState->SetScissor(minX, minY, maxX, maxY);
}

void FVulkanCommandListContext::RHISetViewport(uint32_t minX,
                                               uint32_t minY,
                                               uint32_t maxX,
                                               uint32_t maxY)
{
    m_pGfxState->SetViewport(minX, minY, maxX, maxY);
}

void FVulkanCommandListContext::RHISetDepthBias(float depthBiasConstantFactor,
                                                float depthBiasClamp,
                                                float depthBiasSlopeFactor)
{
    m_pGfxState->SetDepthBias(depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
}

void FVulkanCommandListContext::RHISetLineWidth(float lineWidth)
{
    m_pGfxState->SetLineWidth(lineWidth);
}

void FVulkanCommandListContext::RHISetBlendConstants(const Color& blendConstants)
{
    m_pGfxState->SetBlendConstants(blendConstants.r, blendConstants.g, blendConstants.b,
                                   blendConstants.a);
}

void FVulkanCommandListContext::RHIBindPipeline(RHIPipeline* pPipeline)
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

void FVulkanCommandListContext::RHISetShaderParameters(const RHIBatchedShaderParameters& parameters)
{
    VERIFY_EXPR(m_pCurrentPipeline != nullptr);

    if (m_pCurrentPipeline != nullptr &&
        m_pCurrentPipeline->GetVkPipelineBindPoint() == VK_PIPELINE_BIND_POINT_COMPUTE)
    {
        m_pComputeState->SetShaderParameters(parameters, m_recordedBindlessEpoch);
    }
    else if (m_pCurrentPipeline != nullptr)
    {
        m_pGfxState->SetShaderParameters(parameters, m_recordedBindlessEpoch);
    }
}

void FVulkanCommandListContext::RHIBindVertexBuffers(VectorView<RHIBuffer*> pBuffers,
                                                     VectorView<uint64_t> offsets)
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
    m_pGfxState->PreDraw(this);
    vkCmdDraw(GetCommandBuffer()->GetVkHandle(), vertexCount, instanceCount, firstVertex,
              firstInstance);
}

void FVulkanCommandListContext::RHIDrawIndexed(RHIBuffer* pIndexBuffer,
                                               DataFormat indexFormat,
                                               uint32_t indexBufferOffset,
                                               uint32_t indexCount,
                                               uint32_t instanceCount,
                                               uint32_t firstIndex,
                                               int32_t vertexOffset,
                                               uint32_t firstInstance)
{
    m_pGfxState->PreDraw(this);

    FVulkanCommandBuffer* pCmdBuffer = GetCommandBuffer();
    VulkanBuffer* pVkBuffer          = TO_VK_BUFFER(pIndexBuffer);
    VkIndexType vkIndexType =
        indexFormat == DataFormat::eR16UInt ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

    vkCmdBindIndexBuffer(pCmdBuffer->GetVkHandle(), pVkBuffer->GetVkBuffer(), indexBufferOffset,
                         vkIndexType);
    vkCmdDrawIndexed(pCmdBuffer->GetVkHandle(), indexCount, instanceCount, firstIndex, vertexOffset,
                     firstInstance);
}

void FVulkanCommandListContext::RHIDrawIndexedIndirect(RHIBuffer* pIndirectBuffer,
                                                       RHIBuffer* pIndexBuffer,
                                                       DataFormat indexFormat,
                                                       uint32_t indexBufferOffset,
                                                       uint32_t offset,
                                                       uint32_t drawCount,
                                                       uint32_t stride)
{
    m_pGfxState->PreDraw(this);

    FVulkanCommandBuffer* pCmdBuffer = GetCommandBuffer();
    VkIndexType vkIndexType =
        indexFormat == DataFormat::eR16UInt ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

    vkCmdBindIndexBuffer(pCmdBuffer->GetVkHandle(), TO_VK_BUFFER(pIndexBuffer)->GetVkBuffer(),
                         indexBufferOffset, vkIndexType);
    vkCmdDrawIndexedIndirect(pCmdBuffer->GetVkHandle(),
                             TO_VK_BUFFER(pIndirectBuffer)->GetVkBuffer(), offset, drawCount,
                             stride);
}

void FVulkanCommandListContext::RHIDispatch(uint32_t groupCountX,
                                            uint32_t groupCountY,
                                            uint32_t groupCountZ)
{
    m_pComputeState->PreDispatch(this);
    vkCmdDispatch(GetCommandBuffer()->GetVkHandle(), groupCountX, groupCountY, groupCountZ);
}

void FVulkanCommandListContext::RHIDispatchIndirect(RHIBuffer* pIndirectBuffer, uint32_t offset)
{
    m_pComputeState->PreDispatch(this);
    VulkanBuffer* pVkBuffer = TO_VK_BUFFER(pIndirectBuffer);

    vkCmdDispatchIndirect(GetCommandBuffer()->GetVkHandle(), pVkBuffer->GetVkBuffer(), offset);
}

void FVulkanCommandListContext::RHISetPushConstants(RHIPipeline* pPipeline,
                                                    VectorView<const uint8_t> data,
                                                    uint32_t offset)
{
    VulkanPipeline* pVkPipeline = TO_VK_PIPELINE(pPipeline);
    vkCmdPushConstants(GetCommandBuffer()->GetVkHandle(), pVkPipeline->GetVkPipelineLayout(),
                       pVkPipeline->GetPushConstantsStageFlags(), offset, data.size(), data.data());
}

void FVulkanCommandListContext::RHIAddTransitions(
    BitField<RHIPipelineStageFlagBits> srcStages,
    BitField<RHIPipelineStageFlagBits> dstStages,
    VectorView<RHIMemoryTransition> memoryTransitions,
    VectorView<RHIBufferTransition> bufferTransitions,
    VectorView<RHITextureTransition> textureTransitions)
{
    if (memoryTransitions.empty() && bufferTransitions.empty() && textureTransitions.empty())
    {
        return;
    }

    VulkanPipelineBarrier barrier;
    bool hasBarrier = false;

    for (RHIMemoryTransition const& memoryTransition : memoryTransitions)
    {
        barrier.AddMemoryBarrier(ToVkAccessFlags(memoryTransition.srcAccess),
                                 ToVkAccessFlags(memoryTransition.dstAccess));
        hasBarrier = true;
    }

    for (RHIBufferTransition const& bufferTransition : bufferTransitions)
    {
        VulkanBuffer* pVulkanBuffer = TO_VK_BUFFER(bufferTransition.pBuffer);
        VkAccessFlags srcAccess = RHIBufferUsageToAccessFlagBits(bufferTransition.oldUsage,
                                                                 bufferTransition.oldAccessMode);
        VkAccessFlags dstAccess = RHIBufferUsageToAccessFlagBits(bufferTransition.newUsage,
                                                                 bufferTransition.newAccessMode);
        srcAccess |= ToVkAccessFlags(bufferTransition.additionalSrcAccess);
        barrier.AddBufferBarrier(pVulkanBuffer->GetVkBuffer(), bufferTransition.offset,
                                 bufferTransition.size, srcAccess, dstAccess);
        hasBarrier = true;
    }

    for (RHITextureTransition const& textureTransition : textureTransitions)
    {
        VulkanTexture* pVulkanTexture = TO_VK_TEXTURE(textureTransition.pTexture);

        VkAccessFlags srcAccess = ToVkAccessFlags(RHITextureUsageToAccessFlagBits(
            textureTransition.oldUsage, textureTransition.oldAccessMode));
        VkAccessFlags dstAccess = ToVkAccessFlags(RHITextureUsageToAccessFlagBits(
            textureTransition.newUsage, textureTransition.newAccessMode));
        srcAccess |= ToVkAccessFlags(textureTransition.additionalSrcAccess);
        VkImageLayout oldLayout =
            ToVkImageLayout(RHITextureUsageToLayout(textureTransition.oldUsage));
        VkImageLayout newLayout =
            ToVkImageLayout(RHITextureUsageToLayout(textureTransition.newUsage));
        VkImageSubresourceRange subresourceRange{};
        ToVkImageSubresourceRange(textureTransition.subResourceRange, &subresourceRange);
        // subresourceRange.aspectMask = ToVkAspectFlags(textureTransition.subResourceRange.aspect);
        // subresourceRange.layerCount = textureTransition.subResourceRange.layerCount;
        // subresourceRange.levelCount = textureTransition.subResourceRange.levelCount;
        // subresourceRange.baseArrayLayer = textureTransition.subResourceRange.baseArrayLayer;
        // subresourceRange.baseMipLevel   = textureTransition.subResourceRange.baseMipLevel;

        barrier.AddImageBarrier(pVulkanTexture->GetVkImage(), oldLayout, newLayout,
                                subresourceRange, srcAccess, dstAccess);
        GVulkanRHI->UpdateImageLayout(pVulkanTexture->GetVkImage(), newLayout);
        hasBarrier = true;
    }

    if (hasBarrier)
    {
        barrier.Execute(GetCommandBuffer()->GetVkHandle(), srcStages, dstStages);
    }
}

void FVulkanCommandListContext::RHIAddTextureTransition(RHITexture*, RHITextureLayout)
{
    LOGE("RHIAddTextureTransition is deprecated; use RDG/RHIAddTransitions with explicit old and "
         "new usages");
}

void FVulkanCommandListContext::RHIClearBuffer(RHIBuffer* pBuffer, uint32_t offset, uint32_t size)
{
    vkCmdFillBuffer(GetCommandBuffer()->GetVkHandle(), TO_VK_BUFFER(pBuffer)->GetVkBuffer(), offset,
                    size, 0);
}

void FVulkanCommandListContext::RHICopyBuffer(RHIBuffer* pSrcBuffer,
                                              RHIBuffer* pDstBuffer,
                                              const RHIBufferCopyRegion& region)
{
    VkBufferCopy bufferCopy;
    bufferCopy.srcOffset = region.srcOffset;
    bufferCopy.dstOffset = region.dstOffset;
    bufferCopy.size      = region.size;

    vkCmdCopyBuffer(GetCommandBuffer()->GetVkHandle(), TO_VK_BUFFER(pSrcBuffer)->GetVkBuffer(),
                    TO_VK_BUFFER(pDstBuffer)->GetVkBuffer(), 1, &bufferCopy);
}

void FVulkanCommandListContext::RHIClearTexture(RHITexture* pTexture,
                                                const Color& color,
                                                const RHITextureSubResourceRange& range)
{
    VkImageSubresourceRange vkRange;
    ToVkImageSubresourceRange(range, &vkRange);
    VkClearColorValue colorValue;
    ToVkClearColor(color, &colorValue);
    vkCmdClearColorImage(GetCommandBuffer()->GetVkHandle(), TO_VK_TEXTURE(pTexture)->GetVkImage(),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colorValue, 1, &vkRange);
}

void FVulkanCommandListContext::RHICopyTexture(RHITexture* pSrcTexture,
                                               RHITexture* pDstTexture,
                                               VectorView<RHITextureCopyRegion> regions)
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

void FVulkanCommandListContext::RHIBlitTexture(RHITexture* pSrcTexture,
                                               RHITexture* pDstTexture,
                                               VectorView<RHITextureBlitRegion> regions,
                                               RHISamplerFilter filter)
{
    HeapVector<VkImageBlit> blits(regions.size());

    for (uint32_t i = 0; i < regions.size(); i++)
    {
        ToVkImageBlit(regions[i], &blits[i]);
    }

    vkCmdBlitImage(GetCommandBuffer()->GetVkHandle(), TO_VK_TEXTURE(pSrcTexture)->GetVkImage(),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TO_VK_TEXTURE(pDstTexture)->GetVkImage(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, blits.size(), blits.data(),
                   ToVkFilter(filter));
}

void FVulkanCommandListContext::RHICopyTextureToBuffer(
    RHITexture* pSrcTex,
    RHIBuffer* pDstBuffer,
    VectorView<RHIBufferTextureCopyRegion> regions)
{
    HeapVector<VkBufferImageCopy> copies(regions.size());

    for (uint32_t i = 0; i < regions.size(); i++)
    {
        ToVkBufferImageCopy(regions[i], &copies[i]);
    }

    vkCmdCopyImageToBuffer(GetCommandBuffer()->GetVkHandle(), TO_VK_TEXTURE(pSrcTex)->GetVkImage(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           TO_VK_BUFFER(pDstBuffer)->GetVkBuffer(), copies.size(), copies.data());
}

void FVulkanCommandListContext::RHICopyBufferToTexture(
    RHIBuffer* pSrcBuffer,
    RHITexture* pDstTexture,
    VectorView<RHIBufferTextureCopyRegion> regions)
{
    HeapVector<VkBufferImageCopy> copies(regions.size());

    for (uint32_t i = 0; i < copies.size(); i++)
    {
        ToVkBufferImageCopy(regions[i], &copies[i]);
    }

    vkCmdCopyBufferToImage(GetCommandBuffer()->GetVkHandle(),
                           TO_VK_BUFFER(pSrcBuffer)->GetVkBuffer(),
                           TO_VK_TEXTURE(pDstTexture)->GetVkImage(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copies.size(), copies.data());
}

void FVulkanCommandListContext::RHIResolveTexture(RHITexture* pSrcTexture,
                                                  RHITexture* pDstTexture,
                                                  uint32_t srcLayer,
                                                  uint32_t srcMipmap,
                                                  uint32_t dstLayer,
                                                  uint32_t dstMipmap)
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
    region.extent.width  = std::max(1u, pSrcTexture->GetBaseInfo().width >> srcMipmap);
    region.extent.height = std::max(1u, pSrcTexture->GetBaseInfo().height >> srcMipmap);
    region.extent.depth  = std::max(1u, pSrcTexture->GetBaseInfo().depth >> srcMipmap);
    vkCmdResolveImage(GetCommandBuffer()->GetVkHandle(), TO_VK_TEXTURE(pSrcTexture)->GetVkImage(),
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      TO_VK_TEXTURE(pDstTexture)->GetVkImage(),
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
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

void VulkanRHI::FinalizeCommandLists(VectorView<RHICommandList*> cmdLists,
                                     HeapVector<RHIPlatformCommandList*>& outCommandLists)
{
    if (cmdLists.empty() || m_submissionBlocked)
    {
        return;
    }

    for (RHICommandList* pCmdList : cmdLists)
    {
        VulkanPlatformCommandList* pPlatformCmdList = AcquirePlatformCommandList();

        pCmdList->Execute();

        FVulkanCommandListContext* pContext =
            static_cast<FVulkanCommandListContext*>(pCmdList->GetContext());
        pContext->CollectWorkloads(pPlatformCmdList->m_workloads);

        if (pPlatformCmdList->m_workloads.empty())
        {
            ReleasePlatformCommandList(pPlatformCmdList);
            continue;
        }

        pPlatformCmdList->m_contextWorkloadRanges.push_back(
            VulkanPlatformCommandList::ContextWorkloadRange{
                pContext,
                0,
                static_cast<uint32_t>(pPlatformCmdList->m_workloads.size()),
            });
        outCommandLists.push_back(pPlatformCmdList);
    }
}

void VulkanRHI::SubmitPlatformCommandLists(VectorView<RHIPlatformCommandList*> commandLists)
{
    for (RHIPlatformCommandList* pCommandList : commandLists)
    {
        VulkanPlatformCommandList* pPlatformCmdList =
            static_cast<VulkanPlatformCommandList*>(pCommandList);

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
    HeapVector<VulkanQueue*> queues;

    for (uint32_t i = 0; i < ToUnderlying(RHICommandContextType::eMax); ++i)
    {
        VulkanQueue* queue = m_pDevice->GetQueue(static_cast<RHICommandContextType>(i));

        if (std::find(queues.begin(), queues.end(), queue) == queues.end())
        {
            queues.push_back(queue);
        }
    }

    RHISubmissionResult result =
        m_submissionBlocked ? RHISubmissionResult::eFatal : RHISubmissionResult::eSuccess;
    bool submitted = false;

    for (VulkanQueue* queue : queues)
    {
        if (result != RHISubmissionResult::eSuccess)
        {
            break;
        }

        uint64_t serial = 0;
        result          = queue->SubmitPendingWorkloads(serial);
        submitted |= serial != 0;

        if (result == RHISubmissionResult::eRejected && submitted)
        {
            result = RHISubmissionResult::eFatal;
        }
    }

    if (result == RHISubmissionResult::eFatal)
    {
        BlockSubmissions();
    }

    // Workload objects stay alive until every context has read the actual accepted serials.
    // Shared physical queues must not be polled again before these references are consumed.
    for (VulkanPlatformCommandList* platform : m_pendingPlatformCmdLists)
    {
        for (VulkanPlatformCommandList::ContextWorkloadRange const& range :
             platform->m_contextWorkloadRanges)
        {
            uint64_t serial = 0;

            for (uint32_t i = 0; i < range.workloadCount; ++i)
            {
                serial = std::max(
                    serial,
                    platform->m_workloads[range.firstWorkloadIndex + i]->m_submissionSerial);
            }

            range.pContext->SetLastSubmittedSerial(serial);
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
