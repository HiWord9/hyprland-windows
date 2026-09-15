// Lists visible top-level windows with the bits that decide whether the mod
// will touch them, so a probe target can be picked deliberately.
#include <windows.h>

#include <cstdio>

static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM) {
    char title[200] = "";
    char cls[100] = "";
    if (!IsWindowVisible(hwnd) || !GetWindowTextA(hwnd, title, 200)) {
        return TRUE;
    }
    GetClassNameA(hwnd, cls, 100);
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    RECT rc;
    GetWindowRect(hwnd, &rc);
    printf("pid %-7lu %-28s thick=%d cap=%d %4ldx%-4ld %s\n", pid, cls,
           (style & WS_THICKFRAME) != 0, (style & WS_CAPTION) == WS_CAPTION,
           rc.right - rc.left, rc.bottom - rc.top, title);
    return TRUE;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    EnumWindows(EnumProc, 0);
    return 0;
}
