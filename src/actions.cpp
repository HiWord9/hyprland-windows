// What a mouse gesture with the modifier held does to the window under the
// cursor, and the gesture recognition that decides one happened.
//
// The action itself runs on the window's own thread, like everything else in
// the mod that touches a window: the request is posted with the drag message
// and carried out where the window lives. That way an application that
// handles the system command itself still gets its say, and the UIPI filter
// does not drop the request on its way to a window that belongs to another
// process - a UWP frame, for one.
#include "common.h"

WindowAction ParseWindowAction(PCWSTR raw, WindowAction whenEmpty) {
    std::wstring s = NormalizeSettingString(raw);
    if (s == L"OFF" || s == L"NONE") {
        return WindowAction::None;
    }
    if (s == L"MAXIMIZE") {
        return WindowAction::ToggleMaximize;
    }
    if (s == L"TITLEBAR") {
        return WindowAction::ToggleTitleBar;
    }
    if (s == L"CLOSE") {
        return WindowAction::Close;
    }
    // Anything unrecognized, an empty setting included, is what this binding
    // says it does by default.
    return whenEmpty;
}

// Zero follows the double-click speed from the mouse settings, which is what
// the rest of Windows goes by and what most people want.
int DoubleClickTimeMs() {
    int configured = g_settings.doubleClickTime;
    return configured > 0 ? configured : (int)GetDoubleClickTime();
}

// Runs on the window's own thread.
void DoWindowAction(HWND hwnd, WindowAction action) {
    switch (action) {
        case WindowAction::ToggleMaximize:
            // Through the window's system menu rather than ShowWindow, so an
            // application that does its own thing with SC_MAXIMIZE keeps
            // doing it.
            SendMessageW(hwnd, WM_SYSCOMMAND,
                         IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
            break;
        case WindowAction::ToggleTitleBar:
            HandleFramelessRequest(hwnd, kActionToggle);
            break;
        case WindowAction::Close:
            // SC_CLOSE, not a kill: an application with unsaved work gets to
            // ask about it.
            SendMessageW(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
            break;
        case WindowAction::None:
            break;
    }
}

void RequestWindowAction(HWND root, WindowAction action) {
    if (!PostMessageW(root, g_msgDrag, kDragAction, (LPARAM)action)) {
        Wh_Log(L"Action request for %p failed (%u)", root, GetLastError());
    }
}

////////////////////////////////////////////////////////////////////////////////
// The shortcut: modifiers and one key or mouse button

bool IsMouseButtonVk(UINT vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
           vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

// Which button a message is about, as a virtual key; 0 for anything that is
// not a button press or release.
UINT ButtonVkForMessage(UINT message, WPARAM wParam) {
    switch (message) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONUP:
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
        case WM_NCLBUTTONUP:
            return VK_LBUTTON;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
        case WM_RBUTTONUP:
        case WM_NCRBUTTONDOWN:
        case WM_NCRBUTTONDBLCLK:
        case WM_NCRBUTTONUP:
            return VK_RBUTTON;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
        case WM_MBUTTONUP:
        case WM_NCMBUTTONDOWN:
        case WM_NCMBUTTONDBLCLK:
        case WM_NCMBUTTONUP:
            return VK_MBUTTON;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONDBLCLK:
        case WM_XBUTTONUP:
        case WM_NCXBUTTONDOWN:
        case WM_NCXBUTTONDBLCLK:
        case WM_NCXBUTTONUP:
            // The button is in the high word for both the client and the
            // non-client messages; only the low word differs between them.
            return GET_XBUTTON_WPARAM(wParam) == XBUTTON2 ? VK_XBUTTON2
                                                          : VK_XBUTTON1;
        default:
            return 0;
    }
}

bool MatchesShortcut(const Hotkey& binding, UINT vk, bool now) {
    if (!binding.vk || binding.vk != vk) {
        return false;
    }
    // Ctrl, Alt and Shift as the thread saw them when it took the message,
    // which is what a keyboard shortcut is about - or, for the keyboard hook,
    // whose thread takes no keyboard input of its own, as they are now. The
    // Win key is asked for globally either way: it belongs to the shell, and
    // a thread's own copy of the keyboard state does not reliably hear about
    // it.
    auto held = [now](int vk) {
        return now ? (GetAsyncKeyState(vk) & 0x8000) != 0 : GetKeyState(vk) < 0;
    };
    bool ctrl = held(VK_CONTROL);
    bool alt = held(VK_MENU);
    bool shift = held(VK_SHIFT);
    bool win =
        ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
    return ctrl == binding.ctrl && alt == binding.alt &&
           shift == binding.shift && win == binding.win;
}

// A button press we consumed, so that its release goes the same way instead
// of reaching the application on its own.
thread_local UINT g_swallowShortcutButton;

bool HandleShortcutButton(const MSG* msg) {
    UINT vk = ButtonVkForMessage(msg->message, msg->wParam);
    if (!vk) {
        return false;
    }
    // Any fresh press means the release we were waiting to swallow is not
    // coming any more.
    g_swallowShortcutButton = 0;

    Hotkey binding = g_settings.windowShortcut;
    WindowAction action = g_settings.windowShortcutAction;
    if (g_uninitializing || action == WindowAction::None ||
        !MatchesShortcut(binding, vk)) {
        return false;
    }

    HWND root = GetAncestor(msg->hwnd, GA_ROOT);
    if (!root) {
        root = msg->hwnd;
    }
    if (!IsFrameWindow(root)) {
        return false;  // desktop, taskbar, menus... - normal click
    }

    Wh_Log(L"Shortcut on %p", root);
    ArmWinMask(binding.win);
    g_swallowShortcutButton = vk;
    RequestWindowAction(root, action);
    return true;
}

bool HandleShortcutButtonUp(const MSG* msg) {
    UINT vk = ButtonVkForMessage(msg->message, msg->wParam);
    if (!vk || g_swallowShortcutButton != vk) {
        return false;
    }
    g_swallowShortcutButton = 0;
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Spotting a double click
//
// Not from WM_LBUTTONDBLCLK: that one is only ever sent to windows whose
// class asked for double clicks, so half the applications out there would
// never see the gesture. The first click of the pair has also usually been
// swallowed into a drag of ours by then, which is fine - a double click on a
// title bar starts a zero-length caption drag in Windows too.

thread_local DWORD g_lastPressTick;
thread_local POINT g_lastPressPt;
thread_local HWND g_lastPressRoot;

// Takes the tick rather than reading the clock, so the rules can be tested
// without waiting half a second for each of them.
bool IsDoubleClickAt(HWND root, POINT pt, DWORD tick) {
    // The double-click rectangle is a width and a height around the first
    // click, so what each press may be off by is half of it.
    bool together =
        g_lastPressRoot == root && g_lastPressTick != 0 &&
        (int)(tick - g_lastPressTick) <= DoubleClickTimeMs() &&
        abs(pt.x - g_lastPressPt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
        abs(pt.y - g_lastPressPt.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;

    g_lastPressPt = pt;
    g_lastPressRoot = together ? nullptr : root;
    // A third click starts over rather than counting as a second double one.
    g_lastPressTick = together ? 0 : tick;
    return together;
}

void ForgetLastPress() {
    g_lastPressTick = 0;
    g_lastPressRoot = nullptr;
}
