// Keeping the Start menu shut when the Win key is released after a Win +
// mouse gesture.
//
// Windows opens the Start menu when the Win key is released and the last key
// pressed while it was held was the Win key itself (a lone Win tap). A mouse
// click doesn't count as a key, so after a Win + drag the release would open
// Start. Pressing another key marks the chord as "used", but only if it is the
// LAST key-down before the release - and because the user physically holds
// Win, it auto-repeats, so any mask sent at button-down is undone by the next
// Win auto-repeat. The reliable fix is to catch the physical Win key-up with a
// low-level keyboard hook, swallow it, and re-inject a masking key immediately
// followed by a fresh Win key-up.
//
// Where that hook lives matters as much as what it does. Windows calls a
// low-level hook on the thread that installed it, and when that thread does
// not answer in time it goes on without it (test/mask_busy_probe.cpp):
//
//   * On a thread that is busy when Win comes up - an application taking a
//     window down, the taskbar rearranging its buttons after one has gone -
//     the release goes through and Start opens. The mask is still armed
//     afterwards, so it swallows the next Win press instead: the one that was
//     meant for Start.
//   * On the thread of a window that is being closed, the hook is simply gone
//     by then, together with the whole process when that was its last window.
//
// So the hook lives on a thread of the mod's own that does nothing else and is
// never busy, in the shell's process, which is never the one going away. A
// gesture anywhere asks that thread to arm through a message-only window. If
// no such window can be found - the shell is running an older copy of the mod,
// or there is no shell - the process starts a thread of its own for it. And
// the thread watches the Win key itself: a mask whose press is over comes
// down, whether or not the release ever reached the hook.
#include "common.h"

std::atomic<bool> g_winMaskArmed{false};

constexpr WCHAR kMaskServerClass[] = L"HyprlandWindowsMask_" WH_MOD_ID;
constexpr UINT kMaskArm = WM_APP + 1;  // posted to the server: arm for this press
constexpr UINT kMaskPollMs = 30;
constexpr UINT_PTR kMaskTimerId = 1;
constexpr DWORD kMaskServerStartWaitMs = 2000;

// This process's own server, if it runs one. Guarded by g_maskMutex.
std::mutex g_maskMutex;
HWND g_maskServer;
DWORD g_maskThreadId;

// Only ever touched on the server thread.
HHOOK g_maskHook;

bool WinKeyDown() {
    return ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) !=
           0;
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    ModRef ref;  // the image must not go away under this procedure

    if (code == HC_ACTION && g_winMaskArmed && !g_uninitializing) {
        auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool keyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
        bool isWin = info->vkCode == VK_LWIN || info->vkCode == VK_RWIN;
        bool ours = info->dwExtraInfo == kInjectedMarker;
        if (keyUp && isWin && ours) {
            // Another mask - armed in some other process for the same press -
            // got to this release first. Standing down with it, rather than
            // staying armed, keeps this one from eating the next Win tap.
            g_winMaskArmed = false;
        } else if (keyUp && isWin && !ours) {
            g_winMaskArmed = false;

            INPUT input[3]{};
            input[0].type = INPUT_KEYBOARD;
            input[0].ki.wVk = kMaskVk;
            input[0].ki.dwExtraInfo = kInjectedMarker;
            input[1] = input[0];
            input[1].ki.dwFlags = KEYEVENTF_KEYUP;
            input[2].type = INPUT_KEYBOARD;
            input[2].ki.wVk = (WORD)info->vkCode;
            input[2].ki.dwFlags = KEYEVENTF_KEYUP;
            input[2].ki.dwExtraInfo = kInjectedMarker;
            SendInput(ARRAYSIZE(input), input, sizeof(INPUT));
            return 1;  // swallow the physical Win key-up
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

////////////////////////////////////////////////////////////////////////////////
// The server thread

void DisarmOnServer(HWND hwnd) {
    g_winMaskArmed = false;
    KillTimer(hwnd, kMaskTimerId);
    if (g_maskHook) {
        UnhookWindowsHookEx(g_maskHook);
        g_maskHook = nullptr;
    }
}

void ArmOnServer(HWND hwnd) {
    if (g_uninitializing || !WinKeyDown()) {
        // The press is already over, and a mask armed for it now would be
        // left for the next one.
        return;
    }
    if (!g_maskHook) {
        // A low-level hook procedure is called in the thread that installed
        // it, so no module handle is needed.
        g_maskHook =
            SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr, 0);
        if (!g_maskHook) {
            Wh_Log(L"WH_KEYBOARD_LL hook failed (%u)", GetLastError());
            return;
        }
    }
    g_winMaskArmed = true;
    SetTimer(hwnd, kMaskTimerId, kMaskPollMs, nullptr);
}

LRESULT CALLBACK MaskServerProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
    switch (msg) {
        case kMaskArm:
            ArmOnServer(hwnd);
            return 0;
        case WM_TIMER:
            // The hook takes the mask down when it masks a release. A release
            // can also go by without reaching it at all, and a mask that
            // outlived its press would swallow the next one - so the key being
            // up is what ends it.
            if (!g_winMaskArmed || !WinKeyDown()) {
                DisarmOnServer(hwnd);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HINSTANCE ThisModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&MaskServerProc), &module);
    return module;
}

