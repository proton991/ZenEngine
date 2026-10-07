// Cocoa menu integration, compiled as Objective-C++ without ARC.
#include "Editor/Platform/EditorMenuBar.h"
#include "Templates/Queue.h"
#import <AppKit/AppKit.h>
#include <algorithm>
#include <optional>

namespace
{
bool HasNativeModal()
{
    return [NSApp modalWindow] != nil || [[NSApp mainWindow] attachedSheet] != nil
           || [[NSApp keyWindow] attachedSheet] != nil;
}

NSString* CocoaString(const std::string& value)
{
    NSString* string = [NSString stringWithUTF8String:value.c_str()];

    return string != nil ? string : @"";
}
} // namespace

@interface ZenEditorMenuTarget : NSObject
{
@public
    zen::Queue<std::string>* commands;
}
- (void)dispatchCommand:(NSMenuItem*)item;
@end

@implementation ZenEditorMenuTarget
- (void)dispatchCommand:(NSMenuItem*)item
{
    // Cocoa menus can remain reachable during file panels. The workspace also checks
    // availability again when it consumes this command after event polling.
    if (commands != nullptr && [item isEnabled] && !HasNativeModal())
    {
        NSString* command = [item representedObject];

        if ([command isKindOfClass:[NSString class]] && [command length] != 0)
        {
            commands->Push([command UTF8String]);
        }
    }
}
@end

