#include "Platform/ConfigLoader.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include <gtest/gtest.h>
#include <sstream>
#include <limits>
#include <filesystem>

namespace
{
using zen::platform::ConfigLoader;
using zen::platform::VoxelizerMode;
using zen::rc::AsyncComputeMode;
using zen::rc::AsyncComputeStatus;

TEST(ConfigLoaderTests, VoxelGridDefaultsFollowTheSelectedMethodAndPreserveExplicitValues)
{
    for (const char* method : {"auto", "cone", "dynamic_voxel"})
    {
        std::istringstream defaults(std::string("voxel_gi_method=") + method);
        ConfigLoader config(defaults);
        EXPECT_EQ(config.GetVoxelResolution(), std::string(method) == "dynamic_voxel" ? 64u : 256u);
        for (uint32_t side : {64u, 128u, 256u})
        {
            std::istringstream explicitInput(std::string("voxel_gi_method=") + method +
                                             "\nvoxel_resolution=" + std::to_string(side));
            ConfigLoader explicitConfig(explicitInput);
            EXPECT_EQ(explicitConfig.GetVoxelResolution(), side);
        }
    }
}

TEST(ConfigLoaderTests, ParsesAsyncComputeModesAndDefaultsToDisabled)
{
    struct Case
    {
        const char* input;
        AsyncComputeMode expected;
    };
    const Case cases[] = {
        {"async_compute=auto", AsyncComputeMode::eAuto},
        {" async_compute = auto # request GPU overlap\r\n", AsyncComputeMode::eAuto},
        {"async_compute=off", AsyncComputeMode::eDisabled},
        {"", AsyncComputeMode::eDisabled},
        {"# async_compute=auto", AsyncComputeMode::eDisabled},
        {"async_compute=", AsyncComputeMode::eDisabled},
        {"async_compute=1", AsyncComputeMode::eDisabled},
        {"async_compute=invalid", AsyncComputeMode::eDisabled}};
    for (const Case& test : cases)
    {
        SCOPED_TRACE(test.input);
        std::istringstream input(test.input);
        ConfigLoader config(input);
        EXPECT_EQ(config.GetAsyncComputeMode(), test.expected);
    }
}

TEST(ConfigLoaderTests, AsyncComputeOverrideWinsWithoutChangingCPUExecutionMode)
{
    for (const char* value : {"off", "auto"})
    {
        std::istringstream input(std::string("async_compute=") + value);
        ConfigLoader config(input);
        for (zen::RHIExecutionMode cpuMode :
             {zen::RHIExecutionMode::eInline, zen::RHIExecutionMode::eThreaded})
        {
            zen::rc::RenderConfig renderConfig;
            renderConfig.rhiExecutionMode = cpuMode;
            renderConfig.asyncComputeMode = config.GetAsyncComputeMode();
            EXPECT_TRUE(zen::rc::ParseAsyncComputeOverride("--async-compute=1",
                                                           renderConfig.asyncComputeMode));
            EXPECT_EQ(renderConfig.asyncComputeMode, AsyncComputeMode::eAuto);
            EXPECT_TRUE(zen::rc::ParseAsyncComputeOverride("--async-compute=0",
                                                           renderConfig.asyncComputeMode));
            EXPECT_EQ(renderConfig.asyncComputeMode, AsyncComputeMode::eDisabled);
            EXPECT_EQ(renderConfig.rhiExecutionMode, cpuMode);
        }
    }
    AsyncComputeMode mode = AsyncComputeMode::eAuto;
    for (const char* invalid : {"--async-compute=", "--async-compute=2", "--async-compute=auto",
                                "--async-compute=10", "--rhi-thread=0"})
    {
        EXPECT_FALSE(zen::rc::ParseAsyncComputeOverride(invalid, mode));
        EXPECT_EQ(mode, AsyncComputeMode::eAuto);
    }
}

TEST(ConfigLoaderTests, AsyncComputePolicyReportsEveryFallback)
{
    struct Case
    {
        AsyncComputeMode mode;
        zen::RHIQueueCapabilities capabilities;
        AsyncComputeStatus expected;
        const char* reason;
    };
    const Case cases[] = {{AsyncComputeMode::eDisabled,
                           {true, true, {0, 1, 2}},
                           AsyncComputeStatus::eDisabled,
                           "disabled by configuration"},
                          {AsyncComputeMode::eAuto,
                           {false, true, {0, 1, 2}},
                           AsyncComputeStatus::eComputeUnavailable,
                           "compute queue unavailable"},
                          {AsyncComputeMode::eAuto,
                           {true, true, {0, 0, 2}},
                           AsyncComputeStatus::eSharedGraphicsQueue,
                           "compute shares graphics queue"},
                          {AsyncComputeMode::eAuto,
                           {true, false, {0, 1, 2}},
                           AsyncComputeStatus::eDependenciesUnavailable,
                           "timeline dependencies unavailable"},
                          {AsyncComputeMode::eAuto,
                           {true, true, {0, 1, 2}},
                           AsyncComputeStatus::eAvailable,
                           "available"},
                          {AsyncComputeMode::eAuto,
                           {true, true, {0, 1, 1}},
                           AsyncComputeStatus::eAvailable,
                           "available"}};
    for (const Case& test : cases)
    {
        const AsyncComputeStatus actual =
            zen::rc::ResolveAsyncComputeStatus(test.mode, test.capabilities);
        EXPECT_EQ(actual, test.expected);
        EXPECT_STREQ(zen::rc::GetAsyncComputeStatusReason(actual), test.reason);
        if (test.mode == AsyncComputeMode::eAuto)
        {
            EXPECT_EQ(test.capabilities.SupportsAsyncCompute(),
                      actual == AsyncComputeStatus::eAvailable);
        }
    }
}

TEST(ConfigLoaderTests, ParsesVoxelizerModes)
{
    for (const auto& [value, expected] :
         {std::pair{"auto", VoxelizerMode::eAuto}, std::pair{"comp", VoxelizerMode::eCompute},
          std::pair{"geom", VoxelizerMode::eGeometry}})
    {
        SCOPED_TRACE(value);
        std::istringstream input(std::string("voxelizer=") + value);
        ConfigLoader config(input);
        EXPECT_EQ(config.GetVoxelizerMode(), expected);
    }
}

TEST(ConfigLoaderTests, MissingOrInvalidVoxelizerDefaultsToAuto)
{
    for (const char* text : {"", "voxelizer=", "voxelizer=invalid", "# voxelizer=comp"})
    {
        SCOPED_TRACE(text);
        std::istringstream input(text);
        ConfigLoader config(input);
        EXPECT_EQ(config.GetVoxelizerMode(), VoxelizerMode::eAuto);
    }
}

TEST(ConfigLoaderTests, IgnoresCommentsAndTrimsWhitespace)
{
    const std::string modelRoot =
        (std::filesystem::path(ZEN_CONFIG_PATH).root_path() / "assets/Models").generic_string();
    std::istringstream input("\r\n"
                             " # Configuration with Windows line endings\r\n"
                             " model_base_path = " +
                             modelRoot +
                             " \r\n"
                             " default_model = Sponza # scene\r\n"
                             " skybox_model = Box\r\n"
                             " voxelizer = geom # prefer geometry\r\n"
                             "#voxelizer=comp\r\n"
                             " \t# voxelizer = comp\r\n"
                             "ignored line\r\n");
    ConfigLoader config(input);

    EXPECT_EQ(config.GetVoxelizerMode(), VoxelizerMode::eGeometry);
    EXPECT_EQ(config.GetDefaultGLTFModelPath(), modelRoot + "/Sponza/glTF/Sponza.gltf");
    EXPECT_EQ(config.GetSkyboxModelPath(), modelRoot + "/Box/glTF/Box.gltf");
    EXPECT_EQ(config.GetGLTFModelPath("Box"), modelRoot + "/Box/glTF/Box.gltf");
}

TEST(ConfigLoaderTests, ResolvesRelativeModelRootFromEngineConfigDirectory)
{
    std::istringstream input("model_base_path=../../glTF-Sample-Assets/Models\n"
                             "default_model=Sponza\n"
                             "skybox_model=Box\n");
    ConfigLoader config(input);
    const std::filesystem::path checkoutDirectory =
        std::filesystem::path(ZEN_CONFIG_PATH).parent_path().parent_path().parent_path();
    const std::filesystem::path modelRoot = checkoutDirectory / "glTF-Sample-Assets/Models";

    EXPECT_EQ(config.GetDefaultGLTFModelPath(),
              (modelRoot / "Sponza/glTF/Sponza.gltf").generic_string());
    EXPECT_EQ(config.GetSkyboxModelPath(), (modelRoot / "Box/glTF/Box.gltf").generic_string());
    EXPECT_EQ(config.GetGLTFModelPath("Box"), (modelRoot / "Box/glTF/Box.gltf").generic_string());
}

TEST(ConfigLoaderTests, ResolvesRelativeModelOverrideFromEngineConfigDirectory)
{
    std::istringstream input("default_model=Sponza\n"
                             "default_model_path=Models/../Fixtures/scene.gltf\n");
    ConfigLoader config(input);
    const std::filesystem::path configDirectory =
        std::filesystem::path(ZEN_CONFIG_PATH).parent_path();

    EXPECT_EQ(config.GetDefaultGLTFModelPath(),
              (configDirectory / "Fixtures/scene.gltf").generic_string());
}

TEST(ConfigLoaderTests, PreservesAbsoluteModelOverride)
{
    const std::string modelPath =
        (std::filesystem::path(ZEN_CONFIG_PATH).root_path() / "assets/scene.gltf").generic_string();
    std::istringstream input("default_model=Sponza\ndefault_model_path=" + modelPath);
    ConfigLoader config(input);

    EXPECT_EQ(config.GetDefaultGLTFModelPath(), modelPath);
}

TEST(ConfigLoaderTests, KeepsMissingAndEmptyModelPathsEmpty)
{
    std::istringstream input("default_model=Sponza\ndefault_model_path=\n");
    ConfigLoader config(input);

    EXPECT_TRUE(config.GetDefaultGLTFModelPath().empty());
    EXPECT_TRUE(config.GetSkyboxModelPath().empty());
    EXPECT_TRUE(config.GetGLTFModelPath("Box").empty());
}

TEST(ConfigLoaderTests, ValidatesVoxelizerChoiceAgainstGPUCapabilities)
{
    struct TestCase
    {
        const char* value;
        bool supportsGeometryShader;
        VoxelizerMode expected;
    };
    const TestCase cases[] = {
        {"auto", false, VoxelizerMode::eCompute}, {"auto", true, VoxelizerMode::eGeometry},
        {"comp", false, VoxelizerMode::eCompute}, {"comp", true, VoxelizerMode::eCompute},
        {"geom", false, VoxelizerMode::eCompute}, {"geom", true, VoxelizerMode::eGeometry},
    };
    for (const TestCase& test : cases)
    {
        SCOPED_TRACE(test.value);
        SCOPED_TRACE(test.supportsGeometryShader);
        std::istringstream input(std::string("voxelizer=") + test.value);
        ConfigLoader config(input);
        zen::RHIGPUInfo gpuInfo{};
        gpuInfo.supportGeometryShader = test.supportsGeometryShader;

        EXPECT_EQ(zen::rc::ResolveVoxelizerMode(config.GetVoxelizerMode(), gpuInfo), test.expected);
    }
}
TEST(ConfigLoaderTests, VoxelWorkgroupsRespectInvocationAndPerAxisLimits)
{
    struct Case
    {
        uint32_t invocations;
        std::array<uint32_t, 3> limits;
        glm::uvec3 expected;
    };
    const Case cases[] = {{64, {128, 128, 64}, {4, 4, 4}},  {127, {128, 128, 64}, {4, 4, 4}},
                          {128, {128, 128, 64}, {8, 4, 4}}, {255, {128, 128, 64}, {8, 4, 4}},
                          {256, {128, 128, 64}, {8, 8, 4}}, {511, {128, 128, 64}, {8, 8, 4}},
                          {512, {128, 128, 64}, {8, 8, 8}}, {1024, {1024, 1024, 64}, {8, 8, 8}},
                          {1024, {4, 128, 64}, {4, 4, 4}},  {1024, {128, 4, 64}, {8, 4, 4}},
                          {1024, {128, 128, 4}, {8, 8, 4}}};
    for (const Case& test : cases)
    {
        zen::RHIGPUInfo info;
        info.maxComputeWorkGroupInvocations = test.invocations;
        info.maxComputeWorkGroupSize        = test.limits;
        const glm::uvec3 size               = zen::rc::ResolveVoxelVolumeWorkgroupSize(info);
        EXPECT_EQ(size, test.expected);
        EXPECT_LE(size.x * size.y * size.z, test.invocations);
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            EXPECT_LE(size[axis], test.limits[axis]);
        }
    }
}

