#include "Platform/InputController.h"
#include "SceneGraph/Camera.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <gtest/gtest.h>
#include <array>
#include <cmath>

namespace
{
using namespace zen;

static Mat4 AuthoredCameraWorld(const Vec3& front, const Vec3& up)
{
    const Vec3 direction    = glm::normalize(front);

    const Vec3 right        = glm::normalize(glm::cross(direction, up));

    const Vec3 orthogonalUp = glm::normalize(glm::cross(right, direction));

    Mat4 world(1);

    world[0] = Vec4(right, 0);

    world[1] = Vec4(orthogonalUp, 0);

    world[2] = Vec4(-direction, 0);

    world[3] = Vec4(1, 2, 3, 1);

    return world;
}

static Mat4 ToyCarCameraWorld()
{
    // ToyCar/glTF/ToyCar.gltf Camera001 is a scene root. Retain its original TRS here
    // so this regression is portable and does not require the external asset checkout.
    const Vec3 position(-0.0169006381f, 0.0253599286f, 0.0302319955f);

    const Quat rotation(0.8971969f, -0.330993533f, -0.274300218f, -0.101194724f);

    const Vec3 scale(1.00000024f, 1.0f, 1.00000012f);

    return glm::translate(Mat4(1), position) * glm::mat4_cast(rotation) * glm::scale(Mat4(1), scale);
}

static Vec3 ViewForward(const Mat4& view)
{
    return -Vec3(view[0][2], view[1][2], view[2][2]);
}

static Vec3 ViewRight(const Mat4& view)
{
    return Vec3(view[0][0], view[1][0], view[2][0]);
}

static Vec3 ViewUp(const Mat4& view)
{
    return Vec3(view[0][1], view[1][1], view[2][1]);
}

static float OrientationDifference(const Mat4& first, const Mat4& second)
{
    float difference = 0;

    for (uint32_t column = 0; column < 3; ++column)
    {
        for (uint32_t row = 0; row < 3; ++row)
        {
            const float delta  = first[column][row] - second[column][row];

            difference        += delta * delta;
        }
    }

    return std::sqrt(difference);
}

static void ExpectOrientation(const Mat4& actual, const Mat4& expected)
{
    for (uint32_t column = 0; column < 3; ++column)
    {
        for (uint32_t row = 0; row < 3; ++row)
        {
            EXPECT_NEAR(actual[column][row], expected[column][row], 3e-5f);
        }
    }
}

static void ExpectPosition(const Vec3& actual, const Vec3& expected)
{
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        EXPECT_NEAR(actual[axis], expected[axis], 2e-5f);
    }
}

static void ExpectAuthoredOrientation(const sg::Camera& camera, const Mat4& world)
{
    const Vec3 position(world[3]);

    const Vec3 front = glm::normalize(-Vec3(world[2]));

    ExpectOrientation(camera.GetViewMatrix(), glm::lookAt(position, position + front, Vec3(world[1])));
}

static void ExpectFiniteOrthonormalView(const sg::Camera& camera)
{
    const Mat4 view                 = camera.GetViewMatrix();

    const std::array<Vec3, 3> basis = {ViewRight(view), ViewUp(view), ViewForward(view)};

    for (uint32_t column = 0; column < 4; ++column)
    {
        for (uint32_t row = 0; row < 4; ++row)
        {
            EXPECT_TRUE(std::isfinite(view[column][row]));
        }
    }

    for (uint32_t axis = 0; axis < basis.size(); ++axis)
    {
        EXPECT_NEAR(glm::length(basis[axis]), 1.0f, 2e-5f);

        for (uint32_t other = axis + 1; other < basis.size(); ++other)
        {
            EXPECT_NEAR(glm::dot(basis[axis], basis[other]), 0.0f, 2e-5f);
        }
    }

    const sg::CameraUniformData& published = *reinterpret_cast<const sg::CameraUniformData*>(camera.GetUniformData());

    const Mat4 projectionView              = camera.GetProjectionMatrix() * view;

    for (uint32_t column = 0; column < 4; ++column)
    {
        for (uint32_t row = 0; row < 4; ++row)
        {
            EXPECT_FLOAT_EQ(published.view[column][row], view[column][row]);

            EXPECT_NEAR(published.projViewMatrix[column][row], projectionView[column][row], 2e-5f);
        }
    }
}

