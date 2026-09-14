// Every message an app retrieves passes through here, as does every window it
// creates.
//
// Messages are intercepted with a WH_GETMESSAGE hook rather than by hooking
// GetMessage. Hooking GetMessage looks simpler, but GetMessage *blocks*: an
// idle thread parks inside the hook function, leaving a frame that belongs to
// this mod on its stack for as long as the app has nothing to do. Windhawk
// then cannot unload the mod without those threads eventually returning into
// freed memory, which crashed every process with a message loop (shell
// included) on every disable. A hook procedure only runs for the moment the
// message is handed over, so nothing of ours stays on the stack.
#include "common.h"

#include <tlhelp32.h>

// Called for every message an application removes from its queue. Returns
// with the message replaced by WM_NULL if it was consumed by the mod.
void ProcessRetrievedMessage(MSG* msg) {
    // Never act on the mod's own injected input (used to drive the resize
    // loop), and never turn it into WM_NULL - the native loop needs to see its
    // synthetic left-button-up to end.
    if ((ULONG_PTR)GetMessageExtraInfo() == kInjectedMarker) {
        return;
    }

    bool consumed = false;

    switch (msg->message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            consumed = HandleHotkey(msg);
            break;

        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
            consumed = HandleModifierButtonDown(msg, false);
            break;

        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
        case WM_NCRBUTTONDOWN:
        case WM_NCRBUTTONDBLCLK:
            consumed = HandleModifierButtonDown(msg, true);
            break;

        case WM_LBUTTONUP:
        case WM_NCLBUTTONUP:
            consumed = HandleButtonUp(false);
            break;

        case WM_RBUTTONUP:
        case WM_NCRBUTTONUP:
            consumed = HandleButtonUp(true);
            break;

        default:
            if (msg->message == g_msgFrameless && msg->hwnd) {
                HandleFramelessRequest(msg->hwnd, msg->wParam);
                consumed = true;
            }
            break;
    }

    if (consumed) {
        msg->message = WM_NULL;
        msg->wParam = 0;
        msg->lParam = 0;
    }
}

////////////////////////////////////////////////////////////////////////////////
// The message hook, one per message-pumping thread of this process

std::mutex g_messageHooksMutex;
std::unordered_map<DWORD, HHOOK> g_messageHooks;  // thread id -> its hook

LRESULT CALLBACK GetMessageProc(int code, WPARAM wParam, LPARAM lParam) {
    // PM_NOREMOVE means the app is only looking at the message; it stays in the
    // queue and we must not consume it.
    if (code == HC_ACTION && wParam == PM_REMOVE) {
        ProcessRetrievedMessage(reinterpret_cast<MSG*>(lParam));
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void InstallMessageHookForThread() {
    DWORD threadId = GetCurrentThreadId();
    std::lock_guard<std::mutex> lock(g_messageHooksMutex);
    if (g_messageHooks.count(threadId)) {
        return;
    }
    HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, GetMessageProc,
                                   ModuleInstance(), threadId);
    if (hook) {
        g_messageHooks[threadId] = hook;
    } else {
        Wh_Log(L"WH_GETMESSAGE hook failed for thread %u (%u)", threadId,
               GetLastError());
    }
}

// Covers the threads that already had windows when the mod was loaded; threads
// that come later are caught when they create their first window.
BOOL CALLBACK InstallHookEnumProc(HWND hwnd, LPARAM lParam) {
    DWORD pid = 0;
    DWORD threadId = GetWindowThreadProcessId(hwnd, &pid);
    if (pid != (DWORD)lParam || !threadId) {
        return TRUE;
    }

    std::lock_guard<std::mutex> lock(g_messageHooksMutex);
    if (!g_messageHooks.count(threadId)) {
        HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, GetMessageProc,
                                       ModuleInstance(), threadId);
        if (hook) {
            g_messageHooks[threadId] = hook;
        }
    }
    return TRUE;
}

void InstallMessageHooks() {
    EnumWindows(InstallHookEnumProc, (LPARAM)GetCurrentProcessId());
}

void RemoveMessageHooks() {
    std::lock_guard<std::mutex> lock(g_messageHooksMutex);
    for (const auto& [threadId, hook] : g_messageHooks) {
        UnhookWindowsHookEx(hook);
    }
    g_messageHooks.clear();
}

////////////////////////////////////////////////////////////////////////////////
// Window creation hooks ("hide by default")

void OnWindowCreated(HWND hwnd, DWORD dwStyle) {
    if (!hwnd) {
        return;
    }
    // A thread that creates a window is a thread that will pump messages, so
    // this is where threads born after the mod loaded get their hook.
    InstallMessageHookForThread();

    if ((dwStyle & WS_CHILD) || !g_settings.hideByDefault) {
        return;
    }
    if (IsAutoHideCandidate(hwnd)) {
        // Post instead of hiding right here. We are still inside the app's
        // CreateWindowEx call: the window exists but the code that owns it has
        // not run yet, and changing the frame at that point delivers a resize
        // into a half-initialized window. Some apps don't survive that - they
        // fail to start at all. Posting means the work happens once the window
        // is pumping messages, which is exactly when the hotkey path (which
        // has always worked) does it. The cost is that the title bar can be
        // visible for a frame or two first.
        RequestFrameless(hwnd, kActionAutoHide);
    }
}

CreateWindowExW_t CreateWindowExW_Original;
HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle,
                                 LPCWSTR lpClassName,
                                 LPCWSTR lpWindowName,
                                 DWORD dwStyle,
                                 int X,
                                 int Y,
                                 int nWidth,
                                 int nHeight,
                                 HWND hWndParent,
                                 HMENU hMenu,
                                 HINSTANCE hInstance,
                                 LPVOID lpParam) {
    HWND hwnd = CreateWindowExW_Original(dwExStyle, lpClassName, lpWindowName,
                                         dwStyle, X, Y, nWidth, nHeight,
                                         hWndParent, hMenu, hInstance, lpParam);
    OnWindowCreated(hwnd, dwStyle);
    return hwnd;
}

CreateWindowExA_t CreateWindowExA_Original;
HWND WINAPI CreateWindowExA_Hook(DWORD dwExStyle,
                                 LPCSTR lpClassName,
                                 LPCSTR lpWindowName,
                                 DWORD dwStyle,
                                 int X,
                                 int Y,
                                 int nWidth,
                                 int nHeight,
                                 HWND hWndParent,
                                 HMENU hMenu,
                                 HINSTANCE hInstance,
                                 LPVOID lpParam) {
    HWND hwnd = CreateWindowExA_Original(dwExStyle, lpClassName, lpWindowName,
                                         dwStyle, X, Y, nWidth, nHeight,
                                         hWndParent, hMenu, hInstance, lpParam);
    OnWindowCreated(hwnd, dwStyle);
    return hwnd;
}
