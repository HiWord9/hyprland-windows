// Mod lifecycle: what Windhawk calls to load, reconfigure and unload us.
#include "common.h"

BOOL Wh_ModInit() {
    Wh_Log(L"Init");

    g_msgFrameless = RegisterWindowMessageW(L"HyprlandWindows_" WH_MOD_ID);
    if (!g_msgFrameless) {
        Wh_Log(L"RegisterWindowMessage failed");
        return FALSE;
    }

    LoadSettings();

    WindhawkUtils::SetFunctionHook(GetMessageW, GetMessageW_Hook,
                                   &GetMessageW_Original);
    WindhawkUtils::SetFunctionHook(GetMessageA, GetMessageA_Hook,
                                   &GetMessageA_Original);
    WindhawkUtils::SetFunctionHook(PeekMessageW, PeekMessageW_Hook,
                                   &PeekMessageW_Original);
    WindhawkUtils::SetFunctionHook(PeekMessageA, PeekMessageA_Hook,
                                   &PeekMessageA_Original);
    WindhawkUtils::SetFunctionHook(CreateWindowExW, CreateWindowExW_Hook,
                                   &CreateWindowExW_Original);
    WindhawkUtils::SetFunctionHook(CreateWindowExA, CreateWindowExA_Hook,
                                   &CreateWindowExA_Original);

    return TRUE;
}

void Wh_ModAfterInit() {
    if (g_settings.hideByDefault) {
        AutoHideExistingWindows();
    }
}

void Wh_ModBeforeUninit() {
    Wh_Log(L"BeforeUninit: restoring title bars");

    ShutdownWinMask();

    // Restore synchronously on each window's thread so that no subclass
    // procedure is left behind once the DLL is gone. A hung window can't
    // block the unload forever thanks to the timeout.
    for (HWND hwnd : SnapshotFramelessWindows()) {
        DWORD_PTR result;
        if (!SendMessageTimeoutW(hwnd, g_msgFrameless, kActionShow, 0,
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000,
                                 &result)) {
            Wh_Log(L"Could not restore %p (%u)", hwnd, GetLastError());
        }
    }
}

void Wh_ModUninit() {
    Wh_Log(L"Uninit");
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"SettingsChanged");

    LoadSettings();

    for (HWND hwnd : SnapshotFramelessWindows()) {
        ApplyDwmAttributes(hwnd);
    }

    if (g_settings.hideByDefault) {
        AutoHideExistingWindows();
    }
}
