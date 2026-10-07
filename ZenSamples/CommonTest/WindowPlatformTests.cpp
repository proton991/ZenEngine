#include "Platform/NativeWindow.h"
#include "Platform/InputController.h"
#include "Platform/WindowBackend.h"
#include <gtest/gtest.h>
#include "Graphics/VulkanRHI/VulkanRHI.h"

namespace zen::platform
{
namespace
{
TEST(WindowPlatform, SharedLifetimeAndIndependentCloseRequests)
{
    NativeWindow first({"first", true, 320, 240, 0, false});

    {
        NativeWindow second({"second", true, 240, 180, 0, false});

        second.RequestClose();

        EXPECT_TRUE(second.ShouldClose());

        EXPECT_FALSE(first.ShouldClose());
    }

    first.SetSize(400, 300);

    first.Update(false);

    EXPECT_EQ(first.GetExtent2D().width, 400u);

    EXPECT_GT(first.GetFramebufferExtent().width, 0u);

    EXPECT_GT(first.GetDisplayScale(), 0);

    NativeWindow third({"third", true, 320, 240, 0, false});

    EXPECT_FALSE(third.ShouldClose());
}

TEST(WindowPlatform, TitleGeometryUsesWindowCoordinates)
{
    NativeWindow window({"title geometry", true, 800, 600, 0, false});

    if (window.SetCustomFrame(true))
    {
        window.SetTitleBarRegion({200, 40, 140, false});

        EXPECT_EQ(window.HitTest(300, 20), WindowHit::Caption);

        EXPECT_EQ(window.HitTest(100, 20), WindowHit::Client);

        EXPECT_EQ(window.HitTest(750, 20), WindowHit::Client);

        EXPECT_EQ(window.HitTest(300, 100), WindowHit::Client);

        // A frame that stays decorated, as on macOS, resizes from its native edges.
        EXPECT_EQ(window.HitTest(1, 1), window.IsDecorated() ? WindowHit::Client : WindowHit::TopLeft);

        window.SetTitleBarRegion({200, 40, 140, true});

        EXPECT_EQ(window.HitTest(300, 20), WindowHit::Client);

        // Either the platform keeps its leading buttons or the application draws controls.
        const WindowTitleBarLayout layout = window.GetTitleBarLayout();

        EXPECT_NE(layout.drawsControls, layout.leadingInset > 0);

        ASSERT_TRUE(window.SetCustomFrame(false));

        EXPECT_TRUE(window.IsDecorated());

        EXPECT_FALSE(window.GetTitleBarLayout().drawsControls);

        EXPECT_EQ(window.GetTitleBarLayout().leadingInset, 0.0f);
    }
}

#if defined(ZEN_WINDOW_SDL3)
TEST(WindowPlatform, HeadlessRHIDoesNotInitializeVideo)
{
    EXPECT_EQ(SDL_WasInit(SDL_INIT_VIDEO), 0u);

    VulkanRHI rhi;

    GDynamicRHI = &rhi;

    rhi.Init();

    EXPECT_EQ(SDL_WasInit(SDL_INIT_VIDEO), 0u);

    rhi.Destroy();

    GDynamicRHI = nullptr;

    GVulkanRHI  = nullptr;
}

struct EventLog
{
    void Record(const InputEvent& event)
    {
        if (event.type == InputEventType::Text)
        {
            text += event.text;
        }

        if (event.type == InputEventType::Composition)
        {
            composition = event.text;
        }

        if (event.type == InputEventType::FileDrop)
        {
            file = event.text;
        }

        if (event.type == InputEventType::KeyDown)
        {
            ++presses;

            key       = event.key;

            symbol    = event.symbol;

            modifiers = event.modifiers;
        }
    }

    std::string text;
    std::string composition;
    std::string file;
    int         presses{0};
    Key         key{Key::Unknown};
    char32_t    symbol{0};
    KeyModifier modifiers{KeyModifier::None};
};

TEST(WindowPlatform, SDLTranslationPreservesQuickTapTextCompositionAndDrop)
{
    NativeWindow window({"event routing", true, 320, 240, 0, false});

    NativeWindow::PollEvents();

    KeyboardMouseInput& input = KeyboardMouseInput::GetInstance();

    input.Reset();

    EventLog log;

    window.SetOnInput(std::bind_front(&EventLog::Record, &log));

    const SDL_WindowID id = SDL_GetWindowID(WindowBackend::Borrow(window));

    SDL_Event event{};

    event.type         = SDL_EVENT_KEY_DOWN;

    event.key.windowID = id;

    event.key.scancode = SDL_SCANCODE_W;

    event.key.key      = SDLK_Z;

    event.key.mod      = SDL_KMOD_CTRL;

    ASSERT_TRUE(SDL_PushEvent(&event));

    event.type = SDL_EVENT_KEY_UP;

    ASSERT_TRUE(SDL_PushEvent(&event));

    event               = {};

    event.type          = SDL_EVENT_TEXT_INPUT;

    event.text.windowID = id;

    event.text.text     = "Zen";

    ASSERT_TRUE(SDL_PushEvent(&event));

    event               = {};

    event.type          = SDL_EVENT_TEXT_EDITING;

    event.edit.windowID = id;

    event.edit.text     = "compose";

    ASSERT_TRUE(SDL_PushEvent(&event));

    event               = {};

    event.type          = SDL_EVENT_DROP_FILE;

    event.drop.windowID = id;

    event.drop.data     = "sample.gltf";

    ASSERT_TRUE(SDL_PushEvent(&event));

    NativeWindow::PollEvents();

    EXPECT_TRUE(input.WasKeyPressedOnce(Key::W));

    EXPECT_FALSE(input.IsKeyPressed(Key::W));

    EXPECT_EQ(log.presses, 1);

    EXPECT_EQ(log.key, Key::W);

    EXPECT_EQ(log.symbol, U'z');

    EXPECT_EQ(log.modifiers, KeyModifier::Control);

    EXPECT_EQ(log.text, "Zen");

    EXPECT_EQ(log.composition, "compose");

    EXPECT_EQ(log.file, "sample.gltf");

    input.PressKey(Key::A);

    input.PressMouseButton(MouseButton::Right);

    window.HideCursor();

    EXPECT_TRUE(SDL_GetWindowRelativeMouseMode(WindowBackend::Borrow(window)));

    event                 = {};

    event.type            = SDL_EVENT_WINDOW_FOCUS_LOST;

    event.window.windowID = id;

    ASSERT_TRUE(SDL_PushEvent(&event));

    NativeWindow::PollEvents();

    EXPECT_FALSE(input.IsKeyPressed(Key::A));

    EXPECT_FALSE(input.IsMouseButtonPressed(MouseButton::Right));

    EXPECT_FALSE(SDL_GetWindowRelativeMouseMode(WindowBackend::Borrow(window)));

    window.SetOnInput({});
}
#endif
} // namespace
} // namespace zen::platform
