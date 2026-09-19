// Probe: can a window's own WM_MOVING / WM_SIZING handler override the rect
// the system's move/size loop is about to apply - in a loop started from code
// the way the mod starts one - and what does overriding it cost?
//
// Three questions:
//   1. Does WM_MOVING / WM_SIZING even arrive during such a loop?
//   2. Does writing to the rect stick, i.e. does the window end up where the
//      handler said rather than where the cursor said?
//   3. Does the grab offset survive it? A loop that recomputes the position
//      from the cursor every frame lets a window unstick when you pull away;
//      one that accumulates would drift and the snap would be unusable.
//
// Ran on Windows 11 26200. WM_MOVING arrived 44 times over ~920 ms and
// WM_SIZING 24 times, and writing to the rectangle stuck in both cases. The
// two loops differ in where the next rectangle comes from:
//
//   * WM_SIZING is absolute - worked out from the cursor and the corner that
//     is staying put. The probe overrode 7 frames, the window came off the
//     line when the cursor passed it, and the grab offset was still exactly
//     -40 at the end.
//   * WM_MOVING is relative - the last rectangle applied, plus the cursor's
//     movement. The probe overrode 9 frames instead of the 7 an absolute
//     loop would have, and the window stayed glued to x=300 with the cursor
//     60 px past the point where it should have come off.
//
// So the mod works the position out itself for a move (see src/snap.cpp).
// The probe also prints both rectangles a window has: at 100% scaling the
// visible frame was 9 px inside the window rectangle on three sides, which
// is why distances are measured in frame coordinates.
#include <windows.h>

#include <dwmapi.h>

#include <cstdio>

// Snap the left edge here, if it comes within the threshold.
static const int kTargetLeft = 300;
static const int kThreshold = 40;
// And the right edge here, while resizing.
static const int kTargetRight = 1400;

static int g_movingSeen, g_sizingSeen, g_overridesDone;
static bool g_snapOn = true;
static int g_lastNatural, g_lastApplied;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOVING: {
            RECT* rc = (RECT*)lp;
            g_movingSeen++;
            g_lastNatural = rc->left;
            if (g_snapOn && rc->left > kTargetLeft - kThreshold &&
                rc->left < kTargetLeft + kThreshold) {
                int width = rc->right - rc->left;
                rc->left = kTargetLeft;
                rc->right = kTargetLeft + width;
                g_overridesDone++;
            }
            g_lastApplied = rc->left;
            if (g_movingSeen <= 6 || g_movingSeen % 10 == 0) {
                printf("  WM_MOVING #%d natural left=%d -> applied %d%s\n",
                       g_movingSeen, g_lastNatural, g_lastApplied,
                       g_lastNatural != g_lastApplied ? "  (snapped)" : "");
                fflush(stdout);
            }
            return TRUE;
        }
        case WM_SIZING: {
            RECT* rc = (RECT*)lp;
            g_sizingSeen++;
            g_lastNatural = rc->right;
            if (g_snapOn && rc->right > kTargetRight - kThreshold &&
                rc->right < kTargetRight + kThreshold) {
                rc->right = kTargetRight;
                g_overridesDone++;
            }
            g_lastApplied = rc->right;
            if (g_sizingSeen <= 6 || g_sizingSeen % 10 == 0) {
                printf("  WM_SIZING #%d (edge %u) natural right=%d -> %d%s\n",
                       g_sizingSeen, (unsigned)wp, g_lastNatural, g_lastApplied,
                       g_lastNatural != g_lastApplied ? "  (snapped)" : "");
                fflush(stdout);
            }
            return TRUE;
        }
        case WM_ENTERSIZEMOVE:
            printf("  WM_ENTERSIZEMOVE\n");
            fflush(stdout);
            break;
        case WM_EXITSIZEMOVE:
            printf("  WM_EXITSIZEMOVE\n");
            fflush(stdout);
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(RGB(220, 235, 255));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void Mouse(DWORD flags) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    SendInput(1, &in, sizeof(in));
}

static void DrainClicks() {
    MSG m;
    while (PeekMessageW(&m, nullptr, WM_LBUTTONDOWN, WM_LBUTTONDOWN,
                        PM_REMOVE)) {
    }
}

