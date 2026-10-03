#include "VulkanIntegrationFixture.h"
#include "ScopedVulkanCall.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanMemory.h"
#include "Graphics/VulkanRHI/VulkanDescriptorPool.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/RHI/RHIOptions.h"
#include <gtest/gtest.h>
#include <filesystem>

namespace
{
using namespace zen;

struct PipelineFailure
{
    static inline PFN_vkCreateGraphicsPipelines graphics;
    static inline PFN_vkCreateComputePipelines  compute;
    static inline PFN_vkDestroyPipeline         destroy;
    static inline bool                          partial;
    static inline uint32_t                      destroyed;

    static VKAPI_ATTR VkResult VKAPI_CALL Graphics(VkDevice                            device,
                                                   VkPipelineCache                     cache,
                                                   uint32_t                            count,
                                                   const VkGraphicsPipelineCreateInfo* infos,
                                                   const VkAllocationCallbacks*        allocator,
                                                   VkPipeline*                         pipelines)
    {
        if (partial)
        {
            EXPECT_EQ(graphics(device, cache, count, infos, allocator, pipelines), VK_SUCCESS);
        }

        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Compute(VkDevice                           device,
                                                  VkPipelineCache                    cache,
                                                  uint32_t                           count,
                                                  const VkComputePipelineCreateInfo* infos,
                                                  const VkAllocationCallbacks*       allocator,
                                                  VkPipeline*                        pipelines)
    {
        if (partial)
        {
            EXPECT_EQ(compute(device, cache, count, infos, allocator, pipelines), VK_SUCCESS);
        }

        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    static VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* allocator)
    {
        if (pipeline != VK_NULL_HANDLE)
        {
            ++destroyed;
        }

        destroy(device, pipeline, allocator);
    }
};

void AddProductionStage(RHIShaderCreateInfo& info, RHIShaderStage stage, const char* file)
{
    info.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));

    info.spirvFileName[ToUnderlying(stage)] =
        std::filesystem::relative(std::filesystem::path(RDG_REFLECTION_TEST_PATH) / file, SPV_SHADER_PATH).generic_string();
}

TEST(VulkanProductionFailureTest, GraphicsAndComputeFailuresCleanPartialOutputsAndAllowRetry)
{
    test::VulkanSession session;

    PipelineFailure::graphics = vkCreateGraphicsPipelines;

    PipelineFailure::compute  = vkCreateComputePipelines;

    PipelineFailure::destroy  = vkDestroyPipeline;

    test::ScopedVulkanCall<PFN_vkDestroyPipeline> destruction(vkDestroyPipeline, PipelineFailure::Destroy);

    for (bool compute : {false, true})
    {
        RHIShaderCreateInfo shaderInfo{};

        if (compute)
        {
            AddProductionStage(shaderInfo, RHIShaderStage::eCompute, "pipeline_specialization.comp.spv");
        }
        else
        {
            AddProductionStage(shaderInfo, RHIShaderStage::eVertex, "pipeline.vert.spv");

            AddProductionStage(shaderInfo, RHIShaderStage::eFragment, "pipeline.frag.spv");
        }

        RHIShader* shader = session.rhi.CreateShader(shaderInfo);

        ASSERT_NE(shader, nullptr);

        RHIRenderingLayout layout{};

        layout.SetRenderArea(0, 0, 4, 4);

        layout.numColorRenderTargets        = 1;

        layout.colorRenderTargets[0].format = DataFormat::eR8G8B8A8UNORM;

        RHIGfxPipelineCreateInfo graphicsInfo{};

        graphicsInfo.pShader          = shader;

        graphicsInfo.pRenderingLayout = &layout;

        graphicsInfo.states.colorBlendState.AddAttachment();

        RHIComputePipelineCreateInfo computeInfo{};

        computeInfo.pShader = shader;

        for (bool partial : {false, true})
        {
            PipelineFailure::partial   = partial;

            PipelineFailure::destroyed = 0;

            test::ScopedVulkanCall<PFN_vkCreateGraphicsPipelines> graphicsFailure(vkCreateGraphicsPipelines,
                                                                                  PipelineFailure::Graphics);

            test::ScopedVulkanCall<PFN_vkCreateComputePipelines> computeFailure(vkCreateComputePipelines,
                                                                                PipelineFailure::Compute);

            EXPECT_EQ(compute ? session.rhi.CreatePipeline(computeInfo) : session.rhi.CreatePipeline(graphicsInfo), nullptr);

            EXPECT_EQ(PipelineFailure::destroyed, partial ? 1u : 0u);
        }

        RHIPipeline* retry = compute ? session.rhi.CreatePipeline(computeInfo) : session.rhi.CreatePipeline(graphicsInfo);

        ASSERT_NE(retry, nullptr);

        session.rhi.DestroyPipeline(retry);

        session.rhi.DestroyShader(shader);
    }
}

