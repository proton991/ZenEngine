#include "Graphics/RenderCore/V2/VoxelGISettings.h"
#include "Platform/ConfigLoader.h"
#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#include <span>
#include <string>
#include <tuple>

namespace
{
using namespace zen;

using namespace zen::rc;

TEST(VoxelGIRuntimeSettings, ReloadRejectsInvalidValuesWithoutPartiallyApplying)
{
    const char* rejected[] = {"async_compute=invalid",
                              "voxel_resolution=65",
                              "voxel_gi_samples=3",
                              "voxel_gi_history_frames=0",
                              "voxel_gi_history_frames=257",
                              "voxel_gi_temporal=1",
                              "voxel_gi_filter=invalid",
                              "voxel_gi_ray_provider=invalid",
                              "voxel_gi_ray_provider=legacy",
                              "voxel_gi_rt_shadows=invalid",
                              "voxel_gi_backend=invalid",
                              "voxel_gi_restir_spatial_samples=0",
                              "voxel_gi_restir_spatial_samples=5",
                              "voxel_gi_compute_traversal=invalid",
                              "voxel_gi_field_resolution=0",
                              "voxel_gi_field_resolution=16",
                              "voxel_gi_field_resolution=96",
                              "voxel_gi_field_resolution=128",
                              "voxel_gi_pixel_resolution_divisor=0",
                              "voxel_gi_pixel_resolution_divisor=3",
                              "voxel_gi_pixel_resolution_divisor=8",
                              "voxel_gi_pixel_resolution_divisor=automatic",
                              "voxel_gi_method=restir",
                              "voxel_gi_tracing=hardware",
                              "voxel_gi_denoiser=relax",
                              "voxel_gi_shadows=true",
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

TEST(VoxelGIRuntimeSettings, TierChoiceIgnoresRemovedOverridesAndKeepsLightingEnables)
{
    std::istringstream     stream("voxel_gi_backend=compute\nvoxel_gi_sky_backend=hardware\n"
                                  "voxel_gi_reflections_backend=hardware\nvoxel_gi_shadows_backend=hardware\n"
                                  "voxel_gi_rt_shadows=false\nvoxel_gi_reflections=false\n");
    platform::ConfigLoader config(stream);
    VoxelGIRuntimeSettings settings;
    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings));
    EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 0u);
    EXPECT_EQ(settings.cone.reflections, VoxelGISettings::Reflections::Off);
    EXPECT_FALSE(settings.cone.rtShadows);
    const VoxelGIRuntimeSettings previous = settings;
    settings.cone.rayProvider             = VoxelGISettings::RayProvider::Hardware;
    EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 63u);
    EXPECT_FALSE(RequiresVoxelGIRebuild(previous, settings));
    settings.cone.rayProvider = static_cast<VoxelGISettings::RayProvider>(99);
    EXPECT_FALSE(ValidateVoxelGISettings(settings.cone));
}

TEST(VoxelGIRuntimeSettings, LoadsResourceAndLiveSettingsTogether)
{
    std::istringstream stream("async_compute=auto\nvoxel_resolution=128\n"
                              "voxel_reflectance_policy=averaged\nvoxel_reflectance_budget_mb=128\n"
                              "shadow_map_resolution=512\nvoxel_gi_analytic_lighting=false\n"
                              "voxel_gi_environment_lighting=false\nvoxel_gi_emissive_lighting=false\n");

    platform::ConfigLoader config(stream);

    VoxelGIRuntimeSettings settings;

    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings));


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
                                  "voxel_gi_temporal=false\nvoxel_gi_filter=false\nvoxel_gi_specular_occlusion=false\n"
                                  "voxel_gi_reflections=false\nvoxel_gi_rt_shadows=false\n");
    platform::ConfigLoader config(stream);
    VoxelGIRuntimeSettings settings;
    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings));
    EXPECT_EQ(settings.cone.samples, 4u);
    EXPECT_EQ(settings.cone.historyFrames, 64u);
    EXPECT_EQ(settings.cone.rayProvider, VoxelGISettings::RayProvider::Hardware);
    EXPECT_FALSE(settings.cone.temporal);
    EXPECT_FALSE(settings.cone.filter);
    EXPECT_FALSE(settings.cone.specularOcclusion);
    EXPECT_EQ(settings.cone.reflections, VoxelGISettings::Reflections::Off);
    EXPECT_FALSE(settings.cone.rtShadows);
}

TEST(VoxelGIRuntimeSettings, ReferenceSamplesAcceptOnlyTheDiagnosticBudgets)
{
    // Smaller reference batches keep high-resolution dispatches below the GPU watchdog.
    for (uint32_t samples : {0u, 64u, 256u, 1024u, 4096u})
    {
        VoxelGISettings settings;
        settings.referenceSamples = samples;
        EXPECT_TRUE(ValidateVoxelGISettings(settings)) << samples;
    }
    for (uint32_t samples : {1u, 128u, 512u, 2048u})
    {
        VoxelGISettings settings;
        settings.referenceSamples = samples;
        EXPECT_FALSE(ValidateVoxelGISettings(settings)) << samples;
    }
}

