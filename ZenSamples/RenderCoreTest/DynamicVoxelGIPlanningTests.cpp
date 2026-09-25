#include "Graphics/RenderCore/V2/ComputeDispatch.h"
#include "Graphics/RenderCore/V2/DynamicVoxelGIPlanning.h"
#include "Graphics/Shared/VoxelGI.h"
#include "Graphics/Shared/DynamicVoxelGI.h"
#include <gtest/gtest.h>
#include <limits>
#include <sstream>

namespace
{
using namespace zen;
using namespace zen::rc;

TEST(VoxelReflectancePlanning, ChecksCountRangeBudgetAndRetirementBeforeAllocation)
{
    RHIGPUInfo gpu;
    gpu.maxStorageBufferRange = std::numeric_limits<uint32_t>::max();
    uint64_t peak             = 0;
    constexpr uint64_t maxCount =
        std::numeric_limits<uint32_t>::max() / ZEN_VOXEL_REFLECTANCE_SCALE;
    constexpr uint64_t required = 64ull * 64 * 64 * 20;
    EXPECT_EQ(ValidateVoxelReflectanceResources(64, maxCount, required, 0, gpu, peak),
              GIResourceStatus::eSuccess);
    EXPECT_EQ(peak, required);
    EXPECT_EQ(ValidateVoxelReflectanceResources(64, maxCount + 1, required, 0, gpu, peak),
              GIResourceStatus::eOverflow);
    EXPECT_EQ(ValidateVoxelReflectanceResources(64, 0, required - 1, 0, gpu, peak),
              GIResourceStatus::eBudget);
    EXPECT_EQ(ValidateVoxelReflectanceResources(64, 0, required + 37, 37, gpu, peak),
              GIResourceStatus::eSuccess);
    EXPECT_EQ(peak, required + 37);
    EXPECT_EQ(ValidateVoxelReflectanceResources(64, 0, required + 36, 37, gpu, peak),
              GIResourceStatus::eBudget);
    EXPECT_EQ(ValidateVoxelReflectanceResources(64, 0, required, UINT64_MAX, gpu, peak),
              GIResourceStatus::eOverflow);
    EXPECT_EQ(ValidateVoxelReflectanceResources(64, 0, 0, 0, gpu, peak),
              GIResourceStatus::eInvalidInput);
    EXPECT_EQ(ValidateVoxelReflectanceResources(UINT32_MAX, 0, UINT64_MAX, 0, gpu, peak),
              GIResourceStatus::eInvalidInput);
    gpu.maxStorageBufferRange = 64u * 64 * 64 * 16;
    EXPECT_EQ(ValidateVoxelReflectanceResources(64, 0, required, 0, gpu, peak),
              GIResourceStatus::eSuccess);
    --gpu.maxStorageBufferRange;
    EXPECT_EQ(ValidateVoxelReflectanceResources(64, 0, required, 0, gpu, peak),
              GIResourceStatus::eDescriptorRange);
    gpu.maxStorageBufferRange = 128u * 1024 * 1024;
    EXPECT_EQ(ValidateVoxelReflectanceResources(256, 0, UINT64_MAX, 0, gpu, peak),
              GIResourceStatus::eDescriptorRange);
}

TEST(StaticVoxelGIPlanning, CompactCacheHonorsExactBudgetRangeAndRetirement)
{
    RHIGPUInfo gpu;
    gpu.maxStorageBufferRange = UINT32_MAX;
    for (uint32_t side : {64u, 128u, 256u})
    {
        uint64_t base = 0;
        ASSERT_EQ(ValidateVoxelClassResources(side, true, UINT64_MAX, 0, gpu, base),
                  GIResourceStatus::eSuccess);
        const uint64_t cells        = uint64_t(side) * side * side;
        constexpr uint64_t surfaces = 4096;
        constexpr uint64_t retiring = 1234567;
        base += cells * GI_FRAME_CELL_BYTES + GI_FRAME_FIXED_BYTES +
            32 + surfaces + retiring;
        constexpr uint64_t receiver = GI_FACE_COUNT * GI_FACE_RAYS * GI_COMPACT_HIT_BYTES;
        uint64_t peak               = 0;
        uint32_t capacity           = 0;
        EXPECT_EQ(PlanStaticVoxelGIResources(side, true, base + receiver, gpu, capacity, peak,
                                             surfaces, retiring, true),
                  GIResourceStatus::eSuccess);
        EXPECT_EQ(capacity, 1);
        EXPECT_EQ(peak, base + receiver);
        EXPECT_EQ(PlanStaticVoxelGIResources(side, true, base + receiver - 1, gpu, capacity, peak,
                                             surfaces, retiring, true),
                  GIResourceStatus::eBudget);
        EXPECT_EQ(PlanStaticVoxelGIResources(side, true, UINT64_MAX, gpu, capacity, peak, surfaces,
                                             UINT64_MAX, true),
                  GIResourceStatus::eOverflow);
    }
    gpu.maxStorageBufferRange = 128u * 1024 * 1024;
    uint64_t peak             = 0;
    uint32_t capacity         = 0;
    EXPECT_EQ(PlanStaticVoxelGIResources(64, true, UINT64_MAX, gpu, capacity, peak, 0, 0, true),
              GIResourceStatus::eSuccess);
    EXPECT_EQ(uint64_t(capacity) * GI_FACE_RAYS * GI_COMPACT_HIT_BYTES, gpu.maxStorageBufferRange);
}

TEST(StaticVoxelGIPlanning, AccountsForUnpackedHitsSurfacesAndDescriptorLimits)
{
    RHIGPUInfo gpu;
    gpu.maxStorageBufferRange = UINT32_MAX;
    uint64_t classPeak        = 0;
    ASSERT_EQ(ValidateVoxelClassResources(64, true, UINT64_MAX, 0, gpu, classPeak),
              GIResourceStatus::eSuccess);
    constexpr uint64_t surfaces = 2048ull * 2048 * 16 * 3;
    const uint64_t base = classPeak + 64ull * 64 * 64 * GI_FRAME_CELL_BYTES + GI_FRAME_FIXED_BYTES +
        32 + surfaces;
    constexpr uint64_t receiver = GI_FACE_COUNT * GI_FACE_RAYS * sizeof(GIHit);
    uint64_t peak               = 0;
    uint32_t capacity           = 0;
    EXPECT_EQ(PlanStaticVoxelGIResources(64, true, base + receiver, gpu, capacity, peak, surfaces),
              GIResourceStatus::eSuccess);
    EXPECT_EQ(capacity, 1);
    EXPECT_EQ(peak, base + receiver);
    EXPECT_EQ(
        PlanStaticVoxelGIResources(64, true, base + receiver - 1, gpu, capacity, peak, surfaces),
        GIResourceStatus::eBudget);
    EXPECT_EQ(capacity, 0);
    EXPECT_EQ(PlanStaticVoxelGIResources(64, true, UINT64_MAX, gpu, capacity, peak, UINT64_MAX),
              GIResourceStatus::eOverflow);
    const uint64_t exactSurfaceLimit = UINT64_MAX - (base - surfaces) - receiver;
    EXPECT_EQ(
        PlanStaticVoxelGIResources(64, true, UINT64_MAX, gpu, capacity, peak, exactSurfaceLimit),
        GIResourceStatus::eSuccess);
    EXPECT_EQ(capacity, 1);
    EXPECT_EQ(peak, UINT64_MAX);
    gpu.maxStorageBufferRange = 128ull * 1024 * 1024;
    EXPECT_EQ(PlanStaticVoxelGIResources(64, true, UINT64_MAX, gpu, capacity, peak),
              GIResourceStatus::eSuccess);
    EXPECT_LE(uint64_t(capacity) * GI_FACE_RAYS * sizeof(GIHit), gpu.maxStorageBufferRange);
    EXPECT_GT(uint64_t(capacity + 1) * GI_FACE_RAYS * sizeof(GIHit), gpu.maxStorageBufferRange);
}

TEST(StaticVoxelGIPlanning, WiderGridsIncludeRetiringAllocationsBeforeReservingHits)
{
    RHIGPUInfo gpu;
    gpu.maxStorageBufferRange   = UINT32_MAX;
    constexpr uint64_t surfaces = 256ull * 256 * 16 * 3;
    constexpr uint64_t retiring = 123456789;
    constexpr uint64_t receiver = GI_FACE_COUNT * GI_FACE_RAYS * sizeof(GIHit);
    for (uint32_t side : {64u, 128u, 256u})
    {
        uint64_t classes = 0;
        ASSERT_EQ(ValidateVoxelClassResources(side, true, UINT64_MAX, 0, gpu, classes),
                  GIResourceStatus::eSuccess);
        const uint64_t cells   = uint64_t(side) * side * side;
        const uint64_t minimum = classes + cells * GI_FRAME_CELL_BYTES + GI_FRAME_FIXED_BYTES +
            32 + surfaces + retiring + receiver;
        uint64_t peak     = 0;
        uint32_t capacity = 0;
        EXPECT_EQ(PlanStaticVoxelGIResources(side, true, minimum, gpu, capacity, peak, surfaces,
                                             retiring),
                  GIResourceStatus::eSuccess);
        EXPECT_EQ(capacity, 1);
        EXPECT_EQ(peak, minimum);
        EXPECT_EQ(PlanStaticVoxelGIResources(side, true, minimum - 1, gpu, capacity, peak, surfaces,
                                             retiring),
                  GIResourceStatus::eBudget);
        EXPECT_EQ(capacity, 0);
        EXPECT_EQ(PlanStaticVoxelGIResources(side, true, UINT64_MAX, gpu, capacity, peak, 0,
                                             UINT64_MAX - classes),
                  GIResourceStatus::eOverflow);
        EXPECT_EQ(PlanStaticVoxelGIResources(side, true, UINT64_MAX, gpu, capacity, peak, surfaces,
                                             UINT64_MAX),
                  GIResourceStatus::eOverflow);
    }
    uint64_t peak     = 0;
    uint32_t capacity = 0;
    EXPECT_EQ(PlanStaticVoxelGIResources(256, true, 3072ull * 1024 * 1024, gpu, capacity, peak),
              GIResourceStatus::eBudget);
    EXPECT_EQ(capacity, 0);
}

TEST(DynamicVoxelGIPlanning, SelectionNeverAdvertisesAnUnimplementedProvider)
{
    for (const char* method : {"auto", "cone", "dynamic_voxel"})
    {
        for (const char* backend : {"auto", "voxel_dda", "hardware_rt"})
        {
            std::istringstream input(std::string("voxel_gi_method=") + method +
                                     "\ndynamic_voxel_gi_query_backend=" + backend);
            platform::ConfigLoader config(input);
            DynamicVoxelGISettings settings;
            EXPECT_TRUE(LoadDynamicVoxelGISettings(config, settings));
            const VoxelGISelection selection = ResolveVoxelGISelection(settings);
            EXPECT_EQ(selection.method, VoxelGIMethod::eCone);
            EXPECT_STREQ(selection.backend, "none");
            EXPECT_FALSE(std::string(selection.reason).empty());
        }
    }
}

TEST(DynamicVoxelGIPlanning, ClassResourcesIncludeIndependentScratchAndConeTransition)
{
    RHIGPUInfo gpu;
    gpu.maxStorageBufferRange = UINT32_MAX;
    for (uint32_t side : {64u, 128u, 256u})
    {
        uint64_t owner = 0, averaged = 0, peak = 0;
        EXPECT_EQ(ValidateVoxelClassResources(side, false, UINT64_MAX, 0, gpu, owner),
                  GIResourceStatus::eSuccess);
        EXPECT_EQ(ValidateVoxelClassResources(side, true, UINT64_MAX, 0, gpu, averaged),
                  GIResourceStatus::eSuccess);
        EXPECT_EQ(averaged - owner, uint64_t(side) * side * side * 20 * 3);
        EXPECT_EQ(ValidateVoxelClassResources(side, true, averaged + 73, 73, gpu, peak),
                  GIResourceStatus::eSuccess);
        EXPECT_EQ(peak, averaged + 73);
        EXPECT_EQ(ValidateVoxelClassResources(side, true, averaged + 72, 73, gpu, peak),
                  GIResourceStatus::eBudget);
        EXPECT_EQ(ValidateVoxelClassResources(side, false, owner - 1, 0, gpu, peak),
                  GIResourceStatus::eBudget);
        EXPECT_EQ(ValidateVoxelClassResources(side, true, UINT64_MAX, UINT64_MAX, gpu, peak),
                  GIResourceStatus::eOverflow);
    }
    uint64_t peak = 0;
    EXPECT_EQ(ValidateVoxelClassResources(64, false, 0, 0, gpu, peak),
              GIResourceStatus::eInvalidInput);
    EXPECT_EQ(ValidateVoxelClassResources(63, false, UINT64_MAX, 0, gpu, peak),
              GIResourceStatus::eInvalidInput);
    gpu.maxStorageBufferRange = 128 * 1024 * 1024;
    EXPECT_EQ(ValidateVoxelClassResources(256, false, UINT64_MAX, 0, gpu, peak),
              GIResourceStatus::eSuccess);
    EXPECT_EQ(ValidateVoxelClassResources(256, true, UINT64_MAX, 0, gpu, peak),
              GIResourceStatus::eDescriptorRange);
}

TEST(DynamicVoxelGIPlanning, ConeFallbackHonorsBudgetRetirementAndActualBufferRanges)
{
    RHIGPUInfo gpu;
    gpu.maxStorageBufferRange = UINT32_MAX;
    for (uint32_t side : {64u, 128u, 256u})
    {
        uint64_t owner = 0, averaged = 0, peak = 0;
        ASSERT_EQ(ValidateVoxelConeResources(side, false, UINT64_MAX, 0, gpu, owner),
                  GIResourceStatus::eSuccess);
        ASSERT_EQ(ValidateVoxelConeResources(side, true, UINT64_MAX, 0, gpu, averaged),
                  GIResourceStatus::eSuccess);
        EXPECT_EQ(averaged - owner, uint64_t(side) * side * side * 20);
        EXPECT_EQ(ValidateVoxelConeResources(side, true, averaged + 73, 73, gpu, peak),
                  GIResourceStatus::eSuccess);
        EXPECT_EQ(peak, averaged + 73);
        EXPECT_EQ(ValidateVoxelConeResources(side, true, averaged + 72, 73, gpu, peak),
                  GIResourceStatus::eBudget);
        EXPECT_EQ(ValidateVoxelConeResources(side, false, 1024 * 1024, 0, gpu, peak),
                  GIResourceStatus::eBudget);
        EXPECT_EQ(ValidateVoxelConeResources(side, true, UINT64_MAX, UINT64_MAX, gpu, peak),
                  GIResourceStatus::eOverflow);
    }
    uint64_t peak             = 0;
    gpu.maxStorageBufferRange = 1;
    EXPECT_EQ(ValidateVoxelConeResources(64, false, UINT64_MAX, 0, gpu, peak),
              GIResourceStatus::eSuccess);
    EXPECT_EQ(ValidateVoxelConeResources(64, true, UINT64_MAX, 0, gpu, peak),
              GIResourceStatus::eDescriptorRange);
    EXPECT_EQ(ValidateVoxelClassResources(64, false, UINT64_MAX, 0, gpu, peak),
              GIResourceStatus::eDescriptorRange);
}

TEST(DynamicVoxelGIPlanning, SettingsPreserveExplicitResolutionAndRejectInvalidValues)
{
    std::istringstream defaults("voxel_gi_method=dynamic_voxel");
    platform::ConfigLoader config(defaults);
    DynamicVoxelGISettings settings;
    EXPECT_TRUE(LoadDynamicVoxelGISettings(config, settings));
    EXPECT_EQ(settings.resolution, 64u);
    EXPECT_EQ(settings.memoryBudgetBytes, 0u);
    EXPECT_EQ(settings.temporal, GITemporalMode::eElapsed);
    EXPECT_TRUE(settings.spatialFilter);
    EXPECT_TRUE(settings.analyticLighting);
    EXPECT_TRUE(settings.environmentLighting);
    EXPECT_TRUE(settings.emissiveLighting);
    EXPECT_TRUE(settings.compactCache);
    std::istringstream explicitInput("voxel_resolution=256\ndynamic_voxel_gi_memory_budget_mb=512");
    platform::ConfigLoader explicitConfig(explicitInput);
    EXPECT_TRUE(LoadDynamicVoxelGISettings(explicitConfig, settings));
    EXPECT_EQ(settings.resolution, 256u);
    EXPECT_EQ(settings.memoryBudgetBytes, 512ull * 1024 * 1024);
    for (const char* invalid :
         {"voxel_gi_method=bad", "dynamic_voxel_gi_query_backend=bvh", "voxel_resolution=63",
          "dynamic_voxel_gi_rays_per_face=33", "dynamic_voxel_gi_neighbor_radius=0",
          "dynamic_voxel_gi_memory_budget_mb=-1", "dynamic_voxel_gi_memory_budget_mb=0",
          "dynamic_voxel_gi_memory_budget_mb=18446744073709551615",
          "dynamic_voxel_gi_temporal_filter=bad", "dynamic_voxel_gi_spatial_filter=bad",
          "dynamic_voxel_gi_analytic_lighting=bad", "dynamic_voxel_gi_environment_lighting=bad",
          "dynamic_voxel_gi_emissive_lighting=bad", "dynamic_voxel_gi_cache=bad"})
    {
        SCOPED_TRACE(invalid);
        std::istringstream stream(invalid);
        platform::ConfigLoader invalidConfig(stream);
        EXPECT_FALSE(LoadDynamicVoxelGISettings(invalidConfig, settings));
        EXPECT_EQ(settings.resolution, 256u);
        EXPECT_EQ(settings.memoryBudgetBytes, 512ull * 1024 * 1024);
    }
}

TEST(DynamicVoxelGIPlanning, SamplingPresetsScaleCacheWithoutReducingFrameStorage)
{
    RHIGPUInfo gpu;
    gpu.maxStorageBufferRange = UINT32_MAX;
    uint64_t previous         = 0;
    for (uint32_t rays : {32u, 64u, 128u})
    {
        std::istringstream stream(
            "dynamic_voxel_gi_cache=compact\ndynamic_voxel_gi_rays_per_face=" +
            std::to_string(rays));
        platform::ConfigLoader config(stream);
        DynamicVoxelGISettings settings;
        ASSERT_TRUE(LoadDynamicVoxelGISettings(config, settings));
        EXPECT_EQ(settings.raysPerFace, rays);
        EXPECT_TRUE(settings.compactCache);
        uint64_t peak     = 0;
        uint32_t capacity = 0;
        ASSERT_EQ(
            PlanStaticVoxelGIResources(64, true, UINT64_MAX, gpu, capacity, peak, 0, 0, true, rays),
            GIResourceStatus::eSuccess);
        EXPECT_EQ(capacity, 64u * 64 * 64);
        if (previous != 0)
        {
            EXPECT_EQ(peak - previous,
                      uint64_t(capacity) * (rays / 2) * GI_FACE_COUNT * GI_COMPACT_HIT_BYTES);
        }
        previous = peak;
    }
}

TEST(DynamicVoxelGIPlanning, FilterSettingsAndHistoryResourcesAreExplicit)
{
    uint32_t mode = 0;
    for (const char* temporal : {"off", "fixed", "elapsed"})
    {
        std::istringstream stream(std::string("dynamic_voxel_gi_temporal_filter=") + temporal +
                                  "\ndynamic_voxel_gi_spatial_filter=false");
        platform::ConfigLoader config(stream);
        DynamicVoxelGISettings settings;
        EXPECT_TRUE(LoadDynamicVoxelGISettings(config, settings));
        EXPECT_EQ(static_cast<uint32_t>(settings.temporal), mode++);
        EXPECT_FALSE(settings.spatialFilter);
    }
    // 24 half-float faces, two sky caches, 12 float histories, two uint4 histories,
    // two sender position grids and six uint grids.
    EXPECT_EQ(GI_FRAME_CELL_BYTES, 26 * 8 + 12 * 16 + 4 * 16 + 6 * 4);
    EXPECT_EQ(GI_FRAME_FIXED_BYTES, 3 * sizeof(glm::uvec4));
    RHIGPUInfo gpu;
    gpu.maxStorageBufferRange = 64u * 64 * 64 * 16;
    uint64_t peak             = 0;
    uint32_t capacity         = 0;
    EXPECT_EQ(PlanStaticVoxelGIResources(64, false, UINT64_MAX, gpu, capacity, peak),
              GIResourceStatus::eSuccess);
    --gpu.maxStorageBufferRange;
    EXPECT_EQ(PlanStaticVoxelGIResources(64, false, UINT64_MAX, gpu, capacity, peak),
              GIResourceStatus::eDescriptorRange);
}

TEST(DynamicVoxelGIPlanning, StorageBoundariesAreIndependentOfTheTotalBudget)
{
    RHIGPUInfo gpu;
    gpu.maxStorageBufferRange = 1024;
    uint64_t bytes            = 0;
    EXPECT_EQ(ValidateGIStorageBuffer(128, 8, gpu, bytes), GIResourceStatus::eSuccess);
    EXPECT_EQ(bytes, 1024u);
    EXPECT_EQ(ValidateGIStorageBuffer(129, 8, gpu, bytes), GIResourceStatus::eDescriptorRange);
    gpu.maxStorageBufferRange = std::numeric_limits<uint32_t>::max();
    EXPECT_EQ(ValidateGIStorageBuffer(UINT32_MAX, 1, gpu, bytes), GIResourceStatus::eSuccess);
    EXPECT_EQ(ValidateGIStorageBuffer(uint64_t(UINT32_MAX) + 1, 1, gpu, bytes),
              GIResourceStatus::eBufferSize);
    EXPECT_EQ(ValidateGIStorageBuffer(UINT64_MAX, 2, gpu, bytes), GIResourceStatus::eOverflow);
    EXPECT_EQ(ValidateGIStorageBuffer(1, 0, gpu, bytes), GIResourceStatus::eInvalidInput);
}

TEST(DynamicVoxelGIPlanning, VisibilityLayoutAndTransitionPeakAreCounted)
{
    RHIGPUInfo gpu;
    DynamicVoxelGIResourceRequest request;
    request.settings.memoryBudgetBytes        = UINT64_MAX;
    request.staticCapacity                    = 10;
    request.dynamicCapacity                   = 20;
    const DynamicVoxelGIResourceEstimate base = EstimateDynamicVoxelGIResources(request, gpu);
    EXPECT_EQ(base.status, GIResourceStatus::eSuccess);
    uint64_t visibility = 0;
    for (const GIResourceEstimate& resource : base.resources)
    {
        if (std::string(resource.name).find("visibility") != std::string::npos)
        {
            visibility += resource.bytesPerAllocation * resource.copies;
        }
    }
    EXPECT_EQ(visibility, 12288u * 10u + 6144u * 20u);
    request.coneTransition                          = true;
    request.averagedReflectance                     = true;
    request.retiringBytes                           = 12345;
    const DynamicVoxelGIResourceEstimate transition = EstimateDynamicVoxelGIResources(request, gpu);
    EXPECT_EQ(transition.status, GIResourceStatus::eSuccess);
    EXPECT_GT(transition.peakBytes, base.peakBytes + request.retiringBytes);
    request.settings.memoryBudgetBytes = transition.peakBytes;
    EXPECT_EQ(EstimateDynamicVoxelGIResources(request, gpu).status, GIResourceStatus::eSuccess);
    --request.settings.memoryBudgetBytes;
    EXPECT_EQ(EstimateDynamicVoxelGIResources(request, gpu).status, GIResourceStatus::eBudget);
    request.settings.memoryBudgetBytes = UINT64_MAX;
    request.retiringBytes              = UINT64_MAX;
    EXPECT_EQ(EstimateDynamicVoxelGIResources(request, gpu).status, GIResourceStatus::eOverflow);
}

TEST(DynamicVoxelGIPlanning, RejectsUnavailableOrUnrepresentableLayoutsWithoutAllocation)
{
    RHIGPUInfo gpu;
    DynamicVoxelGIResourceRequest request;
    request.settings.memoryBudgetBytes = UINT64_MAX;
    request.settings.resolution        = 256;
    request.staticCapacity             = 699050;
    gpu.maxStorageBufferRange          = UINT32_MAX;
    EXPECT_EQ(EstimateDynamicVoxelGIResources(request, gpu).status, GIResourceStatus::eSuccess);
    ++request.staticCapacity;
    const DynamicVoxelGIResourceEstimate oversized = EstimateDynamicVoxelGIResources(request, gpu);
    EXPECT_EQ(oversized.status, GIResourceStatus::eBufferSize);
    EXPECT_STREQ(oversized.rejectedResource, "static_visibility");
    request.staticCapacity                          = 0;
    request.averagedReflectance                     = true;
    gpu.maxStorageBufferRange                       = 128u * 1024u * 1024u;
    const DynamicVoxelGIResourceEstimate descriptor = EstimateDynamicVoxelGIResources(request, gpu);
    EXPECT_EQ(descriptor.status, GIResourceStatus::eDescriptorRange);
    EXPECT_STREQ(descriptor.rejectedResource, "reflectance_accumulation");
    request.settings.backend = VoxelGIQueryBackend::eHardwareRT;
    EXPECT_EQ(EstimateDynamicVoxelGIResources(request, gpu).status,
              GIResourceStatus::eUnavailableBackend);
    request.settings.backend           = VoxelGIQueryBackend::eVoxelDDA;
    request.settings.memoryBudgetBytes = 0;
    EXPECT_EQ(EstimateDynamicVoxelGIResources(request, gpu).status,
              GIResourceStatus::eInvalidInput);
    request.settings.memoryBudgetBytes = UINT64_MAX;
    request.settings.resolution        = UINT32_MAX;
    EXPECT_EQ(EstimateDynamicVoxelGIResources(request, gpu).status,
              GIResourceStatus::eInvalidInput);
    request.settings.resolution = 64;
    request.dynamicCapacity     = 64u * 64 * 64 + 1;
    EXPECT_EQ(EstimateDynamicVoxelGIResources(request, gpu).status,
              GIResourceStatus::eInvalidInput);
}

void CheckDispatchCoverage(uint32_t count, const RHIGPUInfo& gpu)
{
    HeapVector<uint32_t> visits(count, 0);
    uint32_t first = 0;
    bool valid     = true;
    do
    {
        ComputeDispatchChunk chunk;
        valid = BuildComputeDispatchChunk(first, count - first, 4, gpu, chunk);
        EXPECT_TRUE(valid);
        if (valid)
        {
            EXPECT_TRUE(gpu.IsDispatchWithinLimits(chunk.groups.x, chunk.groups.y, chunk.groups.z));
            for (uint32_t z = 0; z < chunk.groups.z; ++z)
            {
                for (uint32_t y = 0; y < chunk.groups.y; ++y)
                {
                    for (uint32_t x = 0; x < chunk.groups.x; ++x)
                    {
                        for (uint32_t lane = 0; lane < 4; ++lane)
                        {
                            const uint32_t local =
                                (x + chunk.groups.x * (y + chunk.groups.y * z)) * 4 + lane;
                            if (local < chunk.itemCount)
                            {
                                ++visits[chunk.firstItem + local];
                            }
                        }
                    }
                }
            }
            EXPECT_TRUE(chunk.itemCount > 0 || count == 0);
            first += chunk.itemCount;
            valid = valid && (chunk.itemCount > 0 || count == 0);
        }
    } while (valid && first < count);
    for (const uint32_t visitsPerItem : visits)
    {
        EXPECT_EQ(visitsPerItem, 1u);
    }
}

TEST(ComputeDispatchPlanning, CoversTailsMultidimensionalTilesAndChunksExactlyOnce)
{
    RHIGPUInfo gpu;
    gpu.maxComputeWorkGroupCount = {2, 3, 2};
    for (uint32_t count : {0u, 1u, 7u, 8u, 9u, 23u, 24u, 25u, 48u, 49u, 169u})
    {
        SCOPED_TRACE(count);
        CheckDispatchCoverage(count, gpu);
    }
}

TEST(ComputeDispatchPlanning, RejectsOverflowAndBoundsPaddedShaderIndices)
{
    RHIGPUInfo gpu;
    ComputeDispatchChunk chunk;
    EXPECT_FALSE(BuildComputeDispatchChunk(UINT32_MAX, 1, 64, gpu, chunk));
    EXPECT_FALSE(BuildComputeDispatchChunk(0, 1, 0, gpu, chunk));
    EXPECT_FALSE(
        BuildComputeDispatchChunk(0, 1, gpu.maxComputeWorkGroupInvocations + 1, gpu, chunk));
    EXPECT_TRUE(BuildComputeDispatchChunk(0, UINT32_MAX, 64, gpu, chunk));
    EXPECT_GT(chunk.itemCount, 0u);
    EXPECT_LE(uint64_t(chunk.groups.x) * chunk.groups.y * chunk.groups.z * 64, UINT32_MAX);
    gpu.maxComputeWorkGroupSize[0] = 32;
    EXPECT_FALSE(BuildComputeDispatchChunk(0, 1, 64, gpu, chunk));
    gpu.maxComputeWorkGroupSize[0]  = 128;
    gpu.maxComputeWorkGroupCount[1] = 0;
    EXPECT_FALSE(BuildComputeDispatchChunk(0, 1, 64, gpu, chunk));
}
} // namespace