TEST(ConfigLoaderTests, VoxelDispatchCoversWholeVolumesAndSmallMips)
{
    for (uint32_t invocations : {64u, 128u, 256u, 512u})
    {
        zen::RHIGPUInfo info;
        info.maxComputeWorkGroupInvocations = invocations;
        const glm::uvec3 size               = zen::rc::ResolveVoxelVolumeWorkgroupSize(info);
        for (uint32_t dimension : {1u, 2u, 4u, 7u, 8u, 9u, 64u, 128u, 256u})
        {
            const glm::uvec3 groups = zen::rc::GetVoxelVolumeDispatchGroups(dimension, info);
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                EXPECT_GE(groups[axis] * size[axis], dimension);
                EXPECT_LT((groups[axis] - 1u) * size[axis], dimension);
            }
        }
    }
}
} // namespace
#include "Graphics/RenderCore/V2/SceneLighting.h"

TEST(SceneLightingTests, DefaultsExplicitZeroAndMalformedLights)
{
    std::istringstream defaults("");
    zen::platform::ConfigLoader defaultConfig(defaults);
    EXPECT_EQ(zen::rc::LoadSceneLights(defaultConfig).size(), 4u);
    std::istringstream zero("light_count=0");
    zen::platform::ConfigLoader zeroConfig(zero);
    EXPECT_TRUE(zen::rc::LoadSceneLights(zeroConfig).empty());
    std::istringstream invalid("light_count=4\nlight.0.intensity=nan\nlight.1.range=-2\n"
                               "light.2.position=1,2,3,4\nlight.3.type=spot\n"
                               "light.3.inner_angle_degrees=60\nlight.3.outer_angle_degrees=30\n");
    zen::platform::ConfigLoader invalidConfig(invalid);
    EXPECT_TRUE(zen::rc::LoadSceneLights(invalidConfig).empty());
}

