#include "Graphics/RenderCore/V2/GIVisibilityProvider.h"
#include "Graphics/RenderCore/V2/Renderer/DynamicVoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h"
#include "Graphics/Shared/LightingCapture.h"
#include "Graphics/RenderCore/V2/ComputeDispatch.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
using namespace zen;
using namespace zen::rc;
constexpr uint32_t Side = 7;
struct OccupiedCell
{
    glm::uvec3 cell;
    uint32_t objectClass;
};

uint32_t CellID(glm::uvec3 cell)
{
    return cell.z + Side * (cell.y + Side * cell.x);
}

GIHit Oracle(const GIQuery& ray, const HeapVector<OccupiedCell>& cells, const GIGridUniform& grid)
{
    GIHit hit{};
    hit.identity = glm::uvec4(GI_UNKNOWN, 0, GI_INVALID_CELL, 0);
    bool valid   = ray.originMin.w >= 0 && ray.directionMax.w >= ray.originMin.w &&
        std::abs(glm::dot(Vec3(ray.directionMax), Vec3(ray.directionMax)) - 1.0f) <= 2e-4f &&
        ray.source.x > 0 && ray.source.x <= GI_ALL &&
        (ray.source.y == 0 || ray.source.y == GI_STATIC || ray.source.y == GI_DYNAMIC) &&
        (ray.source.y == 0 ||
         ray.source.z < grid.dimensions.x * grid.dimensions.x * grid.dimensions.x);
    for (uint32_t i = 0; i < 4; ++i)
    {
        valid = valid && std::isfinite(ray.originMin[i]) && std::isfinite(ray.directionMax[i]);
    }
    if (valid &&
        (ray.originMin.w == ray.directionMax.w ||
         (grid.dimensions.y & ray.source.x) == ray.source.x))
    {
        hit.identity.x = GI_MISS;
        double nearest = ray.directionMax.w;
        for (const OccupiedCell& cell : cells)
        {
            const glm::dvec3 minimum = glm::dvec3(Vec3(grid.minimumCellSize)) +
                glm::dvec3(cell.cell) * double(grid.minimumCellSize.w);
            const glm::dvec3 maximum = minimum + double(grid.minimumCellSize.w);
            bool inside              = true;
            bool intersects          = (ray.source.x & cell.objectClass) != 0;
            double lo                = ray.originMin.w;
            double hi                = ray.directionMax.w;
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                const double o = ray.originMin[axis], d = ray.directionMax[axis];
                inside = inside && o >= minimum[axis] && o <= maximum[axis];
                if (d == 0)
                {
                    intersects = intersects && o >= minimum[axis] && o < maximum[axis];
                }
                else
                {
                    const double a = (minimum[axis] - o) / d, b = (maximum[axis] - o) / d;
                    lo = std::max(lo, std::min(a, b));
                    hi = std::min(hi, std::max(a, b));
                }
            }
            const bool self = inside && ray.source.y == cell.objectClass &&
                ray.source.z ==
                    cell.cell.z +
                        grid.dimensions.x * (cell.cell.y + grid.dimensions.x * cell.cell.x);
            const bool nearer =
                lo < nearest || (lo == nearest && cell.objectClass < hit.identity.y);
            if (intersects && !self && lo < hi && nearer)
            {
                nearest      = lo;
                hit.identity = glm::uvec4(GI_HIT, cell.objectClass,
                                          cell.cell.z +
                                              grid.dimensions.x *
                                                  (cell.cell.y + grid.dimensions.x * cell.cell.x),
                                          GI_CELL_PRECISION | GI_SURFACE_VALID);
                hit.positionDistance =
                    Vec4(Vec3(ray.originMin) + Vec3(ray.directionMax) * float(lo), float(lo));
            }
        }
    }
    return hit;
}

void AddRay(HeapVector<GIQuery>& queries,
            Vec3 origin,
            Vec3 direction,
            uint32_t mask        = GI_ALL,
            float tMin           = 0,
            float tMax           = 30,
            uint32_t sourceClass = 0,
            uint32_t sourceCell  = GI_INVALID_CELL)
{
    queries.push_back({Vec4(origin, tMin), Vec4(glm::normalize(direction), tMax),
                       glm::uvec4(mask, sourceClass, sourceCell, 0)});
}

class DynamicVoxelGIIntegrationTest : public testing::TestWithParam<uint32_t>
{
protected:
    RenderDevice* device{nullptr};
    HeapVector<RHIBuffer*> buffers;
    HeapVector<DynamicVoxelGIRenderer*> renderers;
    HeapVector<RHITexture*> textures;
    VkDebugUtilsMessengerEXT messenger{};
    VkInstance instance{};

