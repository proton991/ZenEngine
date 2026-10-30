#include "Runtime/RuntimeLaunchSettings.h"
#include "Runtime/SceneClock.h"
#include "Platform/ConfigLoader.h"
#include "Platform/InputController.h"
#include <gtest/gtest.h>
#include <fstream>
#include <limits>
#include <sstream>

namespace zen::runtime
{
namespace
{
std::string Utf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

RuntimeLaunchSettings Fixture()
{
    RuntimeLaunchSettings setup;
    setup.packagePath            = Utf8(std::filesystem::absolute("scene.zenasset"));
    setup.configurationDirectory = Utf8(std::filesystem::absolute("."));
    setup.engineConfiguration    = "voxel_resolution=128\nmodel_base_path=models\n";
    setup.rendering.scenePath    = "source with spaces/scene.gltf";
    return setup;
}

void ExpectSameMatrix(const Mat4& left, const Mat4& right)
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            EXPECT_NEAR(left[column][row], right[column][row], 0.00001f);
        }
    }
}

void ExpectCameraRays(const NavigationCamera& navigation)
{
    const sg::Camera&             camera     = navigation.GetCamera();
    const asset::CameraParameters parameters = camera.GetParameters();
    const Mat4                    view       = camera.GetViewMatrix();
    const Vec3                    right(view[0][0], view[1][0], view[2][0]);
    const Vec3                    up(view[0][1], view[1][1], view[2][1]);
    const Vec3                    forward = -Vec3(view[0][2], view[1][2], view[2][2]);
    const float                   height =
        parameters.orthographic ? parameters.ymag : parameters.nearPlane * std::tan(parameters.verticalFov * 0.5f);
    const float width = parameters.orthographic ? parameters.xmag : height * parameters.aspect;
    for (Vec2 cursor : {Vec2(0.5f), Vec2(0.1f, 0.8f), Vec2(1, 0)})
    {
        SCOPED_TRACE(testing::Message() << "cursor=" << cursor.x << "," << cursor.y);
        const Vec2 ndc               = cursor * 2.0f - Vec2(1);
        const Vec3 offset            = forward * parameters.nearPlane + right * ndc.x * width - up * ndc.y * height;
        const Vec3 expectedDirection = parameters.orthographic ? forward : glm::normalize(offset);
        Vec3       origin;
        Vec3       direction;
        navigation.MakeRay(cursor, origin, direction);
        EXPECT_LT(glm::distance(origin, camera.GetPos() + offset), 0.0001f);
        EXPECT_LT(glm::distance(direction, expectedDirection), 0.00001f);
        EXPECT_NEAR(glm::length(direction), 1.0f, 0.00001f);
    }
}
} // namespace

TEST(RuntimeConfig, ExplicitSourceDoesNotUseGlobalFile)
{
    const std::string base = Utf8(std::filesystem::absolute("isolated-config"));
    ASSERT_TRUE(platform::ConfigLoader::SetExplicitSource("voxel_resolution=64\nmodel_base_path=owned-models\n", base));
    const platform::ConfigLoader& config = platform::ConfigLoader::GetInstance();
    EXPECT_EQ(config.GetVoxelResolution(), 64u);
    EXPECT_EQ(config.GetModelBasePath(), base + "/owned-models");
    // engine.cfg names a default model; the explicit source does not.
    EXPECT_FALSE(config.HasKey("default_model"));
    EXPECT_FALSE(platform::ConfigLoader::SetExplicitSource("voxel_resolution=256", base));
    EXPECT_EQ(config.GetVoxelResolution(), 64u);
}

