// Win+Tab through the desktops, tried in the shell itself - before Windhawk
// has compiled the mod, and with presses no hand has to make. What the shell
// does after a switch decides whether the desktops flicker back and forth
// (desktops.cpp), and a harness is not the shell: the window holding the front
// has to be the shell's own, and so does the thread its hotkey goes to.
//
// desktops_in_shell.dll is the bundled mod, hooked onto every thread of the
// shell the way the mod's message hook is. A registered message starts its
// desktop thread, and every WM_HOTKEY goes through HandleDesktopHotkey, which
// is all the mod adds to those threads. desktops_in_shell.exe sends the keys,
// marked so that a copy of the mod already in the shell lets them by, and
// counts the desktop changes: one for each press is right, more is the shell
// undoing some of them.
//
//   desktops_in_shell.exe burst [presses] [ms apart]   Tab again and again, Win held
//   desktops_in_shell.exe taps  [presses] [ms apart]   Win+Tab again and again
//   desktops_in_shell.exe shift [presses] [ms apart]   Win+Shift+Tab, Win held
//   desktops_in_shell.exe view                         Win+Ctrl+Tab, twice
//
// With --mod at the end, the DLL stays out of it: the keys go to the copy of
// the mod Windhawk has compiled and loaded into the shell.
//
// Built by build.ps1 -Shell. It switches the desktops of whoever runs it.
#ifdef SHELL_DLL
#include "../build/hyprland-windows.wh.cpp"

