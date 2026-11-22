#include <span>
#include "Math/Math.h"
#include "AssetLib/SceneRuntimeBuilder.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/SceneRayQuery.h"
#include "Graphics/RenderCore/V2/DistanceFieldScene.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/HybridGIRenderer.h"
#include "Graphics/Shared/ReSTIRGI.h"
#include "Graphics/Shared/LightingCapture.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/Shared/EnvironmentSampling.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "ZenRHI/RHIGPUFrameTiming.h"
#include "SceneGraph/Camera.h"
#include <volk.h>
#include "ZenRHI/Vulkan/VulkanInterop.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <chrono>
#include <thread>

namespace
{
using namespace zen;
using namespace zen::rc;

class RayQueryIntegrationTest : public testing::TestWithParam<uint32_t>
{
protected:
    RenderDevice*                         device{nullptr};
    HeapVector<RHIBuffer*>                buffers;
    HeapVector<RHITexture*>               textures;
    HeapVector<RHIAccelerationStructure*> structures;
    VkDebugUtilsMessengerEXT              messenger{VK_NULL_HANDLE};
    bool                                  countersEnabled{false};
    bool                                  validationEnabled{true};
    bool                                  profilerMarkers{false};

    static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                     VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                     void*)
    {
        ADD_FAILURE() << data->pMessage;
        return VK_FALSE;
    }

    void SetUp() override
    {
        SetUpDevice(true);
    }

    void SetUpDevice(bool hardwareRayTracing, bool gpuProfile = false)
    {
        countersEnabled   = RHIOptions::GetInstance().ExecutionCountersEnabled();
        validationEnabled = RHIOptions::GetInstance().ValidationEnabled();
        profilerMarkers   = RHIOptions::GetInstance().GPUProfilerMarkers();
        RHIOptions::GetInstance().SetExecutionCountersEnabled(!gpuProfile);
        RHIOptions::GetInstance().SetValidationEnabled(validationEnabled && !gpuProfile);
        RHIOptions::GetInstance().SetGPUProfilerMarkers(profilerMarkers || gpuProfile);
        RHIOptions::GetInstance().SetRayTracingDisabled(!hardwareRayTracing);
        device = ZEN_NEW()
            RenderDevice(RHIAPIType::eVulkan, 2, (GetParam() & 1) ? RHIExecutionMode::eThreaded : RHIExecutionMode::eInline,
                         (GetParam() & 2) ? AsyncComputeMode::eAuto : AsyncComputeMode::eDisabled);
        device->Init(nullptr);
        if (RHIOptions::GetInstance().ValidationEnabled())
        {
            VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            info.pfnUserCallback = Validation;
            ASSERT_EQ(vkCreateDebugUtilsMessengerEXT(GetVulkanDeviceHandles().instance, &info, nullptr, &messenger),
                      VK_SUCCESS);
        }
        if (device->GetGPUInfo().rayQuery.IsUsable())
        {
            ShaderProgramManager::GetInstance().StoreProgram(
                ZEN_NEW() ComputeFileSP(device, "RayQueryCheckSP", "VoxelGI/Calibration/ray_query_check.comp.spv"));
            ShaderProgramManager::GetInstance().StoreProgram(
                ZEN_NEW() ComputeFileSP(device, "SceneRayQueryCheckSP", "VoxelGI/Calibration/scene_ray_query_check.comp.spv"));
        }
        if (device->GetGPUInfo().rayTracing.IsUsable())
        {
            const HeapVector<ShaderProgram::StageFile> modules = {
                {RHIShaderStage::eRayGeneration, "VoxelGI/Calibration/scene_rt_check.rgen.spv"},
                {RHIShaderStage::eMiss, "VoxelGI/Calibration/scene_rt_check.rmiss.spv"},
                {RHIShaderStage::eClosestHit, "VoxelGI/Calibration/scene_rt_check.rchit.spv"},
                {RHIShaderStage::eAnyHit, "VoxelGI/Calibration/scene_rt_check.rahit.spv"}};
            ShaderProgramManager::GetInstance().StoreProgram(ZEN_NEW() RayTracingFileSP(device, "SceneRTCheckSP", modules));
            ShaderProgramManager::GetInstance().StoreProgram(
                ZEN_NEW() RayTracingFileSP(device, "SceneRTPlainCheckSP", modules, false));
        }
    }

    void TearDown() override
    {
        device->FlushRHIThread();
        device->WaitForIdle();
        for (RHIAccelerationStructure* structure : structures)
        {
            device->DeferReleaseResource(structure);
        }
        for (RHIBuffer* buffer : buffers)
        {
            device->DestroyBuffer(buffer);
        }
        for (RHITexture* texture : textures)
        {
            device->DestroyTexture(texture);
        }
        ShaderProgramManager::GetInstance().Destroy();
        if (messenger != VK_NULL_HANDLE)
        {
            vkDestroyDebugUtilsMessengerEXT(GetVulkanDeviceHandles().instance, messenger, nullptr);
        }
        device->Destroy();
        ZEN_DELETE(device);
        RHIOptions::GetInstance().SetExecutionCountersEnabled(countersEnabled);
        RHIOptions::GetInstance().SetValidationEnabled(validationEnabled);
        RHIOptions::GetInstance().SetGPUProfilerMarkers(profilerMarkers);
        RHIOptions::GetInstance().SetRayTracingDisabled(false);
    }

    RHIBuffer* Buffer(uint32_t                         size,
                      RHIFlags<RHIBufferUsageFlagBits> usage,
                      const void*                      data       = nullptr,
                      RHIBufferAllocateType            allocation = RHIBufferAllocateType::eCPUWriteGPURead)
    {
        RHIBufferCreateInfo info;
        info.size         = size;
        info.usageFlags   = usage;
        info.allocateType = allocation;
        RHIBuffer* buffer = device->CreateBuffer(info);
        buffers.push_back(buffer);
        if (data != nullptr && buffer != nullptr)
        {
            std::memcpy(buffer->Map(), data, size);
            buffer->Unmap();
        }
        return buffer;
    }