    static VKAPI_ATTR VkBool32 VKAPI_CALL
    Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
               VkDebugUtilsMessageTypeFlagsEXT,
               const VkDebugUtilsMessengerCallbackDataEXT* data,
               void*)
    {
        ADD_FAILURE() << data->pMessage;
        return VK_FALSE;
    }

    void SetUp() override
    {
        RHIOptions::GetInstance().SetRayTracingEnabled(false);
        RHIOptions::GetInstance().SetGPUProfilerMarkers(true);
        device = ZEN_NEW()
            RenderDevice(RHIAPIType::eVulkan, 2,
                         (GetParam() & 1) ? RHIExecutionMode::eThreaded : RHIExecutionMode::eInline,
                         (GetParam() & 2) ? AsyncComputeMode::eAuto : AsyncComputeMode::eDisabled);
        device->Init(nullptr);
        EXPECT_FALSE(GVulkanRHI->GetDevice()->GetExtensionFlags().hasAccelerationStructure);
        EXPECT_FALSE(GVulkanRHI->GetDevice()->GetExtensionFlags().hasRayQuery);
        EXPECT_FALSE(GVulkanRHI->GetDevice()->GetExtensionFlags().hasRaytracingPipeline);
        instance = GVulkanRHI->GetInstance();
        VkDebugUtilsMessengerCreateInfoEXT info{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = Validation;
        ASSERT_EQ(vkCreateDebugUtilsMessengerEXT(instance, &info, nullptr, &messenger), VK_SUCCESS);
        ShaderProgramManager::GetInstance().BuildShaderPrograms(device);
    }

    void TearDown() override
    {
        device->FlushRHIThread();
        device->WaitForIdle();
        for (DynamicVoxelGIRenderer* renderer : renderers)
        {
            renderer->Destroy();
            ZEN_DELETE(renderer);
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
        vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
        device->Destroy();
        ZEN_DELETE(device);
        RHIOptions::GetInstance().SetRayTracingEnabled(true);
        RHIOptions::GetInstance().SetGPUProfilerMarkers(false);
    }

    RHIBuffer* Buffer(uint32_t bytes, RHIBufferAllocateType type, const void* data = nullptr)
    {
        RHIBufferCreateInfo info;
        info.size         = bytes;
        info.allocateType = type;
        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer,
                                 RHIBufferUsageFlagBits::eTransferSrcBuffer,
                                 RHIBufferUsageFlagBits::eTransferDstBuffer);
        RHIBuffer* buffer = device->CreateBuffer(info);
        buffers.push_back(buffer);
        if (data != nullptr)
        {
            uint8_t* mapped = buffer->Map();
            EXPECT_NE(mapped, nullptr);
            if (mapped != nullptr)
            {
                std::memcpy(mapped, data, bytes);
                buffer->Unmap();
            }
        }
        return buffer;
    }

    RHITexture* Volume(DataFormat format, uint32_t side = Side)
    {
        RHITextureCreateInfo info;
        info.type  = RHITextureType::e3D;
        info.width = info.height = info.depth = side;
        info.format                           = format;
        info.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage,
                                 RHITextureUsageFlagBits::eTransferDst);
        RHITexture* texture = device->CreateTexture(info);
        textures.push_back(texture);
        return texture;
    }

    VoxelTextures Grid(RenderGraph& graph,
                       const HeapVector<OccupiedCell>& cells,
                       uint32_t objectClass,
                       uint32_t side = Side)
    {
        VoxelTextures result;
        result.pOwner        = Volume(DataFormat::eR32UInt, side);
        result.pAlbedo       = Volume(DataFormat::eR8G8B8A8UNORM, side);
        result.pNormal       = Volume(DataFormat::eR8G8B8A8UNORM, side);
        result.pEmissive     = Volume(DataFormat::eR16G16B16A16SFloat, side);
        result.pReflectance  = Volume(DataFormat::eR8G8B8A8UNORM, side);
        result.pAlbedoView   = result.pAlbedo->GetDefaultView();
        result.pNormalView   = result.pNormal->GetDefaultView();
        result.pEmissiveView = result.pEmissive->GetDefaultView();
        HeapVector<uint32_t> owners(side * side * side, GI_INVALID_CELL);
        for (const OccupiedCell& cell : cells)
        {
            if (cell.objectClass == objectClass)
            {
                owners[cell.cell.x + side * (cell.cell.y + side * cell.cell.z)] = 0;
            }
        }
        RHIBuffer* upload = Buffer(static_cast<uint32_t>(owners.size() * sizeof(uint32_t)),
                                   RHIBufferAllocateType::eCPUWrite, owners.data());
        graph.GetResourceManager()->ImportHostWrittenBuffer(upload);
        RHIBufferTextureCopyRegion region{};
        region.textureSize = Vec3i(side);
        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        graph.AddTransferPass("QueryGridUpload")
            .CopyBufferToTexture(upload, result.pOwner, region)
            .ClearTexture(result.pAlbedo, Color(0.25f, 0.5f, 0.75f, 1))
            .ClearTexture(result.pNormal, Color(0.5f, 0.5f, 1, 0))
            .ClearTexture(result.pEmissive, Color(8, 0.5f, 0.125f, 1))
            .ClearTexture(result.pReflectance, Color(0.125f, 0.25f, 0.5f, 1));
        return result;
    }

    HeapVector<GIQueryResult> RunQueries(RenderGraph& graph,
                                         const GIVisibilityProvider& provider,
                                         const HeapVector<GIQuery>& queries)
    {
        const uint32_t count = static_cast<uint32_t>(queries.size());
        RHIBuffer* input =
            Buffer(count * sizeof(GIQuery), RHIBufferAllocateType::eCPUWrite, queries.data());
        RHIBuffer* output = Buffer(count * sizeof(GIQueryResult), RHIBufferAllocateType::eGPU);
        RHIBuffer* readback =
            Buffer(count * sizeof(GIQueryResult), RHIBufferAllocateType::eCPURead);
        graph.GetResourceManager()->ImportHostWrittenBuffer(input);
        RHIGPUInfo limits               = device->GetGPUInfo();
        limits.maxComputeWorkGroupCount = {2, 2, 1}; // Force both XYZ mapping and multiple chunks.
        EXPECT_TRUE(BuildGIQueryPass(graph, provider, input, output, count, limits,
                                     RDGQueuePreference::ePreferAsyncCompute));
        graph.AddTransferPass("ReadQueryResults")
            .CopyBuffer(output, readback, {0, 0, count * sizeof(GIQueryResult)})
            .NeverCull();
        EXPECT_TRUE(graph.End());
        EXPECT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        device->FlushRHIThread();
        device->WaitForIdle();
        HeapVector<GIQueryResult> result(count);
        const uint8_t* mapped = readback->Map();
        EXPECT_NE(mapped, nullptr);
        if (mapped != nullptr)
        {
            std::memcpy(result.data(), mapped, count * sizeof(GIQueryResult));
            readback->Unmap();
        }
        return result;
    }
