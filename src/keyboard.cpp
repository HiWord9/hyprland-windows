// The mod's keyboard thread: a thread of its own with a low-level keyboard
// hook, for two things - keeping the Start menu shut after a Win + mouse
// gesture (the mask, below), and seeing the key bindings before Windows'
// own shortcuts do (hotkey.cpp).
//
// Windows opens the Start menu when the Win key is released and the last key
// pressed while it was held was the Win key itself (a lone Win tap). A mouse
// click doesn't count as a key, so after a Win + drag the release would open
// Start. Pressing another key marks the chord as "used", but only if it is the
// LAST key-down before the release - and because the user physically holds
// Win, it auto-repeats, so any mask sent at button-down is undone by the next
// Win auto-repeat. The reliable fix is to catch the physical Win key-up with a
// low-level keyboard hook, swallow it, and re-inject a masking key immediately
// followed by a fresh Win key-up (test/startmenu_probe.cpp tries the
// alternatives).
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
//
// Wherever its keyboard hook stays, the thread runs a low-level mouse hook as
// well, for the presses no message hook gets to see (mouse.cpp).
//
// The same hook also sees the key bindings before Windows does, so that one
// can take over a shortcut of Windows' own, such as Win+W (hotkey.cpp). For
// that it stays in place for good in the shell. Keys typed into an elevated
// window don't reach it there - Windows keeps them from the hooks of the
// processes below - so an elevated process with windows runs a server of its
// own as well, whose hook is in place while one of its windows is in front:
// never more than one of those at a time.
#include "common.h"

std::atomic<bool> g_winMaskArmed{false};

constexpr WCHAR kKeyboardServerClass[] = L"HyprlandWindowsKeyboard_" WH_MOD_ID;
constexpr UINT kMaskArm = WM_APP + 1;  // posted to the server: arm for this press
// Posted to the server: a mask in an elevated process has had the release.
constexpr UINT kMaskStandDown = WM_APP + 2;
constexpr UINT kMaskPollMs = 30;
constexpr UINT_PTR kMaskTimerId = 1;
constexpr DWORD kServerStartWaitMs = 2000;

// This process's own server, if it runs one. Guarded by g_serverMutex.
std::mutex g_serverMutex;
HWND g_keyboardServer;
DWORD g_serverThreadId;

// Only ever touched on the server thread.
HHOOK g_keyboardHook;
HHOOK g_mouseHook;
// The hook stays even with no mask armed: in the shell, and in an elevated
// process while one of its windows is in front.
bool g_keepKeyboardHook;
HWINEVENTHOOK g_foregroundHook;

bool WinKeyDown() {
    return ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) !=
           0;
}

// Which keys the armed mask is for. Alt has a lone tap of its own to keep
// from happening, when it is the modifier: the window's menu, or the access
// keys of a XAML app. Only ever touched on the server thread.
bool g_maskWin, g_maskAlt;
constexpr WPARAM kMaskForWin = 1, kMaskForAlt = 2;

HWND FindKeyboardServer();

// A key pressed while an elevated window is in front is never seen down by
// the processes below it, not even once that window is gone: an elevated
// process arming the shell's mask says the keys are down (kMaskOnWord), and
// the mask takes its word for it - until a release has been masked, here or
// there, or for no longer than a press is ever held.
constexpr WPARAM kMaskOnWord = 4;
constexpr DWORD kMaskOnWordMaxMs = 10000;
bool g_maskOnWord;
DWORD g_maskOnWordSince;