namespace zen::editor
{
namespace
{
NSString* KeyEquivalent(platform::Key key)
{
    using platform::Key;

    unichar character = 0;

    if (key >= Key::A && key <= Key::Z)
    {
        character = 'a' + (uint16_t(key) - uint16_t(Key::A));
    }
    else if (key >= Key::Digit0 && key <= Key::Digit9)
    {
        character = '0' + (uint16_t(key) - uint16_t(Key::Digit0));
    }
    else if (key >= Key::F1 && key <= Key::F24)
    {
        character = NSF1FunctionKey + (uint16_t(key) - uint16_t(Key::F1));
    }
    else
    {
        switch (key)
        {
            case Key::Space: character = ' '; break;
            case Key::Apostrophe: character = '\''; break;
            case Key::Comma: character = ','; break;
            case Key::Minus: character = '-'; break;
            case Key::Period: character = '.'; break;
            case Key::Slash: character = '/'; break;
            case Key::Semicolon: character = ';'; break;
            case Key::Equal: character = '='; break;
            case Key::LeftBracket: character = '['; break;
            case Key::Backslash: character = '\\'; break;
            case Key::RightBracket: character = ']'; break;
            case Key::GraveAccent: character = '`'; break;
            case Key::Escape: character = 0x1b; break;
            case Key::Enter: character = NSCarriageReturnCharacter; break;
            case Key::Tab: character = NSTabCharacter; break;
            case Key::Backspace: character = NSDeleteCharacter; break;
            case Key::Insert: character = NSInsertFunctionKey; break;
            case Key::Delete: character = NSDeleteFunctionKey; break;
            case Key::Right: character = NSRightArrowFunctionKey; break;
            case Key::Left: character = NSLeftArrowFunctionKey; break;
            case Key::Down: character = NSDownArrowFunctionKey; break;
            case Key::Up: character = NSUpArrowFunctionKey; break;
            case Key::PageUp: character = NSPageUpFunctionKey; break;
            case Key::PageDown: character = NSPageDownFunctionKey; break;
            case Key::Home: character = NSHomeFunctionKey; break;
            case Key::End: character = NSEndFunctionKey; break;
            case Key::PrintScreen: character = NSPrintScreenFunctionKey; break;
            case Key::ScrollLock: character = NSScrollLockFunctionKey; break;
            case Key::Pause: character = NSPauseFunctionKey; break;
            default: break;
        }
    }

    return character == 0 ? @"" : [NSString stringWithCharacters:&character length:1];
}

void SetShortcut(NSMenuItem* item, EditorShortcut shortcut)
{
    using platform::KeyModifier;

    // Unmodified navigation keys remain in ImGui, where focused view and text entry
    // determine whether they are commands. AppKit owns modified menu shortcuts.
    if ((shortcut.modifiers & (uint16_t(KeyModifier::Control) | uint16_t(KeyModifier::Super))) != 0)
    {
        NSEventModifierFlags modifiers = 0;

        if ((shortcut.modifiers & uint16_t(KeyModifier::Control)) != 0)
        {
            modifiers |= NSEventModifierFlagControl;
        }

        if ((shortcut.modifiers & uint16_t(KeyModifier::Super)) != 0)
        {
            modifiers |= NSEventModifierFlagCommand;
        }

        if ((shortcut.modifiers & uint16_t(KeyModifier::Shift)) != 0)
        {
            modifiers |= NSEventModifierFlagShift;
        }

        if ((shortcut.modifiers & uint16_t(KeyModifier::Alt)) != 0)
        {
            modifiers |= NSEventModifierFlagOption;
        }

        [item setKeyEquivalent:KeyEquivalent(shortcut.key)];

        [item setKeyEquivalentModifierMask:modifiers];
    }
}

NSMenuItem* AddStandardItem(NSMenu* menu, NSString* label, SEL action, NSString* key)
{
    NSMenuItem* item = [menu addItemWithTitle:label action:action keyEquivalent:key];

    [item setTarget:NSApp];

    return item;
}

void DetachCallbacks(NSMenu* menu, ZenEditorMenuTarget* target)
{
    for (NSMenuItem* item in [menu itemArray])
    {
        if ([item target] == target)
        {
            [item setTarget:nil];

            [item setAction:nullptr];
        }

        if ([item submenu] != nil)
        {
            DetachCallbacks([item submenu], target);
        }
    }
}

void PopulateMenu(NSMenu* menu, const HeapVector<EditorMenuItem>& items, ZenEditorMenuTarget* target)
{
    [menu setAutoenablesItems:NO];

    for (const EditorMenuItem& definition : items)
    {
        if (definition.separator)
        {
            [menu addItem:[NSMenuItem separatorItem]];

            continue;
        }

        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:CocoaString(definition.label) action:nullptr keyEquivalent:@""];

        [item setEnabled:definition.enabled ? YES : NO];

        [item setState:definition.checked ? NSControlStateValueOn : NSControlStateValueOff];

        if (!definition.tooltip.empty())
        {
            [item setToolTip:CocoaString(definition.tooltip)];
        }

        if (!definition.children.empty())
        {
            NSMenu* submenu = [[NSMenu alloc] initWithTitle:CocoaString(definition.label)];

            PopulateMenu(submenu, definition.children, target);

            [item setSubmenu:submenu];

            [submenu release];
        }
        else if (!definition.id.empty())
        {
            [item setRepresentedObject:CocoaString(definition.id)];

            [item setTarget:target];

            [item setAction:@selector(dispatchCommand:)];

            SetShortcut(item, definition.shortcut);
        }

        [menu addItem:item];

        [item release];
    }
}
} // namespace

class EditorMenuBar::State
{
public:
    ~State()
    {
        @autoreleasepool
        {
            if (target != nil)
            {
                target->commands = nullptr;

                DetachCallbacks(mainMenu, target);
            }

            // AppKit keeps the first registered Services menu. It belongs to only
            // one parent item at a time, so return it to its original hierarchy.
            if ([servicesItem submenu] == servicesMenu)
            {
                [servicesItem setSubmenu:nil];

                if (previousServicesItem != nil && [previousServicesItem submenu] == nil)
                {
                    [previousServicesItem setSubmenu:previousServicesMenu];
                }
            }

            // Do not overwrite a menu installed by another owner after this one.
            if (enabled && [NSApp mainMenu] == mainMenu)
            {
                [NSApp setMainMenu:previousMainMenu];

                [NSApp setServicesMenu:previousServicesMenu];

                [NSApp setWindowsMenu:previousWindowsMenu];

                [NSApp setHelpMenu:previousHelpMenu];
            }

            [mainMenu release];

            [windowsItem release];

            [windowsMenu release];

            [servicesItem release];

            [servicesMenu release];

            [previousServicesItem release];

            [target release];

            [previousMainMenu release];

            [previousServicesMenu release];

            [previousWindowsMenu release];

            [previousHelpMenu release];
        }
    }

