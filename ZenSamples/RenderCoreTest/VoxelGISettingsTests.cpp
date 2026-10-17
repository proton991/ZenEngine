#include "Graphics/RenderCore/V2/VoxelGISettings.h"
#include "Platform/ConfigLoader.h"
#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#include <tuple>

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
                              "voxel_gi_adaptive_sampling=1",
                              "voxel_gi_history_frames=0",
                              "voxel_gi_history_frames=257",
                              "voxel_gi_temporal=1",
                              "voxel_gi_filter=invalid",
                              "voxel_gi_ray_provider=invalid",
                              "voxel_gi_ray_provider=legacy",
                              "voxel_gi_rt_shadows=invalid",
                              "voxel_gi_backend=auto",
                              "voxel_gi_sky_backend=auto",
                              "voxel_gi_shadows_backend=invalid",
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

TEST(VoxelGIRuntimeSettings, ExplicitBackendsOverrideLegacyControlsAndRemainIndependent)
{
    std::istringstream     stream("voxel_gi_backend=compute\nvoxel_gi_sky_backend=hardware\n"
                                  "voxel_gi_reflections_backend=hardware\nvoxel_gi_bounce_source=rays\n"
                                  "voxel_gi_rt_shadows=false\nvoxel_gi_reflections=false\n");
    platform::ConfigLoader config(stream);
    VoxelGIRuntimeSettings settings;
    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings));
    EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 17u);
    EXPECT_EQ(settings.cone.bounceSource, VoxelGISettings::BounceSource::Auto);
    EXPECT_EQ(settings.cone.reflections, VoxelGISettings::Reflections::On);
    EXPECT_TRUE(settings.cone.rtShadows);
    const VoxelGIRuntimeSettings previous = settings;
    settings.cone.SetFeatureProvider(VoxelGISettings::Feature::Shadows, VoxelGISettings::RayProvider::Hardware);
    EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 49u);
    EXPECT_FALSE(RequiresVoxelGIRebuild(previous, settings));
    settings.cone.SetAllFeatureProviders(VoxelGISettings::RayProvider::Voxel);
    EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 0u);
    settings.cone.SetAllFeatureProviders(VoxelGISettings::RayProvider::Hardware);
    EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 63u);
    settings.cone.featureProviders[0] = static_cast<VoxelGISettings::RayProvider>(99);
    EXPECT_FALSE(ValidateVoxelGISettings(settings.cone));
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
                                  "voxel_gi_adaptive_sampling=false\n"
                                  "voxel_gi_temporal=false\nvoxel_gi_filter=false\nvoxel_gi_specular_occlusion=false\n"
                                  "voxel_gi_reflections=false\nvoxel_gi_rt_shadows=false\n");
    platform::ConfigLoader config(stream);
    VoxelGIRuntimeSettings settings;
    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings));
    EXPECT_EQ(settings.cone.samples, 4u);
    EXPECT_FALSE(settings.cone.adaptiveSampling);
    EXPECT_FALSE(settings.cone.UsesAdaptiveSampling());
    EXPECT_FALSE(settings.cone.UsesAdaptiveSampling());
    EXPECT_EQ(settings.cone.historyFrames, 64u);
    EXPECT_EQ(settings.cone.rayProvider, VoxelGISettings::RayProvider::Hardware);
    EXPECT_FALSE(settings.cone.temporal);
    EXPECT_FALSE(settings.cone.filter);
    EXPECT_FALSE(settings.cone.specularOcclusion);
    EXPECT_EQ(settings.cone.reflections, VoxelGISettings::Reflections::Off);
    EXPECT_FALSE(settings.cone.rtShadows);
    // Ray-hit bounce with hardware queries, cones otherwise (P5 passed on the RT tier only).
    EXPECT_EQ(settings.cone.bounceSource, VoxelGISettings::BounceSource::Auto);
    std::istringstream     bounceStream("voxel_gi_bounce_source=rays\n");
    platform::ConfigLoader bounceConfig(bounceStream);
    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(bounceConfig, settings));
    EXPECT_EQ(settings.cone.bounceSource, VoxelGISettings::BounceSource::Rays);
    EXPECT_TRUE(settings.cone.rtShadows);
    // Explicit backends support reflections on both providers.
    EXPECT_EQ(settings.cone.reflections, VoxelGISettings::Reflections::On);
    std::istringstream     invalidStream("voxel_gi_bounce_source=invalid\n");
    platform::ConfigLoader invalidConfig(invalidStream);
    EXPECT_FALSE(LoadVoxelGIRuntimeSettings(invalidConfig, settings));
    EXPECT_EQ(settings.cone.bounceSource, VoxelGISettings::BounceSource::Rays);
}