TEST(RuntimeLaunch, RoundTripFullConfigurationAndWideIds)
{
    RuntimeLaunchSettings source         = Fixture();
    source.sourcePath                    = "source \"scene\" / \xE5\x9C\xBA\xE6\x99\xAF.gltf";
    source.processingProfile             = "desktop";
    source.rendering.algorithm           = rc::RenderAlgorithm::eVoxelGI;
    source.rendering.normalizationCenter = Vec3(2, 3, 4);
    source.rendering.normalizationScale  = 0.125f;
    source.rendering.gi.cone.samples     = 2;
    source.rendering.gi.cone.SetAllFeatureProviders(rc::VoxelGISettings::RayProvider::Hardware);
    source.rendering.gi.reflectanceBudgetBytes     = 4294967296ull;
    source.rendering.gi.cone.previousFrameRadiance = true;
    source.rendering.gi.cone.probeSpacing          = 8;
    source.rendering.gi.cone.indirectIntensity     = 0.25f;
    source.rendering.cameraLight.position          = Vec3(1, 2, 3);
    source.rendering.cameraLight.followCamera      = false;
    source.rendering.environment.rotationDegrees   = 65.0f;
    source.rendering.lights.push_back({9007199254740993ull, rc::LightOrigin::eManual, {}});
    source.rendering.debug.lightId = source.rendering.lights[0].id;
    source.camera.eye              = Vec3(3, 2, 4);
    source.camera.orthographic     = true;
    source.run.followWindow        = false;
    source.run.presentMode         = RHIPresentMode::eMailbox;
    source.run.rhiMode             = RHIExecutionMode::eInline;
    source.animationEnabled        = false;

    std::string json;
    std::string error;
    ASSERT_TRUE(EncodeRuntimeLaunchSettings(source, json, error)) << error;
    RuntimeLaunchSettings loaded;
    ASSERT_TRUE(DecodeRuntimeLaunchSettings(json, loaded, error)) << error;
    EXPECT_EQ(loaded.sourcePath, source.sourcePath);
    EXPECT_EQ(loaded.rendering.gi, source.rendering.gi);
    EXPECT_EQ(loaded.rendering.lights[0].id, 9007199254740993ull);
    EXPECT_EQ(loaded.rendering.debug.lightId, 9007199254740993ull);
    EXPECT_EQ(loaded.camera.eye, source.camera.eye);
    EXPECT_TRUE(loaded.camera.orthographic);
    EXPECT_FALSE(loaded.run.followWindow);
    EXPECT_EQ(loaded.run.rhiMode, RHIExecutionMode::eInline);
    EXPECT_EQ(loaded.engineConfiguration, source.engineConfiguration);
    EXPECT_FALSE(loaded.animationEnabled);
    std::string encodedAgain;
    ASSERT_TRUE(EncodeRuntimeLaunchSettings(loaded, encodedAgain, error)) << error;
    EXPECT_EQ(encodedAgain, json);
}

TEST(RuntimeLaunch, RejectedInputPreservesPreviousSetup)
{
    RuntimeLaunchSettings current = Fixture();
    current.sourcePath            = "keep-current";
    std::string good;
    std::string error;
    ASSERT_TRUE(EncodeRuntimeLaunchSettings(Fixture(), good, error));
    const std::string version         = "\"version\":1";
    const std::string invalidFields[] = {"\"version\":2", "\"version\":-1", "\"version\":1,\"version\":1",
                                         "\"version\":4294967296", "\"version\":1.5"};
    for (const std::string& field : invalidFields)
    {
        std::string json = good;
        json.replace(json.find(version), version.size(), field);
        EXPECT_FALSE(DecodeRuntimeLaunchSettings(json, current, error));
        EXPECT_EQ(current.sourcePath, "keep-current");
        EXPECT_FALSE(error.empty());
    }
    EXPECT_FALSE(DecodeRuntimeLaunchSettings("{}", current, error));
    EXPECT_FALSE(DecodeRuntimeLaunchSettings("not json", current, error));
    EXPECT_FALSE(DecodeRuntimeLaunchSettings(std::string(MaxRuntimeSetupBytes + 1, ' '), current, error));
    EXPECT_EQ(current.sourcePath, "keep-current");
}