TEST(VulkanProductionFailureTest, InvalidGraphicsLayoutReturnsNull)
{
    test::VulkanSession session;

    RHIGfxPipelineCreateInfo info{};

    EXPECT_EQ(session.rhi.CreatePipeline(info), nullptr);

    RHIShaderCreateInfo shaderInfo{};

    AddProductionStage(shaderInfo, RHIShaderStage::eVertex, "pipeline.vert.spv");

    AddProductionStage(shaderInfo, RHIShaderStage::eFragment, "pipeline.frag.spv");

    RHIShader* shader = session.rhi.CreateShader(shaderInfo);

    ASSERT_NE(shader, nullptr);

    RHIRenderingLayout layout{};

    info.pShader                 = shader;

    info.pRenderingLayout        = &layout;

    layout.numColorRenderTargets = MAX_NUM_COLOR_ATTACHMENTS + 1;

    EXPECT_EQ(session.rhi.CreatePipeline(info), nullptr);

    layout.numColorRenderTargets        = 1;

    layout.colorRenderTargets[0].format = DataFormat::eUndefined;

    EXPECT_EQ(session.rhi.CreatePipeline(info), nullptr);

    session.rhi.DestroyShader(shader);
}

TEST(VulkanProductionFailureTest, BufferAllocationAndMappingFailureReturnNullWithoutLeaks)
{
    test::VulkanSession session;

    for (bool mapping : {false, true})
    {
        VulkanMemoryAllocator allocator;

        const PFN_vkAllocateMemory allocate = vkAllocateMemory;

        test::ScopedVulkanCall<PFN_vkAllocateMemory> allocationFailure(
            vkAllocateMemory, [](VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*) {
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            });

        test::ScopedVulkanCall<PFN_vkMapMemory> mappingFailure(vkMapMemory, [](VkDevice, VkDeviceMemory, VkDeviceSize,
                                                                               VkDeviceSize, VkMemoryMapFlags,
                                                                               void**) { return VK_ERROR_MEMORY_MAP_FAILED; });

        // VMA captures these function pointers at initialization. Restore allocation
        // for the mapping case so the failure happens after native memory is created.
        if (mapping)
        {
            vkAllocateMemory = allocate;
        }

        allocator.Init(session.rhi.GetInstance(), session.rhi.GetDevice()->GetPhysicalDeviceHandle(), session.rhi.GetVkDevice(),
                       false);

        VulkanMemoryAllocator* original = GVkMemAllocator;

        GVkMemAllocator                 = &allocator;

        RHIBufferCreateInfo info{};

        info.size         = 4096;

        info.allocateType = RHIBufferAllocateType::eCPUWrite;

        info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferSrcBuffer);

        EXPECT_EQ(session.rhi.CreateBuffer(info), nullptr);

        // A failed mapping may leave an empty VMA block cached; allocator teardown
        // reports allocation bytes and must release every native commitment.
        if (!mapping)
        {
            EXPECT_EQ(allocator.GetGPUMemoryStats().committedBytes, 0u);
        }

        GVkMemAllocator = original;
    }
}