TEST(VoxelGIRuntimeSettings, SparseReferenceRequiresAReferenceBudgetAndPresetsRestoreDenseSampling)
{
    VoxelGIRuntimeSettings settings;
    settings.cone.referenceTileStride = 8;
    EXPECT_FALSE(ValidateVoxelGISettings(settings.cone));
    settings.cone.referenceSamples = 64;
    EXPECT_TRUE(ValidateVoxelGISettings(settings.cone));
    for (uint32_t stride : {0u, 2u, 4u, 16u})
    {
        settings.cone.referenceTileStride = stride;
        EXPECT_FALSE(ValidateVoxelGISettings(settings.cone)) << stride;
    }
    settings.cone.referenceTileStride = 8;
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
    EXPECT_EQ(settings.cone.referenceSamples, 0u);
    EXPECT_EQ(settings.cone.referenceTileStride, 1u);
    EXPECT_TRUE(ValidateVoxelGISettings(settings.cone));
}

// Compute GI traces the distance field. Older configs that chose the software BVH or occupancy
// bricks load as the field; an occupancy resolution does not set the field resolution.
TEST(VoxelGIRuntimeSettings, LegacyTraversalKeysMigrateToTheFieldAndFieldResolutionsLoad)
{
    const VoxelGISettings defaults;
    EXPECT_EQ(defaults.fieldResolution, 64u);
    for (const char* traversal : {"triangles", "bricks", "field"})
    {
        std::istringstream stream(
            std::string("voxel_gi_compute_traversal=") + traversal
            + "\nvoxel_gi_brick_resolution=256\nvoxel_gi_backend=compute\nvoxel_gi_shadows_backend=hardware\n");
        platform::ConfigLoader config(stream);
        VoxelGIRuntimeSettings settings;
        ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings)) << traversal;
        EXPECT_EQ(settings.cone.fieldResolution, 64u);
        EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 0u);
    }
    for (uint32_t resolution : {32u, 64u})
    {
        std::istringstream     stream("voxel_gi_field_resolution=" + std::to_string(resolution) + "\n");
        platform::ConfigLoader config(stream);
        VoxelGIRuntimeSettings settings;
        ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings));
        EXPECT_EQ(settings.cone.fieldResolution, resolution);
    }
    VoxelGISettings inherited;
    inherited.fieldResolution = 32;
    std::istringstream     empty;
    platform::ConfigLoader config(empty);
    VoxelGISettings        loaded;
    ASSERT_TRUE(LoadVoxelGISettings(config, loaded, inherited));
    EXPECT_EQ(loaded, inherited);
}

TEST(VoxelGIRuntimeSettings, TraversalValidationRejectsInvalidResolutions)
{
    VoxelGISettings invalid;
    invalid.fieldResolution = 96;
    EXPECT_FALSE(ValidateVoxelGISettings(invalid));
    invalid = {};
    EXPECT_TRUE(ValidateVoxelGISettings(invalid));
}

// Brick-traced direct shadows were removed; configs that still name them load with triangle shadows.
TEST(VoxelGIRuntimeSettings, RemovedShadowTraversalKeyIsIgnored)
{
    std::istringstream     stream("voxel_gi_shadow_traversal=bricks\n");
    platform::ConfigLoader config(stream);
    VoxelGIRuntimeSettings settings;
    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings));
    EXPECT_EQ(GetVoxelGIPreset(settings), VoxelGIPreset::Optimal);
}

// The field resolution is live, needs no voxel GI rebuild, and presets restore it.
TEST(VoxelGIRuntimeSettings, FieldResolutionIsLiveAndPresetsRestoreIt)
{
    VoxelGIRuntimeSettings settings;
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
    EXPECT_EQ(settings.cone.fieldResolution, 64u);
    const VoxelGIRuntimeSettings previous = settings;
    settings.cone.fieldResolution         = 32;
    EXPECT_FALSE(RequiresVoxelGIRebuild(previous, settings));
    EXPECT_EQ(GetVoxelGIPreset(settings), VoxelGIPreset::Custom);
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
    EXPECT_EQ(settings.cone.fieldResolution, 64u);
    EXPECT_EQ(GetVoxelGIPreset(settings), VoxelGIPreset::Medium);
}

