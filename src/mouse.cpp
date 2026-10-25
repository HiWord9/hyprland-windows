// The presses no message hook gets to see.
//
// Everything in the mod meets a press in the message hook of the thread it is
// for. Content drawn by composition alone - WinUI and XAML: Paint, Windows
// Terminal, WhatsApp, the tabs of File Explorer - is not delivered that way:
// Windows hands the input over such a window to the app's own input stack,
// past every message queue, and goes by the window's region to decide.
//
// So the shell runs a low-level mouse hook as well, next to the keyboard one
// on the same never-busy thread (keyboard.cpp). A press the mod would want
// that lands on such content is taken there and handed to the window, whose
// own thread opens a hole of a few pixels in the content's region under the
// cursor and presses again: the press goes through the hole to the window
// underneath, as a message, and the region closes again the moment it has
// arrived. From there on it is an ordinary press - Windows keeps the button
// state for it and holds the mouse for the window it went to - and the
// message hook does the rest the way it always does, natively.
//
// The window shortcut is simpler: it wants nothing of the press but which
// window it was over, so that hook takes it as it comes, for any window.
#include "common.h"

constexpr int kHoleHalf = 1;      // around the cursor's pixel
constexpr DWORD kHoldCursorMaxMs = 200;  // see LowLevelMouseProc
constexpr DWORD kUncoverMaxWaitMs = 500;

// Composition draws all of it: a child window without a redirection bitmap.
bool IsCompositionContent(HWND hwnd) {
    return hwnd && (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) &&
           (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP);
}

BOOL CALLBACK FindCompositionChildProc(HWND child, LPARAM lParam) {
    if (IsCompositionContent(child)) {
        *reinterpret_cast<bool*>(lParam) = true;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK FindCompositionWindowProc(HWND hwnd, LPARAM lParam) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) &&
        IsFrameWindow(hwnd)) {
        EnumChildWindows(hwnd, FindCompositionChildProc, lParam);
    }
    return !*reinterpret_cast<bool*>(lParam);
}

// A visible window of this process with such content in it.
bool HasCompositionContentWindow() {
    bool found = false;
    EnumWindows(FindCompositionWindowProc, (LPARAM)&found);
    return found;
}

// The content under the point: the window a hit test finds, or a child of
// it that the hit test went past - XAML Islands content is not found by one,
// and gets the input all the same.
HWND CompositionContentAt(HWND hwnd, POINT pt) {
    if (IsCompositionContent(hwnd)) {
        return hwnd;
    }
    POINT client = pt;
    ScreenToClient(hwnd, &client);
    HWND child = ChildWindowFromPointEx(hwnd, client, CWP_SKIPINVISIBLE);
    return child != hwnd && IsCompositionContent(child) ? child : nullptr;
}

////////////////////////////////////////////////////////////////////////////////
// On the window's thread: the hole, and the press through it

struct Uncovered {
    HWND hwnd;
    HRGN region;  // what it had, or null for none
    DWORD threadId;
};
// In one place for every thread, so that the teardown can close what is open.
std::mutex g_uncoveredMutex;
std::vector<Uncovered> g_uncovered;

// What shows through the hole is the window underneath, which draws nothing
// there: a speck, for a frame. So the pixels under the cursor are taken from
// the screen first, and shown over the hole until it has closed again - in a
// window of the system's own class, layered and passed through by the mouse,
// so that nothing of the mod's runs in it.
struct Patch {
    HWND hwnd;
    DWORD threadId;
};
std::vector<Patch> g_patches;  // guarded by g_uncoveredMutex
constexpr int kHoleSize = 2 * kHoleHalf + 1;

// With the screen magnified, a picture of it is the magnified one: the
// pixels under the cursor are found where the magnifier shows them, and
// shrunk back to size. Asked only while Windows' Magnifier is up.
struct Magnification {
    float level;
    POINT offset;
};

Magnification ScreenMagnification() {
    Magnification none{1, {0, 0}};
    if (!FindWindowW(L"MagUIClass", nullptr)) {
        return none;
    }
    HMODULE lib = LoadLibraryW(L"magnification.dll");
    if (!lib) {
        return none;
    }
    using Init = BOOL(WINAPI*)();
    using Get = BOOL(WINAPI*)(float*, int*, int*);
    auto init = (Init)GetProcAddress(lib, "MagInitialize");
    auto uninit = (Init)GetProcAddress(lib, "MagUninitialize");
    auto get = (Get)GetProcAddress(lib, "MagGetFullscreenTransform");
    Magnification found = none;
    if (init && uninit && get && init()) {
        float level;
        int x, y;
        if (get(&level, &x, &y) && level > 1) {
            found = {level, {x, y}};
        }
        uninit();
    }
    FreeLibrary(lib);
    return found;
}

