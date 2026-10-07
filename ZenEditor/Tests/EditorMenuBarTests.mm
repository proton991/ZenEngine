#include "Editor/Platform/EditorMenuBar.h"
#include "Editor/Platform/EditorApplicationIcon.h"
#include "Editor/Platform/EditorWindowChrome.h"
#include "Platform/WindowBackend.h"
#import <AppKit/AppKit.h>
#if !defined(ZEN_WINDOW_SDL3)
#    define GLFW_EXPOSE_NATIVE_COCOA
#    include <GLFW/glfw3native.h>
#endif
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>

namespace zen::editor
{
namespace
{
EditorMenuItem Command(const char* id, const char* label)
{
    EditorMenuItem item;

    item.id    = id;

    item.label = label;

    return item;
}

HeapVector<EditorMenuItem> FileMenu(EditorMenuItem command)
{
    EditorMenuItem file;

    file.label = "File";

    file.children.push_back(std::move(command));

    return {std::move(file)};
}

NSMenuItem* FileCommand()
{
    return [[[NSApp mainMenu] itemAtIndex:1].submenu itemAtIndex:0];
}

NSBitmapImageRep* RasterizeIcon(NSImage* image)
{
    constexpr NSInteger extent = 256;

    NSBitmapImageRep* bitmap = [[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                     pixelsWide:extent
                                                                     pixelsHigh:extent
                                                                  bitsPerSample:8
                                                                samplesPerPixel:4
                                                                       hasAlpha:YES
                                                                       isPlanar:NO
                                                                 colorSpaceName:NSDeviceRGBColorSpace
                                                                    bytesPerRow:extent * 4
                                                                   bitsPerPixel:32] autorelease];

    if (bitmap != nil)
    {
        std::memset([bitmap bitmapData], 0, [bitmap bytesPerRow] * extent);

        [NSGraphicsContext saveGraphicsState];

        [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap]];

        [image drawInRect:NSMakeRect(0, 0, extent, extent)
                fromRect:NSZeroRect
               operation:NSCompositingOperationCopy
                fraction:1
          respectFlipped:NO
                   hints:nil];

        [NSGraphicsContext restoreGraphicsState];
    }

    return bitmap;
}

class EditorMenuBarTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        pool = [[NSAutoreleasePool alloc] init];

        [NSApplication sharedApplication];

        // Native window backends install application menus before the editor starts.
        // Seed the same non-null ownership here; setServicesMenu:nil does not clear
        // an installed Services menu on AppKit.
        NSMenu* main = [[[NSMenu alloc] initWithTitle:@"Previous application"] autorelease];

        NSMenu* application = [[[NSMenu alloc] initWithTitle:@"Previous application"] autorelease];

        [[main addItemWithTitle:@"Previous application" action:nullptr keyEquivalent:@""] setSubmenu:application];

        NSMenu* services = [NSApp servicesMenu];

        if (services == nil)
        {
            services = [[[NSMenu alloc] initWithTitle:@"Previous Services"] autorelease];
        }
        else
        {
            for (NSMenuItem* item in [[services supermenu] itemArray])
            {
                if ([item submenu] == services)
                {
                    [item setSubmenu:nil];

                    break;
                }
            }
        }

        [[application addItemWithTitle:@"Services" action:nullptr keyEquivalent:@""] setSubmenu:services];

        NSMenu* windows = [[[NSMenu alloc] initWithTitle:@"Previous Window"] autorelease];

        [[main addItemWithTitle:@"Window" action:nullptr keyEquivalent:@""] setSubmenu:windows];

        NSMenu* help = [[[NSMenu alloc] initWithTitle:@"Previous Help"] autorelease];

        [[main addItemWithTitle:@"Help" action:nullptr keyEquivalent:@""] setSubmenu:help];

        [NSApp setMainMenu:main];

        [NSApp setServicesMenu:services];

        [NSApp setWindowsMenu:windows];

        [NSApp setHelpMenu:help];
    }

    void TearDown() override
    {
        [pool drain];
    }

    NSAutoreleasePool* pool{nil};
};