class CameraInput : public testing::Test
{
protected:
    platform::KeyboardMouseInput& input = platform::KeyboardMouseInput::GetInstance();

    void SetUp() override
    {
        ResetInput();
    }

    void TearDown() override
    {
        ResetInput();
    }

    void ResetInput()
    {
        input.SetUICapture(false, false);

        input.Resume();

        input.Reset();

        input.SetCursorPos(0, 0);

        (void)input.CalculateCursorPositionDelta();

        input.SetDirty(false);
    }

    void MoveWithoutRotating(sg::Camera& camera)
    {
        const Mat4 orientation = camera.GetViewMatrix();

        const Vec3 front       = ViewForward(orientation);

        const Vec3 right       = ViewRight(orientation);

        const Vec3 vertical(0, 1, 0);

        const std::array<platform::Key, 6> keys = {platform::Key::W, platform::Key::S,         platform::Key::A,
                                                   platform::Key::D, platform::Key::LeftShift, platform::Key::LeftControl};

        const std::array<Vec3, 6> directions    = {front, -front, -right, right, vertical, -vertical};

        Vec3 expected                           = camera.GetPos();

        camera.SetSpeed(2.0f);

        constexpr float elapsed = 0.03125f;

        for (uint32_t key = 0; key < keys.size(); ++key)
        {
            SCOPED_TRACE(static_cast<uint16_t>(keys[key]));

            input.PressKey(keys[key]);

            for (uint32_t frame = 0; frame < 8; ++frame)
            {
                SCOPED_TRACE(frame);

                expected += directions[key] * 2.0f * elapsed;

                input.SetDirty(true);

                camera.Update(elapsed);

                ExpectPosition(camera.GetPos(), expected);

                ExpectOrientation(camera.GetViewMatrix(), orientation);

                ExpectFiniteOrthonormalView(camera);
            }

            input.ReleaseKey(keys[key]);
        }
    }

    void CheckHorizontalYaw(sg::Camera& camera)
    {
        const Mat4 initial                           = camera.GetViewMatrix();

        const Vec3 initialFront                      = ViewForward(initial);

        const Vec3 initialRight                      = ViewRight(initial);

        const Vec3 initialUp                         = ViewUp(initial);

        const Vec3 position                          = camera.GetPos();

        const std::array<int32_t, 8> cursorPositions = {30, 110, 190, 110, 30, 0, -75, 0};

        for (int32_t cursor : cursorPositions)
        {
            SCOPED_TRACE(cursor);

            const Mat4 rotation = glm::rotate(Mat4(1), glm::radians(-static_cast<float>(cursor) * 0.2f), Vec3(0, 1, 0));

            const Vec3 expectedFront(rotation * Vec4(initialFront, 0));

            const Vec3 expectedRight(rotation * Vec4(initialRight, 0));

            const Vec3 expectedUp(rotation * Vec4(initialUp, 0));

            input.SetCursorPos(cursor, 0);

            input.SetDirty(true);

            camera.Update(0.125f);

            ExpectOrientation(camera.GetViewMatrix(), glm::lookAt(position, position + expectedFront, expectedUp));

            ExpectPosition(ViewForward(camera.GetViewMatrix()), expectedFront);

            ExpectPosition(ViewRight(camera.GetViewMatrix()), expectedRight);

            ExpectPosition(ViewUp(camera.GetViewMatrix()), expectedUp);

            EXPECT_NEAR(ViewForward(camera.GetViewMatrix()).y, initialFront.y, 2e-6f);

            ExpectPosition(camera.GetPos(), position);

            ExpectFiniteOrthonormalView(camera);
        }

        ExpectOrientation(camera.GetViewMatrix(), initial);

        MoveWithoutRotating(camera);
    }
};