TEST(RuntimeLaunch, RejectsNonFiniteAndInvalidRangesBeforeWriting)
{
    RuntimeLaunchSettings settings = Fixture();
    std::string           json     = "unchanged";
    std::string           error;
    settings.rendering.environment.intensity = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(EncodeRuntimeLaunchSettings(settings, json, error));
    EXPECT_EQ(json, "unchanged");
    settings            = Fixture();
    settings.camera.eye = settings.camera.target;
    EXPECT_FALSE(EncodeRuntimeLaunchSettings(settings, json, error));
    settings             = Fixture();
    settings.packagePath = "relative.zenasset";
    EXPECT_FALSE(EncodeRuntimeLaunchSettings(settings, json, error));
    settings                 = Fixture();
    settings.run.presentMode = static_cast<RHIPresentMode>(255);
    EXPECT_FALSE(EncodeRuntimeLaunchSettings(settings, json, error));
    settings = Fixture();
    settings.rendering.lights.push_back({1, rc::LightOrigin::eManual, {}});
    settings.rendering.lights.push_back({1, rc::LightOrigin::eManual, {}});
    EXPECT_FALSE(EncodeRuntimeLaunchSettings(settings, json, error));
}

TEST(RuntimeLaunch, AtomicReplacementAndFailedWritePreservePreviousFile)
{
    std::error_code             fileError;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(fileError) / "ZenRuntimeLaunchTest";
    ASSERT_FALSE(fileError);
    std::filesystem::create_directories(directory, fileError);
    ASSERT_FALSE(fileError);
    const std::filesystem::path file     = directory / std::filesystem::u8path("setup \xE5\x9C\xBA\xE6\x99\xAF.zenrender");
    RuntimeLaunchSettings       settings = Fixture();
    std::string                 error;
    ASSERT_TRUE(WriteRuntimeLaunchSettings(file, settings, error)) << error;
    settings.sourcePath = "replacement";
    ASSERT_TRUE(WriteRuntimeLaunchSettings(file, settings, error)) << error;
    settings.camera.aspect = 0.0f;
    EXPECT_FALSE(WriteRuntimeLaunchSettings(file, settings, error));
    RuntimeLaunchSettings loaded;
    ASSERT_TRUE(ReadRuntimeLaunchSettings(file, loaded, error)) << error;
    EXPECT_EQ(loaded.sourcePath, "replacement");
    EXPECT_EQ(loaded.camera.aspect, 1.0f);
    std::filesystem::remove(file, fileError);
    EXPECT_FALSE(fileError);
}

TEST(RuntimeCamera, SnapshotPreservesProjectionPoseAndSubsequentNavigation)
{
    NavigationCamera editor;
    editor.SetExtent(1600, 900);
    editor.Frame(sg::AABB(Vec3(-2), Vec3(2)));
    editor.OrbitBy(Vec2(0.45f, -0.2f));
    editor.SetOrthographic(true);
    NavigationCamera player;
    ASSERT_TRUE(player.SetState(editor.GetState()));
    ExpectSameMatrix(editor.GetCamera().GetViewMatrix(), player.GetCamera().GetViewMatrix());
    ExpectSameMatrix(editor.GetCamera().GetProjectionMatrix(), player.GetCamera().GetProjectionMatrix());
    CameraInput input;
    input.pan     = Vec2(2, -3);
    input.orbit   = Vec2(-1, 2);
    input.move    = Vec3(1, 0, 1);
    input.seconds = 0.02f;
    input.dolly   = 0.5f;
    editor.Apply(input);
    player.Apply(input);
    ExpectSameMatrix(editor.GetCamera().GetViewMatrix(), player.GetCamera().GetViewMatrix());
    ExpectSameMatrix(editor.GetCamera().GetProjectionMatrix(), player.GetCamera().GetProjectionMatrix());
    NavigationCameraState invalid = player.GetState();
    invalid.nearPlane             = invalid.farPlane;
    EXPECT_FALSE(player.SetState(invalid));
    ExpectSameMatrix(editor.GetCamera().GetProjectionMatrix(), player.GetCamera().GetProjectionMatrix());
}