    bool enabled{false};
    NSMenu* mainMenu{nil};
    NSMenu* windowsMenu{nil};
    NSMenuItem* windowsItem{nil};
    NSMenu* servicesMenu{nil};
    NSMenuItem* servicesItem{nil};
    NSMenuItem* previousServicesItem{nil};
    NSMenu* previousMainMenu{nil};
    NSMenu* previousServicesMenu{nil};
    NSMenu* previousWindowsMenu{nil};
    NSMenu* previousHelpMenu{nil};
    ZenEditorMenuTarget* target{nil};
    HeapVector<EditorMenuItem> snapshot;
    Queue<std::string> commands;
};

EditorMenuBar::EditorMenuBar() : m_state(MakeUnique<State>()) {}

EditorMenuBar::~EditorMenuBar() = default;

bool EditorMenuBar::Initialize()
{
    if (!m_state->enabled && NSApp != nil && [NSThread isMainThread])
    {
        @autoreleasepool
        {
            m_state->previousMainMenu     = [[NSApp mainMenu] retain];

            m_state->previousServicesMenu = [[NSApp servicesMenu] retain];

            m_state->previousWindowsMenu  = [[NSApp windowsMenu] retain];

            m_state->previousHelpMenu     = [[NSApp helpMenu] retain];

            m_state->target               = [[ZenEditorMenuTarget alloc] init];

            m_state->target->commands     = &m_state->commands;

            m_state->mainMenu             = [[NSMenu alloc] initWithTitle:@"ZenEditor"];

            [m_state->mainMenu setAutoenablesItems:NO];

            NSMenuItem* applicationItem = [m_state->mainMenu addItemWithTitle:@"ZenEditor" action:nullptr keyEquivalent:@""];

            NSMenu* applicationMenu = [[NSMenu alloc] initWithTitle:@"ZenEditor"];

            AddStandardItem(applicationMenu, @"About ZenEditor", @selector(orderFrontStandardAboutPanel:), @"");

            [applicationMenu addItem:[NSMenuItem separatorItem]];

            // setServicesMenu: leaves the first registered menu in place on current
            // AppKit. Reuse it so the visible menu receives system Services updates.
            m_state->servicesMenu = m_state->previousServicesMenu != nil
                                        ? [m_state->previousServicesMenu retain]
                                        : [[NSMenu alloc] initWithTitle:@"Services"];

            for (NSMenuItem* item in [[m_state->servicesMenu supermenu] itemArray])
            {
                if ([item submenu] == m_state->servicesMenu)
                {
                    m_state->previousServicesItem = [item retain];

                    [item setSubmenu:nil];

                    break;
                }
            }

            m_state->servicesItem = [[applicationMenu addItemWithTitle:@"Services" action:nullptr keyEquivalent:@""] retain];

            [m_state->servicesItem setSubmenu:m_state->servicesMenu];

            [applicationMenu addItem:[NSMenuItem separatorItem]];

            AddStandardItem(applicationMenu, @"Hide ZenEditor", @selector(hide:), @"h");

            NSMenuItem* hideOthers = AddStandardItem(applicationMenu, @"Hide Others", @selector(hideOtherApplications:), @"h");

            [hideOthers setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];

            AddStandardItem(applicationMenu, @"Show All", @selector(unhideAllApplications:), @"");

            [applicationMenu addItem:[NSMenuItem separatorItem]];

            NSMenuItem* quit = [applicationMenu addItemWithTitle:@"Quit ZenEditor" action:@selector(dispatchCommand:) keyEquivalent:@"q"];

            [quit setTarget:m_state->target];

            [quit setRepresentedObject:CocoaString(actions::Exit)];

            [applicationItem setSubmenu:applicationMenu];

            [applicationMenu release];

            [NSApp setMainMenu:m_state->mainMenu];

            [NSApp setServicesMenu:m_state->servicesMenu];

            m_state->windowsMenu = [[NSMenu alloc] initWithTitle:@"Window"];

            // A nil target follows the key window's responder chain. AppKit validates
            // these commands and keeps its native list of open windows in this menu.
            [m_state->windowsMenu addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];

            [m_state->windowsMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];

            [m_state->windowsMenu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];

            NSMenuItem* fullScreen = [m_state->windowsMenu addItemWithTitle:@"Enter Full Screen"
                                                                   action:@selector(toggleFullScreen:)
                                                            keyEquivalent:@"f"];

            [fullScreen setKeyEquivalentModifierMask:NSEventModifierFlagControl | NSEventModifierFlagCommand];

            [m_state->windowsMenu addItem:[NSMenuItem separatorItem]];

            AddStandardItem(m_state->windowsMenu, @"Bring All to Front", @selector(arrangeInFront:), @"");

            m_state->windowsItem = [[NSMenuItem alloc] initWithTitle:@"Window" action:nullptr keyEquivalent:@""];

            [m_state->windowsItem setSubmenu:m_state->windowsMenu];

            [m_state->mainMenu addItem:m_state->windowsItem];

            [NSApp setWindowsMenu:m_state->windowsMenu];

            [NSApp setHelpMenu:nil];

            m_state->enabled = true;
        }
    }

    return m_state->enabled;
}

