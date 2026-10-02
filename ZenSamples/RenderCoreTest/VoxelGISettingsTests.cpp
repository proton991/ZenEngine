#include "Graphics/RenderCore/V2/VoxelGISettings.h"
#include "Platform/ConfigLoader.h"
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
                              "voxel_gi_cone_count=5",
                              "shadow_map_resolution=0",
                              "voxel_reflectance_policy=invalid",
                              "voxel_reflectance_policy=averaged",
                              "voxel_reflectance_budget_mb=18446744073709551615",
                              "voxel_gi_analytic_lighting=invalid",
                              "voxel_gi_environment_lighting=1",
                              "voxel_gi_emissive_lighting=0"};

    for (const char* input : rejected)
    {
        SCOPED_TRACE(input);

        std::istringstream stream(std::string("voxel_gi_indirect_intensity=2\n") + input);

        platform::ConfigLoader config(stream);

        VoxelGIRuntimeSettings settings;

        settings.resolution = 128;

        EXPECT_FALSE(LoadVoxelGIRuntimeSettings(config, settings));

        EXPECT_EQ(settings.resolution, 128u);

        EXPECT_EQ(settings.cone.indirectIntensity, 1.0f);
    }
}

TEST(VoxelGIRuntimeSettings, LoadsResourceAndLiveSettingsTogether)
{
    std::istringstream stream("voxelizer=comp\nasync_compute=auto\nvoxel_resolution=128\n"
                              "voxel_reflectance_policy=averaged\nvoxel_reflectance_budget_mb=128\n"
                              "shadow_map_resolution=512\nvoxel_gi_analytic_lighting=false\n"
                              "voxel_gi_environment_lighting=false\nvoxel_gi_emissive_lighting=false\n");

    platform::ConfigLoader config(stream);

    VoxelGIRuntimeSettings settings;

    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings));

    EXPECT_EQ(settings.voxelizer, platform::VoxelizerMode::eCompute);

    EXPECT_EQ(settings.asyncCompute, platform::AsyncComputeMode::eAuto);

    EXPECT_EQ(settings.resolution, 128u);

    EXPECT_TRUE(settings.averagedReflectance);

    EXPECT_EQ(settings.reflectanceBudgetBytes, 128ull * 1024 * 1024);

    EXPECT_EQ(settings.shadowMapResolution, 512u);

    EXPECT_FALSE(settings.cone.analyticLighting);

    EXPECT_FALSE(settings.cone.environmentLighting);

    EXPECT_FALSE(settings.cone.emissiveLighting);
}

TEST(VoxelGIRuntimeSettings, RejectsInvalidEnumsAndNonFiniteAPIValues)
{
    VoxelGIRuntimeSettings settings;

    settings.voxelizer = static_cast<platform::VoxelizerMode>(99);

    EXPECT_FALSE(ValidateVoxelGIRuntimeSettings(settings));

    settings              = {};

    settings.asyncCompute = static_cast<platform::AsyncComputeMode>(99);

    EXPECT_FALSE(ValidateVoxelGIRuntimeSettings(settings));

    settings                        = {};

    settings.cone.indirectIntensity = std::numeric_limits<float>::infinity();

    EXPECT_FALSE(ValidateVoxelGIRuntimeSettings(settings));
}

TEST(VoxelGIRuntimeSettings, ContributionChangesAreLiveAndResolutionRequiresRebuild)
{
    VoxelGIRuntimeSettings previous;

    VoxelGIRuntimeSettings next   = previous;

    next.cone.analyticLighting    = false;

    next.cone.environmentLighting = false;

    next.cone.emissiveLighting    = false;

    EXPECT_FALSE(RequiresVoxelGIRebuild(previous, next));

    next.resolution = 64;

    EXPECT_TRUE(RequiresVoxelGIRebuild(previous, next));
}

TEST(VoxelGIRuntimeSettings, InactiveReflectanceBudgetChangesDoNotRebuild)
{
    VoxelGIRuntimeSettings previous;

    VoxelGIRuntimeSettings next = previous;

    next.reflectanceBudgetBytes = 320ull * 1024 * 1024;

    EXPECT_FALSE(RequiresVoxelGIRebuild(previous, next));

    next.averagedReflectance = true;

    EXPECT_TRUE(RequiresVoxelGIRebuild(previous, next));

    previous                     = next;

    next.reflectanceBudgetBytes *= 2;

    EXPECT_TRUE(RequiresVoxelGIRebuild(previous, next));
}
} // namespace
