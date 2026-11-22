#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "Graphics/RenderCore/V2/ComputeDispatch.h"
#include "Graphics/Shared/VoxelGI.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <limits>

namespace
{
using namespace zen;

using namespace zen::rc;

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
                            const uint32_t local = (x + chunk.groups.x * (y + chunk.groups.y * z)) * 4 + lane;

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

            valid  = valid && (chunk.itemCount > 0 || count == 0);
        }
    } while (valid && first < count);

    for (const uint32_t visitsPerItem : visits)
    {
        EXPECT_EQ(visitsPerItem, 1u);
    }
}

struct FieldPacingRun
{
    uint64_t processed{0};
    uint32_t skipped{0};
    uint32_t frames{0};
    uint32_t minimumBatchCap{UINT32_MAX};
};

// Drives one generation's pacing with a modeled frame: the render-only interval plus the construction
// cost submitted three frames earlier (frames in flight), smoothed as DistanceFieldScene smooths it.
FieldPacingRun RunFieldPacing(FieldThroughputPacing&       pacing,
                              const FieldThroughputLimits& limits,
                              double                       renderUs,
                              double                       triangleUs,
                              uint32_t                     maxFrames)
{
    FieldPacingRun run;
    double         costs[3]  = {0.0, 0.0, 0.0};
    double         interval  = renderUs;
    bool           submitted = false;
    while (run.processed < limits.triangleCount && run.frames < maxFrames)
    {
        interval = 0.5 * (interval + renderUs + costs[2]);
        costs[2] = costs[1];
        costs[1] = costs[0];
        costs[0] = 0.0;
        if (PaceFieldThroughput(pacing, limits, interval, submitted))
        {
            const uint64_t work =
                std::min<uint64_t>(limits.triangleCount - run.processed, uint64_t(pacing.batchCap) * pacing.cycles);
            run.processed       += work;
            costs[0]             = triangleUs * static_cast<double>(work);
            submitted            = true;
            run.minimumBatchCap  = std::min(run.minimumBatchCap, pacing.batchCap);
        }
        else
        {
            ++run.skipped;
        }
        ++run.frames;
    }
    return run;
}

FieldThroughputLimits NewSponzaFieldLimits()
{
    // VoxelGIRenderer's construction settings and the recorded 215,361-triangle local generation.
    FieldThroughputLimits limits;
    limits.budgetUs      = 16000.0;
    limits.budgetShare   = 0.25;
    limits.maxCycles     = 4;
    limits.batchCeiling  = 1u << 20;
    limits.triangleCount = 215361;
    limits.probeFrames   = 8;
    return limits;
}

TEST(FieldThroughputPacing, AbsoluteBudgetAloneStarvesSlowFrames)
{
    // Compute GI frames at 4K measured 83-85 ms; with slow rendering, the 16 ms budget alone shrinks
    // the batch cap to one triangle and the generation never publishes.
    FieldThroughputLimits limits = NewSponzaFieldLimits();
    limits.budgetShare           = 0.0;
    FieldThroughputPacing pacing;
    pacing.batchCap = 65536;
    StartFieldThroughputGeneration(pacing);
    const FieldPacingRun run = RunFieldPacing(pacing, limits, 83000.0, 0.25, 1024);
    EXPECT_LT(run.processed, limits.triangleCount);
    EXPECT_EQ(pacing.batchCap, 1u);
    EXPECT_EQ(run.skipped, 0u);
}

TEST(FieldThroughputPacing, SlowRenderingMeasuresItsIntervalAndFinishes)
{
    const FieldThroughputLimits limits = NewSponzaFieldLimits();
    FieldThroughputPacing       pacing;
    pacing.batchCap = 65536;
    StartFieldThroughputGeneration(pacing);
    const FieldPacingRun run = RunFieldPacing(pacing, limits, 83000.0, 0.25, 1024);
    EXPECT_EQ(run.processed, limits.triangleCount);
    EXPECT_LE(run.frames, 64u);
    // One probe per generation, which settles on the render-only interval.
    EXPECT_EQ(run.skipped, limits.probeFrames);
    EXPECT_NEAR(pacing.renderIntervalUs, 83000.0, 83000.0 * 0.05);
    EXPECT_TRUE(pacing.probed);
}

TEST(FieldThroughputPacing, FastRenderingGrowsWithoutProbing)
{
    const FieldThroughputLimits limits = NewSponzaFieldLimits();
    FieldThroughputPacing       pacing;
    pacing.batchCap = 65536;
    StartFieldThroughputGeneration(pacing);
    const FieldPacingRun run = RunFieldPacing(pacing, limits, 6000.0, 0.01, 1024);
    EXPECT_EQ(run.processed, limits.triangleCount);
    EXPECT_EQ(run.skipped, 0u);
    EXPECT_FALSE(pacing.probed);
    EXPECT_EQ(pacing.renderIntervalUs, 0.0);
    EXPECT_GT(pacing.batchCap, 65536u);
}

