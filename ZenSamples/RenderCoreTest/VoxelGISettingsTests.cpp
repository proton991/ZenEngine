#include "Graphics/RenderCore/V2/VoxelGISettings.h"
#include <gtest/gtest.h>
#include <limits>
#include <sstream>

namespace
{
using namespace zen;
using namespace zen::rc;

TEST(VoxelGIRuntimeSettings, ReloadRejectsInvalidValuesWithoutPartiallyApplying)
{
    const char* rejected[] = {"voxelizer=invalid",
                              "async_compute=invalid",
                              "voxel_resolution=65",
                              "voxel_gi_method=invalid",
                              "voxel_gi_cone_count=5",
                              "shadow_map_resolution=0",
                              "dynamic_voxel_gi_rays_per_face=16",
                              "dynamic_voxel_gi_neighbor_radius=3",
                              "dynamic_voxel_gi_cache=invalid",
                              "dynamic_voxel_gi_query_backend=invalid",
                              "dynamic_voxel_gi_memory_budget_mb=0",
                              "voxel_reflectance_policy=invalid",
                              "voxel_reflectance_policy=averaged",
                              "voxel_reflectance_budget_mb=18446744073709551615",
                              "dynamic_voxel_gi_temporal_alpha=0",
                              "dynamic_voxel_gi_temporal_alpha=1.1",
                              "dynamic_voxel_gi_history_gap_seconds=-1",
                              "dynamic_voxel_gi_temporal_reference_hz=nan",
                              "dynamic_voxel_gi_cache_batch_size=0",
                              "dynamic_voxel_gi_cache_batch_size=4097"};

    for (const char* input : rejected)
    {
        SCOPED_TRACE(input);

        std::istringstream stream(std::string("voxel_gi_indirect_intensity=2\n") + input);

        platform::ConfigLoader config(stream);

        VoxelGIRuntimeSettings settings;

        settings.dynamic.resolution = 128;

        EXPECT_FALSE(LoadVoxelGIRuntimeSettings(config, settings));

        EXPECT_EQ(settings.dynamic.resolution, 128u);

        EXPECT_EQ(settings.cone.indirectIntensity, 1.0f);
    }
}

TEST(VoxelGIRuntimeSettings, LoadsResourceAndLiveSettingsTogether)
{
    std::istringstream stream(
        "voxelizer=comp\nasync_compute=auto\nvoxel_gi_method=dynamic_voxel\n"
        "voxel_resolution=128\ndynamic_voxel_gi_rays_per_face=32\n"
        "dynamic_voxel_gi_neighbor_radius=1\ndynamic_voxel_gi_cache=decoded\n"
        "dynamic_voxel_gi_memory_budget_mb=3072\nvoxel_reflectance_policy=averaged\n"
        "voxel_reflectance_budget_mb=128\nshadow_map_resolution=512\n"
        "dynamic_voxel_gi_temporal_alpha=0.1\ndynamic_voxel_gi_history_gap_seconds=0.5\n"
        "dynamic_voxel_gi_temporal_reference_hz=30\ndynamic_voxel_gi_cache_batch_size=512\n");

    platform::ConfigLoader config(stream);

    VoxelGIRuntimeSettings settings;

    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings));

    EXPECT_EQ(settings.voxelizer, platform::VoxelizerMode::eCompute);

    EXPECT_EQ(settings.asyncCompute, platform::AsyncComputeMode::eAuto);

    EXPECT_EQ(settings.dynamic.resolution, 128u);

    EXPECT_EQ(settings.dynamic.raysPerFace, 32u);

    EXPECT_EQ(settings.dynamic.neighborRadius, 1u);

    EXPECT_FALSE(settings.dynamic.compactCache);

    EXPECT_EQ(settings.dynamic.memoryBudgetBytes, 3072ull * 1024 * 1024);

    EXPECT_TRUE(settings.averagedReflectance);

    EXPECT_EQ(settings.reflectanceBudgetBytes, 128ull * 1024 * 1024);

    EXPECT_EQ(settings.shadowMapResolution, 512u);

    EXPECT_FLOAT_EQ(settings.dynamic.temporalAlpha, 0.1f);

    EXPECT_FLOAT_EQ(settings.dynamic.historyGapSeconds, 0.5f);

    EXPECT_FLOAT_EQ(settings.dynamic.temporalReferenceHz, 30.0f);

    EXPECT_EQ(settings.dynamic.cacheBatchSize, 512u);
}

TEST(VoxelGIRuntimeSettings, RejectsInvalidEnumsAndNonFiniteAPIValues)
{
    VoxelGIRuntimeSettings settings;

    settings.voxelizer = static_cast<platform::VoxelizerMode>(99);

    EXPECT_FALSE(ValidateVoxelGIRuntimeSettings(settings));

    settings = {};

    settings.dynamic.backend = static_cast<VoxelGIQueryBackend>(99);

    EXPECT_FALSE(ValidateVoxelGIRuntimeSettings(settings));

    settings = {};

    settings.dynamic.temporalAlpha = std::numeric_limits<float>::infinity();

    EXPECT_FALSE(ValidateVoxelGIRuntimeSettings(settings));
}
} // namespace
