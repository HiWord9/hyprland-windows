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

WindowAction ParseWindowAction(PCWSTR raw) {
    std::wstring s = NormalizeSettingString(raw);
    if (s == L"OFF" || s == L"NONE") {
        return WindowAction::None;
    }
    if (s == L"TITLEBAR") {
        return WindowAction::ToggleTitleBar;
    }
    if (s == L"CLOSE") {
        return WindowAction::Close;
    }
    return WindowAction::ToggleMaximize;  // what the settings say by default
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