TEST_F(EditorMenuBarTest, InstallsSystemMenuAndRestoresPreviousApplicationMenus)
{
    NSMenu* originalMain     = [NSApp mainMenu];

    NSMenu* originalServices = [NSApp servicesMenu];

    NSMenu* originalServicesParent = [originalServices supermenu];

    NSMenuItem* originalServicesItem = [originalServicesParent itemWithTitle:@"Services"];

    NSMenu* originalWindows  = [NSApp windowsMenu];

    NSMenu* originalHelp     = [NSApp helpMenu];

    NSMenuItem* retainedItem = nil;

    {
        EditorMenuBar menus;

        ASSERT_TRUE(menus.Initialize());

        EXPECT_TRUE(menus.IsEnabled());

        EXPECT_NE([NSApp mainMenu], originalMain);

        NSMenu* applicationMenu = [[[NSApp mainMenu] itemAtIndex:0] submenu];

        NSMenuItem* servicesItem = [applicationMenu itemWithTitle:@"Services"];

        EXPECT_EQ([servicesItem submenu], [NSApp servicesMenu]);

        EXPECT_EQ([NSApp servicesMenu], originalServices);

        EXPECT_EQ([originalServicesItem submenu], nil);

        EXPECT_EQ([originalServices supermenu], applicationMenu);

        EXPECT_NE([NSApp windowsMenu], originalWindows);

        EXPECT_TRUE([[[NSApp mainMenu] itemAtIndex:0].title isEqualToString:@"ZenEditor"]);

        NSMenu* windows = [NSApp windowsMenu];

        EXPECT_TRUE([[[windows itemAtIndex:0] keyEquivalent] isEqualToString:@"w"]);

        EXPECT_EQ([[windows itemAtIndex:0] action], @selector(performClose:));

        EXPECT_TRUE([[[windows itemAtIndex:1] keyEquivalent] isEqualToString:@"m"]);

        EXPECT_EQ([[windows itemAtIndex:1] action], @selector(performMiniaturize:));

        EXPECT_EQ([[windows itemAtIndex:3] action], @selector(toggleFullScreen:));

        EXPECT_EQ([[windows itemAtIndex:3] keyEquivalentModifierMask],
                  NSEventModifierFlagCommand | NSEventModifierFlagControl);

        menus.Update(FileMenu(Command(actions::Open, "Open…")));

        EXPECT_EQ([NSApp windowsMenu], windows);

        retainedItem = [FileCommand() retain];

        EXPECT_TRUE([[retainedItem title] isEqualToString:@"Open…"]);

        EXPECT_NE([retainedItem target], nil);
    }

    EXPECT_EQ([NSApp mainMenu], originalMain);

    EXPECT_EQ([NSApp servicesMenu], originalServices);

    EXPECT_EQ([originalServicesItem submenu], originalServices);

    EXPECT_EQ([originalServices supermenu], originalServicesParent);

    EXPECT_EQ([NSApp windowsMenu], originalWindows);

    EXPECT_EQ([NSApp helpMenu], originalHelp);

    EXPECT_EQ([retainedItem target], nil);

    EXPECT_EQ([retainedItem action], nullptr);

    [retainedItem release];
}

TEST_F(EditorMenuBarTest, ClickAndNativeShortcutEachQueueExactlyOneCommand)
{
    EditorMenuBar menus;

    ASSERT_TRUE(menus.Initialize());

    EditorMenuItem open = Command(actions::Open, "Open…");

    open.shortcut = {platform::Key::O, uint16_t(platform::KeyModifier::Super)};

    menus.Update(FileMenu(std::move(open)));

    NSMenuItem* item = FileCommand();

    EXPECT_TRUE([[item keyEquivalent] isEqualToString:@"o"]);

    EXPECT_EQ([item keyEquivalentModifierMask], NSEventModifierFlagCommand);

    ASSERT_TRUE([NSApp sendAction:[item action] to:[item target] from:item]);

    std::string command;

    ASSERT_TRUE(menus.TakeCommand(command));

    EXPECT_EQ(command, actions::Open);

    EXPECT_FALSE(menus.TakeCommand(command));

    NSEvent* event = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                    location:NSZeroPoint
                               modifierFlags:NSEventModifierFlagCommand
                                   timestamp:0
                                windowNumber:0
                                     context:nil
                                  characters:@"o"
                 charactersIgnoringModifiers:@"o"
                                   isARepeat:NO
                                     keyCode:31];

    ASSERT_TRUE([[NSApp mainMenu] performKeyEquivalent:event]);

    ASSERT_TRUE(menus.TakeCommand(command));

    EXPECT_EQ(command, actions::Open);

    EXPECT_FALSE(menus.TakeCommand(command));

    NSMenu* applicationMenu = [[[NSApp mainMenu] itemAtIndex:0] submenu];

    NSMenuItem* quit = [applicationMenu itemAtIndex:[applicationMenu numberOfItems] - 1];

    ASSERT_TRUE([NSApp sendAction:[quit action] to:[quit target] from:quit]);

    ASSERT_TRUE(menus.TakeCommand(command));

    EXPECT_EQ(command, actions::Exit);

    EXPECT_FALSE(menus.TakeCommand(command));
}

