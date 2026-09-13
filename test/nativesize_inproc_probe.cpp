// Validates, in-process, driving the system SC_SIZE modal loop from the RIGHT
// mouse button: hold a synthetic left button, run the loop, and lift the
// synthetic left button when the physical right button is released. Reports
// whether the window actually resized, whether the loop ended on RMB-up, any
// stray click the app's WndProc saw, and whether the cursor was warped.
#include <windows.h>

#include <cstdio>
#include <thread>

static const ULONG_PTR kMark = 0x48797072;

static int g_lbdown, g_lbup, g_enter, g_exit, g_size;
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_LBUTTONDOWN: g_lbdown++; break;
        case WM_LBUTTONUP: g_lbup++; break;
        case WM_ENTERSIZEMOVE: g_enter++; break;
        case WM_EXITSIZEMOVE: g_exit++; break;
        case WM_SIZE: g_size++; break;
    }
    return DefWindowProcW(h, m, w, l);
}

static void Mouse(DWORD f) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = f;
    in.mi.dwExtraInfo = kMark;
    SendInput(1, &in, sizeof(in));
}

static int PhysBtn(bool right) {
    bool swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    return (right != swap) ? VK_RBUTTON : VK_LBUTTON;
}

// Removes our own injected button messages from the queue (identified by the
// marker via GetMessageExtraInfo) without dispatching them, so the app under
// the cursor never sees a stray click. Other messages are dispatched normally.
static void DrainOwnButtonMessages(HWND root) {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        bool btn = m.message == WM_LBUTTONDOWN || m.message == WM_LBUTTONUP ||
                   m.message == WM_NCLBUTTONDOWN || m.message == WM_NCLBUTTONUP;
        if (btn && (ULONG_PTR)GetMessageExtraInfo() == kMark) {
            continue;  // ours: swallow
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// The mechanism under test: hold a synthetic left button (registered before we
// enter, its click message swallowed), run the native corner-resize loop, and
// end it by lifting the synthetic left button when the physical right button
// is released.
static void NativeResizeWithRightButton(HWND root, UINT htCorner, POINT pt) {
    int rbtn = PhysBtn(true);

    Mouse(MOUSEEVENTF_LEFTDOWN);
    // Wait until the system registers the left button as down, discarding the
    // injected click so the app doesn't see it.
    for (int i = 0; i < 100 && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000); i++) {
        DrainOwnButtonMessages(root);
        Sleep(4);
    }
    DrainOwnButtonMessages(root);

    std::thread watcher([root, rbtn] {
        while (GetAsyncKeyState(rbtn) & 0x8000) {
            Sleep(8);
        }
        Mouse(MOUSEEVENTF_LEFTUP);  // ends the native size loop
    });

    DefWindowProcW(root, WM_NCLBUTTONDOWN, htCorner, MAKELPARAM(pt.x, pt.y));
    watcher.join();
    DrainOwnButtonMessages(root);  // swallow the trailing left-up
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NativeSizeInproc";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"NativeSizeInproc", L"probe",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE, 300, 300, 600,
                               420, nullptr, nullptr, wc.hInstance, nullptr);
    SetForegroundWindow(hwnd);
    for (int i = 0; i < 20; i++) { MSG m; while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageW(&m); Sleep(20); }

    RECT r0; GetWindowRect(hwnd, &r0);
    int w0 = r0.right - r0.left, h0 = r0.bottom - r0.top;
    POINT grab{r0.right - 120, r0.bottom - 90};  // inside, bottom-right quadrant
    SetCursorPos(grab.x, grab.y);
    Sleep(60);
    POINT beforeCursor; GetCursorPos(&beforeCursor);

    // Simulate the user holding the physical right button (Win+RMB).
    Mouse(MOUSEEVENTF_RIGHTDOWN);
    Sleep(40);

    g_lbdown = g_lbup = g_enter = g_exit = g_size = 0;

    // Worker drags the cursor to grow, then releases the right button.
    std::thread worker([&] {
        Sleep(200);
        for (int i = 1; i <= 20; i++) {
            SetCursorPos(grab.x + 8 * i, grab.y + 6 * i);
            Sleep(18);
        }
        Sleep(150);
        Mouse(MOUSEEVENTF_RIGHTUP);
    });

    DWORD t0 = GetTickCount();
    NativeResizeWithRightButton(hwnd, HTBOTTOMRIGHT, grab);
    DWORD dt = GetTickCount() - t0;
    worker.join();
    for (int i = 0; i < 10; i++) { MSG m; while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageW(&m); Sleep(20); }

    RECT r1; GetWindowRect(hwnd, &r1);
    POINT afterCursor; GetCursorPos(&afterCursor);
    // Drag moved the cursor by (+160,+120). Relative resize => window grows by
    // ~that. Absolute corner-follow => bottom-right jumps to the final cursor,
    // i.e. grows by (finalCursor - grab) which is the same delta here, but the
    // corner would have teleported to the cursor at the very first move. Detect
    // teleport by comparing the corner to the final cursor position.
    printf("loop ran %lu ms\n", dt);
    printf("resized: W%+ld H%+ld (kept TL corner: %s)\n",
           (r1.right - r1.left) - w0, (r1.bottom - r1.top) - h0,
           (r1.left == r0.left && r1.top == r0.top) ? "yes" : "NO");
    printf("final bottom-right corner (%ld,%ld) vs final cursor (%ld,%ld) -> "
           "%s\n",
           r1.right, r1.bottom, afterCursor.x, afterCursor.y,
           (abs(r1.right - afterCursor.x) < 12 && abs(r1.bottom - afterCursor.y) < 12)
               ? "corner-follows-cursor (teleport/absolute)"
               : "relative");
    printf("WM_SIZE during loop: %d (live tracking: %s)\n", g_size,
           g_size > 3 ? "yes" : "NO");
    printf("stray WM_LBUTTONDOWN seen by app: %d\n", g_lbdown);
    printf("ENTER/EXITSIZEMOVE: %d/%d\n", g_enter, g_exit);
    printf("cursor warped at start: from (%ld,%ld) -> loop end (%ld,%ld)\n",
           beforeCursor.x, beforeCursor.y, afterCursor.x, afterCursor.y);
    printf("left button state after: %d (should be 0)\n",
           (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0);

    DestroyWindow(hwnd);
    return 0;
}
