// Probe: what exactly stops Windows from opening the Start menu when the Win
// key is released after it was used as a mouse modifier?
//
// Each scenario injects a Win key press + something + Win key release, then
// checks whether the Start menu became the foreground window (and closes it).
// The scenarios run on a worker thread while the main thread pumps messages
// for the probe window.
#include <windows.h>

#include <cstdio>
#include <string>
#include <thread>

static void Key(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof(in));
}

static void KeyTap(WORD vk) {
    Key(vk, false);
    Key(vk, true);
}

static void Mouse(DWORD flags) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    SendInput(1, &in, sizeof(in));
}

static std::wstring ForegroundExe() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    WCHAR path[MAX_PATH] = L"?";
    DWORD len = MAX_PATH;
    if (h) {
        QueryFullProcessImageNameW(h, 0, path, &len);
        CloseHandle(h);
    }
    std::wstring s = path;
    size_t slash = s.find_last_of(L'\\');
    return slash == std::wstring::npos ? s : s.substr(slash + 1);
}

static bool StartMenuOpen() {
    std::wstring exe = ForegroundExe();
    return _wcsicmp(exe.c_str(), L"StartMenuExperienceHost.exe") == 0 ||
           _wcsicmp(exe.c_str(), L"SearchHost.exe") == 0;
}

static void CloseStartMenu() {
    for (int i = 0; i < 3 && StartMenuOpen(); i++) {
        KeyTap(VK_ESCAPE);
        Sleep(700);
    }
}

static HWND g_probeWnd;
static const POINT kCenter{450, 400};

static void ActivateProbeWindow() {
    SetCursorPos(kCenter.x, kCenter.y);
    Sleep(50);
    Mouse(MOUSEEVENTF_LEFTDOWN);
    Sleep(30);
    Mouse(MOUSEEVENTF_LEFTUP);
    Sleep(250);
}

static void Run(const char* name, void (*scenario)()) {
    for (int rep = 0; rep < 2; rep++) {
        ActivateProbeWindow();
        bool fgOk = GetForegroundWindow() == g_probeWnd;
        scenario();
        Sleep(900);
        bool open = StartMenuOpen();
        printf("%-60s -> %s%s\n", name, open ? "Start OPENED" : "closed",
               fgOk ? "" : "  [probe window was not foreground]");
        fflush(stdout);
        CloseStartMenu();
        Sleep(400);
    }
}