    RHIAccelerationStructure* Structure(const RHIAccelerationStructureBuildDesc&   desc,
                                        std::span<RHIAccelerationStructure* const> references = {})
    {
        const RHIAccelerationStructureBuildSizes sizes = GDynamicRHI->GetAccelerationStructureBuildSizes(desc);
        RHIAccelerationStructureCreateInfo       info;
        info.type                        = desc.type;
        info.size                        = sizes.storageSize;
        info.referencedStructures        = references;
        RHIAccelerationStructure* result = GDynamicRHI->CreateAccelerationStructure(info);
        structures.push_back(result);
        return result;
    }

    void CheckQueryQueue(const RenderGraph& graph, uint32_t expectedQueries = 1) const
    {
        const bool compute = device->GetAsyncComputeStatus() == AsyncComputeStatus::eAvailable;

        uint32_t queries   = 0;

        for (const RDGSubmissionGroup& group : graph.GetSchedule().groups)
        {
            for (const RDGScheduledPass& pass : group.passes)
            {
                if (pass.preference == RDGQueuePreference::ePreferAsyncCompute)
                {
                    ++queries;

                    EXPECT_EQ(group.queue, compute ? RHICommandContextType::eAsyncCompute : RHICommandContextType::eGraphics);

                    EXPECT_EQ(pass.eligibility == RDGAsyncComputeEligibility::eEligible, compute);
                }
            }
        }

        EXPECT_EQ(queries, expectedQueries);
    }

    void AllocateBuildScratch(RHIAccelerationStructureBuildInfo& build)
    {
        const RHIAccelerationStructureBuildSizes sizes     = GDynamicRHI->GetAccelerationStructureBuildSizes(build.description);
        const uint64_t                           alignment = device->GetGPUInfo().rayQuery.scratchAlignment;
        RHIFlags<RHIBufferUsageFlagBits>         usage;
        usage.SetFlags(RHIBufferUsageFlagBits::eDeviceAddress, RHIBufferUsageFlagBits::eStorageBuffer);
        build.pScratchBuffer =
            Buffer(static_cast<uint32_t>(std::max(sizes.buildScratchSize, sizes.updateScratchSize) + alignment), usage);
        if (build.pScratchBuffer != nullptr)
        {
            build.scratchOffset = (alignment - build.pScratchBuffer->GetDeviceAddress() % alignment) % alignment;
        }
    }

    void CheckSceneHits(RenderScene&   scene,
                        SceneRayQuery& queries,
                        float          dynamicDistance,
                        bool           rawIntersection = false,
                        float          staticDistance  = 1.5f)
    {
        RenderGraph&                     graph = *device->GetCurrentFrameRDG();
        RHIFlags<RHIBufferUsageFlagBits> usage;
        usage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
        RHIBuffer* output = Buffer(4 * sizeof(Vec4), usage, nullptr, RHIBufferAllocateType::eGPU);
        RHIBuffer* readback =
            Buffer(4 * sizeof(Vec4), RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                   RHIBufferAllocateType::eCPURead);
        RDGComputePassDesc pass;
        pass.SetShaderProgramName(rawIntersection ? "RayQueryCheckSP" : "SceneRayQueryCheckSP");
        pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
        pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
        if (rawIntersection)
        {
            // The renderer reconstructs surface positions from current scene buffers.
            // Read the native intersection distance to inspect the committed AS alone.
            RDGComputePassDesc bindings;
            queries.BindInputs(bindings, scene);
            ASSERT_EQ(bindings.accelerationStructureBindings.size(), 1u);
            pass.BindAccelerationStructure("testScene", bindings.accelerationStructureBindings[0].pStructure);
        }
        else
        {
            const Vec4 gi[] = {Vec4(0, 0, 0, 1), Vec4(64), Vec4(0), Vec4(0), Vec4(0), Vec4(1, 0, 0, 0)};
            pass.BindValue("uGISettings", gi, sizeof(gi));
            pass.BindValue("uSceneData", scene.GetSceneUniformData(), sizeof(SceneUniformData));
            queries.BindInputs(pass, scene);
        }
        graph.AddComputePass(std::move(pass)).RecordPassCommands([rawIntersection](RDGPassCmdEncoder& encoder) {
            if (!rawIntersection)
            {
                encoder.SetPushConstants(Vec4(0));
            }
            encoder.Dispatch(4, 1, 1);
        });
        graph.AddTransferPass("ReadSceneQueries").CopyBuffer(output, readback, {0, 0, 4 * sizeof(Vec4)});
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        const bool succeeded = device->ExecuteRenderGraph(graph);
        queries.OnRenderGraphExecuted(succeeded ? RenderFrameResult::eSubmitted : RenderFrameResult::eFailed);
        ASSERT_TRUE(succeeded);
        CheckQueryQueue(graph);
        device->WaitForIdle();
        const Vec4* result = reinterpret_cast<const Vec4*>(readback->Map());
        EXPECT_NEAR(result[0].x, dynamicDistance, 1e-5f);
        EXPECT_NEAR(result[rawIntersection ? 2 : 1].x, staticDistance, 1e-5f);
        if (!rawIntersection)
        {
            EXPECT_EQ(result[0].w, float(queries.GetGeneration()));
        }
        readback->Unmap();
    }

