#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "Graphics/RenderCore/V2/ComputeDispatch.h"
#include "Graphics/Shared/VoxelGI.h"
#include <gtest/gtest.h>
#include <limits>

namespace
{
using namespace zen;

using namespace zen::rc;

void CheckDispatchCoverage(uint32_t count, const RHIGPUInfo& gpu)
{
    HeapVector<uint32_t> visits(count, 0);

    uint32_t first = 0;

    bool valid = true;

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

TEST(VoxelReflectancePlanning, ChecksCountRangeBudgetAndRetirementBeforeAllocation)
{
    RHIGPUInfo gpu;

    gpu.maxStorageBufferRange = std::numeric_limits<uint32_t>::max();

    uint64_t peak = 0;

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

TEST(VoxelReflectancePlanning, ReportsMinimumAtEverySupportedResolution)
{
    EXPECT_EQ(GetVoxelReflectanceRequiredBytes(64), 5ull * 1024 * 1024);

    EXPECT_EQ(GetVoxelReflectanceRequiredBytes(128), 40ull * 1024 * 1024);

    EXPECT_EQ(GetVoxelReflectanceRequiredBytes(256), 320ull * 1024 * 1024);

    EXPECT_EQ(GetVoxelReflectanceRequiredBytes(0), 0u);

    EXPECT_EQ(GetVoxelReflectanceRequiredBytes(UINT32_MAX), 0u);
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

    gpu.maxComputeWorkGroupSize[0] = 128;

    gpu.maxComputeWorkGroupCount[1] = 0;

    EXPECT_FALSE(BuildComputeDispatchChunk(0, 1, 64, gpu, chunk));
}
} // namespace
