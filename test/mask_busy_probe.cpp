// Probe: the Start menu mask against the two things that broke it. Built
// against the mod's own code (the bundled file), the way the test harness is.
//
//   mask_busy.exe busy     the thread that arms the mask is busy at the moment
//                          the Win key comes up
//   mask_busy.exe dying    the process that arms it is gone by then - which
//                          is what closing an application's last window is
//
// Each checks whether the Start menu opened after the release, whether a mask
// is still armed afterwards, and whether the next plain Win tap still opens
// Start. "Start opened" and "next tap eaten" together are the bug.
//
// (mask_busy.exe server / victim are the two halves of "dying", run as
// separate processes.)
#include "../build/hyprland-windows.wh.cpp"

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

static void PumpFor(DWORD ms) {
    DWORD end = GetTickCount() + ms;
    while ((int)(end - GetTickCount()) > 0) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

static void CloseStartMenu() {
    for (int i = 0; i < 4 && StartMenuOpen(); i++) {
        Key(VK_ESCAPE, false);
        Key(VK_ESCAPE, true);
        PumpFor(600);
    }
}

static bool PumpedWinTapOpensStart() {
    CloseStartMenu();
    PumpFor(400);
    Key(VK_LWIN, false);
    PumpFor(80);
    Key(VK_LWIN, true);
    PumpFor(900);
    bool opened = StartMenuOpen();
    CloseStartMenu();
    PumpFor(400);
    return opened;
}

static void Report(const char* name, bool opened, bool stillArmed, bool tap) {
    printf("%-40s Start after release=%-12s armed after=%d  next Win tap=%s\n",
           name, opened ? "OPENED(bad)" : "shut", (int)stillArmed,
           tap ? "works" : "EATEN(bad)");
    fflush(stdout);
}

static int Busy() {
    for (DWORD busyMs : {(DWORD)0, (DWORD)2000}) {
        CloseStartMenu();
        PumpFor(400);
        Key(VK_LWIN, false);
        PumpFor(120);
        ArmWinMask(true);  // what a gesture does, on the thread that took it

        std::thread releaser([] {
            Sleep(150);
            Key(VK_LWIN, true);
        });
        if (busyMs) {
            Sleep(busyMs);  // busy: this thread is not pumping at all
        } else {
            PumpFor(400);
        }
        releaser.join();
        PumpFor(900);

        bool opened = StartMenuOpen();
        bool armed = g_winMaskArmed;
        CloseStartMenu();
        PumpFor(400);
        bool tap = PumpedWinTapOpensStart();
        Report(busyMs ? "arming thread busy 2 s at the release"
                      : "arming thread pumping (control)",
               opened, armed, tap);
    }
    ShutdownWinMask();
    PumpFor(300);
    return 0;
}

static HANDLE Launch(PCWSTR mode) {
    WCHAR self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + self + L"\" " + mode;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static int Dying() {
    HANDLE server = Launch(L"server");
    HWND serverWindow = nullptr;
    for (int i = 0; i < 50 && !serverWindow; i++) {
        Sleep(100);
        serverWindow = FindWindowExW(HWND_MESSAGE, nullptr, kMaskServerClass,
                                     nullptr);
    }
    printf("mask server in another process: %s\n",
           serverWindow ? "up" : "NOT FOUND");
    if (!serverWindow) {
        TerminateProcess(server, 1);
        return 1;
    }

    for (int run = 0; run < 3; run++) {
        CloseStartMenu();
        PumpFor(400);
        Key(VK_LWIN, false);
        PumpFor(120);
        // The gesture's process arms the mask and is gone straight after.
        HANDLE victim = Launch(L"victim");
        WaitForSingleObject(victim, 5000);
        DWORD code = 0;
        GetExitCodeProcess(victim, &code);
        CloseHandle(victim);
        PumpFor(150);
        Key(VK_LWIN, true);
        PumpFor(900);

        bool opened = StartMenuOpen();
        CloseStartMenu();
        PumpFor(400);
        bool tap = PumpedWinTapOpensStart();
        Report("arming process gone before the release", opened, false, tap);
    }

    TerminateProcess(server, 0);
    CloseHandle(server);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    Wh_ModInit();
    g_settings.dragModifier = DragModifier::Win;
    std::wstring mode = argc > 1 ? argv[1] : L"busy";

    if (mode == L"server") {
        StartMaskServer();
        PumpFor(60000);  // until the probe kills it
        return 0;
    }
    if (mode == L"victim") {
        ArmWinMask(true);  // finds the server in the other process
        return 0;          // and is gone before Win comes up
    }
    if (mode == L"dying") {
        return Dying();
    }
    return Busy();
}
