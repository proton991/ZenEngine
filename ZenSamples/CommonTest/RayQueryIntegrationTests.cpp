#include "AssetLib/SceneRuntimeBuilder.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/SceneRayQuery.h"
#include "Graphics/RenderCore/V2/SoftwareSceneRayQuery.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "SceneGraph/Camera.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanAccelerationStructure.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>

namespace
{
using namespace zen;
using namespace zen::rc;

class RayQueryIntegrationTest : public testing::TestWithParam<uint32_t>
{
protected:
    RenderDevice*                         device{nullptr};
    HeapVector<RHIBuffer*>                buffers;
    HeapVector<RHIAccelerationStructure*> structures;
    VkDebugUtilsMessengerEXT              messenger{VK_NULL_HANDLE};
    bool                                  countersEnabled{false};

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
        countersEnabled = RHIOptions::GetInstance().ExecutionCountersEnabled();
        RHIOptions::GetInstance().SetExecutionCountersEnabled(true);
        RHIOptions::GetInstance().SetRayTracingDisabled(false);
        device = ZEN_NEW()
            RenderDevice(RHIAPIType::eVulkan, 2, (GetParam() & 1) ? RHIExecutionMode::eThreaded : RHIExecutionMode::eInline,
                         (GetParam() & 2) ? AsyncComputeMode::eAuto : AsyncComputeMode::eDisabled);
        device->Init(nullptr);
        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = Validation;
        ASSERT_EQ(vkCreateDebugUtilsMessengerEXT(GVulkanRHI->GetInstance(), &info, nullptr, &messenger), VK_SUCCESS);
        if (device->GetGPUInfo().rayQuery.IsUsable())
        {
            ShaderProgramManager::GetInstance().StoreProgram(
                ZEN_NEW() ComputeFileSP(device, "RayQueryCheckSP", "VoxelGI/Calibration/ray_query_check.comp.spv"));
            ShaderProgramManager::GetInstance().StoreProgram(
                ZEN_NEW() ComputeFileSP(device, "SceneRayQueryCheckSP", "VoxelGI/Calibration/scene_ray_query_check.comp.spv"));
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
        ShaderProgramManager::GetInstance().Destroy();
        vkDestroyDebugUtilsMessengerEXT(GVulkanRHI->GetInstance(), messenger, nullptr);
        device->Destroy();
        ZEN_DELETE(device);
        RHIOptions::GetInstance().SetExecutionCountersEnabled(countersEnabled);
    }

    RHIBuffer* Buffer(uint32_t                         size,
                      BitField<RHIBufferUsageFlagBits> usage,
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

    RHIAccelerationStructure* Structure(const RHIAccelerationStructureBuildDesc&    desc,
                                        VectorView<RHIAccelerationStructure* const> references = {})
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

    void CheckQueryQueue(const RenderGraph& graph) const
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

        EXPECT_EQ(queries, 1u);
    }