TEST_F(EditorMenuBarTest, SnapshotChangesRefreshStateAndDetachOldTargets)
{
    EditorMenuBar menus;

    ASSERT_TRUE(menus.Initialize());

    EditorMenuItem panel = Command("panel:scene", "Scene");

    panel.shortcut = {platform::Key::Home};

    HeapVector<EditorMenuItem> snapshot = FileMenu(std::move(panel));

    menus.Update(snapshot);

    NSMenuItem* original = [FileCommand() retain];

    NSMenuItem* applicationItem = [[NSApp mainMenu] itemAtIndex:0];

    EXPECT_TRUE([[original keyEquivalent] isEqualToString:@""]);

    menus.Update(snapshot);

    EXPECT_EQ(FileCommand(), original);

    snapshot[0].children[0].checked = true;

    snapshot[0].children[0].enabled = false;

    snapshot[0].children[0].tooltip = "Disabled during loading";

    menus.Update(snapshot);

    NSMenuItem* updated = FileCommand();

    EXPECT_NE(updated, original);

    EXPECT_EQ([[NSApp mainMenu] itemAtIndex:0], applicationItem);

    EXPECT_EQ([updated state], NSControlStateValueOn);

    EXPECT_FALSE([updated isEnabled]);

    EXPECT_TRUE([[updated toolTip] isEqualToString:@"Disabled during loading"]);

    EXPECT_EQ([original target], nil);

    EXPECT_EQ([original action], nullptr);

    // Even a direct action invocation cannot queue a disabled command.
    [NSApp sendAction:[updated action] to:[updated target] from:updated];

    std::string command;

    EXPECT_FALSE(menus.TakeCommand(command));

    [original release];
}

TEST_F(EditorMenuBarTest, MacEditorRetainsItsNativeTitleAndWindowDecoration)
{
    platform::NativeWindow window({"ZenEditor native title", true, 640, 480, 0, false});

#if defined(ZEN_WINDOW_SDL3)
    NSWindow* cocoaWindow = static_cast<NSWindow*>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(platform::WindowBackend::Borrow(window)),
                               SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr));
#else
    NSWindow* cocoaWindow = glfwGetCocoaWindow(platform::WindowBackend::Borrow(window));
#endif

    ASSERT_NE(cocoaWindow, nil);

    EditorWindowChrome chrome(window);

    EXPECT_FALSE(chrome.Initialize());

    EXPECT_FALSE(chrome.IsEnabled());

    EXPECT_TRUE(window.IsDecorated());

    EXPECT_NE([cocoaWindow styleMask] & NSWindowStyleMaskTitled, 0u);

    EXPECT_EQ([cocoaWindow styleMask] & NSWindowStyleMaskFullSizeContentView, 0u);

    EXPECT_EQ([cocoaWindow titleVisibility], NSWindowTitleVisible);

    EXPECT_FALSE([cocoaWindow titlebarAppearsTransparent]);
}

TEST_F(EditorMenuBarTest, ApplicationIconUsesFullResolutionEditorArtwork)
{
    NSImage* original = [[NSApp applicationIconImage] retain];

    const bool initialized = InitializeEditorApplicationIcon();

    // The application must retain the decoded image after the helper's pool drains.
    NSImage* icon = [NSApp applicationIconImage];

    EXPECT_TRUE(initialized);

    EXPECT_TRUE([icon isValid]);

    NSImage* artwork = [[[NSImage alloc]
        initWithContentsOfFile:[NSString stringWithUTF8String:ZEN_TEXTURE_PATH "Editor/zen_engine.png"]] autorelease];

    EXPECT_TRUE([artwork isValid]);

    NSInteger resolution = 0;

    for (NSImageRep* representation in [icon representations])
    {
        if ([representation pixelsWide] > resolution)
        {
            resolution = [representation pixelsWide];
        }
    }

    EXPECT_GE(resolution, 1024);

    // AppKit can create a Retina representation with different TIFF metadata.
    // Compare the artwork at a common raster size, allowing resampling rounding.
    NSBitmapImageRep* expected = RasterizeIcon(artwork);

    NSBitmapImageRep* actual = RasterizeIcon(icon);

    EXPECT_NE(expected, nil);

    EXPECT_NE(actual, nil);

    if (expected != nil && actual != nil)
    {
        uint64_t difference = 0;

        const NSInteger byteCount = [expected bytesPerRow] * [expected pixelsHigh];

        for (NSInteger byte = 0; byte < byteCount; ++byte)
        {
            difference += std::abs(int([actual bitmapData][byte]) - int([expected bitmapData][byte]));
        }

        EXPECT_LE(double(difference) / double(byteCount), 1.0);
    }

    [NSApp setApplicationIconImage:original];

    [original release];
}
} // namespace
} // namespace zen::editor