    RHIBuffer* RecordScenePipelineCheck(RenderGraph&   graph,
                                        RenderScene&   scene,
                                        SceneRayQuery& queries,
                                        Vec4           worldOffset,
                                        bool           indirect,
                                        bool           reorder)
    {
        RHIBuffer* readback = nullptr;
        if (device->GetGPUInfo().rayTracing.IsUsable())
        {
            RHIFlags<RHIBufferUsageFlagBits> usage;
            usage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
            RHIBuffer* output = Buffer(4 * sizeof(Vec4), usage, nullptr, RHIBufferAllocateType::eGPU);
            readback = Buffer(4 * sizeof(Vec4), RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer),
                              nullptr, RHIBufferAllocateType::eCPURead);
            EXPECT_NE(output, nullptr);
            EXPECT_NE(readback, nullptr);
            if (output != nullptr && readback != nullptr)
            {
                RDGRayTracingPassDesc pass;
                pass.SetShaderProgramName(reorder ? "SceneRTCheckSP" : "SceneRTPlainCheckSP");
                pass.SetPassTag("SceneRTPipelineCheck");
                pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
                pass.SetShaderGroups({{RHIRayTracingShaderGroupType::eRayGeneration, 0},
                                      {RHIRayTracingShaderGroupType::eMiss, 1},
                                      {RHIRayTracingShaderGroupType::eHit, kUnusedRayTracingShader, 2, 3}});
                pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
                const Vec4 gi[] = {Vec4(0, 0, 0, 1), Vec4(64), Vec4(0), Vec4(0), Vec4(0), Vec4(1, 0, 0, 0)};
                pass.BindValue("uGISettings", gi, sizeof(gi));
                pass.BindValue("uSceneData", scene.GetSceneUniformData(), sizeof(SceneUniformData));
                queries.BindInputs(pass, scene);
                RHIBuffer* arguments = nullptr;
                if (indirect && device->GetGPUInfo().rayTracing.indirect)
                {
                    const uint32_t                   dimensions[] = {0, 4, 1, 1};
                    RHIFlags<RHIBufferUsageFlagBits> indirectUsage;
                    indirectUsage.SetFlags(RHIBufferUsageFlagBits::eIndirectBuffer, RHIBufferUsageFlagBits::eDeviceAddress);
                    arguments = Buffer(sizeof(dimensions), indirectUsage, dimensions);
                    EXPECT_NE(arguments, nullptr);
                    if (arguments != nullptr)
                    {
                        graph.GetResourceManager()->ImportBuffer(arguments, RDGImportContents::eDefined);
                        pass.UseIndirectBuffer(arguments);
                    }
                }
                graph.AddRayTracingPass(std::move(pass))
                    .RecordPassCommands([worldOffset, arguments](RDGPassCmdEncoder& encoder) {
                        TraceScenePipeline(encoder, worldOffset, arguments);
                    });
                graph.AddTransferPass("ReadScenePipelineResults").CopyBuffer(output, readback, {0, 0, 4 * sizeof(Vec4)});
            }
        }
        return readback;
    }

    static void TraceScenePipeline(RDGPassCmdEncoder& encoder, Vec4 worldOffset, RHIBuffer* arguments)
    {
        encoder.SetPushConstants(worldOffset);
        if (arguments != nullptr)
        {
            encoder.TraceRaysIndirect(arguments, sizeof(uint32_t));
        }
        else
        {
            encoder.TraceRays(4, 1);
        }
    }

    void Query(RenderGraph& graph, RHIAccelerationStructure* tlas, RHIBuffer* output)
    {
        RHIFlags<RHIBufferUsageFlagBits> usage;
        usage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
        RHIBuffer*         result = Buffer(sizeof(Vec4) * 4, usage, nullptr, RHIBufferAllocateType::eGPU);
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("RayQueryCheckSP");
        pass.SetPassTag("RayQueryCheck");
        pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
        pass.BindAccelerationStructure("testScene", tlas);
        pass.BindStorageBuffer("Results", result, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(4, 1, 1); });
        graph.AddTransferPass("ReadQueryResults").CopyBuffer(result, output, {0, 0, sizeof(Vec4) * 4});
    }
};