// Without usable ray queries GI resolves to Compute, while default direct shadows use maps.
TEST(VoxelGIRuntimeSettings, DefaultPresetUsesHardwareOrComputeField)
{
    std::istringstream     stream("");
    platform::ConfigLoader config(stream);
    VoxelGIRuntimeSettings settings;
    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings));
    EXPECT_EQ(GetVoxelGIPreset(settings), VoxelGIPreset::Optimal);
    EXPECT_EQ(settings.cone.rayProvider, VoxelGISettings::RayProvider::Auto);
    EXPECT_EQ(GetGIMethod(settings.cone), GIMethod::ScreenProbes);
    EXPECT_TRUE(settings.cone.worldRadianceCache);
    EXPECT_TRUE(settings.cone.worldCacheFeedback);
    EXPECT_EQ(settings.cone.fieldResolution, 64u);
    VoxelGIRuntimeSettings compute = settings;
    EXPECT_STREQ(ResolveVoxelGIAutomaticProvider(compute, RHIGPUInfo{}), "ray_query_unavailable");
    EXPECT_EQ(compute.cone.rayProvider, VoxelGISettings::RayProvider::Voxel);
    EXPECT_EQ(compute.cone.GetHardwareFeatureMask(), 0u);
    EXPECT_FALSE(compute.cone.UsesRayShadows(false));
}

TEST(VoxelGIRuntimeSettings, ModeOverridesPreserveUnspecifiedSetupValues)
{
    VoxelGIRuntimeSettings settings;
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Balanced, settings));
    EXPECT_TRUE(settings.cone.restirGI);
    EXPECT_EQ(settings.cone.pixelResolutionDivisor, 4u);
    EXPECT_EQ(settings.cone.restirSpatialSamples, 2u);
    settings.shadowMapResolution = 512;
    settings.asyncCompute        = platform::AsyncComputeMode::eAuto;
    std::istringstream     input("voxel_gi_mode=screen-probes\nvoxel_gi_backend=auto\n");
    platform::ConfigLoader config(input);
    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings, &settings));
    EXPECT_EQ(GetGIMethod(settings.cone), GIMethod::ScreenProbes);
    EXPECT_TRUE(settings.cone.rayTracingPipeline);
    EXPECT_TRUE(settings.cone.worldRadianceCache);
    EXPECT_FALSE(settings.cone.restirGI);
    EXPECT_EQ(settings.shadowMapResolution, 512u);
    EXPECT_EQ(settings.asyncCompute, platform::AsyncComputeMode::eAuto);
    const VoxelGIRuntimeSettings saved = settings;
    std::istringstream           invalid("voxel_gi_mode=typo\n");
    platform::ConfigLoader       bad(invalid);
    EXPECT_FALSE(LoadVoxelGIRuntimeSettings(bad, settings, &settings));
    EXPECT_EQ(settings, saved);
}

TEST(VoxelGIRuntimeSettings, DefaultShadowFallbackKeepsRequestedProviders)
{
    for (VoxelGIPreset preset : {VoxelGIPreset::Low, VoxelGIPreset::Medium, VoxelGIPreset::High, VoxelGIPreset::Ultra})
    {
        VoxelGIRuntimeSettings settings;
        ASSERT_TRUE(ApplyVoxelGIPreset(preset, settings));
        const VoxelGISettings requested = settings.cone;
        EXPECT_FALSE(settings.cone.UsesRayShadows(false));
        EXPECT_EQ(settings.cone.UsesRayShadows(true), preset != VoxelGIPreset::Low);
        EXPECT_EQ(settings.cone, requested);
    }
    VoxelGISettings settings;
    settings.rayProvider = VoxelGISettings::RayProvider::Hardware;
    EXPECT_TRUE(settings.UsesRayShadows(true));
    EXPECT_FALSE(settings.UsesRayShadows(false));
    EXPECT_EQ(settings.rayProvider, VoxelGISettings::RayProvider::Hardware);
    // Compute direct shadows use shadow maps.
    settings.rayProvider = VoxelGISettings::RayProvider::Voxel;
    EXPECT_FALSE(settings.UsesRayShadows(false));
    EXPECT_FALSE(settings.UsesRayShadows(true));
    settings.rayProvider = VoxelGISettings::RayProvider::Hardware;
    settings.rtShadows   = false;
    EXPECT_FALSE(settings.UsesRayShadows(false));
    EXPECT_FALSE(settings.UsesRayShadows(true));
    settings.rtShadows = true;
    settings.shadows   = false;
    EXPECT_FALSE(settings.UsesRayShadows(false));
}

// Configs that selected software BVH shadow rays now use shadow maps; their providers are kept.
TEST(VoxelGIRuntimeSettings, LegacySoftwareShadowConfigsUseShadowMaps)
{
    const char* const configs[]{
        "voxel_gi_backend=compute\n",          "voxel_gi_shadows_backend=compute\n",
        "voxel_gi_ray_provider=voxel\n",       "voxel_gi_ray_provider=voxel\nvoxel_gi_rt_shadows=false\n",
        "voxel_gi_shadows_backend=hardware\n", "voxel_gi_rt_shadows=false\n"};
    for (const char* text : configs)
    {
        SCOPED_TRACE(text);
        std::istringstream     stream(text);
        platform::ConfigLoader config(stream);
        VoxelGIRuntimeSettings settings;
        ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings));
        EXPECT_FALSE(settings.cone.UsesRayShadows(false));
        const VoxelGISettings requested = settings.cone;
        ResolveVoxelGIAutomaticProvider(settings, RHIGPUInfo{});
        EXPECT_FALSE(settings.cone.UsesRayShadows(false));
        EXPECT_EQ(settings.cone.rtShadows, requested.rtShadows);
    }
}

