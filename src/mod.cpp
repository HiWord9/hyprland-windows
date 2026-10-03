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

// How long the unload waits for what of ours is still running before it
// leaves the rest to ReleaseImageThread.
constexpr DWORD kUnloadWaitMs = 1000;
// What the last of it still runs after dropping its reference: the way out
// of its function.
constexpr DWORD kEpilogueGraceMs = 200;

// Lets go of the reference the image took on itself at unload, once nothing
// of ours is left running. FreeLibraryAndExitThread, because this thread runs
// in the image too and must not return into it once it is gone.
DWORD WINAPI ReleaseImageThread(LPVOID module) {
    while (g_modRefCount > 0) {
        Sleep(100);
    }
    JoinModThreads();
    Sleep(kEpilogueGraceMs);
    FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

// Has the window's own thread carry out a teardown request, and waits for it.
// A subclass procedure left behind in an unmapped image crashes its
// application the next time the window gets a message, so a window that does
// not answer is tried again, and then waited for: hanging the unload is bad,
// crashing the app is worse.
void SendTeardown(HWND hwnd, UINT message, WPARAM wParam) {
    for (int attempt = 0; attempt < 3; attempt++) {
        DWORD_PTR result;
        if (SendMessageTimeoutW(hwnd, message, wParam, 0,
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result)) {
            return;
        }
    }
    if (IsWindow(hwnd)) {
        Wh_Log(L"Waiting for %p to answer (%u)", hwnd, GetLastError());
        SendMessageW(hwnd, message, wParam, 0);
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

    // A drag has a subclass of ours on its window, and one whose loop never
    // ended keeps it. Like the title bars below, it comes off on the window's
    // own thread.
    for (HWND hwnd : SnapshotSnappedWindows()) {
        SendTeardown(hwnd, g_msgDrag, kDragUnsnap);
    }
    for (HWND hwnd : SnapshotFramelessWindows()) {
        SendTeardown(hwnd, g_msgFrameless, kActionShow);
    }
}

void Wh_ModUninit() {
    Wh_Log(L"Uninit");

    ChangeWindowMessageFilter(g_msgFrameless, MSGFLT_REMOVE);
    ChangeWindowMessageFilter(g_msgDrag, MSGFLT_REMOVE);

    // UnhookWindowsHookEx does not wait for a hook procedure that is running
    // on another thread, and threads of ours are on their way out, so the
    // image can only be let go once they are all done with it. That takes a
    // moment, as a rule.
    for (DWORD waited = 0; g_modRefCount > 0 && waited < kUnloadWaitMs;
         waited += 20) {
        Sleep(20);
    }
    if (g_modRefCount == 0) {
        JoinModThreads();
        return;
    }

    // Not always: a frameless window passes every message through a
    // procedure of ours, and the application may answer one with a modal
    // loop - a "Save changes?" prompt, a drag - which keeps that call on its
    // stack until the user is done. It returns into the image then, so the
    // image can't go before, and waiting for it here would hold the unload up
    // for as long as the prompt is open. The image keeps a reference of its
    // own instead, and lets go of it once nothing of ours is left running.
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&ReleaseImageThread),
                           &self)) {
        HANDLE thread =
            CreateThread(nullptr, 0, ReleaseImageThread, self, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
            Wh_Log(L"Still in use, the image goes once it is not");
            return;
        }
        FreeLibrary(self);
    }
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