TEST(VulkanProductionFailureTest, ShaderStagesLayoutsAndSamplerRejectNativeFailure)
{
    test::VulkanSession session;

    RHIShaderCreateInfo shaderInfo{};

    AddProductionStage(shaderInfo, RHIShaderStage::eCompute, "descriptor_uniform.comp.spv");
    {
        test::ScopedVulkanCall<PFN_vkCreateShaderModule> fail(
            vkCreateShaderModule, [](VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*) {
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            });

        EXPECT_EQ(session.rhi.CreateShader(shaderInfo), nullptr);
    }

    {
        test::ScopedVulkanCall<PFN_vkCreateDescriptorSetLayout> fail(
            vkCreateDescriptorSetLayout, [](VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*,
                                            VkDescriptorSetLayout*) { return VK_ERROR_OUT_OF_DEVICE_MEMORY; });

        EXPECT_EQ(session.rhi.CreateShader(shaderInfo), nullptr);
    }

    {
        test::ScopedVulkanCall<PFN_vkCreatePipelineLayout> fail(
            vkCreatePipelineLayout, [](VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*,
                                       VkPipelineLayout*) { return VK_ERROR_OUT_OF_DEVICE_MEMORY; });

        EXPECT_EQ(session.rhi.CreateShader(shaderInfo), nullptr);
    }

    {
        test::ScopedVulkanCall<PFN_vkCreateSampler> fail(vkCreateSampler,
                                                         [](VkDevice, const VkSamplerCreateInfo*, const VkAllocationCallbacks*,
                                                            VkSampler*) { return VK_ERROR_OUT_OF_DEVICE_MEMORY; });

        EXPECT_EQ(session.rhi.CreateSampler({}), nullptr);
    }

    RHIShader* retry = session.rhi.CreateShader(shaderInfo);

    ASSERT_NE(retry, nullptr);

    session.rhi.DestroyShader(retry);
}

TEST(VulkanProductionFailureTest, DescriptorPoolAndFenceFailureDoNotPublishOwners)
{
    test::VulkanSession session;
    {
        test::ScopedVulkanCall<PFN_vkCreateDescriptorPool> fail(
            vkCreateDescriptorPool, [](VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*,
                                       VkDescriptorPool*) { return VK_ERROR_OUT_OF_DEVICE_MEMORY; });

        VulkanDescriptorPool pool(session.rhi.GetDevice(), {}, 32, false);

        EXPECT_FALSE(pool.IsValid());
    }

    {
        test::ScopedVulkanCall<PFN_vkCreateFence> fail(vkCreateFence,
                                                       [](VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*,
                                                          VkFence*) { return VK_ERROR_OUT_OF_HOST_MEMORY; });

        EXPECT_EQ(session.rhi.GetDevice()->GetFenceManager()->CreateFence(), nullptr);
    }
}

TEST(VulkanProductionFailureTest, FailedCommandPoolReturnsNoContextAndAllowsRetry)
{
    test::VulkanSession session;
    {
        test::ScopedVulkanCall<PFN_vkCreateCommandPool> fail(
            vkCreateCommandPool, [](VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool*) {
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            });

        EXPECT_EQ(session.rhi.GetCommandContext(RHICommandContextType::eGraphics), nullptr);

        EXPECT_EQ(RHICommandList::Create(nullptr), nullptr);
    }

    IRHICommandContext* retry = session.rhi.GetCommandContext(RHICommandContextType::eGraphics);

    ASSERT_NE(retry, nullptr);

    ZEN_DELETE(retry);
}

TEST(VulkanProductionFailureTest, AbandonedRecordingRollsBackOnlyItsOwnBindlessRegistration)
{
    test::VulkanSession session;

    VulkanBindlessDescriptorPoolManager* manager = session.rhi.GetBindlessDescriptorPoolManager();

    RHISampler* retained                         = session.rhi.CreateSampler({});

    RHISampler* abandoned                        = session.rhi.CreateSampler({});

    ASSERT_NE(retained, nullptr);

    ASSERT_NE(abandoned, nullptr);

    RHIBindlessHandle published;

    ASSERT_TRUE(manager->RegisterBindlessResource(retained, 0, &published));

    FVulkanCommandListContext* context =
        static_cast<FVulkanCommandListContext*>(session.rhi.GetCommandContext(RHICommandContextType::eGraphics));

    const uint64_t transaction = context->GetRecordingTransaction();

    ASSERT_TRUE(manager->RegisterBindlessResource(abandoned, 1, nullptr, 0, transaction));

    EXPECT_EQ(abandoned->GetRefCount(), 2u);

    ZEN_DELETE(context);

    EXPECT_EQ(abandoned->GetRefCount(), 1u);

    EXPECT_TRUE(manager->IsRegistered(published));

    EXPECT_EQ(retained->GetRefCount(), 2u);

    EXPECT_TRUE(manager->UnregisterBindlessResource(published));

    session.rhi.DestroySampler(abandoned);

    session.rhi.DestroySampler(retained);
}

struct RecordingFailureProbe
{
    static inline uint32_t submissions{0};
    static inline uint32_t dispatches{0};

    static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence)
    {
        ++submissions;

        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    static VKAPI_ATTR void VKAPI_CALL Dispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t)
    {
        ++dispatches;
    }
};