TEST(VoxelGIRuntimeSettings, IndirectDiffuseSwitchesLiveAndPresetsRestoreIt)
{
    VoxelGIRuntimeSettings settings;
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
    EXPECT_TRUE(settings.cone.indirectDiffuse);
    const VoxelGIRuntimeSettings previous = settings;
    settings.cone.indirectDiffuse         = false;
    EXPECT_FALSE(RequiresVoxelGIRebuild(previous, settings));
    EXPECT_EQ(GetVoxelGIPreset(settings), VoxelGIPreset::Custom);
    // Presets switch both GI tracing passes back on.
    settings.cone.specularOcclusion = false;
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
    EXPECT_TRUE(settings.cone.indirectDiffuse);
    EXPECT_TRUE(settings.cone.specularOcclusion);
    EXPECT_EQ(GetVoxelGIPreset(settings), VoxelGIPreset::Medium);
    std::istringstream     stream("voxel_gi_indirect_diffuse=false\n");
    platform::ConfigLoader config(stream);
    VoxelGIRuntimeSettings loaded;
    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, loaded));
    EXPECT_FALSE(loaded.cone.indirectDiffuse);
}

TEST(VoxelGIRuntimeSettings, PresetsAllowExplicitOverridesAndRejectInvalidNamesTransactionally)
{
    VoxelGIRuntimeSettings settings;
    for (uint32_t index = 1; index <= static_cast<uint32_t>(VoxelGIPreset::Optimal); ++index)
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

TEST(VoxelGIRuntimeSettings, AutomaticProviderFollowsRayQueryCapability)
{
    VoxelGIRuntimeSettings settings;
    RHIGPUInfo             gpu;
    gpu.rayQuery = {true, true, true, true, true, 256};
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
    EXPECT_STREQ(ResolveVoxelGIAutomaticProvider(settings, gpu), "ray_query_available");
    EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 63u);
    // Device and driver identity never select the tier.
    for (const std::tuple<uint32_t, uint32_t, uint32_t>& identity :
         {std::tuple{0x10DEu, 0x2C02u, 0u}, std::tuple{0x1002u, 0x744Cu, 1u}, std::tuple{0x8086u, 0x56A0u, 0xFFFFFFFFu}})
    {
        gpu.vendorID         = std::get<0>(identity);
        gpu.deviceID         = std::get<1>(identity);
        gpu.driverVersionRaw = std::get<2>(identity);
        ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
        EXPECT_STREQ(ResolveVoxelGIAutomaticProvider(settings, gpu), "ray_query_available");
        EXPECT_EQ(settings.cone.rayProvider, VoxelGISettings::RayProvider::Hardware);
    }
    // Inherited Auto is resolved for any configuration, not only the Medium preset.
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
    settings.resolution = 128;
    EXPECT_STREQ(ResolveVoxelGIAutomaticProvider(settings, gpu), "ray_query_available");
    EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 63u);
    // Native-address support is optional; Metal encodes native instance references.
    gpu.rayQuery.bufferDeviceAddress = false;
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
    EXPECT_STREQ(ResolveVoxelGIAutomaticProvider(settings, gpu), "ray_query_available");
    EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 63u);
    // Each missing required capability selects the whole Compute tier.
    for (bool RHIRayQueryCapabilities::* feature :
         {&RHIRayQueryCapabilities::accelerationStructure, &RHIRayQueryCapabilities::rayQuery,
          &RHIRayQueryCapabilities::graphicsBuild, &RHIRayQueryCapabilities::triangleFloat3})
    {
        gpu.rayQuery          = {true, true, true, true, true, 256};
        gpu.rayQuery.*feature = false;
        ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
        EXPECT_STREQ(ResolveVoxelGIAutomaticProvider(settings, gpu), "ray_query_unavailable");
        EXPECT_EQ(settings.cone.GetHardwareFeatureMask(), 0u);
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

TEST(VoxelGIRuntimeSettings, AutomaticTierRequiresTheConfiguredHardwarePipeline)
{
    VoxelGIRuntimeSettings settings;
    RHIGPUInfo             gpu;
    gpu.rayQuery = {true, true, true, true, true, 256};
    EXPECT_EQ(GetHardwareTierUnavailableReason(settings.cone, gpu), nullptr);
    // The RT-pipeline tier also needs usable RT pipelines with a second recursion level.
    settings.cone.rayTracingPipeline = true;
    EXPECT_STREQ(GetHardwareTierUnavailableReason(settings.cone, gpu), "rt_pipeline_unavailable");
    gpu.rayTracing = {true, false, RHIShaderInvocationReorder::eNone, 32, 32, 64, 4096, 1, 1048576, 32};
    EXPECT_STREQ(GetHardwareTierUnavailableReason(settings.cone, gpu), "rt_pipeline_unavailable");
    gpu.rayTracing.maxRayRecursionDepth = 2;
    EXPECT_EQ(GetHardwareTierUnavailableReason(settings.cone, gpu), nullptr);
    gpu.rayQuery = {};
    EXPECT_STREQ(GetHardwareTierUnavailableReason(settings.cone, gpu), "ray_query_unavailable");

    // Auto resolves to Compute when the configured HW tier cannot run.
    gpu.rayQuery   = {true, true, true, true, true, 256};
    gpu.rayTracing = {};
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
    settings.cone.rayTracingPipeline = true;
    EXPECT_STREQ(ResolveVoxelGIAutomaticProvider(settings, gpu), "rt_pipeline_unavailable");
    EXPECT_EQ(settings.cone.rayProvider, VoxelGISettings::RayProvider::Voxel);
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings));
    settings.cone.rayTracingPipeline = false;
    EXPECT_STREQ(ResolveVoxelGIAutomaticProvider(settings, gpu), "ray_query_available");
    EXPECT_EQ(settings.cone.rayProvider, VoxelGISettings::RayProvider::Hardware);

    // Only failures the next frame cannot fix move an automatic tier to Compute.
    for (const char* reason : {"ray_query_features_disabled_or_unavailable", "acceleration_structure_budget",
                               "scene_build_preparation_failed", "hit_shading_stream_uncommitted_or_allocation_or_budget"})
    {
        EXPECT_TRUE(IsPersistentHardwareTierFailure(reason)) << reason;
    }
    for (const char* reason : {"hardware_ready", "failed_frame", "scene_not_ready", ""})
    {
        EXPECT_FALSE(IsPersistentHardwareTierFailure(reason)) << reason;
    }
    EXPECT_FALSE(IsPersistentHardwareTierFailure(nullptr));
}

