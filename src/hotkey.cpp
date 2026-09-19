// The key bindings: the one that toggles the title bar of the focused window,
// and the window shortcut when it has been bound to a key rather than to a
// mouse button.
#include "common.h"

bool HandleHotkey(const MSG* msg) {
    if (msg->lParam & (1 << 30)) {
        return false;  // auto-repeat
    }

    UINT vk = (UINT)msg->wParam;
    HWND target =
        msg->hwnd ? GetAncestor(msg->hwnd, GA_ROOT) : GetForegroundWindow();

    Hotkey titleBar = g_settings.hotkey;
    if (MatchesShortcut(titleBar, vk)) {
        Wh_Log(L"Hotkey: toggling %p", target);
        RequestFrameless(target, kActionToggle);
        return true;
    }

    // A shortcut bound to a key acts on the focused window - there is no
    // cursor in the gesture to point at anything else.
    Hotkey shortcut = g_settings.windowShortcut;
    WindowAction action = g_settings.windowShortcutAction;
    if (!g_uninitializing && action != WindowAction::None &&
        !IsMouseButtonVk(shortcut.vk) && MatchesShortcut(shortcut, vk) &&
        IsFrameWindow(target)) {
        Wh_Log(L"Shortcut key on %p", target);
        RequestWindowAction(target, action);
        return true;
    }
    return false;
}