#include "DynamicVoxelGIStaticHelpers.inl"
#include "DynamicVoxelGIFrameHelpers.inl"
#include "DynamicVoxelGIFilterHelpers.inl"
#include "DynamicVoxelGILightingHelpers.inl"
};

TEST_P(DynamicVoxelGIIntegrationTest, DiagnosticTraversalCountsActualCellsWithoutChangingHits)
{
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    const HeapVector<OccupiedCell> cells = {{{2, 4, 4}, GI_STATIC}};
    const VoxelTextures s                = Grid(graph, cells, GI_STATIC);
    const VoxelTextures d                = Grid(graph, cells, GI_DYNAMIC);
    GIGridUniform grid{Vec4(-2, -2, -2, 0.5f), glm::uvec4(Side, GI_ALL, 0, 1),
                       glm::uvec4(1, 0, 0, 0)};
    VoxelDDAProvider provider;
    ASSERT_TRUE(provider.Prepare(s, d, grid, 1));
    HeapVector<GIQuery> queries;
    AddRay(queries, Vec3(-3, 0.25f, 0.25f), Vec3(1, 0, 0), GI_STATIC);
    AddRay(queries, Vec3(-3, 0.25f, 0.25f), Vec3(1, 0, 0), GI_DYNAMIC);
    AddRay(queries, Vec3(-3, 3, 0.25f), Vec3(1, 0, 0), GI_ALL);
    const HeapVector<GIQueryResult> output = RunQueries(graph, provider, queries);
    ASSERT_EQ(output.size(), 3);
    EXPECT_EQ(output[0].occlusion, glm::uvec4(GI_HIT, 3, 3, 0));
    EXPECT_EQ(output[1].occlusion, glm::uvec4(GI_MISS, Side, Side, 0));
    EXPECT_EQ(output[2].occlusion, glm::uvec4(GI_MISS, 0, 0, 0));
}

