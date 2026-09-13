// Keeping the Start menu shut when the Win key is released after a
// Win + mouse drag.
#include "common.h"

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
// The hook is installed on the window thread that starts the drag (which has a
// message loop), left global so it sees the release wherever focus ends up,
// and removed as soon as it has masked one release.
std::atomic<bool> g_winMaskArmed{false};
std::mutex g_maskHooksMutex;
std::unordered_map<DWORD, HHOOK> g_maskHooks;  // thread id -> its LL hook

HINSTANCE ModuleInstance() {
    HMODULE module = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ModuleInstance), &module);
    return module;
}

void RemoveAllMaskHooks() {
    std::lock_guard<std::mutex> lock(g_maskHooksMutex);
    for (const auto& [threadId, hook] : g_maskHooks) {
        UnhookWindowsHookEx(hook);
    }
    g_maskHooks.clear();
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_winMaskArmed) {
        auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool keyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
        bool isWin = info->vkCode == VK_LWIN || info->vkCode == VK_RWIN;
        bool ours = info->dwExtraInfo == kInjectedMarker;
        if (keyUp && isWin && !ours) {
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

            RemoveAllMaskHooks();
            return 1;  // swallow the physical Win key-up
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// Arm the Start-menu suppression for the drag that is about to begin. Must run
// on the window's own (message-pumping) thread.
void ArmWinMask() {
    if (g_settings.dragModifier != DragModifier::Win) {
        return;
    }

    DWORD threadId = GetCurrentThreadId();
    std::lock_guard<std::mutex> lock(g_maskHooksMutex);
    if (!g_maskHooks.count(threadId)) {
        HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                       ModuleInstance(), 0);
        if (hook) {
            g_maskHooks[threadId] = hook;
        }
    }
    g_winMaskArmed = true;
}

// Tear the suppression down. A drag may have armed a hook that never saw its
// Win key-up, and the hook procedure must not outlive the DLL.
void ShutdownWinMask() {
    g_winMaskArmed = false;
    RemoveAllMaskHooks();
}
