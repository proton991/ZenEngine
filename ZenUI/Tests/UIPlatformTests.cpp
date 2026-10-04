#include "ImGui/UIContext.h"
#include "Platform/NativeWindow.h"
#include "Platform/WindowBackend.h"
#include "imgui.h"
#include <gtest/gtest.h>

namespace zen::ui
{
namespace
{
void BuildStaticAtlas(UIContext& context)
{
    unsigned char* pixels = nullptr;

    int width             = 0;

    int height            = 0;

    context.GetFonts().GetTexDataAsRGBA32(&pixels, &width, &height);

    context.GetFonts().SetTexID(ImTextureID(1));
}

TEST(UIPlatform, SDLEventsReachEachWindowContextOnceAndUnsubscribeOnDestruction)
{
    platform::NativeWindow first({"first UI", true, 320, 240, 0, false});

    platform::NativeWindow second({"second UI", true, 320, 240, 0, false});

    platform::NativeWindow::PollEvents();

    {
        UIContext firstUI;

        ASSERT_TRUE(firstUI.Init(first));

        BuildStaticAtlas(firstUI);

        UIContext secondUI;

        ASSERT_TRUE(secondUI.Init(second));

        BuildStaticAtlas(secondUI);

        SDL_Event event{};

        event.type          = SDL_EVENT_TEXT_INPUT;

        event.text.windowID = SDL_GetWindowID(platform::WindowBackend::Borrow(first));

        event.text.text     = "A";

        ASSERT_TRUE(SDL_PushEvent(&event));

        event.text.windowID = SDL_GetWindowID(platform::WindowBackend::Borrow(second));

        event.text.text     = "B";

        ASSERT_TRUE(SDL_PushEvent(&event));

        platform::NativeWindow::PollEvents();

        firstUI.BeginFrame(1.0f / 60.0f, 320, 240);

        ASSERT_EQ(ImGui::GetIO().InputQueueCharacters.Size, 1);

        EXPECT_EQ(ImGui::GetIO().InputQueueCharacters[0], 'A');

        firstUI.EndFrame();

        secondUI.BeginFrame(1.0f / 60.0f, 320, 240);

        ASSERT_EQ(ImGui::GetIO().InputQueueCharacters.Size, 1);

        EXPECT_EQ(ImGui::GetIO().InputQueueCharacters[0], 'B');

        secondUI.EndFrame();
    }

    // A later pump and a replacement adapter must not call a destroyed context.
    platform::NativeWindow::PollEvents();

    UIContext replacement;

    EXPECT_TRUE(replacement.Init(first));
}
} // namespace
} // namespace zen::ui