TEST_P(RayQueryIntegrationTest, SceneSnapshotsTrackOpacityDeformationMotionAndRemovalOutsideVoxelBounds)
{
    if (device->GetGPUInfo().rayQuery.IsUsable())
    {
        sg::Scene source;
        source.GetAABB()         = sg::AABB(Vec3(-0.5f), Vec3(0.5f));
        UniquePtr<sg::Mesh> mesh = MakeUnique<sg::Mesh>("shared triangles");
        mesh->SetAABB(Vec3(-0.5f, -0.5f, 0), Vec3(0.5f));
        for (uint32_t i = 0; i < 2; ++i)
        {
            UniquePtr<sg::Material> material    = MakeUnique<sg::Material>("query material");
            material->index                     = i;
            material->data.materialProperties.z = 1.0f;
            material->data.surfaceProperties.y  = i == 0 ? 1.0f : 0.0f;
            material->data.baseColorFactor.a    = float(i);
            // Repeated leaf-card coverage also exercises the production micromap
            // bake/cache path on supporting devices, through edits and retirement.
            UniquePtr<sg::SubMesh> primitive = MakeUnique<sg::SubMesh>("triangle", i * 384, 384, 3);
            primitive->SetMaterial(i, material.Get());
            mesh->AddSubMesh(primitive.Get());
            source.AddComponent(std::move(primitive));
            source.AddComponent(std::move(material));
        }
        HeapVector<UniquePtr<sg::Node>> nodes;
        for (uint32_t i = 0; i < 2; ++i)
        {
            UniquePtr<sg::Node> node = MakeUnique<sg::Node>(i, "instance");
            node->SetData(i, glm::translate(Mat4(1), Vec3(float(i) * 2.0f, 0, 0)));
            node->AddComponent(mesh.Get());
            source.AddRenderableNode(node.Get());
            nodes.push_back(std::move(node));
        }
        source.SetNodes(std::move(nodes));
        source.AddComponent(std::move(mesh));
        asset::Vertex vertices[6]{};
        uint32_t      indices[768];
        for (uint32_t i = 0; i < 768; ++i)
        {
            indices[i] = (i / 384) * 3 + i % 3;
        }
        for (uint32_t i = 0; i < 6; ++i)
        {
            vertices[i].pos    = Vec4(i % 3 == 0   ? -0.5f
                                      : i % 3 == 1 ? 0.5f
                                                   : 0.0f,
                                      i % 3 == 2 ? 0.5f : -0.5f, i < 3 ? 0.0f : 0.5f, 1);
            vertices[i].normal = Vec4(0, 0, 1, 0);
            vertices[i].color  = Vec4(1);
        }
        sg::Camera camera(Vec3(0, 0, -1), Vec3(0), 1);
        SceneData  data{};
        data.pScene      = &source;
        data.pVertices   = vertices;
        data.numVertices = 6;
        data.pIndices    = indices;
        data.numIndices  = 768;
        data.pCamera     = &camera;
        asset::PrepareScenePlacement(*data.pScene, asset::ScenePlacement::CenterAndNormalize);
        RenderScene scene(device, data);
        scene.LoadSceneMaterials();
        scene.PrepareBuffers();
        ASSERT_TRUE(scene.SetVoxelBounds(sg::AABB(Vec3(-0.1f), Vec3(0.1f))));
        SceneRayQuery queries(device);
        // At alpha zero the masked primitive can never occlude and stays out of the BLAS; frame 1
        // raises its alpha past the cutoff, which adds it and rebuilds. Frame 2 makes the masked
        // material single-sided; its back face still blocks, and the BLAS is reused.
        const float      expected[] = {1.5f, 1.0f, 1.0f, 1.25f, -1.0f, -1.0f, 1.25f, 1.25f, 1.25f, 1.25f, 1.75f, 1.25f};
        const uint32_t   built[]    = {1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1};
        const uint32_t   updated[]  = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
        const uint32_t   reused[]   = {0, 0, 1, 0, 1, 0, 0, 1, 1, 1, 0, 0};
        sg::MaterialData material   = source.GetComponents<sg::Material>()[0]->data;
        for (uint32_t frame = 0; frame < 12; ++frame)
        {
            SCOPED_TRACE(frame);
            Vec4 queryOffset(0);
            if (frame == 1)
            {
                material.baseColorFactor.a = 1;
                ASSERT_TRUE(scene.UpdateMaterial(0, material));
            }
            if (frame == 2)
            {
                material.materialProperties.z = 0;
                ASSERT_TRUE(scene.UpdateMaterial(0, material));
            }
            if (frame == 3)
            {
                material.materialProperties.z = 1;
                ASSERT_TRUE(scene.UpdateMaterial(0, material));
                for (asset::Vertex& vertex : vertices)
                {
                    vertex.pos.z += 0.25f;
                }
                ASSERT_TRUE(scene.UpdateVertices(0, MakeVecView(vertices, 6)));
            }
            if (frame == 4)
            {
                ASSERT_TRUE(scene.SetInstanceTransform(0, glm::translate(Mat4(1), Vec3(5, 0, 0))));
            }
            if (frame == 5)
            {
                ASSERT_TRUE(scene.SetInstanceEnabled(0, false));
                ASSERT_TRUE(scene.SetInstanceEnabled(1, false));
            }
            if (frame == 6)
            {
                ASSERT_TRUE(scene.SetInstanceEnabled(0, true));
                ASSERT_TRUE(scene.SetInstanceTransform(0, Mat4(1)));
            }
            if (frame >= 7)
            {
                // Move geometry and rays together. Keep the small voxel bounds at the
                // origin, exercising both distant AS coordinates and out-of-volume hits.
                const float distance = frame == 7 ? 100.0f : frame == 8 ? 10000.0f : 0.0f;
                queryOffset          = Vec4(Vec3(distance), 0);
                ASSERT_TRUE(scene.SetInstanceTransform(0, glm::translate(Mat4(1), Vec3(queryOffset))));
                ASSERT_TRUE(scene.SetInstanceTransform(1, glm::translate(Mat4(1), Vec3(queryOffset) + Vec3(2, 0, 0))));
            }
            if (frame >= 10)
            {
                // Keep the primitive eligible (maximum alpha .75), but open the
                // center of its coverage. Restoring alpha can reuse the disk bake.
                for (uint32_t corner = 0; corner < 3; ++corner)
                {
                    vertices[corner].color.a = frame == 11 ? 1.0f : corner == 2 ? .75f : 0.0f;
                }
                ASSERT_TRUE(scene.UpdateVertices(0, MakeVecView(vertices, 3)));
            }
            ASSERT_TRUE(scene.Update());
            if (frame == 6)
            {
                // Rebinding the same scene address must discard the prior snapshot.
                queries.Destroy();
            }
            RenderGraph& graph = *device->GetCurrentFrameRDG();
            ASSERT_TRUE(graph.Begin());
            ASSERT_TRUE(queries.BuildRenderGraph(scene));
            if (frame == 10)
            {
                ASSERT_TRUE(graph.End()) << graph.GetResult().message;
                queries.OnRenderGraphExecuted(RenderFrameResult::eDeclined);
                ASSERT_TRUE(graph.Begin());
                ASSERT_TRUE(queries.BuildRenderGraph(scene));
            }
            const bool ordinaryAlphaEdit = frame >= 10 && !device->GetGPUInfo().rayQuery.opacityMicromap;
            EXPECT_EQ(queries.GetBuildStatistics().built, ordinaryAlphaEdit ? 0u : built[frame]);
            EXPECT_EQ(queries.GetBuildStatistics().updated, updated[frame]);
            EXPECT_EQ(queries.GetBuildStatistics().reused, ordinaryAlphaEdit ? 1u : reused[frame]);
            RHIFlags<RHIBufferUsageFlagBits> usage;
            usage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
            RHIBuffer* output = Buffer(4 * sizeof(Vec4), usage, nullptr, RHIBufferAllocateType::eGPU);
            RHIBuffer* readback =
                Buffer(4 * sizeof(Vec4), RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                       RHIBufferAllocateType::eCPURead);
            RDGComputePassDesc pass;
            pass.SetShaderProgramName("SceneRayQueryCheckSP");
            pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
            const Vec4 gi[] = {Vec4(0, 0, 0, 1), Vec4(64), Vec4(0), Vec4(0), Vec4(0), Vec4(1, 0, 0, 0)};
            pass.BindValue("uGISettings", gi, sizeof(gi));
            pass.BindValue("uSceneData", scene.GetSceneUniformData(), sizeof(SceneUniformData));
            queries.BindInputs(pass, scene);
            if (frame == 1 && device->GetGPUInfo().rayQuery.opacityMicromap)
            {
                const RHIAccelerationStructure* tlas = pass.accelerationStructureBindings[0].pStructure;
                ASSERT_EQ(tlas->GetReferencedStructures().size(), 1u);
                const RHIAccelerationStructure* blas = tlas->GetReferencedStructures()[0];
                ASSERT_EQ(blas->GetReferencedStructures().size(), 1u);
                EXPECT_EQ(blas->GetReferencedStructures()[0]->GetType(), RHIAccelerationStructureType::eOpacityMicromap);
            }
            graph.AddComputePass(std::move(pass)).RecordPassCommands([queryOffset](RDGPassCmdEncoder& encoder) {
                encoder.SetPushConstants(queryOffset);
                encoder.Dispatch(4, 1, 1);
            });
            graph.AddTransferPass("ReadSceneQueries").CopyBuffer(output, readback, {0, 0, 4 * sizeof(Vec4)});
            RHIBuffer* pipelineReadback =
                RecordScenePipelineCheck(graph, scene, queries, queryOffset, (frame & 1u) != 0, (frame & 2u) != 0);
            ASSERT_TRUE(graph.End()) << graph.GetResult().message;
            const bool succeeded = device->ExecuteRenderGraph(graph);
            queries.OnRenderGraphExecuted(succeeded ? RenderFrameResult::eSubmitted : RenderFrameResult::eFailed);
            ASSERT_TRUE(succeeded);
            device->WaitForIdle();
            const Vec4* result = reinterpret_cast<const Vec4*>(readback->Map());
            EXPECT_NEAR(result[0].x, expected[frame], 1e-5f);
            if (frame < 5)
            {
                EXPECT_GT(result[1].x, 0.0f);
            }
            if (frame == 5)
            {
                EXPECT_EQ(result[1].x, -1.0f);
            }
            if (frame < 4)
            {
                EXPECT_EQ(result[0].y, frame == 0 ? 1.0f : 0.0f);
            }
            EXPECT_EQ(result[0].w, float(queries.GetGeneration()));
            EXPECT_EQ(queries.GetGeneration(), frame >= 6 ? frame - 5u : frame + 1u);
            if (device->GetGPUInfo().rayTracing.IsUsable())
            {
                ASSERT_NE(pipelineReadback, nullptr);
                const Vec4* pipelineResult = reinterpret_cast<const Vec4*>(pipelineReadback->Map());
                ASSERT_NE(pipelineResult, nullptr);
                for (uint32_t ray = 0; ray < 4; ++ray)
                {
                    SCOPED_TRACE(ray);
                    for (uint32_t channel = 0; channel < 4; ++channel)
                    {
                        EXPECT_NEAR(pipelineResult[ray][channel], result[ray][channel], 1e-5f);
                    }
                }
                pipelineReadback->Unmap();
            }
            readback->Unmap();
        }
        EXPECT_FALSE(queries.BuildRenderGraph(scene, 1));
        EXPECT_STREQ(queries.GetReason(), "acceleration_structure_budget");
        queries.Destroy();
        scene.Destroy();
    }
    else
    {
        GTEST_SKIP() << "Ray queries unavailable on this device";
    }
}