TEST(VulkanProductionFailureTest, MemoryBudgetReportsNativeHeapsWhenSupported)
{
    test::VulkanSession session;

    const RHIGPUMemoryStats stats = session.rhi.GetGPUMemoryStats();

    EXPECT_TRUE(stats.available);

    EXPECT_EQ(stats.budgetAvailable, session.rhi.GetDevice()->GetExtensionFlags().hasMemoryBudget != 0);

    if (stats.budgetAvailable)
    {
        EXPECT_GT(stats.heapCount, 0u);

        EXPECT_LE(stats.heapCount, stats.heaps.size());

        for (uint32_t i = 0; i < stats.heapCount; ++i)
        {
            EXPECT_GT(stats.heaps[i].sizeBytes, 0u);

            EXPECT_GT(stats.heaps[i].budgetBytes, 0u);
        }
    }
}

TEST(VulkanProductionFailureTest, ShutdownRejectsOutstandingResourcesAndCommandContexts)
{
    EXPECT_DEATH(
        {
            RHIOptions::GetInstance().SetStrictTeardownChecks(true);

            test::VulkanSession session;

            session.rhi.GetCommandContext(RHICommandContextType::eGraphics);

            session.rhi.Destroy();
        },
        "All RHI command contexts must be released before queue teardown");

    EXPECT_DEATH(
        {
            RHIOptions::GetInstance().SetStrictTeardownChecks(true);

            test::VulkanSession session;

            RHIBufferCreateInfo info{};

            info.size = 16;

            info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);

            session.rhi.CreateBuffer(info);

            session.rhi.Destroy();
        },
        "All RHI resources must be released before backend teardown");
}

TEST(VulkanProductionFailureTest, BackendDestructionClearsGlobalPointers)
{
    VulkanRHI* rhi = ZEN_NEW() VulkanRHI();

    GDynamicRHI    = rhi;

    rhi->Init();

    EXPECT_EQ(GVulkanRHI, rhi);

    rhi->Destroy();

    ZEN_DELETE(rhi);

    EXPECT_EQ(GVulkanRHI, nullptr);

    EXPECT_EQ(GDynamicRHI, nullptr);
}

#if defined(NDEBUG)
// Debug VMA asserts on leaked allocations, so only Release can observe teardown continuing.
// The deliberate leak would also be reported by validation at device destruction.
TEST(VulkanProductionFailureTest, NonStrictShutdownLogsOutstandingResourcesAndContinues)
{
    EXPECT_EXIT(
        {
            RHIOptions::GetInstance().SetStrictTeardownChecks(false);

            RHIOptions::GetInstance().SetValidationEnabled(false);

            test::VulkanSession session;

            RHIBufferCreateInfo info{};

            info.size = 16;

            info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);

            session.rhi.CreateBuffer(info);

            session.rhi.Destroy();

            std::fprintf(stderr, "teardown continued\n");

            std::fflush(stderr);

            std::_Exit(0);
        },
        testing::ExitedWithCode(0), "teardown continued");
}
#endif

class VulkanProductionDiagnosticsTest : public testing::TestWithParam<bool>
{};

TEST_P(VulkanProductionDiagnosticsTest, DeviceLossReportsCompletedGpuBreadcrumbAndDriverFault)
{
    struct DiagnosticsGuard
    {
        bool previous{RHIOptions::GetInstance().DeviceLossDiagnostics()};

        ~DiagnosticsGuard()
        {
            RHIOptions::GetInstance().SetDeviceLossDiagnostics(previous);
        }
    } guard;

    RHIOptions::GetInstance().SetDeviceLossDiagnostics(true);

    test::VulkanSession session;

    if (GetParam())
    {
        session.rhi.GetDevice()->GetExtensionFlags().hasBufferMarker = 0;
    }

    FVulkanCommandListContext* context =
        static_cast<FVulkanCommandListContext*>(session.rhi.GetCommandContext(RHICommandContextType::eGraphics));

    context->RHIBeginDebugLabel("production_diagnostic_pass");

    context->RHIEndDebugLabel();

    ASSERT_EQ(context->SubmitRecordedWorkloads(), RHISubmissionResult::eSuccess);

    context->WaitForLastSubmittedWork(UINT64_MAX);

    // Exercise device-fault reporting without deliberately losing the user's GPU.
    session.rhi.GetDevice()->GetExtensionFlags().hasDeviceFault = 1;

    test::ScopedVulkanCall<PFN_vkGetDeviceFaultInfoEXT> fault(
        vkGetDeviceFaultInfoEXT, [](VkDevice, VkDeviceFaultCountsEXT* counts, VkDeviceFaultInfoEXT* info) {
            counts->addressInfoCount = 0;

            counts->vendorInfoCount  = 0;

            std::strcpy(info->description, "injected driver fault");

            return VK_SUCCESS;
        });

    testing::internal::CaptureStderr();

    ReportVulkanDeviceLoss(VK_ERROR_DEVICE_LOST, "injected submission");

    const std::string diagnostic = testing::internal::GetCapturedStderr();

    EXPECT_NE(diagnostic.find("last_started=production_diagnostic_pass"), std::string::npos);

    EXPECT_NE(diagnostic.find("last_completed=production_diagnostic_pass"), std::string::npos);

    EXPECT_NE(diagnostic.find("injected driver fault"), std::string::npos);

    EXPECT_TRUE(session.rhi.AreSubmissionsBlocked());

    ZEN_DELETE(context);
}

