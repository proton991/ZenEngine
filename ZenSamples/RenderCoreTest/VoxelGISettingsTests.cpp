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
                              "voxel_gi_samples=3",
                              "voxel_gi_history_frames=0",
                              "voxel_gi_history_frames=257",
                              "voxel_gi_temporal=1",
                              "voxel_gi_filter=invalid",
                              "voxel_gi_ray_provider=invalid",
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

TEST(VoxelGIRuntimeSettings, LoadsHybridControlsTransactionally)
{
    std::istringstream     stream("voxel_gi_samples=4\nvoxel_gi_history_frames=64\nvoxel_gi_ray_provider=hardware\n"
                                  "voxel_gi_temporal=false\nvoxel_gi_filter=false\nvoxel_gi_specular_occlusion=false\n");
    platform::ConfigLoader config(stream);
    VoxelGIRuntimeSettings settings;
    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings));
    EXPECT_EQ(settings.cone.samples, 4u);
    EXPECT_EQ(settings.cone.historyFrames, 64u);
    EXPECT_EQ(settings.cone.rayProvider, VoxelGISettings::RayProvider::Hardware);
    EXPECT_FALSE(settings.cone.temporal);
    EXPECT_FALSE(settings.cone.filter);
    EXPECT_FALSE(settings.cone.specularOcclusion);
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

TEST(VoxelGIVisibilityBounds, PadsClosedCellContactsAndUsesGridCoordinates)
{
    const VoxelGIVisibilityBounds bounds =
        BuildVoxelGIVisibilityBounds(sg::AABB(Vec3(2, 4, 6), Vec3(10, 8, 14)), Vec4(-2, -2, -2, 2), 64);

    EXPECT_EQ(bounds.minimum, Vec4(1, 2, 3, 0));

    EXPECT_EQ(bounds.maximum, Vec4(8, 7, 10, 0));

    const VoxelGIVisibilityBounds expanded =
        BuildVoxelGIVisibilityBounds(sg::AABB(Vec3(-100), Vec3(200)), Vec4(-2, -2, -2, 2), 64);

    EXPECT_EQ(expanded.minimum, Vec4(0));

    EXPECT_EQ(expanded.maximum, Vec4(64, 64, 64, 0));
}

TEST(VoxelGIVisibilityBounds, FollowsGeometryOutsideTheOriginalSceneBox)
{
    const Vec4 grid(-32, -32, -32, 1);

    const VoxelGIVisibilityBounds before = BuildVoxelGIVisibilityBounds(sg::AABB(Vec3(-4), Vec3(4)), grid, 64);

    const VoxelGIVisibilityBounds after  = BuildVoxelGIVisibilityBounds(sg::AABB(Vec3(-4), Vec3(4, 16, 4)), grid, 64);

    EXPECT_EQ(before.maximum, Vec4(38, 38, 38, 0));

    EXPECT_EQ(after.minimum, before.minimum);

    EXPECT_EQ(after.maximum, Vec4(38, 50, 38, 0));
}

TEST(VoxelGIVisibilityBounds, InvalidBoundsRetainFullGridTraversal)
{
    const sg::AABB invalid[] = {sg::AABB(), sg::AABB(Vec3(0), Vec3(std::numeric_limits<float>::infinity()))};

    for (const sg::AABB& box : invalid)
    {
        const VoxelGIVisibilityBounds bounds = BuildVoxelGIVisibilityBounds(box, Vec4(0, 0, 0, 1), 128);

        EXPECT_EQ(bounds.minimum, Vec4(0));

        EXPECT_EQ(bounds.maximum, Vec4(128, 128, 128, 0));
    }
}
} // namespace