TEST_P(RayQueryIntegrationTest, DeformingOneMeshRefitsOnlyItInPlaceAndRebuildsAfterBoundedRefits)
{
    if (!device->GetGPUInfo().rayQuery.IsUsable())
    {
        GTEST_SKIP() << "Ray queries unavailable on this device";
    }
    sg::Scene source;
    // The unit source box keeps the renderer's scene normalization at identity.
    source.GetAABB()                    = sg::AABB(Vec3(-0.5f), Vec3(0.5f));
    UniquePtr<sg::Material> material    = MakeUnique<sg::Material>("single-sided opaque");
    material->index                     = 0;
    material->data.materialProperties.z = 0.0f;
    material->data.baseColorFactor.a    = 1.0f;
    HeapVector<UniquePtr<sg::Node>> nodes;
    for (uint32_t i = 0; i < 2; ++i)
    {
        UniquePtr<sg::Mesh> mesh = MakeUnique<sg::Mesh>(i == 0 ? "deforming" : "static");
        mesh->SetAABB(Vec3(-0.5f, -0.5f, 0), Vec3(0.5f, 0.5f, 1.0f));
        UniquePtr<sg::SubMesh> primitive = MakeUnique<sg::SubMesh>("triangle", i * 3, 3, 3);
        primitive->SetMaterial(0, material.Get());
        mesh->AddSubMesh(primitive.Get());
        UniquePtr<sg::Node> node = MakeUnique<sg::Node>(i, "instance");
        node->SetData(i, glm::translate(Mat4(1), Vec3(float(i) * 2.0f, 0, 0)));
        node->AddComponent(mesh.Get());
        source.AddRenderableNode(node.Get());
        nodes.push_back(std::move(node));
        source.AddComponent(std::move(primitive));
        source.AddComponent(std::move(mesh));
    }
    source.AddComponent(std::move(material));
    source.SetNodes(std::move(nodes));
    // Both triangles face +Z, so the rays below meet their back faces.
    asset::Vertex  vertices[6]{};
    const uint32_t indices[] = {0, 1, 2, 3, 4, 5};
    for (uint32_t i = 0; i < 6; ++i)
    {
        vertices[i].pos    = Vec4(i % 3 == 0   ? -0.5f
                                  : i % 3 == 1 ? 0.5f
                                               : 0.0f,
                                  i % 3 == 2 ? 0.5f : -0.5f, i < 3 ? 0.0f : 0.5f, 1);
        vertices[i].normal = Vec4(0, 0, 1, 0);
        vertices[i].color  = Vec4(1);
    }
    sg::Camera camera(Vec3(0, 0, -1), Vec3(0), 1);
    SceneData  data{};
    data.pScene      = &source;
    data.pVertices   = vertices;
    data.numVertices = 6;
    data.pIndices    = indices;
    data.numIndices  = 6;
    data.pCamera     = &camera;
    asset::PrepareScenePlacement(*data.pScene, asset::ScenePlacement::CenterAndNormalize);
    RenderScene scene(device, data);
    scene.LoadSceneMaterials();
    scene.PrepareBuffers();
    SceneRayQuery queries(device);
    for (uint32_t frame = 0; frame <= SceneRayQuery::kMaxRefits + 2; ++frame)
    {
        SCOPED_TRACE(frame);
        const float offset = 0.01f * float(frame);
        if (frame != 0)
        {
            // Only the first mesh deforms. Repeated commits in this frame reuse
            // the same buffer, so BLAS reuse must compare its content generation.
            for (uint32_t i = 0; i < 3; ++i)
            {
                vertices[i].pos.z = offset;
            }
            ASSERT_TRUE(scene.UpdateVertices(0, MakeVecView(vertices, 3)));
        }
        if (frame == 1)
        {
            ASSERT_TRUE(scene.SetInstanceTransform(0, glm::translate(Mat4(1), Vec3(0, 0, 3))));
        }
        ASSERT_TRUE(scene.Update());
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        const RHIExecutionCounters beforeBuild = device->GetRHIThreadMetrics().native;

        ASSERT_GT(beforeBuild.buffersCreated, 0u);

        const uint64_t beforeUploads = device->GetStagedUploadBytes();
        const uint64_t beforeMemory  = queries.GetMemoryBytes();
        ASSERT_TRUE(queries.BuildRenderGraph(scene));
        if (frame >= 2)
        {
            // Two inactive/active snapshots suffice even when every edit uses
            // the same frame slot. Deformation leaves instance metadata unchanged.
            EXPECT_EQ(device->GetRHIThreadMetrics().native.buffersCreated, beforeBuild.buffersCreated);
            EXPECT_EQ(device->GetStagedUploadBytes(), beforeUploads);
            EXPECT_EQ(queries.GetMemoryBytes(), beforeMemory);
        }
        if (frame == 1)
        {
            const uint64_t recordedGeneration = queries.GetGeneration();

            ASSERT_TRUE(graph.End());

            queries.OnRenderGraphExecuted(RenderFrameResult::eDeclined);

            EXPECT_TRUE(queries.IsReady());

            EXPECT_EQ(queries.GetGeneration(), recordedGeneration - 1);

            ASSERT_TRUE(graph.Begin());

            // Upload the declined candidate's changed instances, then query the
            // committed TLAS. Its contents must still describe the previous frame.
            CheckSceneHits(scene, queries, 1.0f, true);

            ASSERT_TRUE(scene.SetInstanceTransform(0, Mat4(1)));
            ASSERT_TRUE(scene.Update());
            ASSERT_TRUE(graph.Begin());

            ASSERT_TRUE(queries.BuildRenderGraph(scene));

            EXPECT_EQ(queries.GetGeneration(), recordedGeneration);
        }

        const SceneRayQuery::BuildStatistics statistics = queries.GetBuildStatistics();
        // Refits 1..kMaxRefits run in place; the next deformation rebuilds the same BLAS.
        const bool rebuild = frame == SceneRayQuery::kMaxRefits + 1;
        EXPECT_EQ(statistics.built, frame == 0 ? 2u : rebuild ? 1u : 0u);
        EXPECT_EQ(statistics.updated, frame == 0 || rebuild ? 0u : 1u);
        EXPECT_EQ(statistics.reused, frame == 0 ? 0u : 1u);
        CheckSceneHits(scene, queries, 1.0f + offset);
        if (frame >= 3)
        {
            device->NextFrame();
        }
    }
    queries.Destroy();
    scene.Destroy();
}