TEST(RuntimeCamera, LegacyTargetPoseRetainsLookAtRounding)
{
    for (const Vec3 eye : {Vec3(3, 2, 4), Vec3(1234.5f, 60, -98), Vec3(-0.2f, 0.8f, 3)})
    {
        sg::Camera camera(eye, Vec3(0), 1.5f);
        for (const Vec3 target : {Vec3(0), Vec3(0.3f, 0.7f, -1)})
        {
            camera.SetPose(eye, target);
            const Vec2 angles = camera.GetViewAngles();
            const Vec3 yawFront(std::cos(angles.x), 0, std::sin(angles.x));
            const Vec3 front    = glm::normalize(std::cos(angles.y) * yawFront + std::sin(angles.y) * Vec3(0, 1, 0));
            const Vec3 right    = glm::normalize(glm::normalize(glm::cross(yawFront, Vec3(0, 1, 0))));
            const Vec3 up       = glm::normalize(glm::cross(right, front));
            const Mat4 expected = glm::lookAt(eye, eye + front, up);
            const Mat4 actual   = camera.GetViewMatrix();
            for (int column = 0; column < 4; ++column)
            {
                for (int row = 0; row < 4; ++row)
                {
                    EXPECT_EQ(actual[column][row], expected[column][row]);
                }
            }
        }
    }
}
TEST(RuntimeCamera, PlatformMouseGesturesMatchEditorScreenCoordinates)
{
    platform::KeyboardMouseInput& keys      = platform::KeyboardMouseInput::GetInstance();
    const platform::MouseButton   buttons[] = {platform::MouseButton::Right, platform::MouseButton::Middle,
                                               platform::MouseButton::Left};
    const Vec2                    drags[]   = {Vec2(12, 18), Vec2(-7, -23)};
    keys.SetUICapture(false, false);
    keys.Resume();
    for (platform::MouseButton button : buttons)
    {
        for (Vec2 drag : drags)
        {
            keys.Reset();
            keys.SetCursorPos(100, 100);
            keys.PressMouseButton(button);
            CameraInput editorInput;
            editorInput.seconds   = 0.02f;
            editorInput.moveSpeed = 2.0f;
            if (button == platform::MouseButton::Right)
            {
                editorInput.look = drag;
                editorInput.move = Vec3(1, 1, 1);
                editorInput.fast = true;
                keys.PressKey(platform::Key::D);
                keys.PressKey(platform::Key::E);
                keys.PressKey(platform::Key::W);
                keys.PressKey(platform::Key::LeftShift);
            }
            else if (button == platform::MouseButton::Middle)
            {
                editorInput.pan = drag;
            }
            else
            {
                editorInput.orbit = drag;
                keys.PressKey(platform::Key::LeftAlt);
            }
            keys.SetCursorPos(100 + drag.x, 100 + drag.y);
            NavigationCamera editor;
            NavigationCamera player;
            editor.SetExtent(960, 540);
            player.SetExtent(960, 540);
            editor.Apply(editorInput);
            player.Apply(ReadNavigationCameraInput(0.02f, 2.0f, 0));
            ExpectSameMatrix(editor.GetCamera().GetViewMatrix(), player.GetCamera().GetViewMatrix());
        }
    }
    keys.Reset();
    CameraInput editorInput;
    editorInput.dolly = 1;
    NavigationCamera editor;
    NavigationCamera player;
    editor.Apply(editorInput);
    player.Apply(ReadNavigationCameraInput(0, 1, 1));
    ExpectSameMatrix(editor.GetCamera().GetViewMatrix(), player.GetCamera().GetViewMatrix());
}

