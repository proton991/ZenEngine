#include "Platform/InputController.h"
#include <gtest/gtest.h>

namespace
{
class InputControllerTest : public testing::Test
{
protected:
    zen::platform::KeyboardMouseInput& input = zen::platform::KeyboardMouseInput::GetInstance();

    void SetUp() override
    {
        ResetKeys();
    }

    void TearDown() override
    {
        ResetKeys();
    }

    void ResetKeys()
    {
        input.SetUICapture(false, false);

        input.Reset();
    }
};

TEST_F(InputControllerTest, QuickTapSurvivesReleaseBeforePolling)
{
    input.PressKey(GLFW_KEY_2);

    input.ReleaseKey(GLFW_KEY_2);

    EXPECT_FALSE(input.IsKeyPressed(GLFW_KEY_2));

    EXPECT_TRUE(input.WasKeyPressedOnce(GLFW_KEY_2));

    EXPECT_FALSE(input.WasKeyPressedOnce(GLFW_KEY_2));
}

TEST_F(InputControllerTest, ConsumingShortcutPreservesHeldStateAndRequiresAnotherPress)
{
    input.PressKey(GLFW_KEY_1);

    EXPECT_TRUE(input.WasKeyPressedOnce(GLFW_KEY_1));

    EXPECT_TRUE(input.IsKeyPressed(GLFW_KEY_1));

    EXPECT_FALSE(input.WasKeyPressedOnce(GLFW_KEY_1));

    input.PressKey(GLFW_KEY_1);

    EXPECT_FALSE(input.WasKeyPressedOnce(GLFW_KEY_1));

    input.ReleaseKey(GLFW_KEY_1);

    EXPECT_FALSE(input.WasKeyPressedOnce(GLFW_KEY_1));

    input.PressKey(GLFW_KEY_1);

    EXPECT_TRUE(input.WasKeyPressedOnce(GLFW_KEY_1));
}

TEST_F(InputControllerTest, ShortcutsKeepIndependentPendingPresses)
{
    input.PressKey(GLFW_KEY_1);

    input.ReleaseKey(GLFW_KEY_1);

    input.PressKey(GLFW_KEY_2);

    input.ReleaseKey(GLFW_KEY_2);

    EXPECT_TRUE(input.WasKeyPressedOnce(GLFW_KEY_2));

    EXPECT_TRUE(input.WasKeyPressedOnce(GLFW_KEY_1));

    EXPECT_FALSE(input.WasKeyPressedOnce(GLFW_KEY_2));

    EXPECT_FALSE(input.WasKeyPressedOnce(GLFW_KEY_1));
}

TEST_F(InputControllerTest, CapturedQuickTapDoesNotEscapeIntoSceneShortcuts)
{
    input.PressKey(GLFW_KEY_1);

    input.ReleaseKey(GLFW_KEY_1);

    input.SetUICapture(false, true);

    EXPECT_FALSE(input.WasKeyPressedOnce(GLFW_KEY_1));

    input.SetUICapture(false, false);

    EXPECT_FALSE(input.WasKeyPressedOnce(GLFW_KEY_1));
}

TEST_F(InputControllerTest, CapturedHeldKeyRemainsSuppressedUntilRelease)
{
    input.SetUICapture(false, true);

    input.PressKey(GLFW_KEY_W);

    input.SetUICapture(false, false);

    EXPECT_FALSE(input.IsKeyPressed(GLFW_KEY_W));

    input.ReleaseKey(GLFW_KEY_W);

    input.PressKey(GLFW_KEY_W);

    EXPECT_TRUE(input.IsKeyPressed(GLFW_KEY_W));
}

TEST_F(InputControllerTest, MouseCaptureDiscardsMotionAndPreservesPressOwnership)
{
    input.Resume();

    input.SetCursorPos(10, 20);

    (void)input.CalculateCursorPositionDelta();

    input.SetUICapture(true, false);

    input.PressMouseButton(GLFW_MOUSE_BUTTON_LEFT);

    input.SetMouseButtonRelease(GLFW_MOUSE_BUTTON_LEFT, false);

    input.SetCursorPos(80, 90);

    EXPECT_EQ(input.CalculateCursorPositionDelta(), (std::array<float, 2>{0, 0}));

    input.SetUICapture(false, false);

    EXPECT_FALSE(input.IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT));

    EXPECT_TRUE(input.IsMouseButtonReleased(GLFW_MOUSE_BUTTON_LEFT));

    EXPECT_EQ(input.CalculateCursorPositionDelta(), (std::array<float, 2>{0, 0}));

    input.ReleaseMouseButton(GLFW_MOUSE_BUTTON_LEFT);

    input.PressMouseButton(GLFW_MOUSE_BUTTON_LEFT);

    EXPECT_TRUE(input.IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT));
}

TEST_F(InputControllerTest, FocusLossResetClearsHeldAndPendingActionsIncludingLastCodes)
{
    input.PressKey(GLFW_KEY_LAST);

    input.PressMouseButton(GLFW_MOUSE_BUTTON_LAST);

    input.Reset();

    EXPECT_FALSE(input.IsKeyPressed(GLFW_KEY_LAST));

    EXPECT_FALSE(input.WasKeyPressedOnce(GLFW_KEY_LAST));

    EXPECT_FALSE(input.IsMouseButtonPressed(GLFW_MOUSE_BUTTON_LAST));

    EXPECT_TRUE(input.IsMouseButtonReleased(GLFW_MOUSE_BUTTON_LAST));
}
} // namespace
