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

// Called for every message an application removes from its queue. Returns
// with the message replaced by WM_NULL if it was consumed by the mod.
void ProcessRetrievedMessage(MSG* msg) {
    bool consumed = false;

    switch (msg->message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            consumed = HandleHotkey(msg);
            break;

        // The shortcut gets first refusal on every button: it can be bound
        // to any of them, and one bound to left or right takes that button
        // away from the drag, which is the user's business to decide.
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
            consumed =
                HandleShortcutButton(msg) || HandleModifierButtonDown(msg, false);
            break;

        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
        case WM_NCRBUTTONDOWN:
        case WM_NCRBUTTONDBLCLK:
            consumed =
                HandleShortcutButton(msg) || HandleModifierButtonDown(msg, true);
            break;

        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
        case WM_NCMBUTTONDOWN:
        case WM_NCMBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONDBLCLK:
        case WM_NCXBUTTONDOWN:
        case WM_NCXBUTTONDBLCLK:
            consumed = HandleShortcutButton(msg);
            break;

        case WM_LBUTTONUP:
        case WM_NCLBUTTONUP:
            consumed = HandleShortcutButtonUp(msg) || HandleLeftButtonUp();
            break;

        case WM_RBUTTONUP:
        case WM_NCRBUTTONUP:
            consumed = HandleShortcutButtonUp(msg) || HandleButtonUp(true);
            break;

        case WM_MBUTTONUP:
        case WM_NCMBUTTONUP:
        case WM_XBUTTONUP:
        case WM_NCXBUTTONUP:
            consumed = HandleShortcutButtonUp(msg);
            break;

        default:
            if (!msg->hwnd) {
                break;
            }
            if (msg->message == g_msgDrag) {
                consumed = !HandleDragRequest(msg);
            } else if (msg->message == g_msgFrameless) {
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

// Two hooks per pumping thread. The first is for the messages an application
// retrieves; the second is for the ones sent to its windows, which never go
// near a queue - a window says that it has gained or lost focus by being sent
// WM_NCACTIVATE, and that is the only word the mod gets about a window it has
// not otherwise touched. The second one is only installed when there is a
// border color to paint with, because until then it listens for nothing.
struct ThreadHooks {
    HHOOK getMessage = nullptr;
    HHOOK callWndProc = nullptr;
};

std::mutex g_messageHooksMutex;
std::unordered_map<DWORD, ThreadHooks> g_messageHooks;

// Set once per thread, so the common case - a thread that has its hook
// already - costs nothing. With "*" as the include pattern this runs for every
// window every application ever creates.
thread_local bool g_messageHookAttempted;

LRESULT CALLBACK GetMessageProc(int code, WPARAM wParam, LPARAM lParam) {
    ModRef ref;  // the image must not go away under this procedure

    // wParam carries the flags the caller passed to PeekMessage, which often
    // include the PM_QS_* filter bits, so it is a bitwise test. PM_NOREMOVE
    // means the app is only looking at the message; it stays in the queue and
    // we must not consume it.
    if (code == HC_ACTION && (wParam & PM_REMOVE) && lParam &&
        !g_uninitializing) {
        ProcessRetrievedMessage(reinterpret_cast<MSG*>(lParam));
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK CallWndProc(int code, WPARAM wParam, LPARAM lParam) {
    ModRef ref;  // the image must not go away under this procedure

    if (code == HC_ACTION && lParam && !g_uninitializing) {
        auto* sent = reinterpret_cast<CWPSTRUCT*>(lParam);
        switch (sent->message) {
            case WM_NCACTIVATE:
                OnWindowActivation(sent->hwnd, sent->wParam != FALSE);
                break;
            case WM_DWMCOLORIZATIONCOLORCHANGED:
                // The accent color moved, so a border set to "accent" follows
                // it. At once, not faded: this is not a focus change.
                if (IsBorderColorTarget(sent->hwnd)) {
                    ApplyBorderColor(sent->hwnd,
                                     GetForegroundWindow() == sent->hwnd);
                }
                break;
            case WM_NCDESTROY:
                ForgetBorderColor(sent->hwnd);
                break;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// Callers hold g_messageHooksMutex, which is also what keeps an installation
// from slipping past the removal at uninit.
void InstallMessageHookLocked(DWORD threadId) {
    if (g_uninitializing) {
        return;
    }
    // A thread hook on a thread of this process wants no module handle.
    ThreadHooks& hooks = g_messageHooks[threadId];
    if (!hooks.getMessage) {
        hooks.getMessage =
            SetWindowsHookExW(WH_GETMESSAGE, GetMessageProc, nullptr, threadId);
        if (!hooks.getMessage) {
            Wh_Log(L"WH_GETMESSAGE hook failed for thread %u (%u)", threadId,
                   GetLastError());
        }
    }
    if (!hooks.callWndProc && BorderColorsWanted()) {
        hooks.callWndProc =
            SetWindowsHookExW(WH_CALLWNDPROC, CallWndProc, nullptr, threadId);
        if (!hooks.callWndProc) {
            Wh_Log(L"WH_CALLWNDPROC hook failed for thread %u (%u)", threadId,
                   GetLastError());
        }
    }
}

void InstallMessageHookForThread() {
    if (g_messageHookAttempted) {
        return;
    }
    g_messageHookAttempted = true;

    std::lock_guard<std::mutex> lock(g_messageHooksMutex);
    InstallMessageHookLocked(GetCurrentThreadId());
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
    InstallMessageHookLocked(threadId);
    return TRUE;
}

void InstallMessageHooks() {
    EnumWindows(InstallHookEnumProc, (LPARAM)GetCurrentProcessId());
}

void RemoveMessageHooks() {
    std::lock_guard<std::mutex> lock(g_messageHooksMutex);
    for (const auto& [threadId, hooks] : g_messageHooks) {
        if (hooks.getMessage) {
            UnhookWindowsHookEx(hooks.getMessage);
        }
        if (hooks.callWndProc) {
            UnhookWindowsHookEx(hooks.callWndProc);
        }
    }
    g_messageHooks.clear();
}

// The settings can take the border colors away again, and then there is
// nothing left for the hook on sent messages to listen for.
void RefreshCallWndProcHooks() {
    if (BorderColorsWanted()) {
        InstallMessageHooks();
        return;
    }
    std::lock_guard<std::mutex> lock(g_messageHooksMutex);
    for (auto& [threadId, hooks] : g_messageHooks) {
        if (hooks.callWndProc) {
            UnhookWindowsHookEx(hooks.callWndProc);
            hooks.callWndProc = nullptr;
        }
    }
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
