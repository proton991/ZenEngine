#include "Math/Math.h"
#include "ZenApp/CameraPath.h"
#include "ZenApp/LightPath.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace
{
using namespace zen;
using namespace zen::runtime;

TEST(CameraPath, HoldsEndpointsAndInterpolatesBySceneFrame)
{
    const HeapVector<CameraPathKey> keys{{4, Vec3(0, 0, 2), Vec3(0)}, {12, Vec3(4, 0, 2), Vec3(4, 0, 0)}};
    NavigationCameraState           base;
    base.aspect = 2;
    EXPECT_EQ(CameraPathState(keys, 1, base).eye, keys.front().eye);
    EXPECT_EQ(CameraPathState(keys, 4, base).eye, keys.front().eye);
    EXPECT_EQ(CameraPathState(keys, 8, base).eye, Vec3(2, 0, 2));
    EXPECT_EQ(CameraPathState(keys, 8, base).target, Vec3(2, 0, 0));
    EXPECT_EQ(CameraPathState(keys, 20, base).eye, keys.back().eye);
    EXPECT_EQ(CameraPathState(keys, 8, base).aspect, base.aspect);

    // A subpixel change in a held view can flip raster coverage on a thin silhouette.
    const Vec3                      eye(-0.051807632f, -0.199212402f, -0.068445134f);
    const Vec3                      target(-1.015718412f, -0.004898672f, -0.250428904f);
    const HeapVector<CameraPathKey> hold{{1030, eye, target}, {1094, eye, target}};
    for (uint64_t frame = 1030; frame <= 1094; ++frame)
    {
        const NavigationCameraState pose = CameraPathState(hold, frame, base);
        EXPECT_EQ(pose.eye, eye) << frame;
        EXPECT_EQ(pose.target, target) << frame;
    }
}

TEST(CameraPath, RejectsInvalidFilesWithoutPublishingPartialKeys)
{
    const std::filesystem::path path        = std::filesystem::temp_directory_path() / "zen-camera-path-test.json";
    const char*                 documents[] = {
        R"({"keys":[{"frame":1,"eye":[0,0,2],"target":[0,0,0]},{"frame":2,"eye":[1,0,2],"target":[0,0,0]}]})",
        R"({"keys":[{"frame":1,"eye":[0,0,2],"target":[0,0,0]},{"frame":1,"eye":[1,0,2],"target":[0,0,0]}]})",
        R"({"keys":[{"frame":0,"eye":[0,0,2],"target":[0,0,0]},{"frame":2,"eye":[1,0,2],"target":[0,0,0]}]})",
        R"({"keys":[{"frame":1,"eye":[0,0,2],"target":[0,0,0]},{"frame":2,"eye":[1e100,0,2],"target":[0,0,0]}]})",
        R"({"keys":[{"frame":1,"eye":[0,0,2],"target":[0,0,0]}]})",
        R"({"keys":)"};
    for (size_t index = 0; index < std::size(documents); ++index)
    {
        SCOPED_TRACE(index);
        std::ofstream file(path);
        file << documents[index];
        file.close();
        HeapVector<CameraPathKey> keys{{99, Vec3(1), Vec3(0)}};
        std::string               error;
        EXPECT_EQ(ReadCameraPath(path.string(), keys, error), index == 0);
        EXPECT_EQ(keys.size(), index == 0 ? 2u : 0u);
        EXPECT_EQ(error.empty(), index == 0);
    }
    std::error_code fileError;
    std::filesystem::remove(path, fileError);
    EXPECT_FALSE(fileError);
}

TEST(LightPath, HoldsKeysAndRejectsInvalidInputWithoutPublishing)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "zen-light-path-test.json";
    const char* documents[] = {R"({"lightId":12,"keys":[{"frame":1,"position":[0,1,2]},{"frame":4,"position":[3,4,5]}]})",
                               R"({"lightId":0,"keys":[{"frame":1,"position":[0,1,2]}]})",
                               R"({"lightId":12,"keys":[{"frame":1,"position":[0,1,2]},{"frame":1,"position":[3,4,5]}]})",
                               R"({"lightId":12,"keys":[{"frame":1,"position":[0,1e100,2]}]})", R"({"lightId":12,"keys":[]})"};
    LightPath   result;
    for (size_t index = 0; index < std::size(documents); ++index)
    {
        SCOPED_TRACE(index);
        std::ofstream file(path);
        file << documents[index];
        file.close();
        std::string error;
        EXPECT_EQ(ReadLightPath(path.string(), result, error), index == 0);
        EXPECT_EQ(result.id, 12u);
        ASSERT_EQ(result.keys.size(), 2u);
        EXPECT_EQ(LightPathPose(result, 1).position, Vec3(0, 1, 2));
        EXPECT_EQ(LightPathPose(result, 3).position, Vec3(0, 1, 2));
        EXPECT_EQ(LightPathPose(result, 4).position, Vec3(3, 4, 5));
        EXPECT_EQ(LightPathPose(result, 10).position, Vec3(3, 4, 5));
    }
    const char* scenePaths[] = {
        R"({"target":"environment","keys":[{"frame":3,"intensity":2,"rotationDegrees":90}]})",
        R"({"target":"environment","keys":[{"frame":3,"intensity":2,"rotationDegrees":90},{"frame":4,"intensity":1}]})",
        R"({"target":"material","index":4294967296,"keys":[{"frame":1,"color":[0,0,0],"emission":[1,2,3]}]})",
        R"({"target":"material","index":0,"keys":[{"frame":1,"color":[2,0,0],"emission":[1,2,3]}]})",
        R"({"target":"animation","keys":[{"frame":2,"seconds":-1}]})",
        R"({"target":"light","lightId":12,"keys":[{"frame":1,"position":[0,0,0],"color":[1,1,1],"intensity":1,"enabled":1}]})"};
    for (size_t index = 0; index < std::size(scenePaths); ++index)
    {
        SCOPED_TRACE(index);
        std::ofstream file(path);
        file << scenePaths[index];
        file.close();
        std::string message;
        EXPECT_EQ(ReadLightPath(path.string(), result, message), index == 0);
        EXPECT_EQ(result.target, PathTarget::Environment);
        ASSERT_EQ(result.keys.size(), 1u);
        EXPECT_FLOAT_EQ(LightPathPose(result, 1).rotationDegrees, 90);
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    EXPECT_FALSE(error);
}
} // namespace