INSTANTIATE_TEST_SUITE_P(NativeAndPortable, VulkanProductionDiagnosticsTest, testing::Bool());

TEST(VulkanProductionFailureTest, FailedCommandRecordingNeverReachesSubmissionAndCanRetry)
{
    test::VulkanSession session;

    for (uint32_t stage = 0; stage < 4; ++stage)
    {
        FVulkanCommandListContext* context =
            static_cast<FVulkanCommandListContext*>(session.rhi.GetCommandContext(RHICommandContextType::eGraphics));

        ASSERT_NE(context, nullptr);

        // Give reset a real, discarded native recording to reset.
        if (stage == 2)
        {
            ASSERT_TRUE(context->EnsureRecording());

            context->DiscardRecording();
        }

        const PFN_vkAllocateCommandBuffers allocate = vkAllocateCommandBuffers;

        const PFN_vkBeginCommandBuffer begin        = vkBeginCommandBuffer;

        const PFN_vkResetCommandBuffer reset        = vkResetCommandBuffer;

        const PFN_vkEndCommandBuffer end            = vkEndCommandBuffer;

        RecordingFailureProbe::submissions          = 0;
        {
            test::ScopedVulkanCall<PFN_vkAllocateCommandBuffers> failAllocate(
                vkAllocateCommandBuffers,
                stage == 0 ?
                    +[](VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*) {
                        return VK_ERROR_OUT_OF_HOST_MEMORY;
                    } :
                    allocate);

            test::ScopedVulkanCall<PFN_vkBeginCommandBuffer> failBegin(
                vkBeginCommandBuffer,
                stage == 1 ?
                    +[](VkCommandBuffer, const VkCommandBufferBeginInfo*) {
                        return VK_ERROR_OUT_OF_HOST_MEMORY;
                    } :
                    begin);

            test::ScopedVulkanCall<PFN_vkResetCommandBuffer> failReset(
                vkResetCommandBuffer,
                stage == 2 ?
                    +[](VkCommandBuffer, VkCommandBufferResetFlags) {
                        return VK_ERROR_OUT_OF_HOST_MEMORY;
                    } :
                    reset);

            test::ScopedVulkanCall<PFN_vkEndCommandBuffer> failEnd(
                vkEndCommandBuffer,
                stage == 3 ?
                    +[](VkCommandBuffer) {
                        return VK_ERROR_OUT_OF_HOST_MEMORY;
                    } :
                    end);

            test::ScopedVulkanCall<PFN_vkQueueSubmit> countSubmit(vkQueueSubmit, RecordingFailureProbe::Submit);

            context->EnsureRecording();

            EXPECT_EQ(context->SubmitRecordedWorkloads(), RHISubmissionResult::eRejected);

            EXPECT_EQ(RecordingFailureProbe::submissions, 0u);
        }

        context->DiscardRecording();

        EXPECT_TRUE(context->EnsureRecording());

        EXPECT_EQ(context->SubmitRecordedWorkloads(), RHISubmissionResult::eSuccess);

        context->WaitForLastSubmittedWork(UINT64_MAX);

        ZEN_DELETE(context);
    }
}