TEST_F(CameraInput, AutomaticPlacementUsesElevatedCornerAndFitsEveryCorner)
{
    const std::array<sg::AABB, 4> bounds = {sg::AABB(Vec3(-0.5f, -0.21f, -0.31f), Vec3(0.5f, 0.21f, 0.31f)),
                                            sg::AABB(Vec3(2, -3, 4), Vec3(12, 8, 8)), sg::AABB(Vec3(-2, 0, -1), Vec3(2, 0, 1)),
                                            sg::AABB(Vec3(4, 5, 6), Vec3(4, 5, 6))};

    for (float aspect : {0.5f, 1.0f, 2.0f})
    {
        SCOPED_TRACE(aspect);

        for (const sg::AABB& box : bounds)
        {
            UniquePtr<sg::Camera> camera = sg::Camera::CreateUnique(Vec3(0, 0, 2), Vec3(0), aspect);

            camera->SetupOnAABB(box);

            EXPECT_GT(camera->GetPos().x, box.GetCenter().x);

            EXPECT_GT(camera->GetPos().z, box.GetCenter().z);

            EXPECT_GT(camera->GetPos().y, box.GetMax().y);

            ExpectPosition(ViewForward(camera->GetViewMatrix()), glm::normalize(box.GetCenter() - camera->GetPos()));

            ExpectOrientation(camera->GetViewMatrix(), glm::lookAt(camera->GetPos(), box.GetCenter(), Vec3(0, 1, 0)));

            ExpectFiniteOrthonormalView(*camera);

            for (uint32_t corner = 0; corner < 8; ++corner)
            {
                const Vec3 point((corner & 1) ? box.GetMax().x : box.GetMin().x, (corner & 2) ? box.GetMax().y : box.GetMin().y,
                                 (corner & 4) ? box.GetMax().z : box.GetMin().z);

                const Vec4 clip = camera->GetProjectionMatrix() * camera->GetViewMatrix() * Vec4(point, 1);

                ASSERT_GT(clip.w, 0.0f);

                EXPECT_LT(std::abs(clip.x / clip.w), 1.0f);

                EXPECT_LT(std::abs(clip.y / clip.w), 1.0f);

                EXPECT_GT(clip.z / clip.w, 0.0f);

                EXPECT_LT(clip.z / clip.w, 1.0f);
            }

            UniquePtr<sg::Camera> factoryCamera = sg::Camera::CreateUniqueOnAABB(box.GetMin(), box.GetMax(), aspect);

            ExpectPosition(factoryCamera->GetPos(), camera->GetPos());

            ExpectFiniteOrthonormalView(*factoryCamera);
        }
    }
}

TEST_F(CameraInput, AutomaticCornerViewRemainsStableDuringMovement)
{
    UniquePtr<sg::Camera> camera = sg::Camera::CreateUnique(Vec3(0, 0, 2), Vec3(0), 1.5f);

    camera->SetupOnAABB(sg::AABB(Vec3(-0.5f), Vec3(0.5f)));

    MoveWithoutRotating(*camera);

    CheckHorizontalYaw(*camera);
}

TEST_F(CameraInput, MovementKeysPreserveAuthoredRollAndExactlyOrNearlyVerticalViews)
{
    const std::array<Mat4, 5> worlds = {AuthoredCameraWorld(Vec3(0.2f, -0.3f, -1), Vec3(0.6f, 1, 0.2f)),
                                        AuthoredCameraWorld(Vec3(0, 1, 0), Vec3(0, 0, 1)),
                                        AuthoredCameraWorld(Vec3(0, -1, 0), Vec3(0.8f, 0, 0.6f)),
                                        AuthoredCameraWorld(Vec3(1e-5f, 1, -2e-5f), Vec3(0, 0, 1)), ToyCarCameraWorld()};

    for (uint32_t index = 0; index < worlds.size(); ++index)
    {
        SCOPED_TRACE(index);

        ResetInput();

        sg::SceneCamera authored("movement_camera");

        authored.worldMatrix         = worlds[index];

        UniquePtr<sg::Camera> camera = sg::Camera::CreateUnique(Vec3(0, 0, 1), Vec3(0), 1.5f);

        camera->SetupFromSceneCamera(authored, 1.5f);

        ExpectAuthoredOrientation(*camera, authored.worldMatrix);

        MoveWithoutRotating(*camera);
    }
}

TEST_F(CameraInput, DirtyIdleFramesDoNotClampOrReconstructAuthoredPoleOrientations)
{
    for (float vertical : {-1.0f, 1.0f})
    {
        ResetInput();

        sg::SceneCamera authored("idle_camera");

        authored.worldMatrix         = AuthoredCameraWorld(Vec3(0, vertical, 0), Vec3(0.6f, 0, 0.8f));

        UniquePtr<sg::Camera> camera = sg::Camera::CreateUnique(Vec3(0, 0, 1), Vec3(0), 1.0f);

        camera->SetupFromSceneCamera(authored, 1.0f);

        const Mat4 initial  = camera->GetViewMatrix();

        const Vec3 position = camera->GetPos();

        for (uint32_t frame = 0; frame < 5; ++frame)
        {
            input.SetDirty(true);

            camera->Update(0.25f);

            ExpectOrientation(camera->GetViewMatrix(), initial);

            ExpectPosition(camera->GetPos(), position);

            ExpectFiniteOrthonormalView(*camera);
        }
    }
}

