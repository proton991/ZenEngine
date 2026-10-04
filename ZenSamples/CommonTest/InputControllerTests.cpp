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
    input.PressKey(zen::platform::Key::Digit2);

    input.ReleaseKey(zen::platform::Key::Digit2);

    EXPECT_FALSE(input.IsKeyPressed(zen::platform::Key::Digit2));

    EXPECT_TRUE(input.WasKeyPressedOnce(zen::platform::Key::Digit2));

    EXPECT_FALSE(input.WasKeyPressedOnce(zen::platform::Key::Digit2));
}

TEST_F(InputControllerTest, ConsumingShortcutPreservesHeldStateAndRequiresAnotherPress)
{
    input.PressKey(zen::platform::Key::Digit1);

    EXPECT_TRUE(input.WasKeyPressedOnce(zen::platform::Key::Digit1));

    EXPECT_TRUE(input.IsKeyPressed(zen::platform::Key::Digit1));

    EXPECT_FALSE(input.WasKeyPressedOnce(zen::platform::Key::Digit1));

    input.PressKey(zen::platform::Key::Digit1);

    EXPECT_FALSE(input.WasKeyPressedOnce(zen::platform::Key::Digit1));

    input.ReleaseKey(zen::platform::Key::Digit1);

    EXPECT_FALSE(input.WasKeyPressedOnce(zen::platform::Key::Digit1));

    input.PressKey(zen::platform::Key::Digit1);

    EXPECT_TRUE(input.WasKeyPressedOnce(zen::platform::Key::Digit1));
}

TEST_F(InputControllerTest, ShortcutsKeepIndependentPendingPresses)
{
    input.PressKey(zen::platform::Key::Digit1);

    input.ReleaseKey(zen::platform::Key::Digit1);

    input.PressKey(zen::platform::Key::Digit2);

    input.ReleaseKey(zen::platform::Key::Digit2);

    EXPECT_TRUE(input.WasKeyPressedOnce(zen::platform::Key::Digit2));

    EXPECT_TRUE(input.WasKeyPressedOnce(zen::platform::Key::Digit1));

    EXPECT_FALSE(input.WasKeyPressedOnce(zen::platform::Key::Digit2));

    EXPECT_FALSE(input.WasKeyPressedOnce(zen::platform::Key::Digit1));
}

TEST_F(InputControllerTest, CapturedQuickTapDoesNotEscapeIntoSceneShortcuts)
{
    input.PressKey(zen::platform::Key::Digit1);

    input.ReleaseKey(zen::platform::Key::Digit1);

    input.SetUICapture(false, true);

    EXPECT_FALSE(input.WasKeyPressedOnce(zen::platform::Key::Digit1));

    input.SetUICapture(false, false);

    EXPECT_FALSE(input.WasKeyPressedOnce(zen::platform::Key::Digit1));
}

TEST_F(InputControllerTest, CapturedHeldKeyRemainsSuppressedUntilRelease)
{
    input.SetUICapture(false, true);

    input.PressKey(zen::platform::Key::W);

    input.SetUICapture(false, false);

    EXPECT_FALSE(input.IsKeyPressed(zen::platform::Key::W));

    input.ReleaseKey(zen::platform::Key::W);

    input.PressKey(zen::platform::Key::W);

    EXPECT_TRUE(input.IsKeyPressed(zen::platform::Key::W));
}

TEST_F(InputControllerTest, MouseCaptureDiscardsMotionAndPreservesPressOwnership)
{
    input.Resume();

    input.SetCursorPos(10, 20);

    (void)input.CalculateCursorPositionDelta();

    input.SetUICapture(true, false);

    input.PressMouseButton(zen::platform::MouseButton::Left);

    input.SetMouseButtonRelease(zen::platform::MouseButton::Left, false);

    input.SetCursorPos(80, 90);

    EXPECT_EQ(input.CalculateCursorPositionDelta(), (std::array<float, 2>{0, 0}));

    input.SetUICapture(false, false);

    EXPECT_FALSE(input.IsMouseButtonPressed(zen::platform::MouseButton::Left));

    EXPECT_TRUE(input.IsMouseButtonReleased(zen::platform::MouseButton::Left));

    EXPECT_EQ(input.CalculateCursorPositionDelta(), (std::array<float, 2>{0, 0}));

    input.ReleaseMouseButton(zen::platform::MouseButton::Left);

    input.PressMouseButton(zen::platform::MouseButton::Left);

    EXPECT_TRUE(input.IsMouseButtonPressed(zen::platform::MouseButton::Left));
}

TEST_F(InputControllerTest, FocusLossResetClearsHeldAndPendingActionsIncludingLastCodes)
{
    input.PressKey(zen::platform::Key::Menu);

    input.PressMouseButton(zen::platform::MouseButton::Extra5);

    input.Reset();

    EXPECT_FALSE(input.IsKeyPressed(zen::platform::Key::Menu));

    EXPECT_FALSE(input.WasKeyPressedOnce(zen::platform::Key::Menu));

    EXPECT_FALSE(input.IsMouseButtonPressed(zen::platform::MouseButton::Extra5));

    EXPECT_TRUE(input.IsMouseButtonReleased(zen::platform::MouseButton::Extra5));
}
} // namespace
