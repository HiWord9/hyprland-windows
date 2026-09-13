// Prototype of the real Start-menu suppression: a WH_KEYBOARD_LL hook that,
// after a Win-modified drag, swallows the physical Win key-up and re-injects a
// masking key + a fresh Win key-up, so the last key-down before Win goes up is
// never the Win key itself (which is what makes the shell open Start).
//
// This mirrors what the mod will do, and is exercised against a physically
// held + auto-repeating Win key.
#include <windows.h>

#include <cstdio>
#include <string>
#include <thread>

static HHOOK g_hook;
static bool g_armed;
static const WORD kMaskVk = 0xE8;  // unassigned VK, used only as a "used a key"
static const ULONG_PTR kOurInput = 0x48797072;  // 'Hypr' marker on our injects

static LRESULT CALLBACK LlKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_armed) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool keyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
        bool isWin = k->vkCode == VK_LWIN || k->vkCode == VK_RWIN;
        bool ours = k->dwExtraInfo == kOurInput;
        if (keyUp && isWin && !ours) {
            g_armed = false;
            INPUT in[3]{};
            in[0].type = INPUT_KEYBOARD;
            in[0].ki.wVk = kMaskVk;
            in[0].ki.dwExtraInfo = kOurInput;
            in[1] = in[0];
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            in[2].type = INPUT_KEYBOARD;
            in[2].ki.wVk = (WORD)k->vkCode;
            in[2].ki.dwFlags = KEYEVENTF_KEYUP;
            in[2].ki.dwExtraInfo = kOurInput;
            SendInput(3, in, sizeof(INPUT));
            return 1;  // swallow the real Win key-up
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// In the mod this installs the hook on the app's UI thread; here the hook is
// installed once on the pumping main thread and Arm() only sets the flag.
static void Arm() { g_armed = true; }

// --- test scaffolding -------------------------------------------------------

static void Key(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof(in));
}
static void KeyTap(WORD vk) { Key(vk, false); Key(vk, true); }
static void Mouse(DWORD f) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = f;
    SendInput(1, &in, sizeof(in));
}

static std::wstring ForegroundExe() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    WCHAR path[MAX_PATH] = L"?";
    DWORD len = MAX_PATH;
    if (h) { QueryFullProcessImageNameW(h, 0, path, &len); CloseHandle(h); }
    std::wstring s = path;
    size_t sl = s.find_last_of(L'\\');
    return sl == std::wstring::npos ? s : s.substr(sl + 1);
}
static bool StartOpen() {
    std::wstring e = ForegroundExe();
    return _wcsicmp(e.c_str(), L"StartMenuExperienceHost.exe") == 0 ||
           _wcsicmp(e.c_str(), L"SearchHost.exe") == 0;
}
static void CloseStart() {
    for (int i = 0; i < 3 && StartOpen(); i++) { KeyTap(VK_ESCAPE); Sleep(700); }
}

static HWND g_wnd;
static const POINT kCenter{450, 400};

static void PumpFor(DWORD ms) {
    DWORD end = GetTickCount() + ms;
    for (;;) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        int left = (int)(end - GetTickCount());
        if (left <= 0) break;
        MsgWaitForMultipleObjectsEx(0, nullptr, left, QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
    }
}

// Runs a scenario on a worker thread while the main thread pumps (so the LL
// hook, owned by the main thread, is serviced).
static volatile bool g_scenarioDone;
static void RunOnMain(const char* name, void (*body)()) {
    // activate our window
    SetCursorPos(kCenter.x, kCenter.y);
    Sleep(40);
    Mouse(MOUSEEVENTF_LEFTDOWN); Sleep(20); Mouse(MOUSEEVENTF_LEFTUP);
    PumpFor(250);

    g_scenarioDone = false;
    std::thread t([body] { body(); g_scenarioDone = true; });
    while (!g_scenarioDone) PumpFor(30);
    t.join();
    PumpFor(900);
    printf("%-58s -> %s\n", name, StartOpen() ? "Start OPENED" : "closed");
    fflush(stdout);
    CloseStart();
    PumpFor(400);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WinMaskProbe";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"WinMaskProbe",
                            L"probe", WS_POPUP, 300, 300, 300, 200, nullptr,
                            nullptr, wc.hInstance, nullptr);
    ShowWindow(g_wnd, SW_SHOW);
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LlKeyboardProc,
                               GetModuleHandleW(nullptr), 0);
    POINT saved; GetCursorPos(&saved);

    // Baseline: no arming, plain Win tap -> should OPEN.
    RunOnMain("baseline: Win down, 300ms, Win up (no hook)", [] {
        Key(VK_LWIN, false); Sleep(300); Key(VK_LWIN, true);
    });

    // The real-world failure case: hold Win (with auto-repeat), arm at the
    // "button down", drag, release Win -> must stay CLOSED.
    RunOnMain("armed: Win held+repeat, arm, drag, Win up", [] {
        Key(VK_LWIN, false);
        Arm();  // like the mod at button-down
        for (int i = 0; i < 6; i++) { Sleep(40); Key(VK_LWIN, false); }  // repeat
        for (int i = 0; i < 8; i++) { SetCursorPos(kCenter.x + i*5, kCenter.y); Sleep(20); }
        for (int i = 0; i < 4; i++) { Sleep(40); Key(VK_LWIN, false); }  // repeat
        Sleep(60);
        Key(VK_LWIN, true);
    });

    RunOnMain("armed: repeats, release Win LONG after (2s)", [] {
        Key(VK_LWIN, false);
        Arm();
        for (int i = 0; i < 10; i++) { Sleep(40); Key(VK_LWIN, false); }
        Sleep(2000);
        for (int i = 0; i < 5; i++) { Sleep(40); Key(VK_LWIN, false); }
        Key(VK_LWIN, true);
    });

    RunOnMain("armed: RWIN variant", [] {
        Key(VK_RWIN, false);
        Arm();
        for (int i = 0; i < 6; i++) { Sleep(40); Key(VK_RWIN, false); }
        Key(VK_RWIN, true);
    });

    // After the armed drags, a fresh plain Win tap must STILL open Start
    // (i.e., the hook disarmed itself and didn't wedge anything).
    RunOnMain("after: plain Win tap still opens", [] {
        Key(VK_LWIN, false); Sleep(300); Key(VK_LWIN, true);
    });

    if (g_hook) UnhookWindowsHookEx(g_hook);
    SetCursorPos(saved.x, saved.y);
    DestroyWindow(g_wnd);
    return 0;
}
