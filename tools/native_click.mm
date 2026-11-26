// Real AppKit mouse events for proc_test, so clicks go through WebKit the way a person's do
// (mouse down, a pause, mouse up), not through element.click().
#import <AppKit/AppKit.h>
#include <juce_gui_basics/juce_gui_basics.h>

static void post (NSView* view, NSEventType type, juce::Point<int> pos)
{
    const NSPoint inWindow = [view convertPoint: NSMakePoint (pos.x, pos.y) toView: nil];
    NSEvent* e = [NSEvent mouseEventWithType: type
                                    location: inWindow
                               modifierFlags: 0
                                   timestamp: [[NSProcessInfo processInfo] systemUptime]
                                windowNumber: [[view window] windowNumber]
                                     context: nil
                                 eventNumber: 0
                                  clickCount: 1
                                    pressure: type == NSEventTypeLeftMouseUp ? 0.0f : 1.0f];
    [[view window] sendEvent: e];
}

// pos is in the editor's coordinates (its peer's view is flipped, like JUCE).
void nativeClick (juce::Component& editor, juce::Point<int> pos, int holdMs, juce::Point<int> dragTo)
{
    auto* view = (NSView*) editor.getPeer()->getNativeHandle();
    if (! [[view window] isKeyWindow])
    {
        [NSApp activateIgnoringOtherApps: YES];
        [[view window] makeKeyAndOrderFront: nil];
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    }
    post (view, NSEventTypeLeftMouseDown, pos);
    const int steps = dragTo == pos ? 1 : 8;
    for (int i = 1; i <= steps; ++i)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (holdMs / steps);
        if (dragTo != pos)
            post (view, NSEventTypeLeftMouseDragged, pos + (dragTo - pos) * i / steps);
    }
    post (view, NSEventTypeLeftMouseUp, dragTo);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
}

// A real key press (down, up) to the window's first responder, e.g. Option+. for pause.
void nativeKey (juce::Component& editor, unsigned short keyCode, const char* chars, const char* charsIgnoringModifiers, bool option)
{
    auto* view = (NSView*) editor.getPeer()->getNativeHandle();
    const NSEventModifierFlags flags = option ? NSEventModifierFlagOption : 0;
    for (NSEventType type : { NSEventTypeKeyDown, NSEventTypeKeyUp })
    {
        NSEvent* e = [NSEvent keyEventWithType: type
                                      location: NSZeroPoint
                                 modifierFlags: flags
                                     timestamp: [[NSProcessInfo processInfo] systemUptime]
                                  windowNumber: [[view window] windowNumber]
                                       context: nil
                                    characters: [NSString stringWithUTF8String: chars]
                   charactersIgnoringModifiers: [NSString stringWithUTF8String: charsIgnoringModifiers]
                                     isARepeat: NO
                                       keyCode: keyCode];
        [[view window] sendEvent: e];
        juce::MessageManager::getInstance()->runDispatchLoopUntil (40);
    }
}