TEST(RuntimeCamera, AnimatedCameraKeepsProjectionAndPoseThroughResizeAndNavigation)
{
    for (bool orthographic : {false, true})
    {
        for (bool fixedAspect : {false, true})
        {
            NavigationCamera navigation;
            sg::SceneCamera  authored("animated");
            authored.worldMatrix  = glm::inverse(glm::lookAt(Vec3(3, 2, 4), Vec3(1, 1, 0), Vec3(0.3f, 1, 0)));
            authored.orthographic = orthographic;
            authored.fixedAspect  = fixedAspect;
            authored.infiniteFar  = !orthographic;
            authored.aspect       = 1.5f;
            authored.verticalFov  = glm::radians(42.0f);
            authored.nearPlane    = 0.1f;
            authored.farPlane     = 250;
            authored.xmag         = 4;
            authored.ymag         = 2;
            navigation.GetCamera().SetupFromSceneCamera(authored, 1.5f);
            const Mat4 animatedView = navigation.GetCamera().GetViewMatrix();
            navigation.SynchronizeFromCamera();
            navigation.SetExtent(800, 400);
            ExpectSameMatrix(navigation.GetCamera().GetViewMatrix(), animatedView);
            const Mat4                    projection = navigation.GetCamera().GetProjectionMatrix();
            const asset::CameraParameters parameters = navigation.GetCamera().GetParameters();
            EXPECT_FLOAT_EQ(parameters.aspect, fixedAspect ? 1.5f : 2.0f);
            EXPECT_EQ(parameters.orthographic, orthographic);
            EXPECT_EQ(parameters.infiniteFar, !orthographic);
            EXPECT_FLOAT_EQ(parameters.verticalFov, authored.verticalFov);
            EXPECT_FLOAT_EQ(parameters.nearPlane, authored.nearPlane);
            EXPECT_FLOAT_EQ(parameters.farPlane, authored.farPlane);
            CameraInput input;
            input.move    = Vec3(0, 1, 0);
            input.seconds = 0.02f;
            navigation.Apply(input);
            EXPECT_LT(glm::distance(navigation.GetCamera().GetPos(), Vec3(3, 2.02f, 4)), 0.00001f);
            ExpectSameMatrix(navigation.GetCamera().GetProjectionMatrix(), projection);
            const Mat4 movedView = navigation.GetCamera().GetViewMatrix();
            for (int column = 0; column < 3; ++column)
            {
                for (int row = 0; row < 3; ++row)
                {
                    EXPECT_NEAR(movedView[column][row], animatedView[column][row], 0.00001f);
                }
            }
        }
    }
}

TEST(RuntimeCamera, AnimatedPoleCameraDoesNotSnapOnTranslation)
{
    for (float vertical : {-1.0f, 1.0f})
    {
        for (Vec3 eye : {Vec3(0, 0, 2), Vec3(2, 3, 4)})
        {
            for (Vec3 authoredUp : {Vec3(0, 0, 1), Vec3(0.6f, 0, 0.8f), Vec3(1, 0, 0)})
            {
                for (uint32_t gesture = 0; gesture < 6; ++gesture)
                {
                    SCOPED_TRACE(testing::Message() << "vertical=" << vertical << " eye.x=" << eye.x << " up.x=" << authoredUp.x
                                                    << " gesture=" << gesture);
                    NavigationCamera navigation;
                    sg::SceneCamera  authored("vertical animation");
                    authored.worldMatrix = glm::inverse(glm::lookAt(eye, eye + Vec3(0, vertical, 0), authoredUp));
                    navigation.GetCamera().SetupFromSceneCamera(authored, 1);
                    navigation.SynchronizeFromCamera();
                    const Mat4  view = navigation.GetCamera().GetViewMatrix();
                    const Vec3  right(view[0][0], view[1][0], view[2][0]);
                    const Vec3  up(view[0][1], view[1][1], view[2][1]);
                    const Vec3  forward = -Vec3(view[0][2], view[1][2], view[2][2]);
                    CameraInput input;
                    Vec3        shift;
                    if (gesture < 2)
                    {
                        input.pan = gesture == 0 ? Vec2(10, 0) : Vec2(0, 10);
                        shift     = (gesture == 0 ? -right : up) * 0.04f;
                    }
                    else if (gesture < 5)
                    {
                        input.move[gesture - 2] = 1;
                        input.seconds           = 0.02f;
                        shift                   = (gesture == 2 ? right : gesture == 3 ? Vec3(0, 1, 0) : forward) * 0.02f;
                    }
                    else
                    {
                        input.dolly = 1;
                        shift       = forward * 2.0f * (1.0f - std::exp(-0.15f));
                    }
                    navigation.Apply(input);
                    EXPECT_LT(glm::distance(navigation.GetCamera().GetPos(), eye + shift), 0.00001f);
                    ExpectSameMatrix(navigation.GetCamera().GetViewMatrix(), glm::translate(view, -shift));
                }
            }
        }
    }
}