TEST(VulkanProductionFailureTest, FailedUniformOrDescriptorPreparationSuppressesDispatch)
{
    test::VulkanSession session;

    RHIShaderCreateInfo shaderInfo{};

    AddProductionStage(shaderInfo, RHIShaderStage::eCompute, "descriptor_uniform.comp.spv");

    RHIShader* shader = session.rhi.CreateShader(shaderInfo);

    ASSERT_NE(shader, nullptr);

    RHIPipeline* pipeline = session.rhi.CreatePipeline(RHIComputePipelineCreateInfo{shader});

    ASSERT_NE(pipeline, nullptr);

    for (bool undersized : {true, false})
    {
        FVulkanCommandListContext* context =
            static_cast<FVulkanCommandListContext*>(session.rhi.GetCommandContext(RHICommandContextType::eGraphics));

        RHIBufferCreateInfo bufferInfo{};

        bufferInfo.size         = undersized ? 4 : 16;

        bufferInfo.allocateType = RHIBufferAllocateType::eCPUWrite;

        bufferInfo.usageFlags.SetFlag(RHIBufferUsageFlagBits::eUniformBuffer);

        RHIBuffer* buffer = session.rhi.CreateBuffer(bufferInfo);

        ASSERT_NE(buffer, nullptr);

        RHIBatchedShaderParameters parameters;

        parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 0), buffer, nullptr, 0);

        context->RHIBindPipeline(pipeline);

        context->RHISetShaderParameters(parameters);

        RecordingFailureProbe::dispatches  = 0;

        RecordingFailureProbe::submissions = 0;
        {
            test::ScopedVulkanCall<PFN_vkAllocateDescriptorSets> fail(
                vkAllocateDescriptorSets,
                [](VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*) { return VK_ERROR_OUT_OF_DEVICE_MEMORY; });

            test::ScopedVulkanCall<PFN_vkCmdDispatch> countDispatch(vkCmdDispatch, RecordingFailureProbe::Dispatch);

            test::ScopedVulkanCall<PFN_vkQueueSubmit> countSubmit(vkQueueSubmit, RecordingFailureProbe::Submit);

            context->RHIDispatch(1, 1, 1);

            context->RHIDispatch(1, 1, 1);

            EXPECT_TRUE(context->GetRecordingError().IsFailure());

            EXPECT_EQ(context->SubmitRecordedWorkloads(), RHISubmissionResult::eRejected);

            EXPECT_EQ(RecordingFailureProbe::dispatches, 0u);

            EXPECT_EQ(RecordingFailureProbe::submissions, 0u);
        }

        ZEN_DELETE(context);

        session.rhi.DestroyBuffer(buffer);
    }

    session.rhi.DestroyPipeline(pipeline);

    session.rhi.DestroyShader(shader);
}

TEST(VulkanProductionFailureDeathTest, DeviceSelectionReportsAnUnsupportedProfile)
{
    EXPECT_DEATH(
        {
            RHIOptions::GetInstance().SetBindlessHeapCapacities({UINT32_MAX, UINT32_MAX, UINT32_MAX});

            VulkanRHI rhi;

            rhi.Init();
        },
        "No Vulkan device satisfies");
}

TEST(VulkanProductionFailureTest, InvalidShaderDoesNotPublishAndSpecializationUsesOwnedBytecode)
{
    test::VulkanSession session;

    RHIShaderCreateInfo invalid;

    invalid.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);

    invalid.spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    invalid.spirv->SetStageSPIRV(RHIShaderStage::eCompute, HeapVector<uint8_t>{0, 1, 2, 3});

    EXPECT_EQ(session.rhi.CreateShader(invalid), nullptr);

    RHIShaderCreateInfo baseInfo;

    AddProductionStage(baseInfo, RHIShaderStage::eCompute, "pipeline_specialization.comp.spv");

    RHIShader* base = session.rhi.CreateShader(baseInfo);

    ASSERT_NE(base, nullptr);

    RHIShaderCreateInfo specialized                                   = base->GetCreateInfo();

    specialized.spirvFileName[ToUnderlying(RHIShaderStage::eCompute)] = "missing-specialization-source.spv";

    RHIShader* shader                                                 = session.rhi.CreateShader(specialized);

    ASSERT_NE(shader, nullptr);

    session.rhi.DestroyShader(shader);

    ASSERT_TRUE(specialized.reflection.has_value());

    specialized.reflection->SRDTable.resize(2);

    RHIShaderResourceDescriptor invalidDescriptor;

    invalidDescriptor.type = RHIShaderResourceType::eMax;

    specialized.reflection->SRDTable[1].push_back(invalidDescriptor);

    EXPECT_EQ(session.rhi.CreateShader(specialized), nullptr);

    session.rhi.DestroyShader(base);
}
} // namespace
