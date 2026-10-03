// Win+Tab through the virtual desktops, the way Alt+Tab goes through windows:
// Win+Tab goes to the next desktop that has windows on it, Win+Shift+Tab to
// the previous one, round and round - at once, on every press. Task View, which
// Win+Tab opens otherwise, is on Win+Ctrl+Tab instead.
//
// It all happens in the shell, on a thread of the mod's own. Win+Tab is a
// hotkey the shell holds for Task View; the shell's thread it is posted to
// hands it over to this one instead (hooks.cpp), and this thread holds the
// other two. A hotkey reaches the shell whichever window is in front, one run
// as administrator included, and Windows keeps the Start menu shut after one.
//
// Switching at once takes the shell's own desktop manager, which Windows does
// not publish: its interface IDs and layout are known for the builds below
// (from VirtualDesktopAccessor) and change with new ones. Which one a build
// has is asked of the shell rather than worked out from the version. On a
// build none of them fits, Win+Tab is left to Windows.
//
// What the shell does after a switch is where the care goes. It brings forward
// the window last used on the desktop it went to, and whenever a window comes
// forward, it makes the desktop that window is on the current one. Both come a
// while later - a few hundred milliseconds, when the shell is busy, as it is
// right after a switch - and by then the next press may have gone on to
// another desktop: the shell then takes it back, and the desktops flicker back
// and forth. So while the presses come, the front is held by a window of the
// mod's own that belongs to no desktop: nothing is brought forward, and there
// is nothing to follow. The window on top of the desktop the presses ended on
// comes forward with the first key that is not for switching, or a while after
// the last one, and a press right after that waits for the shell to have taken
// it in.
//
// Task View and the Alt+Tab switcher are left alone while they are up: a
// Win+Tab then goes to Windows, which closes Task View for it. With no other
// desktop to go to, Win+Tab opens Task View, as in Windows.
#include "common.h"

// Set by the desktop thread once the shell has said it has no desktop manager
// the mod knows.
std::atomic<bool> g_desktopSwitchUnsupported;

////////////////////////////////////////////////////////////////////////////////
// The desktops

const CLSID CLSID_ImmersiveShell = {
    0xC2F03A33, 0x21F5, 0x47FA, {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};
const CLSID CLSID_VirtualDesktopManagerInternal = {
    0xC5E0CDCA, 0x7B6E, 0x41B2, {0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46, 0x7B}};

// The builds whose desktop manager has GetCurrentDesktop, GetDesktops and
// SwitchDesktop in slots 6, 7 and 9, and a desktop with GetId in slot 4. The
// Windows 11 21H2 and Server 2022 ones take a monitor as well, and are left
// to Windows.
struct DesktopManagerLayout {
    IID manager;
    IID desktop;
};
const DesktopManagerLayout kLayouts[] = {
    // Windows 11 24H2 and later
    {{0x53F5CA0B, 0x158F, 0x4124, {0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27}},
     {0x3F07F4BE, 0xB107, 0x441A, {0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C}}},
    // Windows 11 22H2 and 23H2
    {{0xA3175F2D, 0x239C, 0x4BD2, {0x8A, 0xA0, 0xEE, 0xBA, 0x8B, 0x0B, 0x13, 0x8E}},
     {0x3F07F4BE, 0xB107, 0x441A, {0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C}}},
    // Windows 10
    {{0xF31574D6, 0xB682, 0x4CDC, {0xBD, 0x56, 0x18, 0x27, 0x86, 0x0A, 0xBE, 0xC6}},
     {0xFF72FFDD, 0xBE7E, 0x43FC, {0x9C, 0x03, 0xAD, 0x81, 0x68, 0x1E, 0x88, 0xE4}}},
};
constexpr int kGetIdSlot = 4;
constexpr int kGetCurrentDesktopSlot = 6;
constexpr int kGetDesktopsSlot = 7;
constexpr int kSwitchDesktopSlot = 9;

template <typename Method>
Method Slot(void* object, int index) {
    return reinterpret_cast<Method>((*reinterpret_cast<void***>(object))[index]);
}

// Only touched on the desktop thread.
IVirtualDesktopManager* g_desktopManager;  // the public one
IUnknown* g_internalManager;
const DesktopManagerLayout* g_layout;

void ReleaseDesktopManagers() {
    if (g_internalManager) {
        g_internalManager->Release();
        g_internalManager = nullptr;
    }
    if (g_desktopManager) {
        g_desktopManager->Release();
        g_desktopManager = nullptr;
    }
    g_layout = nullptr;
}

bool ConnectDesktopManagers() {
    if (g_internalManager && g_desktopManager) {
        return true;
    }
    ReleaseDesktopManagers();
    CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL,
                     IID_PPV_ARGS(&g_desktopManager));
    IServiceProvider* shell = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ImmersiveShell, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&shell));
    if (FAILED(hr) || !g_desktopManager) {
        // The shell may still be starting up: tried again on the next press.
        Wh_Log(L"No immersive shell yet (0x%08X)", hr);
        ReleaseDesktopManagers();
        return false;
    }
    for (const DesktopManagerLayout& layout : kLayouts) {
        if (SUCCEEDED(shell->QueryService(CLSID_VirtualDesktopManagerInternal,
                                          layout.manager,
                                          (void**)&g_internalManager))) {
            g_layout = &layout;
            break;
        }
    }
    shell->Release();
    if (!g_layout) {
        Wh_Log(L"Unknown desktop manager: Win+Tab is left to Windows");
        g_desktopSwitchUnsupported = true;
        ReleaseDesktopManagers();
        return false;
    }
    return true;
}

