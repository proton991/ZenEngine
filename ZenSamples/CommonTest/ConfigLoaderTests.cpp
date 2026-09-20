#include "Platform/ConfigLoader.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include <gtest/gtest.h>
#include <sstream>

namespace
{
using zen::platform::ConfigLoader;
using zen::platform::VoxelizerMode;
using zen::rc::AsyncComputeMode;
using zen::rc::AsyncComputeStatus;

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
    std::istringstream input("\r\n"
                             " # Configuration with Windows line endings\r\n"
                             " model_base_path = /assets/Models \r\n"
                             " default_model = Sponza # scene\r\n"
                             " skybox_model = Box\r\n"
                             " voxelizer = geom # prefer geometry\r\n"
                             "#voxelizer=comp\r\n"
                             " \t# voxelizer = comp\r\n"
                             "ignored line\r\n");
    ConfigLoader config(input);

    EXPECT_EQ(config.GetVoxelizerMode(), VoxelizerMode::eGeometry);
    EXPECT_EQ(config.GetDefaultGLTFModelPath(), "/assets/Models/Sponza/glTF/Sponza.gltf");
    EXPECT_EQ(config.GetSkyboxModelPath(), "/assets/Models/Box/glTF/Box.gltf");
    EXPECT_EQ(config.GetGLTFModelPath("Box"), "/assets/Models/Box/glTF/Box.gltf");
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
} // namespace