TEST(SceneLightingTests, ParsesTypesAndPreservesConfigIndices)
{
    std::istringstream input(
        "light_count=3\nlight.0.type=unknown\nlight.1.type=directional\n"
        "light.1.direction=1,-2,3\nlight.2.type=spot\nlight.2.enabled=false\n");
    zen::platform::ConfigLoader config(input);
    const zen::HeapVector<zen::rc::ConfiguredLight> lights = zen::rc::LoadSceneLights(config);
    ASSERT_EQ(lights.size(), 2u);
    EXPECT_EQ(lights[0].configIndex, 1u);
    EXPECT_EQ(lights[0].light.type, zen::rc::SceneLightType::eDirectional);
    EXPECT_EQ(lights[1].light.type, zen::rc::SceneLightType::eSpot);
    EXPECT_FALSE(lights[1].light.enabled);
}

TEST(SceneLightingTests, RuntimeEditsRemovalAndFrameSnapshots)
{
    zen::rc::SceneLights lights;
    zen::rc::SceneLight light;
    const zen::rc::LightId first  = lights.Add(light);
    const zen::rc::LightId second = lights.Add(light);
    ASSERT_NE(first, 0u);
    zen::rc::SceneUniformData snapshot;
    lights.WriteUniforms(snapshot);
    EXPECT_EQ(snapshot.lightInfo.x, 2.0f);
    const uint64_t revision = lights.GetRevision();
    light.range             = -1.0f;
    EXPECT_FALSE(lights.Update(first, light));
    EXPECT_EQ(lights.GetRevision(), revision);
    ASSERT_NE(lights.Find(first), nullptr);
    EXPECT_GT(lights.Find(first)->range, 0.0f);
    light.range   = 4.0f;
    light.enabled = false;
    EXPECT_TRUE(lights.Update(first, light));
    EXPECT_TRUE(lights.Remove(second));
    EXPECT_FALSE(lights.Remove(second));
    zen::rc::SceneUniformData next;
    lights.WriteUniforms(next);
    EXPECT_EQ(next.lightInfo.x, 0.0f);
    EXPECT_EQ(snapshot.lightInfo.x, 2.0f);
    const zen::rc::LightId third = lights.Add(light);
    EXPECT_GT(third, second);
    EXPECT_EQ(lights.Find(second), nullptr);
}