// A node authored with zero scale covers no area, and Vulkan requires invertible instance
// transforms. It stays out of the TLAS without costing the frame or the visible instances.
TEST_P(RayQueryIntegrationTest, SingularAuthoredInstanceStaysOutOfTheSceneTLAS)
{
    if (device->GetGPUInfo().rayQuery.IsUsable())
    {
        sg::Scene source;
        source.GetAABB()                    = sg::AABB(Vec3(-0.5f), Vec3(0.5f));
        UniquePtr<sg::Material> material    = MakeUnique<sg::Material>("opaque");
        material->index                     = 0;
        material->data.materialProperties.z = 1.0f;
        material->data.baseColorFactor.a    = 1.0f;
        UniquePtr<sg::Mesh> mesh            = MakeUnique<sg::Mesh>("shared triangle");
        mesh->SetAABB(Vec3(-0.5f, -0.5f, 0), Vec3(0.5f));
        UniquePtr<sg::SubMesh> primitive = MakeUnique<sg::SubMesh>("triangle", 0, 3, 3);
        primitive->SetMaterial(0, material.Get());
        mesh->AddSubMesh(primitive.Get());
        const Mat4                      transforms[] = {Mat4(1), glm::scale(glm::translate(Mat4(1), Vec3(2, 0, 0)), Vec3(0))};
        HeapVector<UniquePtr<sg::Node>> nodes;
        for (uint32_t i = 0; i < 2; ++i)
        {
            UniquePtr<sg::Node> node = MakeUnique<sg::Node>(i, "instance");
            node->SetData(i, transforms[i]);
            node->AddComponent(mesh.Get());
            source.AddRenderableNode(node.Get());
            nodes.push_back(std::move(node));
        }
        source.SetNodes(std::move(nodes));
        source.AddComponent(std::move(primitive));
        source.AddComponent(std::move(mesh));
        source.AddComponent(std::move(material));
        asset::Vertex  vertices[3]{};
        const uint32_t indices[] = {0, 1, 2};
        for (uint32_t i = 0; i < 3; ++i)
        {
            vertices[i].pos    = Vec4(i == 0 ? -0.5f : i == 1 ? 0.5f : 0.0f, i == 2 ? 0.5f : -0.5f, 0, 1);
            vertices[i].normal = Vec4(0, 0, 1, 0);
            vertices[i].color  = Vec4(1);
        }
        sg::Camera camera(Vec3(0, 0, -1), Vec3(0), 1);
        SceneData  data{};
        data.pScene      = &source;
        data.pVertices   = vertices;
        data.numVertices = 3;
        data.pIndices    = indices;
        data.numIndices  = 3;
        data.pCamera     = &camera;
        asset::PrepareScenePlacement(*data.pScene, asset::ScenePlacement::CenterAndNormalize);
        RenderScene scene(device, data);
        scene.LoadSceneMaterials();
        scene.PrepareBuffers();
        ASSERT_TRUE(scene.Update());
        SceneRayQuery queries(device);
        RenderGraph&  graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        ASSERT_TRUE(queries.BuildRenderGraph(scene));
        CheckSceneHits(scene, queries, 1.0f, false, -1.0f);
        EXPECT_TRUE(queries.IsReady());
        queries.Destroy();
        scene.Destroy();
    }
    else
    {
        GTEST_SKIP() << "Ray queries unavailable on this device";
    }
}

