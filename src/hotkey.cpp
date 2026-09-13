// The hotkey that toggles the title bar of the focused window.
#include "common.h"

bool HandleHotkey(const MSG* msg) {
    if (msg->lParam & (1 << 30)) {
        return false;  // auto-repeat
    }

    UINT vk = g_settings.hotkeyVk;
    if (!vk || msg->wParam != vk) {
        return false;
    }

    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    bool alt = GetKeyState(VK_MENU) < 0;
    bool shift = GetKeyState(VK_SHIFT) < 0;
    bool win = GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0;
    if (ctrl != g_settings.hotkeyCtrl || alt != g_settings.hotkeyAlt ||
        shift != g_settings.hotkeyShift || win != g_settings.hotkeyWin) {
        return false;
    }

    HWND target = msg->hwnd ? GetAncestor(msg->hwnd, GA_ROOT)
                            : GetForegroundWindow();
    Wh_Log(L"Hotkey: toggling %p", target);
    RequestFrameless(target, kActionToggle);
    return true;
}