    void CheckSceneHits(RenderScene& scene, SceneRayQuery& queries, float dynamicDistance, bool rawIntersection = false)
    {
        RenderGraph&                     graph = *device->GetCurrentFrameRDG();
        BitField<RHIBufferUsageFlagBits> usage;
        usage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
        RHIBuffer* output = Buffer(4 * sizeof(Vec4), usage, nullptr, RHIBufferAllocateType::eGPU);
        RHIBuffer* readback =
            Buffer(4 * sizeof(Vec4), BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
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
            const Vec4 gi[] = {Vec4(0, 0, 0, 1), Vec4(64), Vec4(0), Vec4(0), Vec4(0)};
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
        EXPECT_NEAR(result[rawIntersection ? 2 : 1].x, 1.5f, 1e-5f);
        if (!rawIntersection)
        {
            EXPECT_EQ(result[0].w, float(queries.GetGeneration()));
        }
        readback->Unmap();
    }

    void Query(RenderGraph& graph, RHIAccelerationStructure* tlas, RHIBuffer* output)
    {
        BitField<RHIBufferUsageFlagBits> usage;
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
            UniquePtr<sg::SubMesh> primitive    = MakeUnique<sg::SubMesh>("triangle", i * 3, 3, 3);
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
        ASSERT_TRUE(scene.SetVoxelBounds(sg::AABB(Vec3(-0.1f), Vec3(0.1f))));
        SceneRayQuery queries(device);
        // Frame 2 makes the masked material single-sided; its back face still blocks.
        const float      expected[] = {1.5f, 1.0f, 1.0f, 1.25f, -1.0f, -1.0f, 1.25f, 1.25f, 1.25f, 1.25f};
        const uint32_t   built[]    = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0};
        const uint32_t   updated[]  = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
        const uint32_t   reused[]   = {0, 1, 1, 0, 1, 0, 0, 1, 1, 1};
        sg::MaterialData material   = source.GetComponents<sg::Material>()[0]->data;
        for (uint32_t frame = 0; frame < 10; ++frame)
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
            ASSERT_TRUE(scene.Update());
            if (frame == 6)
            {
                // Rebinding the same scene address must discard the prior snapshot.
                queries.Destroy();
            }
            RenderGraph& graph = *device->GetCurrentFrameRDG();
            ASSERT_TRUE(graph.Begin());
            ASSERT_TRUE(queries.BuildRenderGraph(scene));
            EXPECT_EQ(queries.GetBuildStatistics().built, built[frame]);
            EXPECT_EQ(queries.GetBuildStatistics().updated, updated[frame]);
            EXPECT_EQ(queries.GetBuildStatistics().reused, reused[frame]);
            BitField<RHIBufferUsageFlagBits> usage;
            usage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
            RHIBuffer* output = Buffer(4 * sizeof(Vec4), usage, nullptr, RHIBufferAllocateType::eGPU);
            RHIBuffer* readback =
                Buffer(4 * sizeof(Vec4), BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                       RHIBufferAllocateType::eCPURead);
            RDGComputePassDesc pass;
            pass.SetShaderProgramName("SceneRayQueryCheckSP");
            pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
            const Vec4 gi[] = {Vec4(0, 0, 0, 1), Vec4(64), Vec4(0), Vec4(0), Vec4(0)};
            pass.BindValue("uGISettings", gi, sizeof(gi));
            pass.BindValue("uSceneData", scene.GetSceneUniformData(), sizeof(SceneUniformData));
            queries.BindInputs(pass, scene);
            graph.AddComputePass(std::move(pass)).RecordPassCommands([queryOffset](RDGPassCmdEncoder& encoder) {
                encoder.SetPushConstants(queryOffset);
                encoder.Dispatch(4, 1, 1);
            });
            graph.AddTransferPass("ReadSceneQueries").CopyBuffer(output, readback, {0, 0, 4 * sizeof(Vec4)});
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

TEST_P(RayQueryIntegrationTest, GraphBuildQueryUpdateAndEmptyScene)
{
    if (device->GetGPUInfo().rayQuery.IsUsable())
    {
        const float                      positions[] = {-0.5f, -0.5f, 0,    0.5f, -0.5f, 0,    0, 0.5f, 0,
                                                        -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0, 0.5f, 0.5f};
        const uint32_t                   indices[]   = {0, 1, 2, 3, 4, 5};
        BitField<RHIBufferUsageFlagBits> inputUsage;
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
            instances[i].transform[0][3]              = float(i) * 2.0f;
            instances[i].customIndexAndMask           = (i == 0 ? 7u : 11u) | (0xffu << 24);
            instances[i].accelerationStructureAddress = blas.pDestination->GetDeviceAddress();
        }
        RHIAccelerationStructureBuildInfo tlas;
        tlas.description.type            = RHIAccelerationStructureType::eTopLevel;
        tlas.description.pInstanceBuffer = Buffer(sizeof(instances), inputUsage, instances);
        tlas.description.instanceCount   = 2;
        tlas.description.allowUpdate     = true;
        tlas.pDestination                = Structure(tlas.description, MakeVecView(&blas.pDestination, 1));
        ASSERT_NE(tlas.pDestination, nullptr);
        const RHIAccelerationStructureBuildSizes blasSizes = GDynamicRHI->GetAccelerationStructureBuildSizes(blas.description);
        const RHIAccelerationStructureBuildSizes tlasSizes = GDynamicRHI->GetAccelerationStructureBuildSizes(tlas.description);
        const uint64_t                           alignment = device->GetGPUInfo().rayQuery.scratchAlignment;
        BitField<RHIBufferUsageFlagBits>         scratchUsage;
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
            Buffer(sizeof(Vec4) * 4, BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                   RHIBufferAllocateType::eCPURead);
        ASSERT_NE(output, nullptr);
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        graph.GetResourceManager()->ImportBuffer(geometry.pVertexBuffer, RDGImportContents::eDefined);
        graph.GetResourceManager()->ImportBuffer(geometry.pIndexBuffer, RDGImportContents::eDefined);
        graph.GetResourceManager()->ImportBuffer(tlas.description.pInstanceBuffer, RDGImportContents::eDefined);
        graph.AddTransferPass("BLAS").BuildAccelerationStructure(blas);
        graph.AddTransferPass("TLAS").BuildAccelerationStructure(tlas);
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

        RHIAccelerationStructureBuildInfo invalid  = tlas;
        invalid.scratchOffset                     += 1;
        EXPECT_FALSE(static_cast<VulkanAccelerationStructure*>(tlas.pDestination)->ValidateBuild(invalid));
        invalid                            = tlas;
        invalid.description.instanceOffset = 1;
        EXPECT_FALSE(static_cast<VulkanAccelerationStructure*>(tlas.pDestination)->ValidateBuild(invalid));

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
        geometry.indexCount                       = 6;
        geometry.pVertexBuffer                    = Buffer(sizeof(deformed), inputUsage, deformed);
        blas.pSource                              = blas.pDestination;
        blas.pDestination                         = Structure(blas.description);
        instances[0].accelerationStructureAddress = instances[1].accelerationStructureAddress =
            blas.pDestination->GetDeviceAddress();
        instances[1].transform[0][3]     = 3.0f;
        tlas.description.pInstanceBuffer = Buffer(sizeof(instances), inputUsage, instances);
        tlas.pSource                     = tlas.pDestination;
        tlas.pDestination                = Structure(tlas.description, MakeVecView(&blas.pDestination, 1));
        ASSERT_TRUE(graph.Begin());
        graph.GetResourceManager()->ImportHostWrittenBuffer(geometry.pVertexBuffer);
        graph.GetResourceManager()->ImportHostWrittenBuffer(tlas.description.pInstanceBuffer);
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
        tlas.description.instanceCount = 0;
        tlas.pSource                   = nullptr;
        tlas.pDestination              = Structure(tlas.description);
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

// Independent double-precision brute force over the authored triangles, never the BVH builder's records.
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

TEST_P(RayQueryIntegrationTest, SoftwareTraversalMatchesBruteForceTriangleOracle)
{
    ShaderProgramManager::GetInstance().StoreProgram(
        ZEN_NEW() ComputeFileSP(device, "SoftwareRayQueryCheckSP", "VoxelGI/Calibration/software_ray_query_check.comp.spv"));
    std::mt19937                          random(29);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const auto                            Uniform = [&](float low, float high) {
        return low + (high - low) * unit(random);
    };
    const auto Direction = [&]() {
        const float z = Uniform(-1, 1), phi = Uniform(0, 6.2831853f), r = std::sqrt(std::max(0.0f, 1 - z * z));
        return Vec3(r * std::cos(phi), r * std::sin(phi), z);
    };

    // Materials: opaque; masked with zero alpha (always rejected); masked by interpolated vertex alpha.
    sg::Scene                           source;
    HeapVector<UniquePtr<sg::Material>> materials;
    for (uint32_t i = 0; i < 3; ++i)
    {
        UniquePtr<sg::Material> material   = MakeUnique<sg::Material>("oracle material");
        material->index                    = i;
        material->data.surfaceProperties.y = i == 0 ? 0.0f : 1.0f;
        material->data.baseColorFactor.a   = i == 1 ? 0.0f : 1.0f;
        materials.push_back(std::move(material));
    }
    HeapVector<asset::Vertex> vertices;
    HeapVector<uint32_t>      indices;
    struct MeshDesc
    {
        HeapVector<std::pair<uint32_t, uint32_t>> submeshes; // first index, material; three indices each
    };
    HeapVector<MeshDesc> meshDescs;
    const auto           Triangle = [&](MeshDesc& mesh, Vec3 a, Vec3 b, Vec3 c, uint32_t material, Vec3 alpha = Vec3(1)) {
        mesh.submeshes.push_back({static_cast<uint32_t>(indices.size()), material});
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            asset::Vertex vertex{};
            vertex.pos    = Vec4(corner == 0 ? a : corner == 1 ? b : c, 1);
            vertex.normal = Vec4(0, 0, 1, 0);
            vertex.color  = Vec4(1, 1, 1, alpha[corner]);
            indices.push_back(static_cast<uint32_t>(vertices.size()));
            vertices.push_back(vertex);
        }
    };
    MeshDesc field, layered, alpha, quad, skewed;
    for (uint32_t i = 0; i < 160; ++i)
    {
        const Vec3  center(Uniform(-1, 1), Uniform(-1, 1), Uniform(-1, 1));
        const float size = Uniform(0.05f, 0.3f);
        Triangle(field, center + Direction() * size, center + Direction() * size, center + Direction() * size, 0);
    }
    // A rejected candidate directly in front of an opaque blocker, a few micro-units apart.
    Triangle(layered, Vec3(-0.5f, -0.5f, 0), Vec3(0.5f, -0.5f, 0), Vec3(0, 0.5f, 0), 1);
    Triangle(layered, Vec3(-0.5f, -0.5f, 0.002f), Vec3(0.5f, -0.5f, 0.002f), Vec3(0, 0.5f, 0.002f), 0);
    // Accepted only where the first vertex's weight reaches the 0.5 cutoff; rejected rays reach the blocker.
    Triangle(alpha, Vec3(-0.5f, -0.5f, 0), Vec3(0.5f, -0.5f, 0), Vec3(0, 0.5f, 0), 2, Vec3(1, 0, 0));
    Triangle(alpha, Vec3(-0.6f, -0.6f, 0.01f), Vec3(0.6f, -0.6f, 0.01f), Vec3(0, 0.6f, 0.01f), 0);
    // Two triangles sharing a diagonal: watertight traversal never misses on it.
    Triangle(quad, Vec3(-0.5f, -0.5f, 0), Vec3(0.5f, -0.5f, 0), Vec3(0.5f, 0.5f, 0), 0);
    Triangle(quad, Vec3(-0.5f, -0.5f, 0), Vec3(0.5f, 0.5f, 0), Vec3(-0.5f, 0.5f, 0), 0);
    // Exponentially spaced centroids produce strongly skewed surface-area splits.
    for (uint32_t i = 0; i < 64; ++i)
    {
        const float x = 0.01f * std::pow(1.2f, float(i));
        Triangle(skewed, Vec3(x, 0, 0), Vec3(x + 0.004f, 0, 0), Vec3(x, 0.004f, 0.002f), 0);
    }
    for (const MeshDesc& mesh : {field, layered, alpha, quad, skewed})
    {
        meshDescs.push_back(mesh);
    }

    struct InstanceDesc
    {
        uint32_t mesh;
        Mat4     transform;
        bool     enabled;
    };
    const InstanceDesc instances[] = {
        {0, Mat4(1), true},
        {1, glm::translate(Mat4(1), Vec3(2.5f, 0, 0)), true},
        {2, glm::translate(Mat4(1), Vec3(-2.5f, 0, 0)), true},
        {3, glm::translate(Mat4(1), Vec3(0, 2.5f, 0)), true},
        {3, glm::scale(glm::translate(Mat4(1), Vec3(0, -2.5f, 0)), Vec3(-1, 1, 1)), true}, // mirrored
        {4, glm::translate(Mat4(1), Vec3(0, 0, 3)), true},
        {0, glm::scale(glm::translate(Mat4(1), Vec3(0.3f, 0.2f, 0.1f)), Vec3(0.9f)), false}, // disabled, overlaps 0
        {0, glm::rotate(glm::translate(Mat4(1), Vec3(0, 0, -3)), 0.7f, glm::normalize(Vec3(1, 1, 0))), true},
    };
    HeapVector<UniquePtr<sg::Mesh>> meshes;
    for (const MeshDesc& desc : meshDescs)
    {
        UniquePtr<sg::Mesh> mesh = MakeUnique<sg::Mesh>("oracle mesh");
        mesh->SetAABB(Vec3(-2), Vec3(2));
        for (const auto& [first, material] : desc.submeshes)
        {
            UniquePtr<sg::SubMesh> primitive = MakeUnique<sg::SubMesh>("triangle", first, 3, 3);
            primitive->SetMaterial(material, materials[material].Get());
            mesh->AddSubMesh(primitive.Get());
            source.AddComponent(std::move(primitive));
        }
        meshes.push_back(std::move(mesh));
    }
    for (UniquePtr<sg::Material>& material : materials)
    {
        source.AddComponent(std::move(material));
    }
    HeapVector<UniquePtr<sg::Node>> nodes;
    for (uint32_t i = 0; i < std::size(instances); ++i)
    {
        UniquePtr<sg::Node> node = MakeUnique<sg::Node>(i, "oracle instance");
        node->SetData(i, instances[i].transform);
        node->AddComponent(meshes[instances[i].mesh].Get());
        source.AddRenderableNode(node.Get());
        nodes.push_back(std::move(node));
    }
    source.SetNodes(std::move(nodes));
    for (UniquePtr<sg::Mesh>& mesh : meshes)
    {
        source.AddComponent(std::move(mesh));
    }
    // A unit box at the origin makes RenderScene's scene normalization the identity,
    // so the oracle can use the authored transforms directly.
    source.GetAABB() = sg::AABB(Vec3(-0.5f), Vec3(0.5f));
    sg::Camera camera(Vec3(0, 0, -5), Vec3(0), 1);
    SceneData  data{};
    data.pScene      = &source;
    data.pVertices   = vertices.data();
    data.numVertices = static_cast<uint32_t>(vertices.size());
    data.pIndices    = indices.data();
    data.numIndices  = static_cast<uint32_t>(indices.size());
    data.pCamera     = &camera;
    asset::PrepareScenePlacement(*data.pScene, asset::ScenePlacement::CenterAndNormalize);
    RenderScene scene(device, data);
    scene.LoadSceneMaterials();
    scene.PrepareBuffers();
    for (uint32_t i = 0; i < std::size(instances); ++i)
    {
        ASSERT_TRUE(scene.SetInstanceEnabled(i, instances[i].enabled));
    }
    ASSERT_TRUE(scene.Update());

    HeapVector<OracleTriangle> oracle;
    for (uint32_t node = 0; node < std::size(instances); ++node)
    {
        const glm::dmat4 transform(instances[node].transform);
        for (const auto& [first, material] : meshDescs[instances[node].mesh].submeshes)
        {
            OracleTriangle triangle;
            glm::dvec3*    corners[] = {&triangle.a, &triangle.b, &triangle.c};
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                const asset::Vertex& vertex = vertices[indices[first + corner]];
                *corners[corner]            = glm::dvec3(transform * glm::dvec4(glm::dvec3(Vec3(vertex.pos)), 1));
                triangle.alpha[corner]      = vertex.color.a;
            }
            triangle.indexOffset = first;
            triangle.node        = node;
            triangle.material    = material;
            if (instances[node].enabled)
            {
                oracle.push_back(triangle);
            }
        }
    }

    // Rays: toward random points on any instance (disabled ones included), axis-aligned rays
    // with zero direction components, rays on the shared diagonal, and perpendicular rays
    // through the masked layers. Finite maxima end some rays short of their target.
    constexpr uint32_t kRays = 2048;
    HeapVector<Vec4>   rays;
    const auto         Add = [&](Vec3 origin, Vec3 direction, float maximum, bool closest) {
        rays.push_back(Vec4(origin, maximum));
        rays.push_back(Vec4(direction, closest ? 1.0f : 0.0f));
    };
    const auto Point = [&](uint32_t node) {
        const MeshDesc& mesh  = meshDescs[instances[node].mesh];
        const uint32_t  first = mesh.submeshes[random() % mesh.submeshes.size()].first;
        float           u = unit(random), v = unit(random);
        if (u + v > 1)
        {
            u = 1 - u;
            v = 1 - v;
        }
        const Vec3 a = Vec3(vertices[indices[first]].pos), b = Vec3(vertices[indices[first + 1]].pos),
                   c = Vec3(vertices[indices[first + 2]].pos);
        return Vec3(instances[node].transform * Vec4(a + (b - a) * u + (c - a) * v, 1));
    };
    while (rays.size() < 2 * 1536)
    {
        const Vec3  target   = Point(random() % std::size(instances));
        const float distance = Uniform(0.2f, 4.0f);
        const Vec3  origin   = target + Direction() * distance;
        Add(origin, glm::normalize(target - origin), unit(random) < 0.3f ? distance * Uniform(0.3f, 1.2f) : 1e20f,
            rays.size() % 4 == 0);
    }
    while (rays.size() < 2 * 1792)
    {
        const Vec3     target = Point(random() % std::size(instances));
        const uint32_t axis   = random() % 3;
        Vec3           direction(0);
        direction[axis] = unit(random) < 0.5f ? 1.0f : -1.0f;
        Add(target - direction * Uniform(0.2f, 3.0f), direction, 1e20f, rays.size() % 4 == 0);
    }
    while (rays.size() < 2 * 1920)
    {
        const float s      = Uniform(0.05f, 0.95f);
        const Vec3  target = Vec3(instances[3].transform * Vec4(Vec3(-0.5f + s, -0.5f + s, 0), 1));
        const Vec3  origin = target + glm::normalize(Vec3(Uniform(-0.3f, 0.3f), Uniform(-0.3f, 0.3f), -1.0f));
        Add(origin, glm::normalize(target - origin), 1e20f, true);
    }
    while (rays.size() < 2 * kRays)
    {
        const uint32_t node   = 1 + random() % 2;
        const Vec3     target = Vec3(instances[node].transform * Vec4(Uniform(-0.3f, 0.3f), Uniform(-0.4f, 0.3f), 0, 1));
        Add(target - Vec3(0, 0, 1), Vec3(0, 0, 1), 1e20f, rays.size() % 4 != 0);
    }

    SoftwareSceneRayQuery            queries(device);
    BitField<RHIBufferUsageFlagBits> storage(RHIBufferUsageFlagBits::eStorageBuffer);
    RHIBuffer* rayBuffer = Buffer(static_cast<uint32_t>(rays.size() * sizeof(Vec4)), storage, rays.data());
    BitField<RHIBufferUsageFlagBits> usage;
    usage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
    const uint32_t resultBytes = kRays * 4 * sizeof(Vec4);
    RHIBuffer*     output      = Buffer(resultBytes, usage, nullptr, RHIBufferAllocateType::eGPU);
    RHIBuffer*     readback = Buffer(resultBytes, BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer),
                                     nullptr, RHIBufferAllocateType::eCPURead);
    const auto     Trace    = [&](HeapVector<Vec4>& values) {
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        graph.GetResourceManager()->ImportHostWrittenBuffer(rayBuffer);
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("SoftwareRayQueryCheckSP");
        pass.BindStorageBuffer("Rays", rayBuffer);
        pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
        const Vec4 gi[] = {Vec4(0, 0, 0, 1), Vec4(64), Vec4(0), Vec4(0), Vec4(0)};
        pass.BindValue("uGISettings", gi, sizeof(gi));
        pass.BindValue("uSceneData", scene.GetSceneUniformData(), sizeof(SceneUniformData));
        queries.BindInputs(pass, scene);
        graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(kRays / 64, 1, 1);
        });
        graph.AddTransferPass("ReadSoftwareQueries").CopyBuffer(output, readback, {0, 0, resultBytes});
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        const bool succeeded = device->ExecuteRenderGraph(graph);
        queries.OnRenderGraphExecuted(succeeded ? RenderFrameResult::eSubmitted : RenderFrameResult::eFailed);
        ASSERT_TRUE(succeeded);
        device->WaitForIdle();
        values.resize(kRays * 4);
        std::memcpy(values.data(), readback->Map(), resultBytes);
        readback->Unmap();
    };

    ASSERT_TRUE(queries.Prepare(scene));
    ASSERT_TRUE(queries.HoldsSceneGeometry());
    HeapVector<Vec4> values;
    Trace(values);
    uint32_t strict = 0, hits = 0, misses = 0, continued = 0, edges = 0, cut = 0, visibilityMismatches = 0;
    for (uint32_t ray = 0; ray < kRays; ++ray)
    {
        SCOPED_TRACE(ray);
        const glm::dvec3 origin    = glm::dvec3(Vec3(rays[2 * ray]));
        const glm::dvec3 direction = glm::dvec3(Vec3(rays[2 * ray + 1]));
        const double     maximum   = rays[2 * ray].w;
        const bool       closest   = rays[2 * ray + 1].w > 0.5f;
        // Candidates near an edge, the alpha cutoff or the maximum distance may resolve
        // either way in float; they make a ray ambiguous only where they could decide it.
        double             nearest = std::numeric_limits<double>::infinity(), ambiguousNearest = nearest;
        double             firstCandidate = nearest;
        uint32_t           chosen = UINT32_MAX, robust = 0;
        OracleCandidate    best{};
        HeapVector<double> accepted;
        for (uint32_t index = 0; index < oracle.size(); ++index)
        {
            OracleCandidate hit;
            if (!OracleIntersect(oracle[index], origin, direction, hit))
            {
                continue;
            }
            const OracleTriangle& triangle = oracle[index];
            const double          w0       = 1 - hit.u - hit.v;
            const double          edge     = std::min({w0, hit.u, hit.v});
            const bool            masked   = triangle.material != 0;
            // Material 1 has zero alpha factor; material 2 interpolates vertex alpha (factor one).
            const double opacity =
                triangle.material == 1 ? 0.0 : triangle.alpha.x * w0 + triangle.alpha.y * hit.u + triangle.alpha.z * hit.v;
            const bool ambiguous = std::abs(edge) < 2e-4 || std::abs(hit.t - maximum) < 1e-4 * (1 + hit.t)
                                || (masked && std::abs(opacity - 0.5) < 1e-3) || hit.t < 1e-4;
            if (edge < -2e-4 || hit.t > maximum + 1e-4 * (1 + maximum))
            {
                continue;
            }
            firstCandidate = std::min(firstCandidate, hit.t);
            if (ambiguous)
            {
                ambiguousNearest = std::min(ambiguousNearest, hit.t);
                continue;
            }
            if (!masked || opacity >= 0.5)
            {
                ++robust;
                accepted.push_back(hit.t);
                if (hit.t < nearest)
                {
                    nearest = hit.t;
                    chosen  = index;
                    best    = hit;
                }
            }
        }
        std::sort(accepted.begin(), accepted.end());
        const bool  tie          = accepted.size() > 1 && accepted[1] - accepted[0] < 1e-5 * (1 + accepted[0]);
        const bool  expectHit    = robust != 0;
        const bool  ambiguousRay = closest ? (ambiguousNearest <= nearest + 1e-5 * (1 + nearest) || tie)
                                           : (!expectHit && std::isfinite(ambiguousNearest));
        const Vec4* result       = &values[4 * ray];
        if (ray >= 1792 && ray < 1920)
        {
            // Rays exactly on the shared diagonal: a hit on one of the two triangles, never a crack.
            ++edges;
            EXPECT_EQ(result[0].x, 1.0f);
            EXPECT_EQ(glm::floatBitsToUint(result[1].y), 3u);
            continue;
        }
        if (ambiguousRay)
        {
            continue;
        }
        ++strict;
        if (result[0].x != (expectHit ? 1.0f : 0.0f))
        {
            if (++visibilityMismatches <= 5)
            {
                ADD_FAILURE() << "ray " << ray << (closest ? " closest" : " any-hit") << " expected "
                              << (expectHit ? "hit" : "miss") << " at " << nearest << " origin (" << origin.x << ", "
                              << origin.y << ", " << origin.z << ") direction (" << direction.x << ", " << direction.y << ", "
                              << direction.z << ") maximum " << maximum;
            }
            continue;
        }
        if (!expectHit)
        {
            ++misses;
            cut += maximum < 1e19 ? 1 : 0;
            continue;
        }
        ++hits;
        if (closest)
        {
            const OracleTriangle& triangle  = oracle[chosen];
            continued                      += firstCandidate < nearest - 1e-5 ? 1 : 0;
            EXPECT_EQ(glm::floatBitsToUint(result[1].x), triangle.indexOffset);
            EXPECT_EQ(glm::floatBitsToUint(result[1].y), triangle.node);
            EXPECT_EQ(glm::floatBitsToUint(result[1].z), triangle.material);
            EXPECT_NEAR(result[0].y, nearest, 1e-4 * (1 + nearest));
            EXPECT_NEAR(result[0].z, best.u, 1e-3);
            EXPECT_NEAR(result[0].w, best.v, 1e-3);
            const glm::dvec3 point = origin + direction * nearest;
            EXPECT_LT(glm::length(glm::dvec3(Vec3(result[2])) - point), 1e-4 * (1 + glm::length(origin) + nearest));
            const glm::dvec3 normal = glm::normalize(glm::cross(triangle.b - triangle.a, triangle.c - triangle.a));
            const glm::dvec3 actual = glm::dvec3(Vec3(result[3]));
            EXPECT_NEAR(glm::length(actual), 1.0, 1e-4);
            EXPECT_LT(glm::dot(actual, direction), 0.0);
            EXPECT_GT(std::abs(glm::dot(actual, normal)), 0.9999);
        }
    }
    EXPECT_EQ(visibilityMismatches, 0u);
    std::printf("[ oracle   ] decided=%u hits=%u misses=%u continued=%u cut=%u diagonal=%u\n", strict, hits, misses, continued,
                cut, edges);
    // The oracle decided most rays, and they exercised every behavior under test.
    EXPECT_GT(strict, 1400u);
    EXPECT_GT(hits, 500u);
    EXPECT_GT(misses, 100u);
    EXPECT_GT(continued, 20u);
    EXPECT_GT(cut, 20u);
    EXPECT_EQ(edges, 128u);

    // Without enabled triangles the BVH has an empty root, and every ray misses.
    for (uint32_t i = 0; i < std::size(instances); ++i)
    {
        ASSERT_TRUE(scene.SetInstanceEnabled(i, false));
    }
    ASSERT_TRUE(scene.Update());
    ASSERT_TRUE(queries.Prepare(scene));
    Trace(values);
    for (uint32_t ray = 0; ray < kRays; ++ray)
    {
        EXPECT_EQ(values[4 * ray].x, 0.0f) << ray;
    }
    // Unused-path placeholders bind valid buffers that hold no scene geometry and never hit.
    ASSERT_TRUE(queries.PrepareUnused());
    EXPECT_FALSE(queries.HoldsSceneGeometry());
    Trace(values);
    for (uint32_t ray = 0; ray < kRays; ++ray)
    {
        EXPECT_EQ(values[4 * ray].x, 0.0f) << ray;
    }
    queries.Destroy();
    scene.Destroy();
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes, RayQueryIntegrationTest, testing::Values(0u, 1u, 2u, 3u));
} // namespace