TEST_P(DynamicVoxelGIIntegrationTest, QueriesMatchIndependentBoxesAndProviderSubstitution)
{
    const HeapVector<OccupiedCell> cells = {{{1, 1, 1}, GI_STATIC},  {{3, 1, 1}, GI_STATIC},
                                            {{1, 1, 1}, GI_DYNAMIC}, {{2, 1, 1}, GI_DYNAMIC},
                                            {{5, 5, 5}, GI_STATIC},  {{4, 3, 2}, GI_DYNAMIC},
                                            {{0, 0, 0}, GI_STATIC},  {{6, 6, 6}, GI_DYNAMIC}};
    GIGridUniform grid{Vec4(-2, -2, -2, 0.5f), glm::uvec4(Side, GI_ALL, 0, 0),
                       glm::uvec4(1, 0, 0, 0)};
    RenderGraph graph("query_oracle");
    ASSERT_TRUE(graph.Begin());
    VoxelTextures s = Grid(graph, cells, GI_STATIC), d = Grid(graph, cells, GI_DYNAMIC);
    VoxelDDAProvider provider;
    EXPECT_FALSE(provider.Prepare(s, d, grid, 0));
    VoxelTextures invalid = s;
    invalid.pAlbedoView   = nullptr;
    EXPECT_FALSE(provider.Prepare(invalid, d, grid, 1));
    RDGComputePassDesc invalidPass;
    EXPECT_FALSE(provider.BindQueryInputs(invalidPass));
    EXPECT_FALSE(BuildGIQueryPass(graph, provider, nullptr, nullptr, 1, device->GetGPUInfo()));
    ASSERT_TRUE(provider.Prepare(s, d, grid, 1));
    HeapVector<GIQuery> queries;
    for (uint32_t mask : {GI_STATIC, GI_DYNAMIC, GI_ALL})
    {
        for (const Vec3 origin :
             {Vec3(-3), Vec3(-1.25f), Vec3(-1.5f), Vec3(0), Vec3(1.5f), Vec3(3)})
        {
            for (int x = -1; x <= 1; ++x)
            {
                for (int y = -1; y <= 1; ++y)
                {
                    for (int z = -1; z <= 1; ++z)
                    {
                        if (x != 0 || y != 0 || z != 0)
                        {
                            AddRay(queries, origin, Vec3(x, y, z), mask);
                        }
                    }
                }
            }
        }
    }
    uint32_t random = 0x51bada;
    for (uint32_t i = 0; i < 2048; ++i)
    {
        Vec3 origin, direction;
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            random          = random * 1664525u + 1013904223u;
            origin[axis]    = float(random & 65535u) / 65535.0f * 8 - 4;
            random          = random * 1664525u + 1013904223u;
            direction[axis] = float(random & 65535u) / 65535.0f * 2 - 1;
        }
        AddRay(queries, origin, direction, i % 3 + 1, 0.125f, 8);
    }
    // Finite endpoints, zero interval, initial face/inside self hints and an invalid hint.
    for (float stop : {0.0f, 0.5f, 0.75f, 1.0f, 2.0f})
    {
        AddRay(queries, Vec3(-2, -1.25f, -1.25f), Vec3(1, 0, 0), GI_ALL, 0, stop);
    }
    for (uint32_t mask : {GI_STATIC, GI_DYNAMIC, GI_ALL})
    {
        AddRay(queries, Vec3(-1.25f), Vec3(1, 0, 0), mask, 0, 10, GI_STATIC, CellID({1, 1, 1}));
        AddRay(queries, Vec3(-1, -1.25f, -1.25f), Vec3(-1, 0, 0), mask, 0, 10, GI_STATIC,
               CellID({1, 1, 1}));
        AddRay(queries, Vec3(-3, -1.25f, -1.25f), Vec3(1, 0, 0), mask, 0, 10, GI_STATIC,
               CellID({1, 1, 1}));
    }
    AddRay(queries, Vec3(0), Vec3(1, 0, 0), GI_ALL, 0, 10, GI_STATIC, GI_INVALID_CELL);
    AddRay(queries, Vec3(0), Vec3(1, 0, 0), GI_ALL, 2, 1);
    queries.push_back({Vec4(0), Vec4(0, 0, 0, 1), glm::uvec4(GI_ALL, 0, GI_INVALID_CELL, 0)});
    queries.push_back({Vec4(0), Vec4(std::numeric_limits<float>::infinity(), 0, 0, 1),
                       glm::uvec4(GI_ALL, 0, GI_INVALID_CELL, 0)});
    const HeapVector<GIQueryResult> actual = RunQueries(graph, provider, queries);
    HeapVector<GIHit> expected;
    for (uint32_t i = 0; i < queries.size(); ++i)
    {
        SCOPED_TRACE(i);
        GIHit hit = Oracle(queries[i], cells, grid);
        EXPECT_EQ(actual[i].closest.identity, hit.identity);
        EXPECT_EQ(actual[i].occlusion.x, hit.identity.x);
        if (hit.identity.x == GI_HIT)
        {
            for (uint32_t c = 0; c < 4; ++c)
            {
                EXPECT_NEAR(actual[i].closest.positionDistance[c], hit.positionDistance[c], 2e-5f);
            }
            EXPECT_NEAR(actual[i].closest.emission.x, 8, 1e-5f);
            EXPECT_NEAR(actual[i].closest.baseColorMetallic.x, 64.0f / 255, 1e-5f);
            const float rho = hit.identity.y == GI_STATIC ? 32.0f / 255 : 64.0f / 255 * 0.96f;
            EXPECT_NEAR(actual[i].closest.diffuseReflectance.x, rho, 1e-5f);
        }
        // Exact decoded data supplies the independent binding/consumer substitution check.
        expected.push_back(actual[i].closest);
    }
    ASSERT_TRUE(graph.Begin());
    RHIBuffer* responses = Buffer(static_cast<uint32_t>(expected.size() * sizeof(GIHit)),
                                  RHIBufferAllocateType::eCPUWrite, expected.data());
    graph.GetResourceManager()->ImportHostWrittenBuffer(responses);
    DeterministicGIProvider reference;
    ASSERT_TRUE(reference.Prepare(responses, static_cast<uint32_t>(expected.size()), 2,
                                  device->GetGPUInfo()));
    const HeapVector<GIQueryResult> substituted = RunQueries(graph, reference, queries);
    EXPECT_EQ(std::memcmp(actual.data(), substituted.data(), actual.size() * sizeof(GIQueryResult)),
              0);
    // Missing attributes are still occluders; never turn them into environment misses.
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("MissingSurface").ClearTexture(s.pAlbedo, Color(0));
    HeapVector<GIQuery> missingSurface;
    AddRay(missingSurface, Vec3(-1.25f), Vec3(1, 0, 0), GI_STATIC);
    const GIHit missing = RunQueries(graph, provider, missingSurface)[0].closest;
    EXPECT_EQ(missing.identity.x, GI_HIT);
    EXPECT_EQ(missing.identity.w, GI_CELL_PRECISION);
    grid.dimensions.y = GI_STATIC;
    ASSERT_TRUE(provider.Prepare(s, d, grid, 3));
    HeapVector<GIQuery> incomplete;
    AddRay(incomplete, Vec3(-3), Vec3(1, 0, 0));
    ASSERT_TRUE(graph.Begin());
    EXPECT_EQ(RunQueries(graph, provider, incomplete)[0].closest.identity.x, GI_UNKNOWN);
    grid.dimensions.y = GI_ALL;
    grid.dimensions.z = 1;
    ASSERT_TRUE(provider.Prepare(s, d, grid, 4));
    HeapVector<GIQuery> truncated;
    AddRay(truncated, Vec3(-3, -1.25f, -1.25f), Vec3(1, 0, 0));
    ASSERT_TRUE(graph.Begin());
    EXPECT_EQ(RunQueries(graph, provider, truncated)[0].closest.identity.x, GI_UNKNOWN);
    ASSERT_TRUE(graph.Begin());
    const HeapVector<OccupiedCell> empty;
    s                 = Grid(graph, empty, GI_STATIC);
    d                 = Grid(graph, empty, GI_DYNAMIC);
    grid.dimensions.z = 0;
    ASSERT_TRUE(provider.Prepare(s, d, grid, 5));
    const HeapVector<GIQueryResult> emptyResults = RunQueries(graph, provider, queries);
    for (uint32_t i = 0; i < queries.size(); ++i)
    {
        EXPECT_EQ(emptyResults[i].closest.identity.x, Oracle(queries[i], empty, grid).identity.x);
    }
}

#include "DynamicVoxelGIStaticTests.inl"
#include "DynamicVoxelGIFrameTests.inl"
#include "DynamicVoxelGIFilterTests.inl"
#include "DynamicVoxelGILightingTests.inl"
#include "DynamicVoxelGIAcceptanceTests.inl"

INSTANTIATE_TEST_SUITE_P(SubmissionModes,
                         DynamicVoxelGIIntegrationTest,
                         testing::Values(0u, 1u, 2u, 3u));
} // namespace