bool EditorMenuBar::IsEnabled() const
{
    return m_state->enabled;
}

void EditorMenuBar::Update(const HeapVector<EditorMenuItem>& menus)
{
    if (m_state->enabled && (menus.size() != m_state->snapshot.size()
                            || !std::equal(menus.begin(), menus.end(), m_state->snapshot.begin())))
    {
        @autoreleasepool
        {
            // Index zero is the standard application menu and keeps its identity.
            for (NSInteger i = [m_state->mainMenu numberOfItems] - 1; i > 0; --i)
            {
                NSMenuItem* item = [m_state->mainMenu itemAtIndex:i];

                DetachCallbacks([item submenu], m_state->target);

                [m_state->mainMenu removeItemAtIndex:i];
            }

            [NSApp setHelpMenu:nil];

            PopulateMenu(m_state->mainMenu, menus, m_state->target);

            bool providedWindowsMenu = false;

            NSInteger windowsIndex = [m_state->mainMenu numberOfItems];

            for (NSMenuItem* item in [m_state->mainMenu itemArray])
            {
                if ([[item title] isEqualToString:@"Help"])
                {
                    [NSApp setHelpMenu:[item submenu]];

                    windowsIndex = [m_state->mainMenu indexOfItem:item];
                }
                else if ([[item title] isEqualToString:@"Window"])
                {
                    [NSApp setWindowsMenu:[item submenu]];

                    providedWindowsMenu = true;
                }
            }

            if (!providedWindowsMenu)
            {
                [m_state->mainMenu insertItem:m_state->windowsItem atIndex:windowsIndex];

                [NSApp setWindowsMenu:m_state->windowsMenu];
            }

            m_state->snapshot = menus;
        }
    }
}

bool EditorMenuBar::TakeCommand(std::string& id)
{
    std::optional<std::string> command = m_state->commands.TryPop();

    if (command.has_value())
    {
        id = std::move(*command);
    }

    return command.has_value();
}
} // namespace zen::editor