bool AltKeyDown() {
    return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

// A modifier held through a drag repeats, and every repeat in the window's
// queue keeps the mouse moves on either side of it from merging into one: a
// window slow to resize then takes a step for every move the mouse made, and
// catches up long after it has stopped. Nothing does anything with a repeated
// modifier, so the repeats of the ones a drag is done with go while a mouse
// button is held. A repeat is a press of a key that is down already - this
// hook is called before Windows has taken the press in.
bool IsDragKey(DWORD vk) {
    UINT keepAspect = g_settings.keepAspectVk;
    UINT snap = g_settings.snapModifierVk;
    switch (vk) {
        case VK_LWIN:
        case VK_RWIN:
            return g_settings.dragModifier == DragModifier::Win;
        case VK_LMENU:
        case VK_RMENU:
            return g_settings.dragModifier == DragModifier::Alt ||
                   keepAspect == VK_MENU || snap == VK_MENU;
        case VK_LCONTROL:
        case VK_RCONTROL:
            return keepAspect == VK_CONTROL || snap == VK_CONTROL;
        case VK_LSHIFT:
        case VK_RSHIFT:
            return keepAspect == VK_SHIFT || snap == VK_SHIFT;
    }
    return false;
}

bool IsModifierRepeatDuringPress(DWORD vk) {
    return IsDragKey(vk) && (GetAsyncKeyState(vk) & 0x8000) &&
           ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
             GetAsyncKeyState(VK_MBUTTON)) &
            0x8000);
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    ModRef ref;  // the image must not go away under this procedure

    if (code != HC_ACTION || g_uninitializing) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    bool keyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
    bool ours = info->dwExtraInfo == kInjectedMarker;
    if (!ours && !keyUp && IsModifierRepeatDuringPress(info->vkCode)) {
        return 1;
    }
    if (!ours && !keyUp) {
        BringDesktopForwardForKey(info->vkCode);
    }
    if (!ours && HandleBindingKey(info->vkCode, !keyUp)) {
        return 1;
    }
    bool isWin = info->vkCode == VK_LWIN || info->vkCode == VK_RWIN;
    bool isModifier = g_settings.dragModifier == DragModifier::Win
                          ? isWin
                          : info->vkCode == VK_LMENU || info->vkCode == VK_RMENU;
    if (keyUp && isModifier) {
        EndWheelTurn();  // the next turn is for the window it starts over
    }
    bool isAlt = info->vkCode == VK_LMENU || info->vkCode == VK_RMENU ||
                 info->vkCode == VK_MENU;
    if (g_winMaskArmed && keyUp &&
        ((isWin && g_maskWin) || (isAlt && g_maskAlt))) {
        // An injected release is another mask's - armed in some other
        // process for the same press - that got to it first. Standing down
        // with it, rather than staying armed, keeps this one from eating the
        // next tap.
        (isWin ? g_maskWin : g_maskAlt) = false;
        g_winMaskArmed = g_maskWin || g_maskAlt;
        if (!ours) {
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
            // The shell's mask, armed on this process's word, never sees
            // this release go by: it is told.
            if (IsElevatedProcess()) {
                HWND shell = FindKeyboardServer();
                if (shell && shell != g_keyboardServer) {
                    PostMessageW(shell, kMaskStandDown,
                                 isWin ? kMaskForWin : kMaskForAlt, 0);
                }
            }
            return 1;  // swallow the physical key-up
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

////////////////////////////////////////////////////////////////////////////////
// The server thread

// In place while there is a mask armed or a reason to keep it, and not a
// moment longer.
void UpdateKeyboardHook() {
    bool wanted = !g_uninitializing && (g_keepKeyboardHook || g_winMaskArmed);
    if (wanted && !g_keyboardHook) {
        // A low-level hook procedure is called in the thread that installed
        // it, so no module handle is needed.
        g_keyboardHook =
            SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr, 0);
        if (!g_keyboardHook) {
            Wh_Log(L"WH_KEYBOARD_LL hook failed (%u)", GetLastError());
        }
    } else if (!wanted && g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }

    // And the mouse hook (mouse.cpp) where the keyboard one stays for good:
    // the shell, and an elevated process while a window of its own is in
    // front - the presses on its windows are kept from the shell's hook. One
    // with content the message hook cannot see keeps it in the background
    // too, so that a press there finds it. Not every elevated process at all
    // times: each low-level hook is a stop every move of the mouse makes.
    bool mouseWanted =
        !g_uninitializing &&
        (g_keepKeyboardHook ||
         (IsElevatedProcess() && HasCompositionContentWindow()));
    if (mouseWanted && !g_mouseHook) {
        g_mouseHook =
            SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);
        if (!g_mouseHook) {
            Wh_Log(L"WH_MOUSE_LL hook failed (%u)", GetLastError());
        }
    } else if (!mouseWanted && g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
}

void DisarmMask(HWND hwnd) {
    g_winMaskArmed = false;
    g_maskWin = g_maskAlt = false;
    g_maskOnWord = false;
    KillTimer(hwnd, kMaskTimerId);
    UpdateKeyboardHook();
}

void ArmMask(HWND hwnd, WPARAM keys) {
    // A press that is already over gets none: a mask armed for it now would
    // be left for the next one.
    bool onWord = (keys & kMaskOnWord) != 0;
    g_maskWin |= (keys & kMaskForWin) && (onWord || WinKeyDown());
    g_maskAlt |= (keys & kMaskForAlt) && (onWord || AltKeyDown());
    if (g_uninitializing || !(g_maskWin || g_maskAlt)) {
        g_maskWin = g_maskAlt = false;
        return;
    }
    if (onWord) {
        g_maskOnWord = true;
        g_maskOnWordSince = GetTickCount();
    }
    g_winMaskArmed = true;
    UpdateKeyboardHook();
    if (!g_keyboardHook) {
        g_winMaskArmed = false;
        return;
    }
    SetTimer(hwnd, kMaskTimerId, kMaskPollMs, nullptr);
}

// What the mask sends ahead of a release, sent by itself: for a key binding
// the hook took, whose Win or Alt is then no lone tap.
void MaskModifierTap() {
    INPUT input[2]{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = kMaskVk;
    input[0].ki.dwExtraInfo = kInjectedMarker;
    input[1] = input[0];
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(ARRAYSIZE(input), input, sizeof(INPUT));
}

// An elevated process keeps the hook while a window of its own is in front,
// which is when the shell's hook is blind.
void FollowForeground(HWND foreground) {
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    g_keepKeyboardHook = pid == GetCurrentProcessId();
    UpdateKeyboardHook();
}

void CALLBACK OnForegroundChanged(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG,
                                  DWORD, DWORD) {
    FollowForeground(hwnd);
}

LRESULT CALLBACK KeyboardServerProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
    switch (msg) {
        case kMaskArm:
            ArmMask(hwnd, wParam);
            return 0;
        case WM_TIMER:
            // The hook takes the mask down when it masks a release. A release
            // can also go by without reaching it at all, and a mask that
            // outlived its press would swallow the next one - so the key being
            // up is what ends it - or, armed on an elevated process's word, a
            // release masked there, or the longest a press is ever held.
            if (g_maskOnWord &&
                GetTickCount() - g_maskOnWordSince > kMaskOnWordMaxMs) {
                g_maskOnWord = false;
            }
            g_maskWin = g_maskWin && (g_maskOnWord || WinKeyDown());
            g_maskAlt = g_maskAlt && (g_maskOnWord || AltKeyDown());
            if (!g_winMaskArmed || !(g_maskWin || g_maskAlt)) {
                DisarmMask(hwnd);
            }
            return 0;
        case kMaskStandDown:
            if (g_maskOnWord) {
                g_maskWin = g_maskWin && !(wParam & kMaskForWin);
                g_maskAlt = g_maskAlt && !(wParam & kMaskForAlt);
                if (!(g_maskWin || g_maskAlt)) {
                    DisarmMask(hwnd);
                }
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HINSTANCE ThisModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&KeyboardServerProc), &module);
    return module;
}

DWORD WINAPI KeyboardServerThread(LPVOID param) {
    // The queue first. The shutdown posts WM_QUIT here, and a post to a thread
    // that has no queue yet is lost: the loop below would wait for good.
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);

    HANDLE ready = param;
    // Registered against this image rather than the process: two copies of
    // the mod can be loaded side by side while one replaces the other, and a
    // window of this one must never end up running the other's procedure,
    // which goes away when that copy does. The class name is the same for
    // both, which is what lets a gesture find whichever server is up.
    HINSTANCE instance = ThisModule();
    WNDCLASSW wc{};
    wc.lpfnWndProc = KeyboardServerProc;
    wc.hInstance = instance;
    wc.lpszClassName = kKeyboardServerClass;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, kKeyboardServerClass, nullptr, 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, instance, nullptr);
    if (hwnd) {
        // Gestures in processes of any integrity level ask for a mask here.
        ChangeWindowMessageFilterEx(hwnd, kMaskArm, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(hwnd, kMaskStandDown, MSGFLT_ALLOW,
                                    nullptr);
    }
    {
        std::lock_guard<std::mutex> lock(g_serverMutex);
        g_keyboardServer = hwnd;
    }
    SetEvent(ready);

    if (hwnd) {
        if (IsShellProcess()) {
            g_keepKeyboardHook = true;
            UpdateKeyboardHook();
        } else if (IsElevatedProcess()) {
            // Only the foreground changing, delivered to this thread's queue:
            // Windows does not wait for it, and nothing else comes of it.
            g_foregroundHook = SetWinEventHook(
                EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                OnForegroundChanged, 0, 0, WINEVENT_OUTOFCONTEXT);
            FollowForeground(GetForegroundWindow());
        }
        // A shutdown from before the queue was there is seen here instead: it
        // sets the flag before it posts.
        while (!g_uninitializing && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            DispatchMessageW(&msg);
        }
        if (g_foregroundHook) {
            UnhookWinEvent(g_foregroundHook);
            g_foregroundHook = nullptr;
        }
        g_keepKeyboardHook = false;
        DisarmMask(hwnd);  // which takes the hook down with it
        DestroyWindow(hwnd);
    }
    // The class's procedure is in this image, so it has to go with it.
    UnregisterClassW(kKeyboardServerClass, instance);
    {
        std::lock_guard<std::mutex> lock(g_serverMutex);
        g_keyboardServer = nullptr;
        g_serverThreadId = 0;
    }
    g_modRefCount--;  // the last thing this thread does in the mod's image
    return 0;
}