DWORD WINAPI MaskServerThread(LPVOID param) {
    HANDLE ready = param;
    // Registered against this image rather than the process: two copies of
    // the mod can be loaded side by side while one replaces the other, and a
    // window of this one must never end up running the other's procedure,
    // which goes away when that copy does. The class name is the same for
    // both, which is what lets a gesture find whichever server is up.
    HINSTANCE instance = ThisModule();
    WNDCLASSW wc{};
    wc.lpfnWndProc = MaskServerProc;
    wc.hInstance = instance;
    wc.lpszClassName = kMaskServerClass;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, kMaskServerClass, nullptr, 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, instance, nullptr);
    if (hwnd) {
        // Gestures in processes of any integrity level ask for a mask here.
        ChangeWindowMessageFilterEx(hwnd, kMaskArm, MSGFLT_ALLOW, nullptr);
    }
    {
        std::lock_guard<std::mutex> lock(g_maskMutex);
        g_maskServer = hwnd;
    }
    SetEvent(ready);

    if (hwnd) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            DispatchMessageW(&msg);
        }
        DisarmOnServer(hwnd);
        DestroyWindow(hwnd);
    }
    // The class's procedure is in this image, so it has to go with it.
    UnregisterClassW(kMaskServerClass, instance);
    {
        std::lock_guard<std::mutex> lock(g_maskMutex);
        g_maskServer = nullptr;
        g_maskThreadId = 0;
    }
    g_modRefCount--;  // the last thing this thread does in the mod's image
    return 0;
}

// This process's server, started if it is not running yet. Waits for its
// window, since the caller is about to post to it.
HWND StartMaskServer() {
    std::unique_lock<std::mutex> lock(g_maskMutex);
    if (g_maskServer || g_maskThreadId || g_uninitializing) {
        return g_maskServer;  // up, or on its way up
    }
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) {
        return nullptr;
    }
    g_modRefCount++;
    HANDLE thread = CreateThread(nullptr, 0, MaskServerThread, ready, 0,
                                 &g_maskThreadId);
    if (!thread) {
        Wh_Log(L"CreateThread failed (%u)", GetLastError());
        g_modRefCount--;
        CloseHandle(ready);
        return nullptr;
    }
    CloseHandle(thread);
    // The thread takes the lock to publish its window, so it is let go of
    // while waiting.
    lock.unlock();
    WaitForSingleObject(ready, kMaskServerStartWaitMs);
    lock.lock();
    CloseHandle(ready);
    return g_maskServer;
}

// The server that should take this press: the one in the shell's own process
// when there is one. Explorer can run a second process for folder windows,
// and that one goes away with them.
HWND FindMaskServer() {
    DWORD shellPid = 0;
    if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr)) {
        GetWindowThreadProcessId(tray, &shellPid);
    }
    HWND any = nullptr;
    HWND found = nullptr;
    while ((found = FindWindowExW(HWND_MESSAGE, found, kMaskServerClass,
                                  nullptr))) {
        DWORD pid = 0;
        GetWindowThreadProcessId(found, &pid);
        if (shellPid && pid == shellPid) {
            return found;
        }
        if (!any) {
            any = found;
        }
    }
    return any;
}

bool IsShellProcess() {
    WCHAR path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!len || len >= MAX_PATH) {
        return false;
    }
    PCWSTR name = wcsrchr(path, L'\\');
    return name && _wcsicmp(name + 1, L"explorer.exe") == 0;
}

// Called from Wh_ModAfterInit: the shell has its server up from the start,
// so that it is there to be found before anybody needs it.
void StartShellMaskServer() {
    if (IsShellProcess()) {
        StartMaskServer();
    }
}

// Called by whatever just consumed a Win + mouse gesture; `usingWin` is that
// gesture's own answer to whether the Win key is part of it, because the
// modifier is not the same setting for every one of them.
void ArmWinMask(bool usingWin) {
    if (!usingWin || g_uninitializing || !WinKeyDown()) {
        return;
    }
    HWND server = FindMaskServer();
    if (!server) {
        server = StartMaskServer();
    }
    if (!server || !PostMessageW(server, kMaskArm, 0, 0)) {
        Wh_Log(L"No Start menu mask to arm (%u)", GetLastError());
    }
}

// Tear the suppression down: this process's server, if it has one, and with
// it the hook it may still hold. The thread keeps a reference on the image
// until it is out, which Wh_ModUninit waits for.
void ShutdownWinMask() {
    g_winMaskArmed = false;
    std::lock_guard<std::mutex> lock(g_maskMutex);
    if (g_maskThreadId) {
        PostThreadMessageW(g_maskThreadId, WM_QUIT, 0, 0);
    }
}