HWND ShowPatch(POINT pt) {
    POINT pos{pt.x - kHoleHalf, pt.y - kHoleHalf};
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, kHoleSize, kHoleSize);
    HGDIOBJ old = SelectObject(memory, bitmap);
    Magnification mag = ScreenMagnification();
    if (mag.level > 1) {
        int side = (int)(kHoleSize * mag.level + 0.5f);
        StretchBlt(memory, 0, 0, kHoleSize, kHoleSize, screen,
                   (int)((pos.x - mag.offset.x) * mag.level + 0.5f),
                   (int)((pos.y - mag.offset.y) * mag.level + 0.5f), side, side,
                   SRCCOPY);
    } else {
        BitBlt(memory, 0, 0, kHoleSize, kHoleSize, screen, pos.x, pos.y,
               SRCCOPY);
    }
    HWND patch = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE,
        L"Static", nullptr, WS_POPUP, pos.x, pos.y, kHoleSize, kHoleSize,
        nullptr, nullptr, nullptr, nullptr);
    if (patch) {
        POINT source{0, 0};
        SIZE size{kHoleSize, kHoleSize};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, 0};
        UpdateLayeredWindow(patch, screen, &pos, &size, memory, &source, 0,
                            &blend, ULW_OPAQUE);
        ShowWindow(patch, SW_SHOWNOACTIVATE);
    }
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return patch;
}

void RestoreUncovered(bool allThreads) {
    DWORD threadId = GetCurrentThreadId();
    std::vector<Uncovered> closing;
    std::vector<HWND> patches;
    {
        std::lock_guard<std::mutex> lock(g_uncoveredMutex);
        for (auto it = g_uncovered.begin(); it != g_uncovered.end();) {
            if (allThreads || it->threadId == threadId) {
                closing.push_back(*it);
                it = g_uncovered.erase(it);
            } else {
                it++;
            }
        }
        for (auto it = g_patches.begin(); it != g_patches.end();) {
            if (it->threadId == threadId) {
                patches.push_back(it->hwnd);
                it = g_patches.erase(it);
            } else if (allThreads) {
                PostMessageW(it->hwnd, WM_CLOSE, 0, 0);
                it = g_patches.erase(it);
            } else {
                it++;
            }
        }
    }
    // Outside the lock: the teardown, closing a window of another thread,
    // waits for that thread, which may be about to take the lock itself. The
    // window keeps the region it is given, so the saved one is handed back
    // rather than freed here.
    for (const Uncovered& u : closing) {
        SetWindowRgn(u.hwnd, u.region, FALSE);
    }
    // The patch goes once the closed hole is on the screen, and no later: a
    // drag about to start takes the window away from under it, and a patch
    // left behind would show where the window was.
    if (!patches.empty()) {
        DwmFlush();
        for (HWND patch : patches) {
            DestroyWindow(patch);
        }
    }
}

void RestoreAllUncovered() {
    RestoreUncovered(true);
}

// The press has come through, so the content can have its region back.
// Should it have gone somewhere else after all, the shell's hook, which saw
// it go, says so too (kDragCover).
void RestoreUncoveredAfterReplay() {
    if (GetMessageExtraInfo() == (LPARAM)kReplayMarker) {
        RestoreUncovered();
    }
}

struct LayerSearch {
    POINT pt;
    std::vector<HWND> layers;
};

BOOL CALLBACK FindLayerProc(HWND child, LPARAM lParam) {
    auto* search = reinterpret_cast<LayerSearch*>(lParam);
    RECT rc;
    if (IsWindowVisible(child) && IsCompositionContent(child) &&
        GetWindowRect(child, &rc) && PtInRect(&rc, search->pt)) {
        search->layers.push_back(child);
    }
    return TRUE;
}

// Under g_uncoveredMutex.
bool OpenHole(HWND content, POINT pt) {
    RECT rc;
    GetWindowRect(content, &rc);
    HRGN saved = CreateRectRgn(0, 0, 0, 0);
    if (GetWindowRgn(content, saved) == ERROR) {
        DeleteObject(saved);
        saved = nullptr;
    }
    HRGN region = CreateRectRgn(0, 0, rc.right - rc.left, rc.bottom - rc.top);
    if (saved) {
        CombineRgn(region, saved, nullptr, RGN_COPY);
    }
    int x = pt.x - rc.left, y = pt.y - rc.top;
    HRGN hole = CreateRectRgn(x - kHoleHalf, y - kHoleHalf, x + kHoleHalf + 1,
                              y + kHoleHalf + 1);
    CombineRgn(region, region, hole, RGN_DIFF);
    DeleteObject(hole);
    if (!SetWindowRgn(content, region, FALSE)) {
        DeleteObject(region);
        if (saved) {
            DeleteObject(saved);
        }
        return false;
    }
    g_uncovered.push_back({content, saved, GetCurrentThreadId()});
    return true;
}