// The optimal configuration follows device capability and keeps the lighting content.
TEST(VoxelGIRuntimeSettings, OptimalConfigurationFollowsCapabilityAndKeepsContent)
{
    VoxelGIRuntimeSettings requested;
    requested.cone.indirectIntensity = 2.0f;
    requested.cone.reflections       = VoxelGISettings::Reflections::Off;
    requested.cone.shadows           = false;
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Optimal, requested));
    EXPECT_TRUE(requested.optimal);
    EXPECT_EQ(requested.cone.indirectIntensity, 2.0f);
    EXPECT_EQ(requested.cone.reflections, VoxelGISettings::Reflections::Off);
    EXPECT_FALSE(requested.cone.shadows);
    EXPECT_TRUE(ValidateVoxelGIRuntimeSettings(requested));

    RHIGPUInfo pipeline;
    pipeline.rayQuery   = {true, true, true, true, true, 256};
    pipeline.rayTracing = {true, false, RHIShaderInvocationReorder::eNone, 32, 32, 64, 4096, 2, 1048576, 32};
    RHIGPUInfo queries;
    queries.rayQuery = pipeline.rayQuery;
    const RHIGPUInfo                                         none{};
    const std::tuple<const RHIGPUInfo*, GITracing, GIMethod> devices[] = {
        {&pipeline, GITracing::RayTracingPipeline, GIMethod::ScreenProbes},
        {&queries, GITracing::Compute, GIMethod::ScreenProbes},
        {&none, GITracing::Compute, GIMethod::ScreenProbes}};
    for (const std::tuple<const RHIGPUInfo*, GITracing, GIMethod>& device : devices)
    {
        const RHIGPUInfo& gpu = *std::get<0>(device);
        EXPECT_EQ(GetOptimalGITracing(gpu.rayQuery.IsUsable(), &gpu == &pipeline), std::get<1>(device));
        VoxelGIRuntimeSettings settings = requested;
        ResolveVoxelGIAutomaticProvider(settings, gpu);
        EXPECT_TRUE(settings.optimal);
        EXPECT_EQ(GetGITracing(settings.cone), std::get<1>(device));
        EXPECT_EQ(GetGIMethod(settings.cone), std::get<2>(device));
        EXPECT_TRUE(settings.cone.worldRadianceCache);
        EXPECT_FALSE(settings.cone.restirGI);
        EXPECT_EQ(settings.cone.pixelResolutionDivisor, 1u);
        EXPECT_EQ(GetGIDenoiser(settings.cone), GIDenoiser::Spatiotemporal);
        EXPECT_EQ(GetGIShadows(settings.cone), GIShadows::RayTracedReuse);
        EXPECT_EQ(settings.cone.indirectIntensity, 2.0f);
        EXPECT_EQ(settings.cone.reflections, VoxelGISettings::Reflections::Off);
        EXPECT_FALSE(settings.cone.shadows);
        EXPECT_TRUE(ValidateVoxelGIRuntimeSettings(settings));
    }
    // A forced provider (ZenApp --backend) stays; the configuration follows its tier.
    VoxelGIRuntimeSettings forced = requested;
    forced.cone.rayProvider       = VoxelGISettings::RayProvider::Voxel;
    EXPECT_STREQ(ResolveVoxelGIAutomaticProvider(forced, pipeline), "explicit_provider");
    EXPECT_EQ(GetGITracing(forced.cone), GITracing::Compute);
    EXPECT_EQ(GetGIMethod(forced.cone), GIMethod::ScreenProbes);
    forced                  = requested;
    forced.cone.rayProvider = VoxelGISettings::RayProvider::Hardware;
    EXPECT_STREQ(ResolveVoxelGIAutomaticProvider(forced, queries), "explicit_provider");
    EXPECT_EQ(GetGITracing(forced.cone), GITracing::RayQueries);
    EXPECT_EQ(GetGIMethod(forced.cone), GIMethod::PixelReSTIR);
    EXPECT_FALSE(forced.cone.worldRadianceCache);
    EXPECT_EQ(forced.cone.pixelResolutionDivisor, 0u);
}