// This process's server, started if it is not running yet. Waits for its
// window, since the caller is about to post to it.
HWND StartServer() {
    std::unique_lock<std::mutex> lock(g_serverMutex);
    if (g_keyboardServer || g_serverThreadId || g_uninitializing) {
        return g_keyboardServer;  // up, or on its way up
    }
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) {
        return nullptr;
    }
    if (!StartModThread(KeyboardServerThread, ready, &g_serverThreadId)) {
        CloseHandle(ready);
        return nullptr;
    }
    // The thread takes the lock to publish its window, so it is let go of
    // while waiting.
    lock.unlock();
    WaitForSingleObject(ready, kServerStartWaitMs);
    lock.lock();
    CloseHandle(ready);
    return g_keyboardServer;
}

// The server that should take this press: the one in the shell's own process
// when there is one. Explorer can run a second process for folder windows,
// and that one goes away with them.
HWND FindKeyboardServer() {
    DWORD shellPid = 0;
    if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr)) {
        GetWindowThreadProcessId(tray, &shellPid);
    }
    HWND any = nullptr;
    HWND found = nullptr;
    while ((found = FindWindowExW(HWND_MESSAGE, found, kKeyboardServerClass,
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

// The shell's own Explorer. With folder windows in a separate process, other
// copies of explorer.exe run them, and those must not take the shell's part: a
// second permanent keyboard hook, and hotkeys only the shell gets. The shell's
// copy is the one that owns the shell window, or the one starting up before
// there is any, as after an Explorer restart. Decided once, because that
// window comes and goes with the shell.
bool IsShellProcess() {
    static const bool shell = [] {
        if (_wcsicmp(ThisProgramName().c_str(), L"explorer.exe") != 0) {
            return false;
        }
        HWND shellWindow = GetShellWindow();
        DWORD owner = 0;
        return !shellWindow ||
               (GetWindowThreadProcessId(shellWindow, &owner) &&
                owner == GetCurrentProcessId());
    }();
    return shell;
}

BOOL CALLBACK FindOwnWindowProc(HWND hwnd, LPARAM lParam) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        *reinterpret_cast<bool*>(lParam) = true;
        return FALSE;
    }
    return TRUE;
}

// Called from Wh_ModAfterInit: the shell has its server up from the start, so
// that it is there to be found before anybody needs it, and its hook sees the
// key bindings. So does an elevated process that has windows already.
void StartKeyboardServer() {
    bool hasWindows = false;
    if (!IsShellProcess() && IsElevatedProcess()) {
        EnumWindows(FindOwnWindowProc, (LPARAM)&hasWindows);
    }
    if (IsShellProcess() || hasWindows) {
        StartServer();
    }
}

// Called for every top-level window a process creates: the one that gives an
// elevated process a window to follow. Processes without one - services and
// the like - never run a server at all.
void StartKeyboardServerForWindow() {
    if (IsElevatedProcess()) {
        StartServer();
    }
}

bool IsElevatedProcess() {
    static const bool elevated = [] {
        HANDLE token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            return false;
        }
        TOKEN_ELEVATION elevation{};
        DWORD len = 0;
        bool result = GetTokenInformation(token, TokenElevation, &elevation,
                                          sizeof(elevation), &len) &&
                      elevation.TokenIsElevated;
        CloseHandle(token);
        return result;
    }();
    return elevated;
}