void UncoverAndPress(HWND root, const MSG* msg) {
    RestoreUncovered();
    // A window too busy to get to it in time doesn't press long after the
    // fact: the press is lost, the way it would be to a window that hangs.
    if (g_uninitializing || GetTickCount() - msg->time > kUncoverMaxWaitMs) {
        return;
    }
    // In pixels, whatever the application is aware of: the region, the
    // cursor and the window under it all have to agree.
    DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    POINT pt;
    GetCursorPos(&pt);
    HWND content = CompositionContentAt(WindowFromPoint(pt), pt);
    if (content && GetAncestor(content, GA_ROOT) == root) {
        // Such content can lie on more of it - File Explorer's title bar on
        // its tabs: the hole goes through every layer at the point, or the
        // press would only find the next one down.
        LayerSearch search{pt, {}};
        EnumChildWindows(root, FindLayerProc, (LPARAM)&search);

        // The patch on the screen first, then the holes under it.
        HWND patch = ShowPatch(pt);
        if (patch) {
            DwmFlush();
        }
        std::lock_guard<std::mutex> lock(g_uncoveredMutex);
        // Once the teardown has closed everything there was, nothing new is
        // opened for it to miss: the press goes in as it is.
        bool opened = false;
        for (HWND layer : search.layers) {
            opened |= !g_uninitializing && OpenHole(layer, pt);
        }
        if (patch && opened) {
            g_patches.push_back({patch, GetCurrentThreadId()});
        } else if (patch) {
            DestroyWindow(patch);
        }
    }

    // On the hole, where the cursor is: the shell's hook keeps it there for
    // as long as the press is waiting to go in again.
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = LOWORD(msg->lParam);
    input.mi.mouseData = HIWORD(msg->lParam);
    input.mi.dwExtraInfo = kReplayMarker;
    SetThreadDpiAwarenessContext(previous);
    SendInput(1, &input, sizeof(input));
}

////////////////////////////////////////////////////////////////////////////////
// In the shell: the hook

constexpr UINT kButtonVk[5] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1,
                               VK_XBUTTON2};
constexpr DWORD kDownFlag[5] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN,
                                MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_XDOWN,
                                MOUSEEVENTF_XDOWN};
constexpr DWORD kUpFlag[5] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP,
                              MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_XUP,
                              MOUSEEVENTF_XUP};
constexpr DWORD kXButton[5] = {0, 0, 0, XBUTTON1, XBUTTON2};

int ButtonIndex(WPARAM wParam, DWORD mouseData) {
    switch (wParam) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            return 0;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            return 1;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            return 2;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            return HIWORD(mouseData) == XBUTTON2 ? 4 : 3;
    }
    return -1;
}

// Would the message hook take this press for a drag, if it saw it?
bool WantsPress(int button, HWND root) {
    return button < 2 && IsDragModifierDown() && IsFrameWindow(root);
}

// Only ever touched on the server thread: the button of a shortcut press
// taken here, whose release goes with it; -1 for none.
int g_shortcutButton = -1;

// The window shortcut needs nothing of the press but where it was, so it is
// asked of the window under the cursor right from here, for whatever window
// that is. A press only reaches the message hook once the window takes it as
// a message, and not every one does: Task Manager's title bar takes its
// presses past every queue, and a window busy enough to open a hole late
// loses them to the Start menu, the Win key let go of by then.
bool TakeShortcutPress(int button, POINT pt) {
    Hotkey shortcut = g_settings.windowShortcut;
    WindowAction action = g_settings.windowShortcutAction;
    if (action == WindowAction::None ||
        !MatchesShortcut(shortcut, kButtonVk[button], true)) {
        return false;
    }
    HWND root = GetAncestor(WindowFromPoint(pt), GA_ROOT);
    if (!CanTakeAction(root, action)) {
        return false;  // desktop, taskbar, menus... - a normal click
    }
    ArmWinMask(shortcut.win, shortcut.alt);
    RequestWindowAction(root, action);
    g_shortcutButton = button;
    return true;
}

