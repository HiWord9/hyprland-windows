// End-to-end probe for the Win + mouse drags, run against the *installed*
// mod: it creates an ordinary top-level window, pumps messages like any
// application, and injects a Win + right button drag. Everything the mod does
// to the messages on the way is logged, so it shows where the resize stops.
//
//   cl/clang++ e2e_probe.cpp -o e2e_probe.exe -luser32
//   e2e_probe.exe [right|left]
#include <windows.h>
#include <shlwapi.h>

#include <cstdio>
#include <cstring>
#include <thread>

static UINT g_msgDragLocal, g_msgDragPlain, g_msgFramelessLocal;

static const char* MsgName(UINT msg) {
    switch (msg) {
        case WM_LBUTTONDOWN: return "WM_LBUTTONDOWN";
        case WM_LBUTTONUP: return "WM_LBUTTONUP";
        case WM_LBUTTONDBLCLK: return "WM_LBUTTONDBLCLK";
        case WM_RBUTTONDOWN: return "WM_RBUTTONDOWN";
        case WM_RBUTTONUP: return "WM_RBUTTONUP";
        case WM_NCLBUTTONDOWN: return "WM_NCLBUTTONDOWN";
        case WM_NCLBUTTONUP: return "WM_NCLBUTTONUP";
        case WM_NCRBUTTONDOWN: return "WM_NCRBUTTONDOWN";
        case WM_NCRBUTTONUP: return "WM_NCRBUTTONUP";
        case WM_SYSCOMMAND: return "WM_SYSCOMMAND";
        case WM_ENTERSIZEMOVE: return "WM_ENTERSIZEMOVE";
        case WM_EXITSIZEMOVE: return "WM_EXITSIZEMOVE";
        case WM_SIZING: return "WM_SIZING";
        case WM_MOVING: return "WM_MOVING";
        case WM_CAPTURECHANGED: return "WM_CAPTURECHANGED";
        case WM_NULL: return "WM_NULL";
        default: return nullptr;
    }
}

static bool Interesting(UINT msg) {
    return MsgName(msg) != nullptr || msg == g_msgDragLocal ||
           msg == g_msgDragPlain || msg == g_msgFramelessLocal;
}