TEST(FieldThroughputPacing, ProbesOncePerGenerationAndKeepsTheBatchCap)
{
    // A generation too large to finish here, with construction far over the allowance: one probe,
    // then the batch cap shrinks toward the share left above the measured render-only interval.
    FieldThroughputLimits limits = NewSponzaFieldLimits();
    limits.triangleCount         = 50000000;
    FieldThroughputPacing pacing;
    pacing.batchCap = 65536;
    StartFieldThroughputGeneration(pacing);
    const FieldPacingRun first = RunFieldPacing(pacing, limits, 83000.0, 10.0, 256);
    EXPECT_EQ(first.frames, 256u);
    EXPECT_EQ(first.skipped, limits.probeFrames);
    EXPECT_GT(first.processed, 0u);
    EXPECT_LT(first.minimumBatchCap, 2048u);
    // Construction submitted before the probe can still be in flight: the measurement is an upper bound.
    EXPECT_GE(pacing.renderIntervalUs, 83000.0);
    EXPECT_LE(pacing.renderIntervalUs, 83000.0 * 1.15);
    const uint32_t retainedCap = pacing.batchCap;
    StartFieldThroughputGeneration(pacing);
    EXPECT_EQ(pacing.batchCap, retainedCap);
    EXPECT_EQ(pacing.cycles, 1u);
    EXPECT_FALSE(pacing.probed);
    const FieldPacingRun second = RunFieldPacing(pacing, limits, 120000.0, 10.0, 256);
    EXPECT_EQ(second.skipped, limits.probeFrames);
    EXPECT_GE(pacing.renderIntervalUs, 120000.0);
    EXPECT_LE(pacing.renderIntervalUs, 120000.0 * 1.15);
}

TEST(FieldThroughputPacing, UnsubmittedGenerationRunsOneCycle)
{
    const FieldThroughputLimits limits = NewSponzaFieldLimits();
    FieldThroughputPacing       pacing;
    pacing.batchCap = 4096;
    pacing.cycles   = 4;
    EXPECT_TRUE(PaceFieldThroughput(pacing, limits, 1.0e6, false));
    EXPECT_EQ(pacing.cycles, 1u);
    EXPECT_EQ(pacing.batchCap, 4096u);
    EXPECT_FALSE(pacing.probed);
}

TEST(VoxelReflectancePlanning, ChecksCountRangeBudgetAndRetirementBeforeAllocation)
{
    RHIGPUInfo gpu;

    gpu.maxStorageBufferRange   = std::numeric_limits<uint32_t>::max();

    uint64_t peak               = 0;

    constexpr uint64_t maxCount = std::numeric_limits<uint32_t>::max() / ZEN_VOXEL_REFLECTANCE_SCALE;

    constexpr uint64_t required = 64ull * 64 * 64 * 20;

    EXPECT_EQ(ValidateVoxelReflectanceResources(64, maxCount, required, 0, gpu, peak), GIResourceStatus::eSuccess);

    EXPECT_EQ(peak, required);

    EXPECT_EQ(ValidateVoxelReflectanceResources(64, maxCount + 1, required, 0, gpu, peak), GIResourceStatus::eOverflow);

    EXPECT_EQ(ValidateVoxelReflectanceResources(64, 0, required - 1, 0, gpu, peak), GIResourceStatus::eBudget);

    EXPECT_EQ(ValidateVoxelReflectanceResources(64, 0, required + 37, 37, gpu, peak), GIResourceStatus::eSuccess);

    EXPECT_EQ(peak, required + 37);

    EXPECT_EQ(ValidateVoxelReflectanceResources(64, 0, required + 36, 37, gpu, peak), GIResourceStatus::eBudget);

    EXPECT_EQ(ValidateVoxelReflectanceResources(64, 0, required, UINT64_MAX, gpu, peak), GIResourceStatus::eOverflow);

    EXPECT_EQ(ValidateVoxelReflectanceResources(64, 0, 0, 0, gpu, peak), GIResourceStatus::eInvalidInput);

    EXPECT_EQ(ValidateVoxelReflectanceResources(UINT32_MAX, 0, UINT64_MAX, 0, gpu, peak), GIResourceStatus::eInvalidInput);

    gpu.maxStorageBufferRange = 64u * 64 * 64 * 16;

    EXPECT_EQ(ValidateVoxelReflectanceResources(64, 0, required, 0, gpu, peak), GIResourceStatus::eSuccess);

    --gpu.maxStorageBufferRange;

    EXPECT_EQ(ValidateVoxelReflectanceResources(64, 0, required, 0, gpu, peak), GIResourceStatus::eDescriptorRange);

    gpu.maxStorageBufferRange = 128u * 1024 * 1024;

    EXPECT_EQ(ValidateVoxelReflectanceResources(256, 0, UINT64_MAX, 0, gpu, peak), GIResourceStatus::eDescriptorRange);
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

    EXPECT_FALSE(BuildComputeDispatchChunk(0, 1, gpu.maxComputeWorkGroupInvocations + 1, gpu, chunk));

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