GUID DesktopIdOf(IUnknown* desktop) {
    GUID id{};
    using GetId = HRESULT(STDMETHODCALLTYPE*)(void*, GUID*);
    Slot<GetId>(desktop, kGetIdSlot)(desktop, &id);
    return id;
}

struct Desktops {
    std::vector<GUID> ids;
    std::vector<IUnknown*> objects;
    GUID current{};
    ~Desktops() {
        for (IUnknown* object : objects) {
            object->Release();
        }
    }
};

bool ReadDesktops(Desktops* desktops) {
    using GetDesktops = HRESULT(STDMETHODCALLTYPE*)(void*, IObjectArray**);
    using GetCurrent = HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown**);
    IObjectArray* array = nullptr;
    if (FAILED(Slot<GetDesktops>(g_internalManager, kGetDesktopsSlot)(
            g_internalManager, &array))) {
        return false;
    }
    UINT count = 0;
    array->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        IUnknown* desktop = nullptr;
        if (SUCCEEDED(array->GetAt(i, g_layout->desktop, (void**)&desktop))) {
            desktops->ids.push_back(DesktopIdOf(desktop));
            desktops->objects.push_back(desktop);
        }
    }
    array->Release();
    IUnknown* current = nullptr;
    if (FAILED(Slot<GetCurrent>(g_internalManager, kGetCurrentDesktopSlot)(
            g_internalManager, &current)) ||
        !current) {
        return false;
    }
    desktops->current = DesktopIdOf(current);
    current->Release();
    return !desktops->ids.empty();
}

// The desktops with a window of their own on them.
BOOL CALLBACK CollectDesktopProc(HWND hwnd, LPARAM lParam) {
    auto* withWindows = reinterpret_cast<std::vector<GUID>*>(lParam);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) ||
        (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) ||
        !IsFrameWindow(hwnd)) {
        return TRUE;
    }
    // A window pinned to every desktop answers with an ID that is none of
    // theirs, and so counts for none of them.
    GUID desktop{};
    if (SUCCEEDED(g_desktopManager->GetWindowDesktopId(hwnd, &desktop)) &&
        desktop != GUID_NULL &&
        std::find(withWindows->begin(), withWindows->end(), desktop) ==
            withWindows->end()) {
        withWindows->push_back(desktop);
    }
    return TRUE;
}