static void Log(const char* where, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    char name[64];
    if (const char* n = MsgName(msg)) {
        strcpy_s(name, n);
    } else if (msg == g_msgDragLocal) {
        strcpy_s(name, "DRAG(local@)");
    } else if (msg == g_msgDragPlain) {
        strcpy_s(name, "DRAG(plain)");
    } else if (msg == g_msgFramelessLocal) {
        strcpy_s(name, "FRAMELESS");
    } else {
        sprintf_s(name, "0x%04X", msg);
    }

    GUITHREADINFO gti{sizeof(gti)};
    GetGUIThreadInfo(GetCurrentThreadId(), &gti);
    printf("%6lu %-9s %-18s hwnd=%p wp=0x%IX lp=0x%IX  keyLB=%d asyncLB=%d "
           "asyncRB=%d inMoveSize=%d capture=%p\n",
           GetTickCount() % 1000000, where, name, (void*)hwnd, wp, lp,
           GetKeyState(VK_LBUTTON) < 0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0,
           (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0,
           (gti.flags & GUI_INMOVESIZE) != 0, (void*)GetCapture());
    fflush(stdout);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (Interesting(msg)) {
        Log("wndproc", hwnd, msg, wp, lp);
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void Pump(DWORD ms) {
    DWORD end = GetTickCount() + ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (Interesting(msg.message)) {
                Log("retrieved", msg.hwnd, msg.message, msg.wParam, msg.lParam);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        int left = (int)(end - GetTickCount());
        if (left <= 0) {
            return;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, left, QS_ALLINPUT);
    }
}

static void Key(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof(in));
}

static void Button(DWORD flags) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    SendInput(1, &in, sizeof(in));
}

struct Found {
    const char* needle;
    HWND hwnd;
};

static BOOL CALLBACK FindProc(HWND hwnd, LPARAM lParam) {
    auto* found = reinterpret_cast<Found*>(lParam);
    char title[256];
    if (!IsWindowVisible(hwnd) || !GetWindowTextA(hwnd, title, 256)) {
        return TRUE;
    }
    if (!StrStrIA(title, found->needle)) {
        return TRUE;
    }
    found->hwnd = hwnd;
    return FALSE;
}

// Injects the same drag over a window of another process and reports what it
// did to it - the log above is not available there, but the outcome is.
static int DriveForeignWindow(const char* needle, bool right, bool fast) {
    Found found{needle, nullptr};
    EnumWindows(FindProc, (LPARAM)&found);
    if (!found.hwnd) {
        printf("no visible window whose title contains %s\n", needle);
        return 1;
    }
    HWND hwnd = found.hwnd;
    char title[256] = "";
    GetWindowTextA(hwnd, title, 256);
    DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);
    // Topmost for the duration of the probe, so the injected input can't be
    // swallowed by whatever else happens to be on top.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SwitchToThisWindow(hwnd, TRUE);
    Sleep(400);

    RECT rc;
    GetWindowRect(hwnd, &rc);
    POINT start{rc.left + (rc.right - rc.left) * 3 / 4,
                rc.top + (rc.bottom - rc.top) * 3 / 4};
    HWND under = WindowFromPoint(start);
    GUITHREADINFO before{sizeof(before)};
    GetGUIThreadInfo(threadId, &before);
    printf("target %p '%s' (%ld,%ld)-(%ld,%ld) thread %u\n", (void*)hwnd,
           title, rc.left, rc.top, rc.right, rc.bottom, threadId);
    printf("  under the grab point: %p, root %p, style 0x%08lX\n",
           (void*)under, (void*)GetAncestor(under, GA_ROOT),
           (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE));
    printf("  in move/size before: %d\n", (before.flags & GUI_INMOVESIZE) != 0);
    if (GetAncestor(under, GA_ROOT) != hwnd) {
        printf("  ABORT: the grab point is over another window\n");
        return 1;
    }

    SetCursorPos(start.x, start.y);
    Sleep(120);
    Key(VK_LWIN, false);
    Sleep(80);
    Button(right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN);

    // A timeline of what the mod does, seen from outside: the synthetic left
    // button it holds for a resize is global state, and the loop it starts
    // shows up in the target thread.
    DWORD t0 = GetTickCount();
    char lastLine[160] = "";
    for (int i = 0; i < 90; i++) {
        if (i == (fast ? 1 : 25)) {
            for (int step = 1; step <= 12; step++) {
                SetCursorPos(start.x + step * 5, start.y + step * 5);
                Sleep(5);
            }
        }
        GUITHREADINFO gti{sizeof(gti)};
        GetGUIThreadInfo(threadId, &gti);
        RECT now;
        GetWindowRect(hwnd, &now);
        char line[160];
        sprintf_s(line, "asyncLB=%d asyncRB=%d inMoveSize=%d capture=%p "
                  "moveSize=%p size=%ldx%ld",
                  (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0,
                  (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0,
                  (gti.flags & GUI_INMOVESIZE) != 0, (void*)gti.hwndCapture,
                  (void*)gti.hwndMoveSize, now.right - now.left,
                  now.bottom - now.top);
        if (strcmp(line, lastLine) != 0) {
            printf("  +%4lu %s\n", GetTickCount() - t0, line);
            strcpy_s(lastLine, line);
        }
        Sleep(10);
    }
    GUITHREADINFO during{sizeof(during)};
    GetGUIThreadInfo(threadId, &during);
    RECT mid;
    GetWindowRect(hwnd, &mid);
    Button(right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP);
    Sleep(80);
    Key(VK_LWIN, true);
    Sleep(400);

    RECT after;
    GetWindowRect(hwnd, &after);
    GUITHREADINFO end{sizeof(end)};
    GetGUIThreadInfo(threadId, &end);
    printf("  mid-drag: in move/size %d, size delta (%ld,%ld)\n",
           (during.flags & GUI_INMOVESIZE) != 0,
           (mid.right - mid.left) - (rc.right - rc.left),
           (mid.bottom - mid.top) - (rc.bottom - rc.top));
    // Put the window back the way it was found.
    SetWindowPos(hwnd, HWND_NOTOPMOST, rc.left, rc.top, rc.right - rc.left,
                 rc.bottom - rc.top, SWP_NOACTIVATE);
    printf("  after: size delta (%ld,%ld) pos delta (%ld,%ld), still in "
           "move/size %d, async LB %d\n",
           (after.right - after.left) - (rc.right - rc.left),
           (after.bottom - after.top) - (rc.bottom - rc.top),
           after.left - rc.left, after.top - rc.top,
           (end.flags & GUI_INMOVESIZE) != 0,
           (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    return 0;
}

// Compares the two ways of asking a window to start a resize loop, posted to
// it the way the mod does: the message an app with its own frame may well
// handle itself (WM_NCLBUTTONDOWN + HT*) against the system command it
// normally passes on (WM_SYSCOMMAND + SC_SIZE | WMSZ_*).
static int MechanismTest(const char* needle, bool sysCommand) {
    Found found{needle, nullptr};
    EnumWindows(FindProc, (LPARAM)&found);
    if (!found.hwnd) {
        printf("no visible window whose title contains %s\n", needle);
        return 1;
    }
    HWND hwnd = found.hwnd;
    DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    Sleep(400);

    RECT rc;
    GetWindowRect(hwnd, &rc);
    POINT start{rc.left + (rc.right - rc.left) * 3 / 4,
                rc.top + (rc.bottom - rc.top) * 3 / 4};
    printf("%s on %p (%ldx%ld)\n", sysCommand ? "SC_SIZE" : "WM_NCLBUTTONDOWN",
           (void*)hwnd, rc.right - rc.left, rc.bottom - rc.top);

    SetCursorPos(start.x, start.y);
    Sleep(120);
    Button(MOUSEEVENTF_LEFTDOWN);
    Sleep(120);
    if (sysCommand) {
        PostMessageW(hwnd, WM_SYSCOMMAND, SC_SIZE | WMSZ_BOTTOMRIGHT,
                     MAKELPARAM(start.x, start.y));
    } else {
        PostMessageW(hwnd, WM_NCLBUTTONDOWN, HTBOTTOMRIGHT,
                     MAKELPARAM(start.x, start.y));
    }
    Sleep(200);
    for (int i = 1; i <= 12; i++) {
        SetCursorPos(start.x + i * 5, start.y + i * 5);
        Sleep(20);
    }
    Sleep(150);
    GUITHREADINFO during{sizeof(during)};
    GetGUIThreadInfo(threadId, &during);
    Button(MOUSEEVENTF_LEFTUP);
    Sleep(400);

    RECT after;
    GetWindowRect(hwnd, &after);
    printf("  mid-drag in move/size %d; after: size delta (%ld,%ld) pos delta "
           "(%ld,%ld)\n",
           (during.flags & GUI_INMOVESIZE) != 0,
           (after.right - after.left) - (rc.right - rc.left),
           (after.bottom - after.top) - (rc.bottom - rc.top),
           after.left - rc.left, after.top - rc.top);
    SetWindowPos(hwnd, HWND_NOTOPMOST, rc.left, rc.top, rc.right - rc.left,
                 rc.bottom - rc.top, SWP_NOACTIVATE);
    return 0;
}

// Tries the resize without touching global mouse state at all: the thread's
// own synchronized button state is faked, the loop is asked for with the
// system command an app passes on, and it is ended with a posted button-up
// instead of injected input. No physical button is held at any point.
static int SynthResizeTest(HWND hwnd) {
    RECT rc;
    GetWindowRect(hwnd, &rc);
    POINT start{rc.left + (rc.right - rc.left) * 3 / 4,
                rc.top + (rc.bottom - rc.top) * 3 / 4};
    SetCursorPos(start.x, start.y);
    Pump(150);

    BYTE keyState[256];
    if (GetKeyboardState(keyState)) {
        keyState[VK_LBUTTON] |= 0x80;
        SetKeyboardState(keyState);
    }
    printf("faked key state: keyLB=%d asyncLB=%d\n", GetKeyState(VK_LBUTTON) < 0,
           (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);

    PostMessageW(hwnd, WM_SYSCOMMAND, SC_SIZE | WMSZ_BOTTOMRIGHT,
                 MAKELPARAM(start.x, start.y));

    std::thread mover([=] {
        Sleep(250);
        for (int i = 1; i <= 12; i++) {
            SetCursorPos(start.x + i * 5, start.y + i * 5);
            Sleep(20);
        }
        Sleep(150);
        GUITHREADINFO gti{sizeof(gti)};
        GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, nullptr), &gti);
        printf("  mid-drag: in move/size %d\n", (gti.flags & GUI_INMOVESIZE) != 0);
        // What ends the loop, in place of a released mouse button.
        POINT end{start.x + 60, start.y + 60};
        PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(end.x, end.y));
    });

    for (int i = 0; i < 40; i++) {
        Pump(50);
    }
    mover.join();
    Pump(200);

    RECT after;
    GetWindowRect(hwnd, &after);
    GUITHREADINFO gti{sizeof(gti)};
    GetGUIThreadInfo(GetCurrentThreadId(), &gti);
    printf("after: size delta (%ld,%ld) pos delta (%ld,%ld), still in move/size "
           "%d\n",
           (after.right - after.left) - (rc.right - rc.left),
           (after.bottom - after.top) - (rc.bottom - rc.top),
           after.left - rc.left, after.top - rc.top,
           (gti.flags & GUI_INMOVESIZE) != 0);
    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool right = !(argc > 1 && !strcmp(argv[1], "left")) &&
                 !(argc > 2 && !strcmp(argv[2], "left"));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc > 1 && !strcmp(argv[1], "target")) {
        bool fast = false;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "fast")) {
                fast = true;
            }
        }
        return DriveForeignWindow(argc > 2 ? argv[2] : "", right, fast);
    }
    if (argc > 1 && !strcmp(argv[1], "mech")) {
        return MechanismTest(argc > 2 ? argv[2] : "",
                             argc > 3 && !strcmp(argv[3], "sc"));
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_msgDragLocal =
        RegisterWindowMessageW(L"HyprlandWindowsDrag_local@hyprland-windows");
    g_msgDragPlain =
        RegisterWindowMessageW(L"HyprlandWindowsDrag_hyprland-windows");
    g_msgFramelessLocal =
        RegisterWindowMessageW(L"HyprlandWindows_local@hyprland-windows");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"HyprE2EProbe";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName,
                                L"Hypr e2e probe",
                                WS_OVERLAPPEDWINDOW, 300, 300, 600, 400,
                                nullptr, nullptr, wc.hInstance, nullptr);
    bool child = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "child")) {
            child = true;
        }
    }
    if (child) {
        // What the cursor sits on in most real applications: a child window
        // filling the client area, not the frame window itself.
        WNDCLASSW cc = wc;
        cc.lpszClassName = L"HyprE2EProbeChild";
        RegisterClassW(&cc);
        CreateWindowExW(0, cc.lpszClassName, L"child", WS_CHILD | WS_VISIBLE,
                        0, 0, 2000, 2000, hwnd, nullptr, wc.hInstance,
                        nullptr);
    }
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    Pump(600);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "synth")) {
            return SynthResizeTest(hwnd);
        }
    }

    RECT rc;
    GetWindowRect(hwnd, &rc);
    POINT start{rc.right - 120, rc.bottom - 90};
    HWND under = WindowFromPoint(start);
    printf("window (%ld,%ld)-(%ld,%ld), dragging from (%ld,%ld) with the %s "
           "button; under that point: %p (ours %p), foreground %p\n",
           rc.left, rc.top, rc.right, rc.bottom, start.x, start.y,
           right ? "right" : "left", (void*)under, (void*)hwnd,
           (void*)GetForegroundWindow());
    printf("grab point is on %s window\n",
           under == hwnd ? "the frame" : "a child");
    if (GetAncestor(under, GA_ROOT) != hwnd) {
        printf("ABORT: something else is on top of the probe window\n");
        return 1;
    }

    std::thread input([=] {
        Sleep(200);
        SetCursorPos(start.x, start.y);
        Sleep(120);
        Key(VK_LWIN, false);
        Sleep(80);
        Button(right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN);
        Sleep(300);
        for (int i = 1; i <= 12; i++) {
            SetCursorPos(start.x + i * 5, start.y + i * 5);
            Sleep(20);
        }
        Sleep(200);
        Button(right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP);
        Sleep(80);
        Key(VK_LWIN, true);
    });

    Pump(2500);
    input.join();
    Pump(500);

    RECT after;
    GetWindowRect(hwnd, &after);
    GUITHREADINFO gti{sizeof(gti)};
    GetGUIThreadInfo(GetCurrentThreadId(), &gti);
    printf("after (%ld,%ld)-(%ld,%ld)  delta size (%ld,%ld) pos (%ld,%ld)\n",
           after.left, after.top, after.right, after.bottom,
           (after.right - after.left) - (rc.right - rc.left),
           (after.bottom - after.top) - (rc.bottom - rc.top),
           after.left - rc.left, after.top - rc.top);
    printf("still in move/size loop: %d, capture %p, async LB %d\n",
           (gti.flags & GUI_INMOVESIZE) != 0, (void*)GetCapture(),
           (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    return 0;
}