// Called by whatever just consumed a Win + mouse gesture; `usingWin` and
// `usingAlt` are that gesture's own answer to which of the keys are part of
// it, because the modifier is not the same setting for every one of them.
void ArmWinMask(bool usingWin, bool usingAlt) {
    WPARAM keys = (usingWin && WinKeyDown() ? kMaskForWin : 0) |
                  (usingAlt && AltKeyDown() ? kMaskForAlt : 0);
    if (!keys || g_uninitializing) {
        return;
    }
    HWND server = FindKeyboardServer();
    if (!server) {
        server = StartServer();
    }
    // Windows keeps the keys typed into an elevated window - Task Manager,
    // anything run as administrator - from the low-level hooks of the
    // processes below it, the shell's included. Such a process arms a mask
    // of its own as well, which sees them. The shell's is armed too, on this
    // process's word that the keys are down, since it cannot tell: if the
    // gesture closed the process's last window, the release goes to whatever
    // window comes forward next.
    HWND own = IsElevatedProcess() ? StartServer() : nullptr;
    WPARAM word = own && own != server ? kMaskOnWord : 0;
    if (!server || !PostMessageW(server, kMaskArm, keys | word, 0)) {
        Wh_Log(L"No Start menu mask to arm (%u)", GetLastError());
    }
    if (own && own != server) {
        PostMessageW(own, kMaskArm, keys, 0);
    }
}

// Tear the suppression down: this process's server, if it has one, and with
// it the hook it may still hold. The thread keeps a reference on the image
// until it is out, which Wh_ModUninit waits for.
void ShutdownKeyboardServer() {
    g_winMaskArmed = false;
    std::lock_guard<std::mutex> lock(g_serverMutex);
    if (g_serverThreadId) {
        PostThreadMessageW(g_serverThreadId, WM_QUIT, 0, 0);
    }
}