// One of the shell's XAML views you work in - Task View, the Alt+Tab
// switcher - is up, or coming up. The shell makes the window when it opens one
// and destroys it once it has closed, so this holds for the closing animation
// as well; while it opens, the window is in front a moment before it is shown.
// The little one that shows a desktop's name after every switch is a view
// too, but one that never takes the focus, and it is not what this is about.
// They are looked for by class: EnumWindows passes over them altogether.
// Windows 10's are of a class of their own.
bool ShellViewUp() {
    HWND foreground = GetForegroundWindow();
    for (PCWSTR viewClass :
         {L"XamlExplorerHostIslandWindow", L"MultitaskingViewFrame"}) {
        HWND view = nullptr;
        while ((view = FindWindowExW(nullptr, view, viewClass, nullptr))) {
            if (view == foreground ||
                (IsWindowVisible(view) &&
                 !(GetWindowLongPtrW(view, GWL_EXSTYLE) & WS_EX_NOACTIVATE))) {
                return true;
            }
        }
    }
    return false;
}

////////////////////////////////////////////////////////////////////////////////
// The desktop thread, in the shell

// The window that holds the front while the presses come: a tool window,
// which no desktop has for its own, out of sight.
//
// The window on top of the desktop the presses ended on comes forward with the
// first key that is not for switching - before that key gets anywhere, so it
// goes to that window - or by itself once Win is up and this long has gone by
// since the last switch. Not sooner: a window that comes forward between two
// presses is one the shell may take the desktops back to.
constexpr DWORD kBringForwardAfterMs = 1000;
constexpr UINT kBringForwardPollMs = 15;
constexpr UINT_PTR kBringForwardTimer = 1;
// And a switch waits for the front to have stayed put this long: the shell
// takes in every window that comes forward a while later, and a switch made
// before it has is undone by it. While the mod holds the front, it stays put.
constexpr DWORD kQuietFrontMs = 200;
// A window that has just lost the front to the holder can take it back - File
// Explorer's do, at once, and not only theirs - so the holder is watched this
// long before a switch.
constexpr DWORD kHoldWatchMs = 50;
constexpr int kHoldAttempts = 3;

constexpr UINT kDesktopStepMessage = WM_APP + 2;      // wParam: +1 or -1
constexpr UINT kDesktopSettingsMessage = WM_APP + 3;  // the settings changed
constexpr int kPreviousHotkey = 1;                    // Win+Shift+Tab
constexpr int kTaskViewHotkey = 2;                    // Win+Ctrl+Tab

std::mutex g_desktopThreadMutex;
std::atomic<DWORD> g_desktopThreadId;
constexpr DWORD kDesktopThreadStartWaitMs = 2000;

// Made and destroyed on the desktop thread, and looked at by the keyboard
// thread as well.
std::atomic<HWND> g_frontHolder;

// Only touched on the desktop thread.
bool g_hotkeysRegistered;
bool g_holdingFront;  // the holder has the front on purpose
bool g_bringForwardPending;
DWORD g_switchedAt;
HWINEVENTHOOK g_frontChangeHook;
DWORD g_frontChangedAt;  // last time a window other than the holder came forward

// The window the shell would bring forward on the current desktop: the
// topmost one there that takes the focus.
BOOL CALLBACK FindTopWindowProc(HWND hwnd, LPARAM lParam) {
    DWORD cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (hwnd == g_frontHolder || cloaked || !IsWindowVisible(hwnd) ||
        IsIconic(hwnd) || !IsWindowEnabled(hwnd) ||
        (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) &
         (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) ||
        !IsFrameWindow(hwnd)) {
        return TRUE;
    }
    *reinterpret_cast<HWND*>(lParam) = hwnd;
    return FALSE;
}

