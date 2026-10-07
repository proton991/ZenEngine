#include "Editor/Platform/EditorApplicationIcon.h"
#import <AppKit/AppKit.h>

namespace zen::editor
{
bool InitializeEditorApplicationIcon()
{
    bool valid = false;

    if (NSApp != nil && [NSThread isMainThread])
    {
        @autoreleasepool
        {
            NSString* path = [[NSBundle mainBundle] pathForResource:@"ZenEditor" ofType:@"icns"];

            if (path == nil)
            {
                path = [NSString stringWithUTF8String:ZEN_TEXTURE_PATH "Editor/zen_engine.png"];
            }

            NSImage* icon = [[NSImage alloc] initWithContentsOfFile:path];

            valid = icon != nil && [icon isValid];

            if (valid)
            {
                // AppKit retains the image and scales it for the Dock. This also
                // supplies the application icon when an IDE runs the executable directly.
                [NSApp setApplicationIconImage:icon];
            }

            [icon release];
        }
    }

    return valid;
}
} // namespace zen::editor
