// Win + mouse: moving and resizing windows through the system's own
// move/resize loops.
#include "common.h"

bool IsDragModifierDown() {
    if (g_settings.dragModifier == DragModifier::Alt) {
        return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    }
    return ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) !=
           0;
}
int PhysicalButtonVk(bool right) {
    bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    return (right != swapped) ? VK_RBUTTON : VK_LBUTTON;
}

// Injected mouse input carries kInjectedMarker in dwExtraInfo so the mod can
// tell its own synthetic clicks apart from the user's.
void InjectMouseButton(DWORD flags) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    input.mi.dwExtraInfo = kInjectedMarker;
    SendInput(1, &input, sizeof(input));
}

// Removes the mod's own left-button messages from this thread's queue without
// dispatching them, so the app never sees a stray click. During a resize the
// mod is the only source of left-button input (the user holds the right
// button), so any left-button message here is ours. Uses the un-hooked
// PeekMessage so the message hook doesn't re-enter.
void DrainInjectedLeftButton() {
    PeekMessageW_t peek =
        PeekMessageW_Original ? PeekMessageW_Original : PeekMessageW;
    MSG msg;
    for (UINT m : {WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK,
                   WM_NCLBUTTONDOWN, WM_NCLBUTTONUP}) {
        while (peek(&msg, nullptr, m, m, PM_REMOVE)) {
            // swallow
        }
    }
}

// Nearest corner to the cursor, as an HT* hit-test code (Hyprland resizes from
// the nearest corner).
UINT ResizeCornerForPoint(const RECT& rc, POINT pt) {
    bool left = pt.x < (rc.left + rc.right) / 2;
    bool top = pt.y < (rc.top + rc.bottom) / 2;
    if (top) {
        return left ? HTTOPLEFT : HTTOPRIGHT;
    }
    return left ? HTBOTTOMLEFT : HTBOTTOMRIGHT;
}

// Runs on a worker thread: once the user releases the physical (right) mouse
// button, release the mod's synthetic left button, which ends the native
// resize loop. `stop` lets the main thread cancel the wait (e.g. the loop
// ended via Esc while the right button was still down).
struct ResizeWatch {
    int rightVk;
    std::atomic<bool> stop;
};

DWORD WINAPI ResizeWatchThread(LPVOID param) {
    auto* watch = reinterpret_cast<ResizeWatch*>(param);
    while (!watch->stop && (GetAsyncKeyState(watch->rightVk) & 0x8000)) {
        Sleep(8);
    }
    if (!watch->stop) {
        InjectMouseButton(MOUSEEVENTF_LEFTUP);
    }
    return 0;
}

void StartMove(HWND root, POINT pt) {
    if (IsIconic(root)) {
        return;
    }

    // Hand the move to the window's own thread so the *system* runs its native
    // caption-drag loop. That gives everything a title-bar drag would: Aero
    // Snap at the screen edges, the Windows 11 snap-layouts flyout when the
    // cursor reaches the top, and automatic restore of a maximized window.
    LPARAM lp = MAKELPARAM(pt.x, pt.y);
    if (GetWindowThreadProcessId(root, nullptr) == GetCurrentThreadId()) {
        DefWindowProcW(root, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, lp);
    } else {
        PostMessageW(root, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, lp);
    }
}

void StartResize(HWND root, POINT pt) {
    if (IsIconic(root) || IsZoomed(root)) {
        return;
    }
    if (!(GetWindowLongPtrW(root, GWL_STYLE) & WS_THICKFRAME)) {
        return;  // fixed-size window
    }

    RECT rc;
    if (!GetWindowRect(root, &rc)) {
        return;
    }
    UINT htCorner = ResizeCornerForPoint(rc, pt);

    // The system's border-resize loop only tracks the cursor while the left
    // mouse button is down, so hold a synthetic one (its click swallowed so
    // the app never sees it) and release it when the user lets go of the
    // physical right button. Entering with the cursor away from the corner
    // keeps the grab offset, so the resize is relative - like Hyprland - and
    // because this is the real resize path, GPU-composited windows (Chrome,
    // Electron) reflow live and native menu bars don't flicker.
    InjectMouseButton(MOUSEEVENTF_LEFTDOWN);
    for (int i = 0; i < 100 && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000); i++) {
        DrainInjectedLeftButton();
        Sleep(2);
    }
    DrainInjectedLeftButton();

    ResizeWatch watch{PhysicalButtonVk(true), false};
    HANDLE thread =
        CreateThread(nullptr, 0, ResizeWatchThread, &watch, 0, nullptr);

    DefWindowProcW(root, WM_NCLBUTTONDOWN, htCorner, MAKELPARAM(pt.x, pt.y));

    watch.stop = true;
    if (thread) {
        WaitForSingleObject(thread, 2000);
        CloseHandle(thread);
    }
    // Make sure the synthetic left button is released (e.g. if the loop ended
    // via Esc before the watcher fired), then swallow its messages.
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
        InjectMouseButton(MOUSEEVENTF_LEFTUP);
    }
    DrainInjectedLeftButton();
}

// Per-thread: a button-up to swallow because we swallowed its button-down.
thread_local bool g_swallowButtonUp[2];  // [0] = left, [1] = right

bool HandleModifierButtonDown(const MSG* msg, bool right) {
    if (!IsDragModifierDown()) {
        return false;
    }

    HWND root = GetAncestor(msg->hwnd, GA_ROOT);
    if (!root) {
        root = msg->hwnd;
    }
    if (!IsFrameWindow(root)) {
        return false;  // desktop, taskbar, menus... - normal click
    }

    Wh_Log(L"%s %p", right ? L"Resize" : L"Move", root);
    ArmWinMask();

    if (right) {
        // We consumed the right button-down, so swallow its matching up too:
        // the native resize loop ends on the mod's synthetic left-up, not the
        // physical right-up, which would otherwise reach the app.
        g_swallowButtonUp[1] = true;
        StartResize(root, msg->pt);
    } else {
        // The native move loop consumes the physical left-up itself, so there
        // is nothing left to swallow.
        StartMove(root, msg->pt);
    }
    return true;
}

bool HandleButtonUp(bool right) {
    if (!g_swallowButtonUp[right]) {
        return false;
    }
    g_swallowButtonUp[right] = false;
    return true;
}
