// Mod lifecycle: what Windhawk calls to load, reconfigure and unload us.
#include "common.h"

std::atomic<bool> g_uninitializing;
std::atomic<int> g_modRefCount;

// Every thread of the mod is started here, and its handle kept for
// Wh_ModUninit to wait on. The reference count alone is not enough: dropping
// its reference is the last thing a thread does, but it still has the rest
// of its function to return through, in the image.
std::mutex g_threadsMutex;
std::vector<HANDLE> g_threads;

bool StartModThread(LPTHREAD_START_ROUTINE proc, void* param, DWORD* threadId) {
    g_modRefCount++;  // the thread's own, dropped as it ends
    HANDLE thread = CreateThread(nullptr, 0, proc, param, 0, threadId);
    if (!thread) {
        Wh_Log(L"CreateThread failed (%u)", GetLastError());
        g_modRefCount--;
        return false;
    }
    std::lock_guard<std::mutex> lock(g_threadsMutex);
    // The ones that are done go, or a long session piles up a handle a drag.
    g_threads.erase(std::remove_if(g_threads.begin(), g_threads.end(),
                                   [](HANDLE h) {
                                       if (WaitForSingleObject(h, 0) !=
                                           WAIT_OBJECT_0) {
                                           return false;
                                       }
                                       CloseHandle(h);
                                       return true;
                                   }),
                    g_threads.end());
    g_threads.push_back(thread);
    return true;
}

void JoinModThreads() {
    std::vector<HANDLE> threads;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        threads.swap(g_threads);
    }
    for (HANDLE thread : threads) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
}

BOOL Wh_ModInit() {
    Wh_Log(L"Init");

    g_msgFrameless = RegisterWindowMessageW(L"HyprlandWindows_" WH_MOD_ID);
    g_msgDrag = RegisterWindowMessageW(L"HyprlandWindowsDrag_" WH_MOD_ID);
    if (!g_msgFrameless || !g_msgDrag) {
        Wh_Log(L"RegisterWindowMessage failed");
        return FALSE;
    }

    // Both messages cross process boundaries: a UWP window, for one, is framed
    // by a window of ApplicationFrameHost.exe, which runs at a higher
    // integrity level than the app itself. Without this the UIPI message
    // filter drops the request and nothing happens at all.
    ChangeWindowMessageFilter(g_msgFrameless, MSGFLT_ADD);
    ChangeWindowMessageFilter(g_msgDrag, MSGFLT_ADD);

    LoadSettings();

    // Only short, non-blocking functions are hooked. Message interception is a
    // WH_GETMESSAGE hook instead, installed per thread below.
    WindhawkUtils::SetFunctionHook(CreateWindowExW, CreateWindowExW_Hook,
                                   &CreateWindowExW_Original);
    WindhawkUtils::SetFunctionHook(CreateWindowExA, CreateWindowExA_Hook,
                                   &CreateWindowExA_Original);

    return TRUE;
}

void Wh_ModAfterInit() {
    InstallMessageHooks();
    // In the shell, the thread that keeps the Start menu shut after a Win +
    // mouse gesture and sees the key bindings first - up from the start, so it
    // is there to be found.
    StartKeyboardServer();
    // And the one that takes Win+Tab through the desktops.
    if (IsShellProcess()) {
        StartDesktopThread();
    }
    RefreshBorderColors();
    if (g_settings.hideByDefault) {
        AutoHideExistingWindows();
    }
}

void Wh_ModBeforeUninit() {
    Wh_Log(L"BeforeUninit: restoring title bars");

    // Before anything else, so that nothing installs a hook or subclasses a
    // window behind the teardown's back.
    g_uninitializing = true;

    // Take our hook procedures out before the DLL goes away.
    RemoveMessageHooks();
    ShutdownKeyboardServer();
    ShutdownDesktopThread();

    // And stop the border fades before the windows get their defaults back:
    // a color written after that would stay on the window for good.
    FinishBorderFades();
    RestoreAllBorderColors();

    // A drag in progress has a subclass of ours on its window as well, and
    // the same rule applies to it: it has to come off on the window's own
    // thread, which is where this sent message is answered.
    for (HWND hwnd : SnapshotSnappedWindows()) {
        DWORD_PTR result;
        SendMessageTimeoutW(hwnd, g_msgDrag, kDragUnsnap, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result);
    }

    // Restore synchronously on each window's thread, so that no subclass
    // procedure is left behind once the DLL is gone. A subclass procedure in
    // an unmapped image crashes its application the next time the window gets
    // a message, so a window that does not answer is tried again, and then
    // waited for: hanging the unload is bad, crashing the app is worse.
    for (HWND hwnd : SnapshotFramelessWindows()) {
        bool restored = false;
        for (int attempt = 0; attempt < 3 && !restored; attempt++) {
            DWORD_PTR result;
            restored = SendMessageTimeoutW(hwnd, g_msgFrameless, kActionShow, 0,
                                           SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000,
                                           &result) != 0;
        }
        if (!restored && IsWindow(hwnd)) {
            Wh_Log(L"Waiting for %p to restore (%u)", hwnd, GetLastError());
            SendMessageW(hwnd, g_msgFrameless, kActionShow, 0);
        }
    }
}

void Wh_ModUninit() {
    Wh_Log(L"Uninit");

    ChangeWindowMessageFilter(g_msgFrameless, MSGFLT_REMOVE);
    ChangeWindowMessageFilter(g_msgDrag, MSGFLT_REMOVE);

    // UnhookWindowsHookEx does not wait for a hook procedure that is running
    // on another thread, and the resize watcher is a thread of ours, so the
    // image can only be let go once both are done with it.
    while (g_modRefCount > 0) {
        Sleep(100);
    }
    JoinModThreads();
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"SettingsChanged");

    bool wasHidingByDefault = g_settings.hideByDefault;
    LoadSettings();

    for (HWND hwnd : SnapshotFramelessWindows()) {
        ApplyCorners(hwnd);
    }
    // Which windows the border colors apply to is a setting of its own, so
    // this goes over all of them rather than over the frameless ones.
    RefreshCallWndProcHooks();
    RefreshBorderColors();
    DesktopSettingsChanged();

    if (g_settings.hideByDefault) {
        AutoHideExistingWindows();
    } else if (wasHidingByDefault) {
        RestoreAutoHiddenWindows();
    }
}