TEST(RuntimeCamera, AnimatedPoleCameraKeepsItsBasisDuringYawAndLeavingThePole)
{
    for (float vertical : {-1.0f, 1.0f})
    {
        for (float tilt : {0.0f, 0.00001f})
        {
            for (Vec3 authoredUp : {Vec3(0, 0, 1), Vec3(0.6f, 0, 0.8f), Vec3(1, 0, 0)})
            {
                for (uint32_t gesture = 0; gesture < 3; ++gesture)
                {
                    SCOPED_TRACE(testing::Message() << "vertical=" << vertical << " tilt=" << tilt << " up.x=" << authoredUp.x
                                                    << " gesture=" << gesture);
                    const Vec3       eye(0, 0, 2);
                    const Vec3       forward = glm::normalize(Vec3(tilt, vertical, -2 * tilt));
                    NavigationCamera navigation;
                    sg::SceneCamera  authored("vertical animation");
                    authored.worldMatrix = glm::inverse(glm::lookAt(eye, eye + forward, authoredUp));
                    navigation.GetCamera().SetupFromSceneCamera(authored, 1);
                    navigation.SynchronizeFromCamera();
                    const Mat4 world  = glm::inverse(navigation.GetCamera().GetViewMatrix());
                    const Vec3 target = navigation.GetState().target;
                    for (uint32_t step = 1; step <= 4; ++step)
                    {
                        CameraInput input;
                        if (gesture == 0)
                        {
                            input.look = Vec2(25, 0);
                            navigation.Apply(input);
                        }
                        else if (gesture == 1)
                        {
                            input.orbit = Vec2(25, 0);
                            navigation.Apply(input);
                        }
                        else
                        {
                            navigation.OrbitBy(Vec2(0.1f, 0));
                        }
                        const Mat4 rotation = glm::rotate(Mat4(1), -0.1f * step, Vec3(0, 1, 0));
                        Mat4       expected = rotation * world;
                        expected[3]         = Vec4(gesture == 0 ? eye : target + Vec3(expected[2]) * 2.0f, 1);
                        ExpectSameMatrix(navigation.GetCamera().GetViewMatrix(), glm::inverse(expected));
                    }
                    const Mat4  poleView = navigation.GetCamera().GetViewMatrix();
                    CameraInput input;
                    input.look = Vec2(0, vertical);
                    navigation.Apply(input);
                    const Mat4 tiltedView = navigation.GetCamera().GetViewMatrix();
                    float      difference = 0;
                    for (uint32_t column = 0; column < 3; ++column)
                    {
                        for (uint32_t row = 0; row < 3; ++row)
                        {
                            difference += std::abs(tiltedView[column][row] - poleView[column][row]);
                        }
                    }
                    EXPECT_GT(difference, 0.001f);
                    EXPECT_LT(difference, 0.03f);
                    input         = CameraInput{};
                    input.move    = Vec3(0, 1, 0);
                    input.seconds = 0.02f;
                    navigation.Apply(input);
                    ExpectSameMatrix(navigation.GetCamera().GetViewMatrix(), glm::translate(tiltedView, Vec3(0, -0.02f, 0)));
                }
            }
        }
    }
}

