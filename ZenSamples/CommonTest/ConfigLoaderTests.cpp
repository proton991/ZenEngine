#include "Platform/ConfigLoader.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include <gtest/gtest.h>
#include <sstream>

namespace
{
using zen::platform::ConfigLoader;
using zen::platform::VoxelizerMode;

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