TEST(VoxelGIRuntimeSettings, AdaptiveBudgetIsIndependentOfTraversalBackend)
{
    VoxelGISettings settings;
    // Both backends sample the same triangle visibility distribution.
    settings.SetAllFeatureProviders(VoxelGISettings::RayProvider::Hardware);
    EXPECT_TRUE(settings.UsesAdaptiveSampling());
    settings.referenceSamples = 1024;
    EXPECT_FALSE(settings.UsesAdaptiveSampling());
    settings.referenceSamples = 0;
    settings.samples          = 2;
    EXPECT_FALSE(settings.UsesAdaptiveSampling());
}

TEST(VoxelGIRuntimeSettings, ReferenceSamplesAcceptOnlyTheDiagnosticBudgets)
{
    // 256 x 256 frames splits the 1024 x 64 tier reference into frames below the GPU watchdog.
    for (uint32_t samples : {0u, 256u, 1024u, 4096u})
    {
        VoxelGISettings settings;
        settings.referenceSamples = samples;
        EXPECT_TRUE(ValidateVoxelGISettings(settings)) << samples;
        EXPECT_EQ(settings.UsesAdaptiveSampling(), samples == 0) << samples;
    }
    for (uint32_t samples : {1u, 128u, 512u, 2048u})
    {
        VoxelGISettings settings;
        settings.referenceSamples = samples;
        EXPECT_FALSE(ValidateVoxelGISettings(settings)) << samples;
    }
}

TEST(VoxelGIRuntimeSettings, DiffuseResolutionDefaultsToFullAndRejectsUnknownNames)
{
    EXPECT_EQ(VoxelGISettings{}.diffuseResolution, VoxelGISettings::DiffuseResolution::Full);
    const std::pair<const char*, VoxelGISettings::DiffuseResolution> accepted[] = {
        {"auto", VoxelGISettings::DiffuseResolution::Auto},
        {"full", VoxelGISettings::DiffuseResolution::Full},
        {"half", VoxelGISettings::DiffuseResolution::Half}};
    for (const auto& [name, resolution] : accepted)
    {
        std::istringstream     stream(std::string("voxel_gi_diffuse_resolution=") + name + "\n");
        platform::ConfigLoader config(stream);
        VoxelGIRuntimeSettings settings;
        ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings)) << name;
        EXPECT_EQ(settings.cone.diffuseResolution, resolution) << name;
    }
    std::istringstream     stream("voxel_gi_diffuse_resolution=quarter\n");
    platform::ConfigLoader config(stream);
    VoxelGIRuntimeSettings settings;
    settings.cone.diffuseResolution = VoxelGISettings::DiffuseResolution::Half;
    EXPECT_FALSE(LoadVoxelGIRuntimeSettings(config, settings));
    EXPECT_EQ(settings.cone.diffuseResolution, VoxelGISettings::DiffuseResolution::Half);
    VoxelGISettings invalid;
    invalid.diffuseResolution = static_cast<VoxelGISettings::DiffuseResolution>(3);
    EXPECT_FALSE(ValidateVoxelGISettings(invalid));
}