// Only ever touched on the server thread: a press taken and not pressed again
// yet, per button, and whether its button has come up meanwhile.
struct TakenPress {
    bool active;
    bool released;
    DWORD time;
    HWND root;
};
TakenPress g_taken[5];

// The release of a press that came up before the press went in again: sent
// once it has, from a timer, outside the hook.
int g_lateRelease = -1;

void CALLBACK LateReleaseTimer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    int button = g_lateRelease;
    g_lateRelease = -1;
    if (button < 0) {
        return;
    }
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = kUpFlag[button];
    input.mi.mouseData = kXButton[button];
    input.mi.dwExtraInfo = kReplayMarker;
    SendInput(1, &input, sizeof(input));
}

// Two fingers on a touchpad, over a program that scrolls with them itself -
// Chrome, Discord, Spotify: Windows asks the program first, with
// DM_POINTERHITTEST, whether it takes the contact. With the modifier held,
// and the wheel scaling windows, the program is not to hear the question, and
// is left the wheel Windows makes of the gesture for a program that does not
// take it - which the shell's mouse hook takes in turn (TakeWheel).
//
// The question is sent, not posted, so the message hook never sees it. What
// does is the hook on sent messages, a moment before the window's procedure
// is called: it puts a subclass of ours on the window for just that one
// message, which answers it the way a window that does not take touchpads
// does, and takes itself off again. File Explorer asks nothing of the kind:
// it scrolls its touchpad gestures some other way, and goes on doing so.
bool TouchpadGoesToWheel(WPARAM pointerHitTest) {
    POINTER_INPUT_TYPE type;
    return g_settings.wheelScale && IsDragModifierDown() &&
           GetPointerType(GET_POINTERID_WPARAM(pointerHitTest), &type) &&
           type == PT_TOUCHPAD;
}

constexpr UINT_PTR kTouchpadSubclassId = 0x48597074;  // 'Hypt'
// The windows with that subclass on, for the teardown to take it off should
// the message it waits for not have come.
std::mutex g_touchpadMutex;
std::vector<HWND> g_touchpadAsked;

void EndKeepingTouchpad(HWND hwnd);