TEST_F(CameraInput, MouseMotionPublishesNewOrientationImmediatelyAndMovementUsesIt)
{
    const std::array<Mat4, 2> worlds = {AuthoredCameraWorld(Vec3(0.1f, -0.2f, -1), Vec3(0.7f, 1, 0.1f)), ToyCarCameraWorld()};

    for (const Mat4& world : worlds)
    {
        ResetInput();

        sg::SceneCamera authored("mouse_camera");

        authored.worldMatrix         = world;

        UniquePtr<sg::Camera> camera = sg::Camera::CreateUnique(Vec3(0, 0, 1), Vec3(0), 1.0f);

        camera->SetupFromSceneCamera(authored, 1.0f);

        const Mat4 initial  = camera->GetViewMatrix();

        const Vec3 position = camera->GetPos();

        input.SetCursorPos(40, -20);

        input.SetDirty(true);

        camera->Update(0.125f);

        EXPECT_GT(OrientationDifference(camera->GetViewMatrix(), initial), 0.01f);

        ExpectPosition(camera->GetPos(), position);

        ExpectFiniteOrthonormalView(*camera);

        MoveWithoutRotating(*camera);
    }
}

TEST_F(CameraInput, SmallMouseStepsAtAuthoredPolesStayFiniteAndKeyboardDoesNotApplyDelayedRotation)
{
    for (float vertical : {-1.0f, 1.0f})
    {
        ResetInput();

        sg::SceneCamera authored("pole_mouse_camera");

        authored.worldMatrix         = AuthoredCameraWorld(Vec3(0, vertical, 0), Vec3(0.6f, 0, 0.8f));

        UniquePtr<sg::Camera> camera = sg::Camera::CreateUnique(Vec3(0, 0, 1), Vec3(0), 1.0f);

        camera->SetupFromSceneCamera(authored, 1.0f);

        for (uint32_t frame = 0; frame < 24; ++frame)
        {
            const Mat4 previous  = camera->GetViewMatrix();

            const int32_t cursor = static_cast<int32_t>(frame + 1) * 3;

            input.SetCursorPos(cursor, -cursor);

            input.SetDirty(true);

            camera->Update(0.125f);

            EXPECT_GT(OrientationDifference(camera->GetViewMatrix(), previous), 1e-4f);

            EXPECT_LT(OrientationDifference(camera->GetViewMatrix(), previous), 0.06f);

            ExpectFiniteOrthonormalView(*camera);
        }

        MoveWithoutRotating(*camera);
    }
}

TEST_F(CameraInput, LargeMouseMotionClampsWorldPitchAndCanLeaveEitherLimit)
{
    sg::SceneCamera authored("pitch_limit_camera");

    authored.worldMatrix = ToyCarCameraWorld();

    const Vec3 referenceUp(0, 1, 0);

    UniquePtr<sg::Camera> camera = sg::Camera::CreateUnique(Vec3(0, 0, 1), Vec3(0), 1.0f);

    camera->SetupFromSceneCamera(authored, 1.0f);

    EXPECT_NEAR(glm::degrees(glm::asin(ViewForward(camera->GetViewMatrix()).y)), -40.5f, 0.1f);

    input.SetCursorPos(0, -20000);

    input.SetDirty(true);

    camera->Update(0.125f);

    ExpectFiniteOrthonormalView(*camera);

    EXPECT_NEAR(glm::dot(ViewForward(camera->GetViewMatrix()), referenceUp), glm::sin(glm::radians(89.0f)), 2e-5f);

    const Mat4 upper = camera->GetViewMatrix();

    input.SetCursorPos(0, -20001);

    input.SetDirty(true);

    camera->Update(0.125f);

    ExpectOrientation(camera->GetViewMatrix(), upper);

    input.SetCursorPos(0, -19991);

    input.SetDirty(true);

    camera->Update(0.125f);

    EXPECT_NEAR(glm::dot(ViewForward(camera->GetViewMatrix()), referenceUp), glm::sin(glm::radians(87.0f)), 2e-5f);

    input.SetCursorPos(0, 20000);

    input.SetDirty(true);

    camera->Update(0.125f);

    ExpectFiniteOrthonormalView(*camera);

    EXPECT_NEAR(glm::dot(ViewForward(camera->GetViewMatrix()), referenceUp), -glm::sin(glm::radians(89.0f)), 2e-5f);

    input.SetCursorPos(0, 19990);

    input.SetDirty(true);

    camera->Update(0.125f);

    EXPECT_NEAR(glm::dot(ViewForward(camera->GetViewMatrix()), referenceUp), -glm::sin(glm::radians(87.0f)), 2e-5f);

    MoveWithoutRotating(*camera);
}