TEST(VoxelGIRuntimeSettings, PresetsAllowExplicitOverridesAndRejectInvalidNamesTransactionally)
{
    VoxelGIRuntimeSettings settings;
    for (uint32_t index = 1; index <= 4; ++index)
    {
        const VoxelGIPreset    preset = static_cast<VoxelGIPreset>(index);
        std::istringstream     stream(std::string("voxel_gi_preset=") + GetVoxelGIPresetName(preset));
        platform::ConfigLoader config(stream);
        ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings));
        EXPECT_EQ(GetVoxelGIPreset(settings), preset);
    }
    std::istringstream     overrides("voxel_gi_preset=ultra\nvoxel_resolution=64\nvoxel_gi_ray_provider=voxel\n");
    platform::ConfigLoader overrideConfig(overrides);
    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(overrideConfig, settings));
    EXPECT_EQ(GetVoxelGIPreset(settings), VoxelGIPreset::Medium);
    EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 0u);
    const VoxelGIRuntimeSettings previous = settings;
    std::istringstream           invalid("voxel_gi_preset=invalid\n");
    platform::ConfigLoader       invalidConfig(invalid);
    EXPECT_FALSE(LoadVoxelGIRuntimeSettings(invalidConfig, settings));
    EXPECT_EQ(settings, previous);
    EXPECT_FALSE(ApplyVoxelGIPreset(VoxelGIPreset::Custom, settings));
    EXPECT_EQ(settings, previous);
    // The name reported for unmatched settings loads: no preset baseline, explicit keys still apply.
    std::istringstream     custom(std::string("voxel_gi_preset=") + GetVoxelGIPresetName(VoxelGIPreset::Custom)
                                  + "\nvoxel_resolution=128\nvoxel_gi_samples=2\n");
    platform::ConfigLoader customConfig(custom);
    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(customConfig, settings));
    EXPECT_EQ(settings.resolution, 128u);
    EXPECT_EQ(settings.cone.samples, 2u);
    EXPECT_EQ(GetVoxelGIPreset(settings), VoxelGIPreset::Custom);
    std::istringstream     unspecified("voxel_gi_preset=custom\n");
    platform::ConfigLoader unspecifiedConfig(unspecified);
    VoxelGIRuntimeSettings defaults;
    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(unspecifiedConfig, settings));
    EXPECT_EQ(settings.cone, defaults.cone);
}

TEST(VoxelGIRuntimeSettings, AutomaticProviderFollowsRayQueryCapabilityAndKeepsExplicitFeatures)
{
    VoxelGIRuntimeSettings settings;
    RHIGPUInfo             gpu;
    gpu.rayQuery = {true, true, true, true, true, 256};
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
    EXPECT_STREQ(ResolveVoxelGIAutomaticProvider(settings, gpu), "ray_query_available");
    EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 63u);
    // Device and driver identity never select the tier.
    for (const auto& [vendor, device, driver] :
         {std::tuple{0x10DEu, 0x2C02u, 0u}, std::tuple{0x1002u, 0x744Cu, 1u}, std::tuple{0x8086u, 0x56A0u, 0xFFFFFFFFu}})
    {
        gpu.vendorID         = vendor;
        gpu.deviceID         = device;
        gpu.driverVersionRaw = driver;
        ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
        EXPECT_STREQ(ResolveVoxelGIAutomaticProvider(settings, gpu), "ray_query_available");
        EXPECT_EQ(settings.cone.rayProvider, VoxelGISettings::RayProvider::Hardware);
    }
    // Inherited Auto is resolved for any configuration, not only the Medium preset.
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
    settings.resolution = 128;
    EXPECT_STREQ(ResolveVoxelGIAutomaticProvider(settings, gpu), "ray_query_available");
    EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 63u);
    // Each missing capability selects Compute; explicit per-feature requests survive.
    for (bool RHIRayQueryCapabilities::* feature :
         {&RHIRayQueryCapabilities::bufferDeviceAddress, &RHIRayQueryCapabilities::accelerationStructure,
          &RHIRayQueryCapabilities::rayQuery, &RHIRayQueryCapabilities::graphicsBuild,
          &RHIRayQueryCapabilities::triangleFloat3})
    {
        gpu.rayQuery          = {true, true, true, true, true, 256};
        gpu.rayQuery.*feature = false;
        ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
        settings.cone.SetFeatureProvider(VoxelGISettings::Feature::Shadows, VoxelGISettings::RayProvider::Hardware);
        EXPECT_STREQ(ResolveVoxelGIAutomaticProvider(settings, gpu), "ray_query_unavailable");
        EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 32u);
    }
    gpu.rayQuery = {true, true, true, true, true, 0};
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
    EXPECT_STREQ(ResolveVoxelGIAutomaticProvider(settings, gpu), "ray_query_unavailable");
    EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 0u);
    // Explicit backends are never re-resolved.
    gpu.rayQuery = {true, true, true, true, true, 256};
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Low, settings));
    EXPECT_STREQ(ResolveVoxelGIAutomaticProvider(settings, gpu), "explicit_provider");
    EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 0u);
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
