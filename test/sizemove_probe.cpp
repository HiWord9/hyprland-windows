// Probe: how does the system's SC_MOVE / SC_SIZE modal loop behave when
// started from code with the left or right mouse button held?
#include <windows.h>

#include <cstdio>
#include <thread>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ENTERSIZEMOVE) printf("    WM_ENTERSIZEMOVE\n");
    if (m == WM_EXITSIZEMOVE) printf("    WM_EXITSIZEMOVE\n");
    return DefWindowProcW(h, m, w, l);
}

static void PumpAll() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void Mouse(DWORD flags) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    SendInput(1, &in, sizeof(in));
}

static void Key(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof(in));
}

// pumpFirst: retrieve the button-down message before starting the loop (the
// mod always runs right after GetMessage retrieved it).
static void Probe(HWND hwnd, const char* name, bool right, UINT cmd,
                  bool pumpFirst) {
    printf("== %s ==\n", name);
    RECT r0;
    GetWindowRect(hwnd, &r0);
    POINT start{(r0.left + r0.right) / 2 + 100, (r0.top + r0.bottom) / 2 + 60};
    SetCursorPos(start.x, start.y);
    Sleep(60);
    Mouse(right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN);
    Sleep(60);
    if (pumpFirst) PumpAll();

    volatile bool done = false;
    std::thread mover([&] {
        Sleep(200);
        for (int i = 1; i <= 10; i++) {
            SetCursorPos(start.x + 8 * i, start.y + 5 * i);
            Sleep(20);
        }
        Sleep(100);
        Mouse(right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP);
        // Safety net: if the loop is still running, press Esc.
        for (int i = 0; i < 30 && !done; i++) Sleep(100);
        if (!done) {
            printf("    loop still running 3s after button-up -> sending Esc\n");
            Key(VK_ESCAPE, false);
            Key(VK_ESCAPE, true);
        }
    });

    DWORD t0 = GetTickCount();
    DefWindowProcW(hwnd, WM_SYSCOMMAND, cmd, MAKELPARAM(start.x, start.y));
    DWORD dt = GetTickCount() - t0;
    done = true;
    mover.join();
    PumpAll();

    RECT r1;
    GetWindowRect(hwnd, &r1);
    POINT cur;
    GetCursorPos(&cur);
    printf("    loop: %lu ms; rect delta L%ld T%ld R%ld B%ld; cursor delta %ld,%ld\n",
           (unsigned long)dt, r1.left - r0.left, r1.top - r0.top,
           r1.right - r0.right, r1.bottom - r0.bottom, cur.x - start.x,
           cur.y - start.y);
    // Any leftover button state?
    printf("    async LBUTTON=%d RBUTTON=%d\n",
           (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0,
           (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
    SetWindowPos(hwnd, nullptr, r0.left, r0.top, r0.right - r0.left,
                 r0.bottom - r0.top, SWP_NOZORDER);
    PumpAll();
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SizeMoveProbe";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"SizeMoveProbe", L"probe",
                                WS_OVERLAPPEDWINDOW, 200, 200, 500, 320,
                                nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    PumpAll();
    Sleep(300);
    POINT saved;
    GetCursorPos(&saved);

    Probe(hwnd, "SC_MOVE|HTCAPTION, left button, no pump", false,
          SC_MOVE | HTCAPTION, false);
    Probe(hwnd, "SC_MOVE|HTCAPTION, left button, pumped", false,
          SC_MOVE | HTCAPTION, true);
    Probe(hwnd, "SC_SIZE|BOTTOMRIGHT, left button, pumped", false,
          SC_SIZE | WMSZ_BOTTOMRIGHT, true);
    Probe(hwnd, "SC_SIZE|BOTTOMRIGHT, right button, pumped", true,
          SC_SIZE | WMSZ_BOTTOMRIGHT, true);
    Probe(hwnd, "SC_MOVE|HTCAPTION, right button, pumped", true,
          SC_MOVE | HTCAPTION, true);

    SetCursorPos(saved.x, saved.y);
    DestroyWindow(hwnd);
    return 0;
}