TEST(SceneLightingTests, StructuralChangesResetGIWhileLightAnimationRetainsHistory)
{
    zen::rc::SceneLights lights;
    zen::rc::SceneLight light;
    const zen::rc::LightId first  = lights.Add(light);
    const zen::rc::LightId second = lights.Add(light);
    uint64_t structure            = lights.GetStructureRevision();
    light.position.x += 1;
    light.intensity *= 0.5f;
    EXPECT_TRUE(lights.Update(first, light));
    EXPECT_EQ(lights.GetStructureRevision(), structure);
    light.enabled = false;
    EXPECT_TRUE(lights.Update(first, light));
    EXPECT_GT(lights.GetStructureRevision(), structure);
    structure = lights.GetStructureRevision();
    EXPECT_TRUE(lights.Remove(second));
    light.enabled = true;
    EXPECT_NE(lights.Add(light), 0);
    EXPECT_GT(lights.GetStructureRevision(), structure);
    zen::rc::SceneUniformData snapshot;
    lights.WriteUniforms(snapshot);
    EXPECT_EQ(snapshot.lightInfo.x, 1);
    EXPECT_EQ(snapshot.lights[1].colorIntensity, zen::Vec4(0));
}

TEST(SceneLightingTests, EnforcesCapacityWithoutReusingRemovedIds)
{
    zen::rc::SceneLights lights;
    zen::rc::LightId first = 0;
    for (uint32_t i = 0; i < zen::rc::MaxSceneLights; ++i)
    {
        const zen::rc::LightId id = lights.Add({});
        ASSERT_NE(id, 0u);
        if (i == 0)
        {
            first = id;
        }
    }
    const uint64_t revision = lights.GetRevision();
    EXPECT_EQ(lights.Add({}), 0u);
    EXPECT_EQ(lights.GetRevision(), revision);
    EXPECT_TRUE(lights.Remove(first));
    EXPECT_GT(lights.Add({}), zen::rc::MaxSceneLights);
    std::istringstream input("light_count=33");
    zen::platform::ConfigLoader config(input);
    EXPECT_TRUE(zen::rc::LoadSceneLights(config).empty());
}

