// Cocoa title-bar integration for the SDL backend. Compiled as Objective-C++ without ARC.
#include "Platform/WindowBackend.h"
#import <AppKit/AppKit.h>

namespace zen::platform
{
namespace
{
NSWindow* GetCocoaWindow(const NativeWindow& window)
{
    const SDL_PropertiesID properties = SDL_GetWindowProperties(WindowBackend::Borrow(window));

    return static_cast<NSWindow*>(SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr));
}

// Follows the user's "Double-click a window's title bar" setting; zoom is the default.
void PerformTitleBarDoubleClick(NSWindow* nswindow)
{
    NSString* action = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleActionOnDoubleClick"];

    if ([action isEqualToString:@"Minimize"])
    {
        [nswindow miniaturize:nil];
    }
    else if (![action isEqualToString:@"None"])
    {
        [nswindow zoom:nil];
    }
}

// SDL consumes clicks in draggable regions without a click count, and AppKit has no
// title bar under application content, so caption double-clicks are handled here.
NSEvent* HandleLeftMouseDown(const NativeWindow& window, NSWindow* nswindow, NSEvent* event)
{
    NSEvent* result = event;

    if ([event window] == nswindow && [event clickCount] == 2 && ([nswindow styleMask] & NSWindowStyleMaskFullScreen) == 0)
    {
        const NSPoint point = [event locationInWindow];

        // Cocoa window coordinates start at the bottom edge.
        const float y = float(NSHeight([[nswindow contentView] frame]) - point.y);

        if (window.HitTest(float(point.x), y) == WindowHit::Caption)
        {
            PerformTitleBarDoubleClick(nswindow);

            result = nil;
        }
    }

    return result;
}

void SetFullSizeContent(NSWindow* nswindow, bool enabled)
{
    const NSWindowStyleMask current = [nswindow styleMask];

    const NSWindowStyleMask style =
        enabled ? (current | NSWindowStyleMaskFullSizeContentView) : (current & ~NSWindowStyleMaskFullSizeContentView);

    if (style != current)
    {
        NSView* view           = [nswindow contentView];

        NSResponder* responder = [view nextResponder];

        // As SDL does for its own style changes: setStyleMask disturbs the responder
        // chain that routes input from the content view to SDL's listener.
        [view setNextResponder:nil];

        [nswindow setStyleMask:style];

        [view setNextResponder:responder];

        // The frame is unchanged, so AppKit posts no resize; SDL reads the new content size.
        [[NSNotificationCenter defaultCenter] postNotificationName:NSWindowDidResizeNotification object:nswindow];
    }

    [nswindow setTitlebarAppearsTransparent:enabled ? YES : NO];

    [nswindow setTitleVisibility:enabled ? NSWindowTitleHidden : NSWindowTitleVisible];
}
} // namespace

bool WindowBackend::SetCocoaTitleBar(NativeWindow& window, bool enabled)
{
    bool success = false;

    @autoreleasepool
    {
        NSWindow* nswindow = GetCocoaWindow(window);

        success            = nswindow != nil;

        if (success && enabled && window.m_frameData == nullptr)
        {
            NativeWindow* owner = &window;

            id monitor          = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown
                                                                        handler:^NSEvent*(NSEvent* event) {
                                                                   return HandleLeftMouseDown(*owner, nswindow, event);
                                                                        }];

            window.m_frameData  = [monitor retain];
        }
        else if (success && !enabled && window.m_frameData != nullptr)
        {
            id monitor = static_cast<id>(window.m_frameData);

            [NSEvent removeMonitor:monitor];

            [monitor release];

            window.m_frameData = nullptr;
        }

        if (success)
        {
            SetFullSizeContent(nswindow, enabled);
        }
    }

    return success;
}

WindowTitleBarLayout WindowBackend::GetCocoaTitleBarLayout(const NativeWindow& window)
{
    WindowTitleBarLayout layout;

    @autoreleasepool
    {
        NSWindow* nswindow = GetCocoaWindow(window);

        // A full-screen space hides the title bar and its buttons.
        if (nswindow != nil && ([nswindow styleMask] & NSWindowStyleMaskFullScreen) == 0)
        {
            NSButton* close = [nswindow standardWindowButton:NSWindowCloseButton];

            NSButton* zoom  = [nswindow standardWindowButton:NSWindowZoomButton];

            layout.height   = float(NSHeight([nswindow frame]) - NSHeight([nswindow contentLayoutRect]));

            if (close != nil && zoom != nil)
            {
                const NSRect first = [close convertRect:[close bounds] toView:nil];

                const NSRect last  = [zoom convertRect:[zoom bounds] toView:nil];

                // Repeat the native margin before the buttons after them.
                layout.leadingInset = float(NSMaxX(last) + NSMinX(first));
            }
        }
    }

    return layout;
}
} // namespace zen::platform