static void Scenarios() {
    Run("A: Win down, 300ms, Win up (baseline)", [] {
        Key(VK_LWIN, false);
        Sleep(300);
        Key(VK_LWIN, true);
    });
    Run("B: Win down, tap VK 0xE8, 300ms, Win up", [] {
        Key(VK_LWIN, false);
        Sleep(50);
        KeyTap(0xE8);
        Sleep(300);
        Key(VK_LWIN, true);
    });
    Run("C: Win down, tap Ctrl, 300ms, Win up", [] {
        Key(VK_LWIN, false);
        Sleep(50);
        KeyTap(VK_CONTROL);
        Sleep(300);
        Key(VK_LWIN, true);
    });
    Run("D: Win down, LMB click, 300ms, Win up", [] {
        Key(VK_LWIN, false);
        Sleep(50);
        Mouse(MOUSEEVENTF_LEFTDOWN);
        Sleep(50);
        Mouse(MOUSEEVENTF_LEFTUP);
        Sleep(300);
        Key(VK_LWIN, true);
    });
    Run("E: Win down, tap 0xE8, hold 2500ms, Win up", [] {
        Key(VK_LWIN, false);
        Sleep(50);
        KeyTap(0xE8);
        Sleep(2500);
        Key(VK_LWIN, true);
    });
    Run("F: Win down, LMB down, tap 0xE8, drag, Win up, LMB up", [] {
        Key(VK_LWIN, false);
        Sleep(50);
        Mouse(MOUSEEVENTF_LEFTDOWN);
        Sleep(30);
        KeyTap(0xE8);
        for (int i = 0; i < 10; i++) {
            SetCursorPos(kCenter.x + i * 5, kCenter.y + i * 3);
            Sleep(20);
        }
        Sleep(200);
        Key(VK_LWIN, true);
        Sleep(200);
        Mouse(MOUSEEVENTF_LEFTUP);
    });
    Run("G: Win down, LMB down, tap 0xE8, drag, LMB up, Win up", [] {
        Key(VK_LWIN, false);
        Sleep(50);
        Mouse(MOUSEEVENTF_LEFTDOWN);
        Sleep(30);
        KeyTap(0xE8);
        for (int i = 0; i < 10; i++) {
            SetCursorPos(kCenter.x + i * 5, kCenter.y + i * 3);
            Sleep(20);
        }
        Sleep(200);
        Mouse(MOUSEEVENTF_LEFTUP);
        Sleep(200);
        Key(VK_LWIN, true);
    });
    Run("H: Win down, 0xE8 down (held), Win up, 0xE8 up", [] {
        Key(VK_LWIN, false);
        Sleep(50);
        Key(0xE8, false);
        Sleep(300);
        Key(VK_LWIN, true);
        Sleep(50);
        Key(0xE8, true);
    });
    Run("I: Win down, 400ms, tap 0xE8, Win up", [] {
        Key(VK_LWIN, false);
        Sleep(400);
        KeyTap(0xE8);
        Key(VK_LWIN, true);
    });
    Run("J: Win down, 400ms, tap Ctrl, Win up", [] {
        Key(VK_LWIN, false);
        Sleep(400);
        KeyTap(VK_CONTROL);
        Key(VK_LWIN, true);
    });
    Run("K: Win down, LMB down, drag, LMB up, Win up (no mask)", [] {
        Key(VK_LWIN, false);
        Sleep(50);
        Mouse(MOUSEEVENTF_LEFTDOWN);
        Sleep(30);
        for (int i = 0; i < 10; i++) {
            SetCursorPos(kCenter.x + i * 5, kCenter.y + i * 3);
            Sleep(20);
        }
        Sleep(200);
        Mouse(MOUSEEVENTF_LEFTUP);
        Sleep(200);
        Key(VK_LWIN, true);
    });
    Run("M: Win dn, tap 0xE8, Win dn (repeat), Win up", [] {
        Key(VK_LWIN, false);
        Sleep(50);
        KeyTap(0xE8);
        Sleep(50);
        Key(VK_LWIN, false);  // simulated autorepeat AFTER the mask
        Sleep(50);
        Key(VK_LWIN, true);
    });
    Run("N: Win dn x5 (repeat), tap 0xE8, Win up", [] {
        Key(VK_LWIN, false);
        for (int i = 0; i < 5; i++) {
            Sleep(40);
            Key(VK_LWIN, false);  // autorepeats BEFORE the mask
        }
        KeyTap(0xE8);
        Sleep(50);
        Key(VK_LWIN, true);
    });
    Run("O: Win dn, repeat x3, tap 0xE8, repeat x3, Win up", [] {
        Key(VK_LWIN, false);
        for (int i = 0; i < 3; i++) { Sleep(40); Key(VK_LWIN, false); }
        KeyTap(0xE8);
        for (int i = 0; i < 3; i++) { Sleep(40); Key(VK_LWIN, false); }
        Sleep(40);
        Key(VK_LWIN, true);
    });
    Run("P: like M but mask again right before Win up", [] {
        Key(VK_LWIN, false);
        Sleep(50);
        KeyTap(0xE8);
        Sleep(50);
        Key(VK_LWIN, false);  // autorepeat
        Sleep(50);
        KeyTap(0xE8);         // re-mask right before releasing
        Key(VK_LWIN, true);
    });
    Run("L: Win down, 300ms, Win up (baseline again)", [] {
        Key(VK_LWIN, false);
        Sleep(300);
        Key(VK_LWIN, true);
    });
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // A frameless popup so the hyprland-windows mod (if installed) ignores
    // clicks on it and doesn't inject anything itself.
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"StartMenuProbe";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_probeWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                 L"StartMenuProbe", L"probe", WS_POPUP, 300,
                                 300, 300, 200, nullptr, nullptr, wc.hInstance,
                                 nullptr);
    ShowWindow(g_probeWnd, SW_SHOW);

    POINT saved;
    GetCursorPos(&saved);

    std::thread worker([&] {
        Sleep(300);
        Scenarios();
        SetCursorPos(saved.x, saved.y);
        PostMessageW(g_probeWnd, WM_CLOSE, 0, 0);
    });

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_CLOSE && msg.hwnd == g_probeWnd) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    worker.join();
    DestroyWindow(g_probeWnd);
    return 0;
}