TEST(RuntimeCamera, AnimatedRolledCameraPanAndStrafeFollowTheCurrentView)
{
    for (const Vec3& authoredUp : {Vec3(0, 1, 0), Vec3(1, 0, 0), Vec3(-0.6f, 0.8f, 0)})
    {
        for (const Vec2& look : {Vec2(0), Vec2(23, -15)})
        {
            for (uint32_t gesture = 0; gesture < 3; ++gesture)
            {
                SCOPED_TRACE(testing::Message() << "up.x=" << authoredUp.x << " look.x=" << look.x << " gesture=" << gesture);
                sg::SceneCamera authored("rolled animation");
                authored.worldMatrix = glm::inverse(glm::lookAt(Vec3(0, 0, 2), Vec3(0), authoredUp));
                NavigationCamera navigation;
                navigation.GetCamera().SetupFromSceneCamera(authored, 1);
                navigation.SynchronizeFromCamera();
                NavigationCamera rotated = navigation;
                CameraInput      input;
                input.look = look;
                rotated.Apply(input);
                const Mat4 view = rotated.GetCamera().GetViewMatrix();
                const Vec3 right(view[0][0], view[1][0], view[2][0]);
                const Vec3 up(view[0][1], view[1][1], view[2][1]);
                Vec3       expectedShift;
                if (gesture == 0)
                {
                    input.pan     = Vec2(10, 0);
                    expectedShift = -right * 0.04f;
                }
                else if (gesture == 1)
                {
                    input.pan     = Vec2(0, -10);
                    expectedShift = -up * 0.04f;
                }
                else
                {
                    input.move    = Vec3(1, 0, 0);
                    input.seconds = 0.02f;
                    expectedShift = right * 0.02f;
                }
                navigation.Apply(input);
                EXPECT_LT(glm::distance(navigation.GetCamera().GetPos(), rotated.GetCamera().GetPos() + expectedShift),
                          0.00001f);
                ExpectSameMatrix(navigation.GetCamera().GetViewMatrix(), glm::translate(view, -expectedShift));
                ExpectSameMatrix(navigation.GetCamera().GetProjectionMatrix(), rotated.GetCamera().GetProjectionMatrix());
            }
        }
    }
}

TEST(RuntimeCamera, FrameFitsBoundsWithTheRetainedAnimatedProjection)
{
    const sg::AABB bounds(Vec3(-0.5f), Vec3(0.5f));
    for (bool orthographic : {false, true})
    {
        for (float fov : {1.0f, 10.0f, 70.0f})
        {
            for (float aspect : {0.5f, 1.0f, 2.0f})
            {
                SCOPED_TRACE(testing::Message() << "orthographic=" << orthographic << " fov=" << fov << " aspect=" << aspect);
                sg::SceneCamera authored("animated projection");
                authored.worldMatrix  = glm::inverse(glm::lookAt(Vec3(3, 2, 4), Vec3(0), Vec3(0.6f, 1, 0.2f)));
                authored.orthographic = orthographic;
                authored.verticalFov  = glm::radians(fov);
                authored.fixedAspect  = true;
                authored.aspect       = aspect;
                NavigationCamera navigation;
                navigation.GetCamera().SetupFromSceneCamera(authored, 1);
                navigation.SynchronizeFromCamera();
                navigation.SetExtent(320, 240);
                navigation.Frame(bounds);
                const asset::CameraParameters parameters = navigation.GetCamera().GetParameters();
                EXPECT_EQ(parameters.orthographic, orthographic);
                EXPECT_FLOAT_EQ(parameters.verticalFov, authored.verticalFov);
                EXPECT_FLOAT_EQ(parameters.aspect, aspect);
                const Mat4 projectionView =
                    navigation.GetCamera().GetProjectionMatrix() * navigation.GetCamera().GetViewMatrix();
                for (uint32_t corner = 0; corner < 8; ++corner)
                {
                    SCOPED_TRACE(corner);
                    const Vec4 point((corner & 1) ? 0.5f : -0.5f, (corner & 2) ? 0.5f : -0.5f, (corner & 4) ? 0.5f : -0.5f, 1);
                    const Vec4 clip = projectionView * point;
                    ASSERT_GT(clip.w, 0);
                    EXPECT_LE(std::abs(clip.x / clip.w), 1);
                    EXPECT_LE(std::abs(clip.y / clip.w), 1);
                    EXPECT_GE(clip.z / clip.w, 0);
                    EXPECT_LE(clip.z / clip.w, 1);
                }
                // Framing through a narrow lens may exceed the usual dolly range.
                // The next translation and wheel step must not snap the camera closer.
                const float framedDistance = glm::distance(navigation.GetState().eye, navigation.GetState().target);
                const Vec3  before         = navigation.GetCamera().GetPos();
                CameraInput input;
                input.move    = Vec3(0, 1, 0);
                input.seconds = 0.02f;
                navigation.Apply(input);
                EXPECT_LT(glm::distance(navigation.GetCamera().GetPos(), before + Vec3(0, 0.02f, 0)), 0.0001f);
                input       = CameraInput{};
                input.dolly = 0.5f;
                navigation.Apply(input);
                EXPECT_NEAR(glm::distance(navigation.GetState().eye, navigation.GetState().target),
                            framedDistance * std::exp(-0.075f), 0.0001f);
            }
        }
    }
}

