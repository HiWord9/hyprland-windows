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

////////////////////////////////////////////////////////////////////////////////
// The same bindings, before Windows sees them
//
// Windows takes the keys of its own shortcuts - Win+W, Win+E... - before any
// window gets them, so a binding on one of those never reaches HandleHotkey.
// The keyboard thread's hook (keyboard.cpp) sees every key first and hands
// it here. A key it takes never reaches a window, so nothing is done
// twice, and HandleHotkey keeps working where there is no such hook.

// The key whose press was taken, so that its repeats and its release are
// taken too. Only touched on the server thread.
UINT g_takenKey;

// Returns true if the key is to be swallowed.
bool HandleBindingKey(UINT vk, bool down) {
    if (!down) {
        if (vk != g_takenKey) {
            return false;
        }
        g_takenKey = 0;
        return true;
    }
    if (vk == g_takenKey) {
        return true;  // auto-repeat of a key that was taken
    }

    Hotkey titleBar = g_settings.hotkey;
    Hotkey shortcut = g_settings.windowShortcut;
    WindowAction action = g_settings.windowShortcutAction;
    bool toggle = MatchesShortcut(titleBar, vk, true);
    bool act = !toggle && action != WindowAction::None &&
               !IsMouseButtonVk(shortcut.vk) &&
               MatchesShortcut(shortcut, vk, true);
    if (!toggle && !act) {
        return false;
    }
    // Taken even with nothing to act on - the desktop, the taskbar: a binding
    // is the user's, and Windows' own shortcut on the same keys doesn't come
    // back just because no window is in front.
    HWND target = GetAncestor(GetForegroundWindow(), GA_ROOT);
    g_takenKey = vk;
    // Win or Alt would otherwise count as tapped on their own when they come
    // up, the key between them having been taken: Start would open, or the
    // window's menu.
    Hotkey used = toggle ? titleBar : shortcut;
    if (used.win || used.alt) {
        MaskModifierTap();
    }
    if (toggle) {
        Wh_Log(L"Hotkey: toggling %p (keyboard hook)", target);
        RequestFrameless(target, kActionToggle);
    } else if (IsFrameWindow(target)) {
        Wh_Log(L"Shortcut key on %p (keyboard hook)", target);
        RequestWindowAction(target, action);
    }
    return true;
}