// Content keys keep the optimal configuration; any configuration key makes it custom.
TEST(VoxelGIRuntimeSettings, ConfigurationKeysTurnTheOptimalConfigurationOff)
{
    for (const char* content : {"voxel_gi_indirect_intensity=2\n", "voxel_gi_reflections=false\n",
                                "voxel_gi_shadow_enabled=false\n", "voxel_gi_preset=optimal\n"})
    {
        std::istringstream     stream(content);
        platform::ConfigLoader config(stream);
        VoxelGIRuntimeSettings settings;
        ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings)) << content;
        EXPECT_TRUE(settings.optimal) << content;
    }
    for (const char* configuration :
         {"voxel_gi_denoiser=temporal\n", "voxel_gi_method=screen-probes\n", "voxel_gi_tracing=compute\n",
          "voxel_gi_shadows=maps\n", "voxel_gi_restir=false\n", "voxel_gi_pixel_resolution_divisor=2\n",
          "voxel_gi_mode=per-pixel\n", "voxel_gi_preset=balanced\n", "voxel_gi_preset=custom\n"})
    {
        std::istringstream     stream(configuration);
        platform::ConfigLoader config(stream);
        VoxelGIRuntimeSettings settings;
        ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings)) << configuration;
        EXPECT_FALSE(settings.optimal) << configuration;
    }
    std::istringstream     stream("voxel_gi_denoiser=temporal\nvoxel_gi_shadows=ray-traced\nvoxel_gi_method=per-pixel-restir\n"
                                  "voxel_gi_pixel_resolution_divisor=auto\nvoxel_gi_tracing=ray-queries\n");
    platform::ConfigLoader config(stream);
    VoxelGIRuntimeSettings settings;
    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, settings));
    EXPECT_EQ(GetGIDenoiser(settings.cone), GIDenoiser::Temporal);
    EXPECT_EQ(GetGIShadows(settings.cone), GIShadows::RayTraced);
    EXPECT_EQ(GetGITracing(settings.cone), GITracing::RayQueries);
    EXPECT_EQ(GetGIMethod(settings.cone), GIMethod::PixelReSTIR);
    EXPECT_EQ(settings.cone.pixelResolutionDivisor, 0u);
}

// The panel's export, engine.cfg and ZenApp's --gi-* arguments describe the same settings.
TEST(VoxelGIRuntimeSettings, EveryCombinationRoundTripsThroughEngineConfigAndZenAppArguments)
{
    for (uint32_t method = 0; method < static_cast<uint32_t>(GIMethod::Count); ++method)
    {
        for (uint32_t tracing = 0; tracing < static_cast<uint32_t>(GITracing::Count); ++tracing)
        {
            for (uint32_t denoiser = 0; denoiser < static_cast<uint32_t>(GIDenoiser::Count); ++denoiser)
            {
                for (uint32_t shadows = 0; shadows < static_cast<uint32_t>(GIShadows::Count); ++shadows)
                {
                    VoxelGIRuntimeSettings settings;
                    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Optimal, settings));
                    settings.optimal = false;
                    SetGIMethod(settings.cone, static_cast<GIMethod>(method));
                    SetGITracing(settings.cone, static_cast<GITracing>(tracing));
                    SetGIDenoiser(settings.cone, static_cast<GIDenoiser>(denoiser));
                    SetGIShadows(settings.cone, static_cast<GIShadows>(shadows));
                    settings.cone.pixelResolutionDivisor = (method + shadows) % 2 == 0 ? 0u : 2u;
                    settings.cone.indirectIntensity      = 0.75f;
                    settings.cone.antiFirefly            = denoiser != 0;
                    EXPECT_EQ(GetGIMethod(settings.cone), static_cast<GIMethod>(method));
                    EXPECT_EQ(GetGITracing(settings.cone), static_cast<GITracing>(tracing));
                    EXPECT_EQ(GetGIDenoiser(settings.cone), static_cast<GIDenoiser>(denoiser));
                    EXPECT_EQ(GetGIShadows(settings.cone), static_cast<GIShadows>(shadows));

                    const std::string      text = FormatGIConfig(settings, true);
                    std::istringstream     stream(text);
                    platform::ConfigLoader config(stream);
                    VoxelGIRuntimeSettings loaded;
                    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, loaded)) << text;
                    EXPECT_EQ(loaded, settings) << text;

                    // ZenApp turns --gi-<name>=<value> into the option's engine.cfg line.
                    std::istringstream arguments(FormatGIArguments(settings, true));
                    std::string        argument;
                    std::string        lines;
                    while (arguments >> argument)
                    {
                        ASSERT_TRUE(argument.starts_with("--gi-")) << argument;
                        const size_t          equals = argument.find('=');
                        const GIConfigOption* option = FindGIConfigArgument(argument.substr(5, equals - 5));
                        ASSERT_NE(option, nullptr) << argument;
                        lines += std::string(option->key) + "=" + argument.substr(equals + 1) + "\n";
                    }
                    EXPECT_EQ(lines, text);
                }
            }
        }
    }
    // The optimal configuration writes only the preset and the content.
    VoxelGIRuntimeSettings optimal;
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Optimal, optimal));
    optimal.cone.emissiveLighting = false;
    const std::string text        = FormatGIConfig(optimal, false);
    EXPECT_NE(text.find("voxel_gi_preset=optimal\n"), std::string::npos);
    EXPECT_NE(text.find("voxel_gi_enabled=false\n"), std::string::npos);
    EXPECT_EQ(text.find("voxel_gi_method"), std::string::npos);
    std::istringstream     stream(text);
    platform::ConfigLoader config(stream);
    VoxelGIRuntimeSettings loaded;
    ASSERT_TRUE(LoadVoxelGIRuntimeSettings(config, loaded));
    EXPECT_EQ(loaded, optimal);
}