TEST_P(RayQueryIntegrationTest, GraphBuildQueryUpdateAndEmptyScene)
{
    if (device->GetGPUInfo().rayQuery.IsUsable())
    {
        const float                      positions[] = {-0.5f, -0.5f, 0,    0.5f, -0.5f, 0,    0, 0.5f, 0,
                                                        -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0, 0.5f, 0.5f};
        const uint32_t                   indices[]   = {0, 1, 2, 3, 4, 5};
        RHIFlags<RHIBufferUsageFlagBits> inputUsage;
        inputUsage.SetFlags(RHIBufferUsageFlagBits::eDeviceAddress, RHIBufferUsageFlagBits::eAccelerationStructureInput);
        RHIAccelerationStructureGeometry geometry;
        geometry.pVertexBuffer = Buffer(sizeof(positions), inputUsage, positions);
        geometry.pIndexBuffer  = Buffer(sizeof(indices), inputUsage, indices);
        geometry.vertexCount   = 6;
        geometry.vertexStride  = 12;
        geometry.indexCount    = 6;
        geometry.opaque        = false;
        RHIAccelerationStructureBuildInfo blas;
        blas.description.geometries  = MakeVecView(&geometry, 1);
        blas.description.allowUpdate = true;
        blas.pDestination            = Structure(blas.description);
        ASSERT_NE(blas.pDestination, nullptr);
        RHIAccelerationStructureInstance instances[2]{};
        for (uint32_t i = 0; i < 2; ++i)
        {
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                instances[i].transform[axis][axis] = 1.0f;
            }
            instances[i].transform[0][3] = float(i) * 2.0f;
            instances[i].customIndex     = i == 0 ? 7u : 11u;
            instances[i].pStructure      = blas.pDestination;
        }
        RHIAccelerationStructureBuildInfo tlas;
        tlas.description.type        = RHIAccelerationStructureType::eTopLevel;
        tlas.description.instances   = MakeVecView(instances, 2);
        tlas.description.allowUpdate = true;
        tlas.pDestination            = Structure(tlas.description, MakeVecView(&blas.pDestination, 1));
        ASSERT_NE(tlas.pDestination, nullptr);
        const RHIAccelerationStructureBuildSizes blasSizes = GDynamicRHI->GetAccelerationStructureBuildSizes(blas.description);
        const RHIAccelerationStructureBuildSizes tlasSizes = GDynamicRHI->GetAccelerationStructureBuildSizes(tlas.description);
        const uint64_t                           alignment = device->GetGPUInfo().rayQuery.scratchAlignment;
        RHIFlags<RHIBufferUsageFlagBits>         scratchUsage;
        scratchUsage.SetFlags(RHIBufferUsageFlagBits::eDeviceAddress, RHIBufferUsageFlagBits::eStorageBuffer);
        blas.pScratchBuffer = Buffer(static_cast<uint32_t>(std::max({blasSizes.buildScratchSize, blasSizes.updateScratchSize,
                                                                     tlasSizes.buildScratchSize, tlasSizes.updateScratchSize})
                                                           + alignment),
                                     scratchUsage);
        ASSERT_NE(blas.pScratchBuffer, nullptr);
        blas.scratchOffset  = (alignment - blas.pScratchBuffer->GetDeviceAddress() % alignment) % alignment;
        tlas.pScratchBuffer = blas.pScratchBuffer;
        tlas.scratchOffset  = blas.scratchOffset;
        RHIBuffer* output =
            Buffer(sizeof(Vec4) * 4, RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                   RHIBufferAllocateType::eCPURead);
        ASSERT_NE(output, nullptr);
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        graph.GetResourceManager()->ImportBuffer(geometry.pVertexBuffer, RDGImportContents::eDefined);
        graph.GetResourceManager()->ImportBuffer(geometry.pIndexBuffer, RDGImportContents::eDefined);
        graph.AddTransferPass("BLAS").BuildAccelerationStructure(blas);
        graph.AddTransferPass("TLAS").BuildAccelerationStructure(tlas);
        const RHIAccelerationStructureInstance original = instances[0];
        instances[0].customIndex                        = 1234;
        instances[0].transform[0][3]                    = 100;
        Query(graph, tlas.pDestination, output);
        // Prove the graph owns the geometry array, including threaded translation.
        geometry.indexCount = 0;
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        CheckQueryQueue(graph);
        device->WaitForIdle();
        Vec4 result[4];
        std::memcpy(result, output->Map(), sizeof(result));
        output->Unmap();
        EXPECT_NEAR(result[0].x, 1.0f, 1e-5f);
        EXPECT_NEAR(result[0].y, 0.25f, 1e-5f);
        EXPECT_NEAR(result[0].z, 0.5f, 1e-5f);
        EXPECT_EQ(result[0].w, 7.0f);
        EXPECT_NEAR(result[1].x, 1.5f, 1e-5f);
        EXPECT_EQ(result[2].w, 11.0f);
        EXPECT_EQ(result[3].x, -1.0f);
        instances[0] = original;

        // Out-of-place update shares BLASes, with old-query -> update dependencies.
        tlas.pSource      = tlas.pDestination;
        tlas.pDestination = Structure(tlas.description, MakeVecView(&blas.pDestination, 1));
        ASSERT_TRUE(graph.Begin());
        graph.AddTransferPass("UpdateTLAS").BuildAccelerationStructure(tlas);
        Query(graph, tlas.pDestination, output);
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        CheckQueryQueue(graph);
        device->WaitForIdle();
        std::memcpy(result, output->Map(), sizeof(result));
        output->Unmap();
        EXPECT_EQ(result[0].w, 7.0f);

        // A deformed raster buffer produces a new BLAS; its replacement TLAS
        // retains the new dependency while the old generations remain alive.
        float deformed[18];
        std::memcpy(deformed, positions, sizeof(deformed));
        for (uint32_t vertex = 0; vertex < 6; ++vertex)
        {
            deformed[vertex * 3 + 2] += 0.25f;
        }
        geometry.indexCount     = 6;
        geometry.pVertexBuffer  = Buffer(sizeof(deformed), inputUsage, deformed);
        blas.pSource            = blas.pDestination;
        blas.pDestination       = Structure(blas.description);
        instances[0].pStructure = instances[1].pStructure = blas.pDestination;
        instances[1].transform[0][3]                      = 3.0f;
        tlas.pSource                                      = tlas.pDestination;
        tlas.pDestination                                 = Structure(tlas.description, MakeVecView(&blas.pDestination, 1));
        ASSERT_TRUE(graph.Begin());
        graph.GetResourceManager()->ImportHostWrittenBuffer(geometry.pVertexBuffer);
        graph.AddTransferPass("DeformBLAS").BuildAccelerationStructure(blas);
        graph.AddTransferPass("MoveTLAS").BuildAccelerationStructure(tlas);
        Query(graph, tlas.pDestination, output);
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        CheckQueryQueue(graph);
        device->WaitForIdle();
        std::memcpy(result, output->Map(), sizeof(result));
        output->Unmap();
        EXPECT_NEAR(result[0].x, 1.25f, 1e-5f);
        EXPECT_NEAR(result[1].x, 1.75f, 1e-5f);
        EXPECT_EQ(result[2].x, -1.0f);

        // Empty TLAS is valid and misses every ray; never bind an unbuilt/null AS.
        tlas.description.instances = {};
        tlas.pSource               = nullptr;
        tlas.pDestination          = Structure(tlas.description);
        ASSERT_TRUE(graph.Begin());
        graph.AddTransferPass("EmptyTLAS").BuildAccelerationStructure(tlas);
        Query(graph, tlas.pDestination, output);
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        CheckQueryQueue(graph);
        device->WaitForIdle();
        std::memcpy(result, output->Map(), sizeof(result));
        output->Unmap();
        for (const Vec4& value : result)
        {
            EXPECT_EQ(value.x, -1.0f);
        }
    }
    else
    {
        GTEST_SKIP() << "Ray queries unavailable on this device";
    }
}