// Once a window has the front again, the holder goes to the bottom: when the
// window in front closes, Windows hands the front to the one under it, and
// that is not to be the holder.
void StopBringingForward() {
    g_holdingFront = false;
    g_bringForwardPending = false;
    if (HWND holder = g_frontHolder) {
        KillTimer(holder, kBringForwardTimer);
        if (GetForegroundWindow() != holder) {
            SetWindowPos(holder, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
}

void BringTopWindowForward() {
    HWND top = nullptr;
    EnumWindows(FindTopWindowProc, (LPARAM)&top);
    // A desktop with no window of that kind has its wallpaper in front.
    SetForegroundWindow(top ? top : GetShellWindow());
}

void BringForward() {
    BringTopWindowForward();
    StopBringingForward();
}

// On the timer, while a window is to come forward.
void OnBringForwardTimer() {
    if (!g_bringForwardPending || GetForegroundWindow() != g_frontHolder) {
        // Someone has brought something forward meanwhile: the user, with a
        // click. That is the window for this desktop, then.
        StopBringingForward();
        return;
    }
    if (WinKeyDown() || GetTickCount() - g_switchedAt < kBringForwardAfterMs) {
        return;
    }
    BringForward();
}

void CALLBACK OnFrontChanged(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD,
                             DWORD) {
    if (hwnd != g_frontHolder) {
        g_frontChangedAt = GetTickCount();
    } else if (!g_holdingFront) {
        // The holder came forward by itself - handed the front of a window
        // that closed: it goes on to the window that should have it.
        BringForward();
    }
}

// Waits, hearing of the front's changes meanwhile - the event is delivered by
// looking at the queue - until `done` says so or `ms` have gone by.
template <typename Done>
void WaitHearingFront(DWORD ms, Done done) {
    DWORD start = GetTickCount();
    for (;;) {
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        DWORD spent = GetTickCount() - start;
        if (done() || spent >= ms) {
            return;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, ms - spent, QS_ALLINPUT);
    }
}

bool FrontQuiet() {
    return !g_frontChangedAt || GetTickCount() - g_frontChangedAt >= kQuietFrontMs;
}

// The front held by the holder, and quiet: what a switch needs.
bool HoldFront() {
    if (!g_frontHolder || !IsWindow(g_frontHolder)) {
        g_frontHolder = CreateWindowExW(
            WS_EX_TOOLWINDOW, L"STATIC", nullptr, WS_POPUP | WS_VISIBLE, -32000,
            -32000, 1, 1, nullptr, nullptr, nullptr, nullptr);
    }
    g_holdingFront = true;
    for (int attempt = 0; attempt < kHoldAttempts; attempt++) {
        WaitHearingFront(kQuietFrontMs, FrontQuiet);
        if (GetForegroundWindow() == g_frontHolder) {
            return true;
        }
        if (!g_frontHolder || !SetForegroundWindow(g_frontHolder)) {
            return false;
        }
        WaitHearingFront(kHoldWatchMs, [] {
            return GetForegroundWindow() != g_frontHolder;
        });
        if (GetForegroundWindow() == g_frontHolder) {
            return true;
        }
    }
    return false;
}

enum class Target { kNone, kNowhere, kFound };

// The desktop `steps` stops round from the current one, among those that have
// windows on them (and the current one, which is where the counting starts).
Target FindTarget(int steps, Desktops* desktops, int* target) {
    if (!ReadDesktops(desktops)) {
        // The shell may have been restarted under us: connect again.
        ReleaseDesktopManagers();
        if (!ConnectDesktopManagers() || !ReadDesktops(desktops)) {
            return Target::kNone;
        }
    }
    std::vector<GUID> withWindows;
    EnumWindows(CollectDesktopProc, (LPARAM)&withWindows);

    std::vector<int> stops;
    int here = -1;
    for (int i = 0; i < (int)desktops->ids.size(); i++) {
        bool current = desktops->ids[i] == desktops->current;
        if (current || std::find(withWindows.begin(), withWindows.end(),
                                 desktops->ids[i]) != withWindows.end()) {
            if (current) {
                here = (int)stops.size();
            }
            stops.push_back(i);
        }
    }
    if (here < 0) {
        return Target::kNone;
    }
    if (stops.size() < 2) {
        return Target::kNowhere;
    }
    int count = (int)stops.size();
    *target = stops[((here + steps) % count + count) % count];
    return *target != stops[here] ? Target::kFound : Target::kNone;
}

// Task View, opened - or closed, when it is up - the way its shortcut does it.
void ToggleTaskView() {
    ShellExecuteW(nullptr, L"open",
                  L"shell:::{3080F90E-D7AD-11D9-BD98-0000947B0257}", nullptr,
                  nullptr, SW_SHOWNORMAL);
}

void StepDesktop(int steps) {
    if (ShellViewUp() || !ConnectDesktopManagers()) {
        return;
    }
    Desktops desktops;
    int target = -1;
    Target found = FindTarget(steps, &desktops, &target);
    if (found == Target::kNowhere) {
        // No other desktop has windows: Win+Tab is what it is in Windows.
        ToggleTaskView();
        return;
    }
    if (found != Target::kFound) {
        return;
    }
    if (!HoldFront()) {
        Wh_Log(L"Could not hold the front (%u)", GetLastError());
    }
    using Switch = HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*);
    HRESULT hr = Slot<Switch>(g_internalManager, kSwitchDesktopSlot)(
        g_internalManager, desktops.objects[target]);
    if (FAILED(hr)) {
        Wh_Log(L"SwitchDesktop failed (0x%08X)", hr);
        ReleaseDesktopManagers();
        BringForward();
        return;
    }
    g_switchedAt = GetTickCount();
    g_bringForwardPending = true;
    SetTimer(g_frontHolder, kBringForwardTimer, kBringForwardPollMs, nullptr);
}

// The two hotkeys of the mod's own, held while Win+Tab is the mod's.
void UpdateHotkeys() {
    bool wanted = g_settings.desktopWinTab && !g_desktopSwitchUnsupported &&
                  !g_uninitializing;
    if (wanted && !g_hotkeysRegistered) {
        if (!RegisterHotKey(nullptr, kPreviousHotkey,
                            MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, VK_TAB) ||
            !RegisterHotKey(nullptr, kTaskViewHotkey,
                            MOD_WIN | MOD_CONTROL | MOD_NOREPEAT, VK_TAB)) {
            Wh_Log(L"RegisterHotKey failed (%u)", GetLastError());
        }
        g_hotkeysRegistered = true;
    } else if (!wanted && g_hotkeysRegistered) {
        UnregisterHotKey(nullptr, kPreviousHotkey);
        UnregisterHotKey(nullptr, kTaskViewHotkey);
        g_hotkeysRegistered = false;
    }
}

DWORD WINAPI DesktopThread(LPVOID ready) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // the queue, before anyone posts
    UpdateHotkeys();
    // Only the front changing, delivered to this thread's queue.
    g_frontChangeHook =
        SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                        OnFrontChanged, 0, 0, WINEVENT_OUTOFCONTEXT);
    SetEvent(static_cast<HANDLE>(ready));
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_uninitializing) {
            continue;
        }
        if (msg.message == kDesktopStepMessage) {
            StepDesktop((int)msg.wParam);
        } else if (msg.message == WM_HOTKEY && !msg.hwnd) {
            if (msg.wParam == kPreviousHotkey) {
                StepDesktop(-1);
            } else if (msg.wParam == kTaskViewHotkey) {
                ToggleTaskView();
            }
        } else if (msg.message == kDesktopSettingsMessage) {
            UpdateHotkeys();
        } else if (msg.message == WM_TIMER && msg.hwnd == g_frontHolder &&
                   msg.wParam == kBringForwardTimer) {
            OnBringForwardTimer();
        } else {
            DispatchMessageW(&msg);
        }
        if (g_desktopSwitchUnsupported) {
            UpdateHotkeys();
        }
    }
    UnregisterHotKey(nullptr, kPreviousHotkey);
    UnregisterHotKey(nullptr, kTaskViewHotkey);
    g_hotkeysRegistered = false;
    if (g_frontChangeHook) {
        UnhookWinEvent(g_frontChangeHook);
        g_frontChangeHook = nullptr;
    }
    if (g_frontHolder) {
        if (GetForegroundWindow() == g_frontHolder) {
            BringForward();
        }
        DestroyWindow(g_frontHolder);
        g_frontHolder = nullptr;
    }
    ReleaseDesktopManagers();
    CoUninitialize();
    g_desktopThreadId = 0;
    g_modRefCount--;  // the last thing this thread does in the mod's image
    return 0;
}