TEST(VoxelGIRuntimeSettings, OptionTableNamesAreUniqueAndFound)
{
    std::span<const GIConfigOption> options = GetGIConfigOptions();
    for (const GIConfigOption& option : options)
    {
        const std::string name = GetGIConfigArgumentName(option);
        EXPECT_EQ(name.find('_'), std::string::npos) << name;
        EXPECT_EQ(FindGIConfigArgument(name), &option) << name;
    }
    EXPECT_STREQ(FindGIConfigArgument("shadow-map-resolution")->key, "shadow_map_resolution");
    EXPECT_STREQ(FindGIConfigArgument("voxel-resolution")->key, "voxel_resolution");
    EXPECT_EQ(FindGIConfigArgument("mode"), nullptr);
    EXPECT_EQ(FindGIConfigArgument("rt-shadows"), nullptr);
}

TEST(VoxelGIRuntimeSettings, SavingReplacesOnlyTheGIOptionsOfAnEngineConfig)
{
    VoxelGIRuntimeSettings settings;
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Optimal, settings));
    const std::string before = "model_base_path=Models\n# GI below\nvoxel_gi_enabled=true\nvoxel_gi_preset=balanced\n"
                               "# voxel_gi_backend=auto\nvoxel_gi_rt_shadows=false\nenvironment_intensity=1.0\n"
                               "voxel_gi_mode=per-pixel\n";
    const std::string after  = UpdateGIConfigText(before, settings, true);
    EXPECT_EQ(after, "model_base_path=Models\n# GI below\n" + FormatGIConfig(settings, true)
                         + "# voxel_gi_backend=auto\nenvironment_intensity=1.0\n");
    EXPECT_EQ(UpdateGIConfigText(after, settings, true), after);
    EXPECT_EQ(UpdateGIConfigText("default_model=Sponza\n", settings, false),
              "default_model=Sponza\n\n" + FormatGIConfig(settings, false));
}

TEST(VoxelGIRuntimeSettings, AutomaticPixelResolutionKeepsAtLeast540Lines)
{
    EXPECT_EQ(ResolvePixelResolutionDivisor(0, 2160), 4u);
    EXPECT_EQ(ResolvePixelResolutionDivisor(0, 1440), 2u);
    EXPECT_EQ(ResolvePixelResolutionDivisor(0, 1080), 2u);
    EXPECT_EQ(ResolvePixelResolutionDivisor(0, 1079), 1u);
    EXPECT_EQ(ResolvePixelResolutionDivisor(0, 720), 1u);
    EXPECT_EQ(ResolvePixelResolutionDivisor(2, 2160), 2u);
    EXPECT_EQ(ResolvePixelResolutionDivisor(1, 2160), 1u);
}