LRESULT CALLBACK TouchpadSubclassProc(HWND hwnd,
                                      UINT uMsg,
                                      WPARAM wParam,
                                      LPARAM lParam,
                                      UINT_PTR,
                                      DWORD_PTR) {
    ModRef ref;  // the image must not go away under this procedure

    if (uMsg == DM_POINTERHITTEST) {
        EndKeepingTouchpad(hwnd);
        if (TouchpadGoesToWheel(wParam)) {
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
        }
    } else if (uMsg == WM_NCDESTROY) {
        EndKeepingTouchpad(hwnd);
    } else if (uMsg == g_msgDrag && wParam == kDragUntouch) {
        EndKeepingTouchpad(hwnd);
        return 0;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

void EndKeepingTouchpad(HWND hwnd) {
    {
        std::lock_guard<std::mutex> lock(g_touchpadMutex);
        auto it = std::find(g_touchpadAsked.begin(), g_touchpadAsked.end(), hwnd);
        if (it == g_touchpadAsked.end()) {
            return;
        }
        g_touchpadAsked.erase(it);
    }
    RemoveWindowSubclass(hwnd, TouchpadSubclassProc, kTouchpadSubclassId);
}

// From the hook on sent messages, on the window's own thread.
void KeepTouchpadFromApp(HWND hwnd, WPARAM pointerHitTest) {
    if (g_uninitializing || !TouchpadGoesToWheel(pointerHitTest)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_touchpadMutex);
    if (std::find(g_touchpadAsked.begin(), g_touchpadAsked.end(), hwnd) !=
            g_touchpadAsked.end() ||
        g_uninitializing) {
        return;
    }
    if (SetWindowSubclass(hwnd, TouchpadSubclassProc, kTouchpadSubclassId, 0)) {
        g_touchpadAsked.push_back(hwnd);
    }
}

std::vector<HWND> SnapshotTouchpadWindows() {
    std::lock_guard<std::mutex> lock(g_touchpadMutex);
    return g_touchpadAsked;
}

// Only touched on the server thread: the window the wheel last went to, and
// when. A turn of the wheel stays with its window for as long as it goes on:
// a window grown or shrunk out from under the cursor is still the one the
// next notch is for, not whatever the cursor is over now. It ends when the
// modifier comes up, or when the wheel has rested this long.
HWND g_wheelRoot;
DWORD g_wheelAt;
constexpr DWORD kWheelTurnMs = 500;

void EndWheelTurn() {
    g_wheelRoot = nullptr;
}

// The window the wheel is for: the one the turn is going to, or one being
// moved, wherever the cursor has got to by now - or else the one under the
// cursor.
HWND WheelTarget(POINT pt, DWORD time) {
    if (g_wheelRoot && time - g_wheelAt < kWheelTurnMs &&
        IsWindow(g_wheelRoot) && CanScale(g_wheelRoot)) {
        return g_wheelRoot;
    }
    HWND under = GetAncestor(WindowFromPoint(pt), GA_ROOT);
    for (HWND candidate : {under, g_wheelRoot}) {
        GUITHREADINFO info{sizeof(info)};
        if (candidate && IsWindow(candidate) &&
            GetGUIThreadInfo(GetWindowThreadProcessId(candidate, nullptr),
                             &info) &&
            (info.flags & GUI_INMOVESIZE) && info.hwndMoveSize &&
            (candidate == under ||
             GetAncestor(info.hwndMoveSize, GA_ROOT) == candidate)) {
            return GetAncestor(info.hwndMoveSize, GA_ROOT);
        }
    }
    return under;
}

// The wheel turned with the modifier held, over a window: it is the window's
// to grow or shrink by (actions.cpp), rather than the app's to scroll. Taken
// here for every window alike, so it needs no hole: the window is asked, and
// not pressed again.
bool TakeWheel(const MSLLHOOKSTRUCT* info) {
    if (!g_settings.wheelScale || info->dwExtraInfo == kInjectedMarker ||
        !IsDragModifierDown()) {
        return false;
    }
    // Any window's, here as in the shell: with an elevated window in front,
    // the shell's hook is not called at all, and this one - in front of it in
    // the chain - is the only one to see the wheel.
    HWND root = WheelTarget(info->pt, info->time);
    if (!CanScale(root)) {
        return false;
    }
    g_wheelRoot = root;
    g_wheelAt = info->time;
    PostMessageW(root, g_msgDrag, kDragScale, (short)HIWORD(info->mouseData));
    ArmWinMask(g_settings.dragModifier == DragModifier::Win,
               g_settings.dragModifier == DragModifier::Alt);
    return true;
}

LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam) {
    ModRef ref;  // the image must not go away under this procedure

    auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    if (code != HC_ACTION || g_uninitializing) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    // The cursor stays put between a press taken and the press again: moved
    // on, it would take the press past the hole and onto the content. A few
    // milliseconds, and never long, should the window not answer.
    if (wParam == WM_MOUSEMOVE && info->dwExtraInfo != kReplayMarker) {
        for (TakenPress& taken : g_taken) {
            if (taken.active && info->time - taken.time < kHoldCursorMaxMs) {
                return 1;
            }
        }
    }
    if (wParam == WM_MOUSEWHEEL && TakeWheel(info)) {
        return 1;
    }
    int button = ButtonIndex(wParam, info->mouseData);
    if (button < 0) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    bool down = wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
                wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN;
    TakenPress& taken = g_taken[button];

    if (info->dwExtraInfo == kReplayMarker) {
        // The press again, through the hole: what it does is up to the
        // window now. One whose button is up already gets its release.
        if (down && taken.active) {
            taken.active = false;
            PostMessageW(taken.root, g_msgDrag, kDragCover, 0);
            if (taken.released) {
                g_lateRelease = button;
                SetTimer(nullptr, 0, 0, LateReleaseTimer);
            }
        }
    } else if (info->dwExtraInfo != kInjectedMarker) {
        if (down && TakeShortcutPress(button, info->pt)) {
            return 1;
        }
        if (!down && button == g_shortcutButton) {
            g_shortcutButton = -1;
            return 1;
        }
        if (down) {
            HWND hwnd = WindowFromPoint(info->pt);
            HWND root = GetAncestor(hwnd, GA_ROOT);
            // An elevated process takes the presses on its own windows, the
            // ones kept from the shell's hook, and leaves the rest to it.
            DWORD pid = 0;
            GetWindowThreadProcessId(root, &pid);
            if ((IsShellProcess() || pid == GetCurrentProcessId()) &&
                CompositionContentAt(hwnd, info->pt) &&
                WantsPress(button, root)) {
                taken = {true, false, info->time, root};
                PostMessageW(root, g_msgDrag, kDragUncover,
                             MAKELPARAM(kDownFlag[button], kXButton[button]));
                return 1;
            }
            taken.active = false;
        } else if (taken.active) {
            taken.released = true;
            return 1;  // let go of before it went in again: it comes after
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}