TEST(SceneLightingTests, InvalidNumbersAndVectorsPreserveDefaults)
{
    std::istringstream input("number=inf\nvector=1,2,3 trailing\nboolean=1\nvoxel_resolution=65");
    zen::platform::ConfigLoader config(input);
    float number = 2;
    zen::Vec3 vector(4);
    bool boolean = false;
    EXPECT_FALSE(config.ReadNumber("number", number));
    EXPECT_FALSE(config.ReadVec3("vector", vector));
    EXPECT_FALSE(config.ReadBool("boolean", boolean));
    EXPECT_EQ(number, 2);
    EXPECT_EQ(vector, zen::Vec3(4));
    EXPECT_FALSE(boolean);
    EXPECT_EQ(config.GetVoxelResolution(), 256u);
}

TEST(SceneLightingTests, LargeFiniteDirectionsSurviveAddUpdateAndUniformSnapshots)
{
    struct Case
    {
        zen::Vec3 direction;
        zen::Vec3 expected;
    };
    const float largest = std::numeric_limits<float>::max();
    const Case cases[]  = {
        {zen::Vec3(0, 0, -1e20f), zen::Vec3(0, 0, -1)},
        {zen::Vec3(largest, largest, largest), zen::Vec3(1.0f / std::sqrt(3.0f))},
        {zen::Vec3(largest * 0.6f, -largest * 0.8f, 0), zen::Vec3(0.6f, -0.8f, 0)}};
    for (zen::rc::SceneLightType type :
         {zen::rc::SceneLightType::eDirectional, zen::rc::SceneLightType::eSpot})
    {
        for (const Case& test : cases)
        {
            zen::rc::SceneLights lights;
            zen::rc::SceneLight light;
            light.type                = type;
            light.direction           = test.direction;
            const zen::rc::LightId id = lights.Add(light);
            ASSERT_NE(id, 0u);
            zen::rc::SceneUniformData snapshot;
            lights.WriteUniforms(snapshot);
            EXPECT_NEAR(glm::length(zen::Vec3(snapshot.lights[0].directionType) - test.expected),
                        0.0f, 1e-6f);

            light.direction = -test.direction;
            EXPECT_TRUE(lights.Update(id, light));
            zen::rc::SceneUniformData updated;
            lights.WriteUniforms(updated);
            EXPECT_NEAR(glm::length(zen::Vec3(updated.lights[0].directionType) + test.expected),
                        0.0f, 1e-6f);
            EXPECT_NEAR(glm::length(zen::Vec3(snapshot.lights[0].directionType) - test.expected),
                        0.0f, 1e-6f);

            const uint64_t revision = lights.GetRevision();
            for (const zen::Vec3& invalid :
                 {zen::Vec3(0), zen::Vec3(1e-8f), zen::Vec3(std::numeric_limits<float>::infinity()),
                  zen::Vec3(std::numeric_limits<float>::quiet_NaN())})
            {
                light.direction = invalid;
                EXPECT_FALSE(lights.Update(id, light));
                EXPECT_EQ(lights.GetRevision(), revision);
                EXPECT_EQ(lights.Find(id)->direction, -test.direction);
                zen::rc::SceneUniformData unchanged;
                lights.WriteUniforms(unchanged);
                EXPECT_EQ(unchanged.lights[0].directionType, updated.lights[0].directionType);
            }
        }
    }
}