extern "C" __declspec(dllexport) LRESULT CALLBACK GetMsgProc(int code,
                                                             WPARAM wParam,
                                                             LPARAM lParam) {
    static UINT control = RegisterWindowMessageW(L"HyprlandWindowsShellTest");
    if (code == HC_ACTION && (wParam & PM_REMOVE)) {
        MSG* msg = reinterpret_cast<MSG*>(lParam);
        if (msg->message == control) {
            if (msg->wParam) {
                g_settings.desktopWinTab = true;
                StartDesktopThread();
            } else {
                // Out before the hooks come off and the image can go.
                ShutdownDesktopThread();
                for (int i = 0; i < 300 && (g_desktopThreadId || g_modRefCount > 0);
                     i++) {
                    Sleep(10);
                }
            }
            msg->message = WM_NULL;
        } else if (msg->message == WM_HOTKEY && HandleDesktopHotkey(msg)) {
            msg->message = WM_NULL;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
#else
#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

constexpr ULONG_PTR kInjectedMarker = 0x48797072;  // as the mod's

static void Key(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    in.ki.dwExtraInfo = kInjectedMarker;
    SendInput(1, &in, sizeof(in));
}

static const wchar_t kDesktopsKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";

static GUID CurrentDesktop() {
    GUID desktop{};
    DWORD size = sizeof(desktop);
    RegGetValueW(HKEY_CURRENT_USER, kDesktopsKey, L"CurrentVirtualDesktop",
                 RRF_RT_REG_BINARY, nullptr, &desktop, &size);
    return desktop;
}

static int DesktopNumber(GUID desktop) {
    GUID ids[32]{};
    DWORD size = sizeof(ids);
    RegGetValueW(HKEY_CURRENT_USER, kDesktopsKey, L"VirtualDesktopIDs",
                 RRF_RT_REG_BINARY, nullptr, ids, &size);
    for (int i = 0; i < (int)(size / sizeof(GUID)); i++) {
        if (ids[i] == desktop) {
            return i + 1;
        }
    }
    return 0;
}

// What is in front, for the log: the mod's holder is a 1x1 static.
static std::string Front() {
    HWND front = GetForegroundWindow();
    char cls[64] = "";
    GetClassNameA(front, cls, sizeof(cls));
    RECT rect{};
    GetWindowRect(front, &rect);
    if (!strcmp(cls, "Static") && rect.right - rect.left <= 2) {
        return "(the mod's holder)";
    }
    DWORD cloaked = 0;
    DwmGetWindowAttribute(front, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return std::string(cls) + (cloaked ? " (on another desktop)" : "");
}

// An elevated window in front - Windhawk's own, for one - takes no keys from
// this program, and the mod brings one forward like any other when it is on
// top of a desktop. Whatever the keys were to do after that, did not happen.
static bool ElevatedInFront() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return false;
    }
    bool elevated = true;  // a process whose token is not to be looked at
    HANDLE token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        elevated = GetTokenInformation(token, TokenElevation, &elevation,
                                       sizeof(elevation), &size) &&
                   elevation.TokenIsElevated;
        CloseHandle(token);
    }
    CloseHandle(process);
    return elevated;
}

static bool TaskViewUp() {
    HWND view = nullptr;
    while ((view = FindWindowExW(nullptr, view, L"XamlExplorerHostIslandWindow",
                                 nullptr))) {
        if (IsWindowVisible(view) &&
            !(GetWindowLongPtrW(view, GWL_EXSTYLE) & WS_EX_NOACTIVATE)) {
            return true;
        }
    }
    return false;
}

// Every change of the desktop and of what is in front, from a thread of its
// own.
struct Watch {
    std::atomic<bool> stop{false};
    int changes = 0;
    std::string log;
    std::thread thread;
    void Start() {
        thread = std::thread([this] {
            DWORD start = GetTickCount();
            GUID desktop = CurrentDesktop();
            std::string front = Front();
            while (!stop) {
                GUID nowDesktop = CurrentDesktop();
                std::string nowFront = Front();
                if (nowDesktop != desktop || nowFront != front) {
                    char line[200];
                    snprintf(line, sizeof(line), "  %5lu ms  desktop %d, in front %s\n",
                             GetTickCount() - start, DesktopNumber(nowDesktop),
                             nowFront.c_str());
                    log += line;
                    changes += nowDesktop != desktop;
                    desktop = nowDesktop;
                    front = nowFront;
                }
                Sleep(2);
            }
        });
    }
    void Stop() {
        stop = true;
        thread.join();
    }
};

static void Press(WORD vk) {
    Key(vk, false);
    Sleep(40);
    Key(vk, true);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool installed = argc > 1 && !strcmp(argv[argc - 1], "--mod");
    if (installed) {
        argc--;
    }
    std::string test = argc > 1 ? argv[1] : "burst";
    int presses = argc > 2 ? atoi(argv[2]) : 12;
    int gap = argc > 3 ? atoi(argv[3]) : 110;
    if (gap < 80) {
        gap = 80;
    }
    timeBeginPeriod(1);

    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD shellPid = 0;
    DWORD trayThread = GetWindowThreadProcessId(tray, &shellPid);
    WCHAR dllPath[MAX_PATH];
    GetModuleFileNameW(nullptr, dllPath, MAX_PATH);
    wcscpy(wcsrchr(dllPath, L'.'), L".dll");
    HMODULE dll = installed ? nullptr : LoadLibraryW(dllPath);
    auto proc = dll ? (HOOKPROC)GetProcAddress(dll, "GetMsgProc") : nullptr;
    if (!tray || (!installed && !proc)) {
        printf("no shell, or no %ls\n", dllPath);
        return 1;
    }
    std::vector<HHOOK> hooks;
    UINT control = RegisterWindowMessageW(L"HyprlandWindowsShellTest");
    if (!installed) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 entry{sizeof(entry)};
        for (BOOL ok = Thread32First(snapshot, &entry); ok;
             ok = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID == shellPid) {
                if (HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, proc, dll,
                                                   entry.th32ThreadID)) {
                    hooks.push_back(hook);
                }
            }
        }
        CloseHandle(snapshot);
        PostThreadMessageW(trayThread, control, 1, 0);
        Sleep(500);
    }

    GUID start = CurrentDesktop();
    Watch watch;
    watch.Start();
    int expected = presses;
    if (test == "burst" || test == "shift") {
        Key(VK_LWIN, false);
        if (test == "shift") {
            Key(VK_SHIFT, false);
        }
        for (int i = 0; i < presses; i++) {
            Press(VK_TAB);
            Sleep(gap - 40);
        }
        Sleep(150);
        printf("with Win still down, in front: %s\n", Front().c_str());
        if (test == "shift") {
            Key(VK_SHIFT, true);
        }
        Key(VK_LWIN, true);
    } else if (test == "taps") {
        for (int i = 0; i < presses; i++) {
            Key(VK_LWIN, false);
            Press(VK_TAB);
            Sleep(30);
            Key(VK_LWIN, true);
            Sleep(gap - 70);
        }
    } else if (test == "view") {
        // A Win+Tab there and back first, for the mod to have seen the
        // shell's own, which Task View then goes by.
        expected = 2;
        Key(VK_LWIN, false);
        Press(VK_TAB);
        Sleep(300);
        Key(VK_SHIFT, false);
        Press(VK_TAB);
        Key(VK_SHIFT, true);
        Key(VK_LWIN, true);
        Sleep(800);
        for (int i = 0; i < 2; i++) {
            Key(VK_LWIN, false);
            Key(VK_CONTROL, false);
            Press(VK_TAB);
            Key(VK_CONTROL, true);
            Key(VK_LWIN, true);
            bool wanted = i == 0;
            DWORD asked = GetTickCount();
            while (TaskViewUp() != wanted && GetTickCount() - asked < 2000) {
                Sleep(5);
            }
            printf("Win+Ctrl+Tab: Task View %s after %lu ms\n",
                   TaskViewUp() ? "up" : "down", GetTickCount() - asked);
            Sleep(600);
        }
    }
    Sleep(1200);
    watch.Stop();
    bool clean = watch.changes == expected;
    bool blocked = !clean && watch.changes < expected && ElevatedInFront();
    printf("%s, %d presses %d ms apart, from desktop %d: %d desktop changes, "
           "ended on %d with %s in front - %s\n",
           test.c_str(), presses, gap, DesktopNumber(start), watch.changes,
           DesktopNumber(CurrentDesktop()), Front().c_str(),
           clean     ? "CLEAN"
           : blocked ? "NOT CONCLUSIVE: an elevated window came in front, and "
                       "the keys after it never reached the shell"
                     : "WENT BACK AND FORTH");
    if (!clean && !blocked) {
        printf("%s", watch.log.c_str());
    }

    if (!installed) {
        PostThreadMessageW(trayThread, control, 0, 0);
        Sleep(1500);
        for (HHOOK hook : hooks) {
            UnhookWindowsHookEx(hook);
        }
    }
    timeEndPeriod(1);
    return clean ? 0 : 1;
}
#endif