// Called from Wh_ModAfterInit in the shell.
void StartDesktopThread() {
    std::lock_guard<std::mutex> lock(g_desktopThreadMutex);
    if (g_desktopThreadId || g_uninitializing) {
        return;
    }
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) {
        return;
    }
    DWORD threadId = 0;
    if (StartModThread(DesktopThread, ready, &threadId)) {
        // Published once the thread has its queue: a step posted before that
        // would be lost.
        WaitForSingleObject(ready, kDesktopThreadStartWaitMs);
        g_desktopThreadId = threadId;
    }
    CloseHandle(ready);
}

void DesktopSettingsChanged() {
    if (DWORD threadId = g_desktopThreadId) {
        PostThreadMessageW(threadId, kDesktopSettingsMessage, 0, 0);
    }
}

void ShutdownDesktopThread() {
    if (DWORD threadId = g_desktopThreadId) {
        PostThreadMessageW(threadId, WM_QUIT, 0, 0);
    }
}

// Whether there is more than the one desktop. Asked of the registry, where
// the shell keeps their IDs, rather than of the shell, which the thread asking
// - one of the shell's own - must not wait on.
bool SeveralDesktops() {
    DWORD size = 0;
    return RegGetValueW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer"
                        L"\\VirtualDesktops",
                        L"VirtualDesktopIDs", RRF_RT_REG_BINARY, nullptr, nullptr,
                        &size) == ERROR_SUCCESS &&
           size > sizeof(GUID);
}

