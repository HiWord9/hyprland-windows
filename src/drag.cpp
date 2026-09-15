// Win + mouse: moving and resizing windows through the system's own
// move/resize loops.
//
// The mod never runs such a loop itself. A drag is requested by posting
// g_msgDrag to the window that is to be moved or resized, and when that
// window's own thread retrieves the request, the message is rewritten into the
// one the system starts the loop from. The loop then runs where a title-bar
// drag would run it - inside the application's DispatchMessage - instead of
// inside the mod's message hook, which matters twice:
//
//   * A drag can last minutes. With the loop below one of our frames, the mod
//     could not be unloaded for as long as it lasts: the image would go away
//     under a thread that still has to return into it.
//   * A request survives crossing a thread or a process boundary, which a
//     WM_SYSCOMMAND posted into another process would not - the UIPI message
//     filter drops that one.
#include "common.h"

UINT g_msgDrag;  // RegisterWindowMessage, set in Wh_ModInit

bool IsDragModifierDown() {
    if (g_settings.dragModifier == DragModifier::Alt) {
        return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    }
    return ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) !=
           0;
}

// Whether a system move/resize loop is running on this thread. The loop pumps
// messages, so the mod sees them while it runs.
bool IsInMoveSizeLoop() {
    GUITHREADINFO info{sizeof(info)};
    return GetGUIThreadInfo(GetCurrentThreadId(), &info) &&
           (info.flags & GUI_INMOVESIZE);
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

void RequestDrag(HWND root, WPARAM kind, POINT pt) {
    // Posted, not sent: the loop has to run on the window's own thread. It
    // also means the request is retrieved before any input still queued, so a
    // button release already on its way is seen by the loop rather than by the
    // application.
    if (!PostMessageW(root, g_msgDrag, kind, MAKELPARAM(pt.x, pt.y))) {
        Wh_Log(L"Drag request for %p failed (%u)", root, GetLastError());
    }
}

////////////////////////////////////////////////////////////////////////////////
// Move

bool StartMove(HWND root, POINT pt, MSG* msg) {
    if (IsIconic(root)) {
        return false;
    }
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        return false;  // released already; the loop would stick to the cursor
    }

    // The loop only follows the mouse if this thread's synchronized button
    // state says the button is down, which it is not when another thread
    // retrieved the press.
    if (GetKeyState(VK_LBUTTON) >= 0) {
        BYTE keyState[256];
        if (GetKeyboardState(keyState)) {
            keyState[VK_LBUTTON] |= 0x80;
            SetKeyboardState(keyState);
        }
    }

    // The system's own caption-drag loop, so the drag gives everything a
    // title-bar drag would: Aero Snap at the screen edges, the Windows 11
    // snap-layouts flyout when the cursor reaches the top, and automatic
    // restore of a maximized window.
    msg->message = WM_SYSCOMMAND;
    msg->wParam = SC_MOVE | HTCAPTION;
    msg->lParam = MAKELPARAM(pt.x, pt.y);
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Resize
//
// The system's border-resize loop only tracks the LEFT mouse button, so a
// synthetic one is held for as long as the resize lasts: the watcher thread
// presses it, and releases it once the user lets go of the physical right
// button. That press comes back as an ordinary button-down message, and it is
// that message which the loop is started from - so the button state the loop
// sees is real, and the click never reaches the application. Entering with the
// cursor away from the corner keeps the grab offset, so the resize is
// relative, like Hyprland's, and because this is the real resize path,
// GPU-composited windows (Chrome, Electron) reflow live and native menu bars
// don't flicker.

// The resize the next left button-down on this thread belongs to: while this
// is set, a press is the watcher's, not the user's.
struct PendingResize {
    HWND root = nullptr;
    UINT corner = 0;
    ULONGLONG tick = 0;
};
thread_local PendingResize g_pendingResize;

// The press should arrive a message or two later. Anything later than that is
// a click that went elsewhere - the cursor left the window just as the resize
// started, say - and starting a resize from it would come as a surprise.
constexpr ULONGLONG kPendingResizeTimeoutMs = 1000;

struct ResizeWatch {
    int rightVk;
};

DWORD WINAPI ResizeWatchThread(LPVOID param) {
    {
        std::unique_ptr<ResizeWatch> watch(static_cast<ResizeWatch*>(param));
        InjectMouseButton(MOUSEEVENTF_LEFTDOWN);
        while ((GetAsyncKeyState(watch->rightVk) & 0x8000) &&
               !g_uninitializing) {
            Sleep(8);
        }
        InjectMouseButton(MOUSEEVENTF_LEFTUP);
    }
    g_modRefCount--;  // the last thing this thread does in the mod's image
    return 0;
}

// The thread owns its state and nothing waits for it, so a resize leaves no
// frame of the mod's on the stack of the window's thread. The reference it
// holds is what keeps the image around for as long as it runs.
bool StartResizeWatcher() {
    auto* watch = new ResizeWatch{PhysicalButtonVk(true)};
    g_modRefCount++;
    HANDLE thread =
        CreateThread(nullptr, 0, ResizeWatchThread, watch, 0, nullptr);
    if (!thread) {
        Wh_Log(L"CreateThread failed (%u)", GetLastError());
        g_modRefCount--;
        delete watch;
        return false;
    }
    CloseHandle(thread);
    return true;
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

    g_pendingResize = {root, ResizeCornerForPoint(rc, pt), GetTickCount64()};
    if (!StartResizeWatcher()) {
        g_pendingResize = {};
    }
}

bool HasPendingResize() {
    return g_pendingResize.root != nullptr &&
           GetTickCount64() - g_pendingResize.tick <= kPendingResizeTimeoutMs;
}

bool HandleInjectedResizeEntry(MSG* msg) {
    PendingResize pending = g_pendingResize;
    g_pendingResize = {};
    if (!pending.root ||
        GetTickCount64() - pending.tick > kPendingResizeTimeoutMs) {
        return false;
    }
    if (!(GetAsyncKeyState(PhysicalButtonVk(true)) & 0x8000)) {
        // Let go before the loop could start. The watcher notices as well and
        // releases the synthetic button on its own.
        return false;
    }

    msg->hwnd = pending.root;
    msg->message = WM_NCLBUTTONDOWN;
    msg->wParam = pending.corner;
    msg->lParam = MAKELPARAM(msg->pt.x, msg->pt.y);
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// The requests, and the presses that make them

bool HandleDragRequest(MSG* msg) {
    HWND root = msg->hwnd;
    POINT pt{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};

    // Checked again here: the request came from another thread, possibly in
    // another process, so the window may no longer be what it was.
    if (g_uninitializing || !IsFrameWindow(root)) {
        return false;
    }
    if (msg->wParam == kDragResize) {
        StartResize(root, pt);
        return false;  // the loop is started from the injected press
    }
    return StartMove(root, pt, msg);
}

// Per-thread: a button-up to swallow because we swallowed its button-down.
thread_local bool g_swallowButtonUp[2];  // [0] = left, [1] = right

bool HandleModifierButtonDown(const MSG* msg, bool right) {
    // A fresh press means a release we were still waiting to swallow is not
    // coming: a drag cancelled with Esc can leave the cursor outside the
    // window it started on, and the release then goes somewhere else.
    g_swallowButtonUp[right] = false;

    // A drag is already running on this thread: its loop pumps messages, so
    // presses during one arrive here too.
    if (g_uninitializing || IsInMoveSizeLoop() || !IsDragModifierDown()) {
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
        // the resize loop ends on the mod's synthetic left-up, not on the
        // physical right-up, which would otherwise reach the app.
        g_swallowButtonUp[1] = true;
    }
    // The move loop consumes the physical left-up itself, so a move leaves
    // nothing to swallow.
    RequestDrag(root, right ? kDragResize : kDragMove, msg->pt);
    return true;
}

bool HandleButtonUp(bool right) {
    if (!g_swallowButtonUp[right]) {
        return false;
    }
    g_swallowButtonUp[right] = false;
    return true;
}