// Methods report the tracing tiers they need; unavailable choices still render the per-pixel trace.
TEST(VoxelGIRuntimeSettings, MethodAvailabilityFollowsTheTracingTier)
{
    EXPECT_EQ(GetGIMethodUnavailableReason(GIMethod::Pixel, GITracing::Compute), nullptr);
    EXPECT_EQ(GetGIMethodUnavailableReason(GIMethod::Pixel, GITracing::RayQueries), nullptr);
    EXPECT_NE(GetGIMethodUnavailableReason(GIMethod::PixelReSTIR, GITracing::Compute), nullptr);
    EXPECT_EQ(GetGIMethodUnavailableReason(GIMethod::PixelReSTIR, GITracing::RayQueries), nullptr);
    EXPECT_NE(GetGIMethodUnavailableReason(GIMethod::PixelCache, GITracing::RayQueries), nullptr);
    EXPECT_EQ(GetGIMethodUnavailableReason(GIMethod::PixelCache, GITracing::Compute), nullptr);
    EXPECT_NE(GetGIMethodUnavailableReason(GIMethod::PixelReSTIRCache, GITracing::Compute), nullptr);
    EXPECT_NE(GetGIMethodUnavailableReason(GIMethod::PixelReSTIRCache, GITracing::RayQueries), nullptr);
    EXPECT_NE(GetGIMethodUnavailableReason(GIMethod::ScreenProbes, GITracing::RayQueries), nullptr);
    for (uint32_t method = 0; method < static_cast<uint32_t>(GIMethod::Count); ++method)
    {
        EXPECT_EQ(GetGIMethodUnavailableReason(static_cast<GIMethod>(method), GITracing::RayTracingPipeline), nullptr);
    }
    EXPECT_EQ(GetGITracingUnavailableReason(GITracing::Compute, RHIGPUInfo{}), nullptr);
    EXPECT_STREQ(GetGITracingUnavailableReason(GITracing::RayQueries, RHIGPUInfo{}), "ray_query_unavailable");
    RHIGPUInfo queries;
    queries.rayQuery = {true, true, true, true, true, 256};
    EXPECT_EQ(GetGITracingUnavailableReason(GITracing::RayQueries, queries), nullptr);
    EXPECT_STREQ(GetGITracingUnavailableReason(GITracing::RayTracingPipeline, queries), "rt_pipeline_unavailable");
}

// Setups written before shadowReuse keep exact per-pixel shadows; screen probes keep their reuse.
TEST(VoxelGIRuntimeSettings, ShadowReuseFollowsTheMethodsFlag)
{
    VoxelGISettings settings;
    EXPECT_EQ(GetGIShadows(settings), GIShadows::RayTraced);
    EXPECT_FALSE(settings.UsesShadowReuse());
    SetGIMethod(settings, GIMethod::ScreenProbes);
    EXPECT_TRUE(settings.UsesShadowReuse());
    SetGIShadows(settings, GIShadows::RayTraced);
    EXPECT_FALSE(settings.UsesShadowReuse());
    SetGIShadows(settings, GIShadows::RayTracedReuse);
    SetGIMethod(settings, GIMethod::PixelReSTIR);
    EXPECT_TRUE(settings.UsesShadowReuse());
    SetGIShadows(settings, GIShadows::ShadowMaps);
    EXPECT_FALSE(settings.rtShadows);
}

// Like an engine.cfg configuration key, a configuration edit makes optimal settings custom. Content
// edits and a forced provider keep them optimal.
TEST(VoxelGIRuntimeSettings, EditingAConfigurationFieldMakesOptimalSettingsCustom)
{
    VoxelGIRuntimeSettings settings;
    ASSERT_TRUE(ApplyVoxelGIPreset(VoxelGIPreset::Optimal, settings));
    EXPECT_TRUE(IsOptimalVoxelGIConfiguration(settings));
    settings.cone.indirectIntensity = 3.0f;
    settings.cone.shadows           = false;
    settings.cone.rayProvider       = VoxelGISettings::RayProvider::Hardware;
    EXPECT_TRUE(IsOptimalVoxelGIConfiguration(settings));
    EXPECT_EQ(GetVoxelGIPreset(settings), VoxelGIPreset::Optimal);
    VoxelGIRuntimeSettings edited      = settings;
    edited.cone.pixelResolutionDivisor = 2;
    EXPECT_TRUE(edited.optimal);
    EXPECT_FALSE(IsOptimalVoxelGIConfiguration(edited));
    EXPECT_EQ(GetVoxelGIPreset(edited), VoxelGIPreset::Custom);
    EXPECT_NE(FormatGIConfig(edited, true).find("voxel_gi_preset=custom\n"), std::string::npos);
    RHIGPUInfo gpu;
    gpu.rayQuery   = {true, true, true, true, true, 256};
    gpu.rayTracing = {true, false, RHIShaderInvocationReorder::eNone, 32, 32, 64, 4096, 2, 1048576, 32};
    ResolveVoxelGIAutomaticProvider(edited, gpu);
    EXPECT_FALSE(edited.optimal);
    EXPECT_EQ(edited.cone.pixelResolutionDivisor, 2u);
    edited                     = settings;
    edited.shadowMapResolution = 512;
    EXPECT_FALSE(IsOptimalVoxelGIConfiguration(edited));
    edited                     = settings;
    edited.cone.worldCacheSeed = true;
    EXPECT_FALSE(IsOptimalVoxelGIConfiguration(edited));
}

TEST(VoxelGIRuntimeSettings, RejectsInvalidEnumsAndNonFiniteAPIValues)
{
    VoxelGIRuntimeSettings settings;

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