TEST(RuntimeCamera, RaysMatchTheRetainedLensThroughEditorOperations)
{
    const Vec3 eye(3, 2, 4);
    const Mat4 worlds[] = {glm::inverse(glm::lookAt(eye, Vec3(0), Vec3(0.6f, 1, 0.2f))),
                           glm::inverse(glm::lookAt(eye, eye + Vec3(0, 1, 0), Vec3(0.6f, 0, 0.8f))),
                           glm::inverse(glm::lookAt(eye, eye - Vec3(0, 1, 0), Vec3(0, 0, 1)))};
    for (uint32_t projection = 0; projection < 3; ++projection)
    {
        for (bool fixedAspect : {false, true})
        {
            for (float fov : {1.0f, 70.0f})
            {
                for (const Mat4& world : worlds)
                {
                    SCOPED_TRACE(testing::Message() << "projection=" << projection << " fixed=" << fixedAspect << " fov=" << fov
                                                    << " up.y=" << world[1].y);
                    NavigationCamera navigation;
                    sg::SceneCamera  authored("animated lens");
                    authored.worldMatrix  = world;
                    authored.orthographic = projection == 2;
                    authored.infiniteFar  = projection == 1;
                    authored.fixedAspect  = fixedAspect;
                    authored.aspect       = 0.75f;
                    authored.verticalFov  = glm::radians(fov);
                    authored.nearPlane    = 0.1f;
                    authored.xmag         = 3;
                    authored.ymag         = 2;
                    navigation.GetCamera().SetupFromSceneCamera(authored, 1);
                    navigation.SynchronizeFromCamera();
                    ExpectCameraRays(navigation);
                    navigation.SetExtent(400, 800);
                    ExpectCameraRays(navigation);
                    CameraInput input;
                    input.look    = Vec2(12, 0);
                    input.move    = Vec3(1, 0, 0);
                    input.seconds = 0.02f;
                    input.dolly   = 0.5f;
                    navigation.Apply(input);
                    ExpectCameraRays(navigation);
                    navigation.Frame(sg::AABB(Vec3(-0.5f), Vec3(0.5f)));
                    ExpectCameraRays(navigation);
                    navigation.LookAlong(Vec3(0, 0, -1));
                    ExpectCameraRays(navigation);
                    navigation.SetOrthographic(!authored.orthographic);
                    ExpectCameraRays(navigation);
                    navigation.SetOrthographic(authored.orthographic);
                    ExpectCameraRays(navigation);
                }
            }
        }
    }
}

TEST(RuntimeClock, PauseExcludesElapsedTimeWithoutResumeCatchup)
{
    SceneClock                  clock;
    const SceneClock::TimePoint start{};
    EXPECT_EQ(clock.Advance(start), 0.0f);
    EXPECT_FLOAT_EQ(clock.Advance(start + std::chrono::milliseconds(20)), 0.02f);
    clock.SetPaused(true, start + std::chrono::milliseconds(20));
    const double frozen = clock.GetElapsed();
    EXPECT_EQ(clock.Advance(start + std::chrono::seconds(5)), 0.0f);
    EXPECT_EQ(clock.Advance(start + std::chrono::seconds(10), true), 0.0f);
    EXPECT_EQ(clock.GetElapsed(), frozen);
    clock.SetPaused(false, start + std::chrono::seconds(20));
    EXPECT_FLOAT_EQ(clock.Advance(start + std::chrono::seconds(20) + std::chrono::milliseconds(10)), 0.01f);
    EXPECT_NEAR(clock.GetElapsed(), 0.03, 0.000001);
    EXPECT_FLOAT_EQ(clock.Advance(start + std::chrono::seconds(30), true), 1.0f / 60.0f);
}
} // namespace zen::runtime