static void PrintRect(const char* label, HWND hwnd) {
    RECT rc;
    GetWindowRect(hwnd, &rc);
    RECT frame{};
    DwmGetWindowAttribute(hwnd, 9 /*DWMWA_EXTENDED_FRAME_BOUNDS*/, &frame,
                          sizeof(frame));
    POINT cur;
    GetCursorPos(&cur);
    printf("%s: window (%ld,%ld)-(%ld,%ld)  visible frame (%ld,%ld)-(%ld,%ld)"
           "  cursor (%ld,%ld)\n",
           label, rc.left, rc.top, rc.right, rc.bottom, frame.left, frame.top,
           frame.right, frame.bottom, cur.x, cur.y);
    fflush(stdout);
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SnapProbe";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"SnapProbe", L"snap probe",
                                WS_OVERLAPPEDWINDOW, 700, 300, 500, 320,
                                nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    Sleep(300);

    ////////////////////////////////////////////////////////////////////////////
    printf("\n== move, with the left edge snapping to x=%d ==\n", kTargetLeft);
    PrintRect("before", hwnd);

    RECT rc0;
    GetWindowRect(hwnd, &rc0);
    POINT grab{(rc0.left + rc0.right) / 2, rc0.top + 60};
    int offsetX = grab.x - rc0.left;
    SetCursorPos(grab.x, grab.y);
    Sleep(120);
    Mouse(MOUSEEVENTF_LEFTDOWN);
    Sleep(60);
    DrainClicks();  // the press is ours, the way the mod consumes it

    // Walk the cursor left, through the snap zone and well past it, then back.
    HANDLE mover = CreateThread(
        nullptr, 0,
        [](LPVOID param) -> DWORD {
            POINT from = *(POINT*)param;
            // Natural left goes 700 -> 240: into the zone around 300, then out.
            for (int i = 1; i <= 46; i++) {
                SetCursorPos(from.x - i * 10, from.y);
                Sleep(20);
            }
            Sleep(150);
            Mouse(MOUSEEVENTF_LEFTUP);
            return 0;
        },
        &grab, 0, nullptr);

    // The loop runs inside this call.
    SendMessageW(hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION,
                 MAKELPARAM(grab.x, grab.y));
    WaitForSingleObject(mover, 5000);
    CloseHandle(mover);

    RECT rc1;
    GetWindowRect(hwnd, &rc1);
    POINT end;
    GetCursorPos(&end);
    PrintRect("after", hwnd);
    printf("WM_MOVING seen %d, overridden %d\n", g_movingSeen, g_overridesDone);
    printf("grab offset was %d; at the end cursor.x - window.left = %ld %s\n",
           offsetX, end.x - rc1.left,
           (end.x - rc1.left) == offsetX ? "(offset kept, no drift)"
                                         : "(DRIFTED)");

    ////////////////////////////////////////////////////////////////////////////
    printf("\n== resize, with the right edge snapping to x=%d ==\n",
           kTargetRight);
    SetWindowPos(hwnd, nullptr, 700, 300, 500, 320, SWP_NOZORDER);
    Sleep(200);
    g_overridesDone = 0;
    PrintRect("before", hwnd);

    RECT rs0;
    GetWindowRect(hwnd, &rs0);
    POINT grab2{rs0.right - 40, rs0.bottom - 40};
    int offsetR = grab2.x - rs0.right;
    SetCursorPos(grab2.x, grab2.y);
    Sleep(120);
    Mouse(MOUSEEVENTF_LEFTDOWN);
    Sleep(60);
    DrainClicks();

    HANDLE mover2 = CreateThread(
        nullptr, 0,
        [](LPVOID param) -> DWORD {
            POINT from = *(POINT*)param;
            for (int i = 1; i <= 24; i++) {
                SetCursorPos(from.x + i * 10, from.y);
                Sleep(20);
            }
            Sleep(150);
            Mouse(MOUSEEVENTF_LEFTUP);
            return 0;
        },
        &grab2, 0, nullptr);

    SendMessageW(hwnd, WM_SYSCOMMAND, SC_SIZE | WMSZ_BOTTOMRIGHT,
                 MAKELPARAM(grab2.x, grab2.y));
    WaitForSingleObject(mover2, 5000);
    CloseHandle(mover2);

    RECT rs1;
    GetWindowRect(hwnd, &rs1);
    GetCursorPos(&end);
    PrintRect("after", hwnd);
    printf("WM_SIZING seen %d, overridden %d\n", g_sizingSeen, g_overridesDone);
    printf("grab offset was %d; at the end cursor.x - window.right = %ld %s\n",
           offsetR, end.x - rs1.right,
           (end.x - rs1.right) == offsetR ? "(offset kept, no drift)"
                                          : "(DRIFTED)");

    DestroyWindow(hwnd);
    return 0;
}