// Called for every WM_HOTKEY a thread of this process retrieves: the shell's
// Win+Tab, posted to one of its own, goes to the desktop thread instead - but
// with only the one desktop it is left to the shell, as Task View.
// Returns true if it was taken.
bool HandleDesktopHotkey(const MSG* msg) {
    DWORD threadId = g_desktopThreadId;
    if (!threadId || LOWORD(msg->lParam) != MOD_WIN ||
        HIWORD(msg->lParam) != VK_TAB || !g_settings.desktopWinTab ||
        g_desktopSwitchUnsupported || ShellViewUp() || !SeveralDesktops()) {
        return false;
    }
    return PostThreadMessageW(threadId, kDesktopStepMessage, 1, 0) != FALSE;
}

// Called by the shell's keyboard thread for every key that goes down. While
// the front is held, a key that is not part of a switch brings the window on
// top of the desktop forward at once - before the key gets anywhere, so that it
// goes to that window rather than to the holder.
void BringDesktopForwardForKey(UINT vk) {
    HWND holder = g_frontHolder;
    if (!holder || GetForegroundWindow() != holder) {
        return;
    }
    switch (vk) {
        case VK_LWIN:
        case VK_RWIN:
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
            return;  // held for a chord - the next switch, perhaps
        case VK_TAB:
            if (WinKeyDown()) {
                return;  // the next switch itself
            }
            break;
    }
    BringTopWindowForward();
}