// Independent double-precision brute force over the authored triangles, never a traversal structure's records.
#include "OpacityMicromapTests.inl"
#include "RayHitLightingTests.inl"

struct OracleTriangle
{
    glm::dvec3 a, b, c;
    uint32_t   indexOffset, node, material;
    Vec3       alpha; // vertex alpha at a, b, c
};

struct OracleCandidate
{
    double   t, u, v;
    uint32_t triangle;
};

bool OracleIntersect(const OracleTriangle& triangle,
                     const glm::dvec3&     origin,
                     const glm::dvec3&     direction,
                     OracleCandidate&      hit)
{
    // Moller-Trumbore, both faces: u weights b, v weights c, matching the shader's barycentrics.
    const glm::dvec3 e1 = triangle.b - triangle.a, e2 = triangle.c - triangle.a, p = glm::cross(direction, e2);
    const double     determinant = glm::dot(e1, p);
    if (std::abs(determinant) < 1e-30)
    {
        return false;
    }
    const glm::dvec3 s = origin - triangle.a, q = glm::cross(s, e1);
    hit.u = glm::dot(s, p) / determinant;
    hit.v = glm::dot(direction, q) / determinant;
    hit.t = glm::dot(e2, q) / determinant;
    return hit.u > -1e-3 && hit.v > -1e-3 && hit.u + hit.v < 1.001 && hit.t > -1e-6;
}

class DistanceFieldSceneIntegrationTest : public RayQueryIntegrationTest
{
protected:
    void SetUp() override
    {
        // Explicit profiling runs retain marker names but disable validation and
        // execution counters. Ordinary correctness tests keep their validation.
        const char* profile = std::getenv("ZEN_FIELD_GPU_PROFILE");
        SetUpDevice(false, profile != nullptr && std::strcmp(profile, "1") == 0);
        EXPECT_FALSE(device->GetGPUInfo().rayQuery.IsUsable());
    }
};

#include "ForwardBlendFixture.inl"
#include "RayForwardBlendTests.inl"
#include "RayGITraceTests.inl"
#include "RayOriginTests.inl"
#include "GICacheTests.inl"
#include "HybridGIPassListTests.inl"
#include "ReSTIRRayQueryTests.inl"
#include "DistanceFieldSceneTests.inl"
#include "DistanceFieldDiagnostics.inl"
#include "DistanceFieldLanternTests.inl"
#include "DistanceFieldProfiling.inl"
#include "DistanceFieldAssetTests.inl"

INSTANTIATE_TEST_SUITE_P(ExecutionModes, RayQueryIntegrationTest, testing::Values(0u, 1u, 2u, 3u));
INSTANTIATE_TEST_SUITE_P(ExecutionModes, DistanceFieldSceneIntegrationTest, testing::Values(0u, 1u, 2u, 3u));
} // namespace
