// Win + mouse: moving and resizing windows through the system's own
// move/resize loops.
//
// The mod never runs such a loop itself. A drag is requested by posting
// g_msgDrag to the window that is to be moved or resized, and when that
// window's own thread retrieves the request, the message is rewritten into the
// system command the loop starts from. The loop then runs where a title-bar
// drag would run it - inside the application's DispatchMessage - instead of
// inside the mod's message hook, which matters twice:
//
//   * A drag can last minutes. With the loop below one of our frames, the mod
//     could not be unloaded for as long as it lasts: the image would go away
//     under a thread that still has to return into it.
//   * A request survives crossing a thread or a process boundary, which a
//     WM_SYSCOMMAND posted into another process would not - the UIPI message
//     filter drops that one.
//
// Neither drag touches the global mouse state. The only thing the loops want
// that a Win + mouse drag cannot give them is the left mouse button: they
// track the mouse while it is held and end when it is released. Both of those
// are per-thread rather than global - the button state a loop reads is its own
// thread's synchronized copy, and the release is just a message - so the mod
// fakes the first and posts the second, and no other window ever sees a click
// that wasn't there.
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

// A move or size loop only follows the mouse if this thread's synchronized
// button state says the left button is down. For a resize it never is - the
// user is holding the right one - and for a move it isn't either when another
// thread retrieved the press. Without this the loop starts in its keyboard
// mode instead, where it waits for the arrow keys and ignores the mouse.
void ForceLeftButtonDown() {
    if (GetKeyState(VK_LBUTTON) < 0) {
        return;
    }
    BYTE keyState[256];
    if (GetKeyboardState(keyState)) {
        keyState[VK_LBUTTON] |= 0x80;
        SetKeyboardState(keyState);
    }
}

// Nearest corner to the cursor, as the WMSZ_* code SC_SIZE expects (Hyprland
// resizes from the nearest corner).
UINT ResizeEdgeForPoint(const RECT& rc, POINT pt) {
    bool left = pt.x < (rc.left + rc.right) / 2;
    bool top = pt.y < (rc.top + rc.bottom) / 2;
    if (top) {
        return left ? WMSZ_TOPLEFT : WMSZ_TOPRIGHT;
    }
    return left ? WMSZ_BOTTOMLEFT : WMSZ_BOTTOMRIGHT;
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
    ForceLeftButtonDown();

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
// Started from where the cursor is rather than from the corner, so the grab
// offset is kept and the resize is relative, like Hyprland's. Because this is
// the system's own resize loop, GPU-composited windows (Chrome, Electron)
// reflow live and native menu bars don't flicker.
//
// The loop ends when the left button is released, which is never going to
// happen here - the user releases the right one - so a thread waits for that
// and posts the release the loop is waiting for.

// Set while a resize of ours is running: the release below is the mod's, and
// if the loop has already ended without it - cancelled with Esc - it is the
// mod's to swallow as well.
thread_local bool g_pendingRelease;

struct ResizeRelease {
    HWND root;
    int rightVk;
};

DWORD WINAPI ResizeReleaseThread(LPVOID param) {
    {
        std::unique_ptr<ResizeRelease> release(
            static_cast<ResizeRelease*>(param));
        while ((GetAsyncKeyState(release->rightVk) & 0x8000) &&
               !g_uninitializing) {
            Sleep(8);
        }
        POINT pt;
        GetCursorPos(&pt);
        PostMessageW(release->root, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
    }
    g_modRefCount--;  // the last thing this thread does in the mod's image
    return 0;
}

// The thread owns its state and nothing waits for it, so a resize leaves no
// frame of the mod's on the stack of the window's thread. The reference it
// holds is what keeps the image around for as long as it runs.
bool StartResizeRelease(HWND root) {
    auto* release = new ResizeRelease{root, PhysicalButtonVk(true)};
    g_modRefCount++;
    HANDLE thread =
        CreateThread(nullptr, 0, ResizeReleaseThread, release, 0, nullptr);
    if (!thread) {
        Wh_Log(L"CreateThread failed (%u)", GetLastError());
        g_modRefCount--;
        delete release;
        return false;
    }
    CloseHandle(thread);
    return true;
}

bool StartResize(HWND root, POINT pt, MSG* msg) {
    if (IsIconic(root) || IsZoomed(root)) {
        return false;
    }
    if (!(GetWindowLongPtrW(root, GWL_STYLE) & WS_THICKFRAME)) {
        return false;  // fixed-size window
    }
    RECT rc;
    if (!GetWindowRect(root, &rc)) {
        return false;
    }
    if (!(GetAsyncKeyState(PhysicalButtonVk(true)) & 0x8000)) {
        return false;  // released already; the loop would stick to the cursor
    }
    if (!StartResizeRelease(root)) {
        return false;  // nothing would end the loop
    }

    g_pendingRelease = true;
    ForceLeftButtonDown();
    msg->message = WM_SYSCOMMAND;
    msg->wParam = SC_SIZE | ResizeEdgeForPoint(rc, pt);
    msg->lParam = MAKELPARAM(pt.x, pt.y);
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// The requests, and the presses that make them

bool HandleDragRequest(MSG* msg) {
    HWND root = msg->hwnd;
    POINT pt{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};

    if (msg->wParam == kDragUnfade) {
        EndDragFade(root);
        return false;  // ours, and there is nothing to dispatch
    }

    // Checked again here: the request came from another thread, possibly in
    // another process, so the window may no longer be what it was.
    if (g_uninitializing || !IsFrameWindow(root)) {
        return false;
    }
    // Kept, because starting the drag is what overwrites it with the system
    // command the loop needs.
    WPARAM kind = msg->wParam;
    bool started = kind == kDragResize ? StartResize(root, pt, msg)
                                       : StartMove(root, pt, msg);
    if (started) {
        BeginDragFade(root, kind);
    }
    return started;
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
        // the resize loop ends on the release the mod posts, not on the
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

bool HandleLeftButtonUp() {
    if (IsInMoveSizeLoop()) {
        // The loop is what this release is for, and what consumes it.
        g_pendingRelease = false;
        return false;
    }
    if (g_pendingRelease) {
        // The loop ended without it, so the application never saw the press
        // this would have released either.
        g_pendingRelease = false;
        return true;
    }
    return HandleButtonUp(false);
}