TEST_F(CameraInput, StandardFirstPersonCameraKeepsWorldUpAndCurrentFrameMouseMovementConventions)
{
    const Vec3 eye(0, 0, 2);

    const Vec3 target(0);

    UniquePtr<sg::Camera> camera = sg::Camera::CreateUnique(eye, target, 1.0f);

    ExpectOrientation(camera->GetViewMatrix(), glm::lookAt(eye, target, Vec3(0, 1, 0)));

    MoveWithoutRotating(*camera);

    const Vec3 position = camera->GetPos();

    input.PressKey(platform::Key::W);

    input.SetCursorPos(20, -10);

    input.SetDirty(true);

    camera->Update(0.125f);

    const float yaw   = glm::radians(-86.0f);

    const float pitch = glm::radians(2.0f);

    const Vec3 expectedFront(glm::cos(yaw) * glm::cos(pitch), glm::sin(pitch), glm::sin(yaw) * glm::cos(pitch));

    ExpectPosition(ViewForward(camera->GetViewMatrix()), expectedFront);

    ExpectPosition(camera->GetPos(), position + expectedFront * 0.25f);

    ExpectFiniteOrthonormalView(*camera);

    input.ReleaseKey(platform::Key::W);

    MoveWithoutRotating(*camera);

    const Vec3 relocated(2, 1, 4);

    camera->SetPosition(relocated);

    ExpectPosition(camera->GetPos(), relocated);

    ExpectOrientation(camera->GetViewMatrix(), glm::lookAt(relocated, target, Vec3(0, 1, 0)));

    MoveWithoutRotating(*camera);
}

TEST_F(CameraInput, HorizontalMouseYawUsesWorldYAndPreservesAuthoredPitchAndRoll)
{
    const std::array<Mat4, 2> worlds = {ToyCarCameraWorld(), AuthoredCameraWorld(Vec3(0.2f, -0.3f, -1), Vec3(0.6f, 1, 0.2f))};

    for (uint32_t index = 0; index < worlds.size(); ++index)
    {
        SCOPED_TRACE(index);

        ResetInput();

        sg::SceneCamera authored("horizontal_yaw_camera");

        authored.worldMatrix         = worlds[index];

        UniquePtr<sg::Camera> camera = sg::Camera::CreateUnique(Vec3(0, 0, 1), Vec3(0), 1.0f);

        camera->SetupFromSceneCamera(authored, 1.0f);

        ExpectAuthoredOrientation(*camera, authored.worldMatrix);

        CheckHorizontalYaw(*camera);
    }
}

TEST_F(CameraInput, HorizontalYawAtAuthoredPolesDoesNotClampPitchOrResetRoll)
{
    const std::array<Mat4, 4> worlds = {AuthoredCameraWorld(Vec3(0, 1, 0), Vec3(0.6f, 0, 0.8f)),
                                        AuthoredCameraWorld(Vec3(0, -1, 0), Vec3(0.6f, 0, 0.8f)),
                                        AuthoredCameraWorld(Vec3(1e-5f, 1, -2e-5f), Vec3(0, 0, 1)),
                                        AuthoredCameraWorld(Vec3(1e-5f, -1, -2e-5f), Vec3(0, 0, 1))};

    for (uint32_t index = 0; index < worlds.size(); ++index)
    {
        SCOPED_TRACE(index);

        ResetInput();

        sg::SceneCamera authored("horizontal_pole_camera");

        authored.worldMatrix         = worlds[index];

        UniquePtr<sg::Camera> camera = sg::Camera::CreateUnique(Vec3(0, 0, 1), Vec3(0), 1.0f);

        camera->SetupFromSceneCamera(authored, 1.0f);

        ExpectAuthoredOrientation(*camera, authored.worldMatrix);

        CheckHorizontalYaw(*camera);
    }
}
} // namespace
