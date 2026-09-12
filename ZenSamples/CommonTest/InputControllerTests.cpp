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
        for (int key : {GLFW_KEY_1, GLFW_KEY_2})
        {
            input.ReleaseKey(key);
            (void)input.WasKeyPressedOnce(key);
        }
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
} // namespace
