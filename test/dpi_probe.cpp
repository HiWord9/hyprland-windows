// Probe: which of these are virtualized for a DPI-unaware process, and which
// come back in physical pixels? Mixing the two is what makes a window land in
// the wrong place.
//
//   dpi_probe.exe            per-monitor aware
//   dpi_probe.exe unaware    left unaware
#include <windows.h>

#include <dwmapi.h>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    bool unaware = argc > 1 && !strcmp(argv[1], "unaware");
    if (!unaware) {
        SetProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    printf("== %s ==\n", unaware ? "DPI-unaware" : "per-monitor aware");

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DpiProbe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"DpiProbe", L"probe", WS_OVERLAPPEDWINDOW,
                                400, 300, 600, 400, nullptr, nullptr,
                                wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    Sleep(200);

    RECT window{};
    GetWindowRect(hwnd, &window);
    RECT frame{};
    DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame,
                          sizeof(frame));
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    POINT cursor;
    GetCursorPos(&cursor);
    UINT dpi = GetDpiForWindow(hwnd);

    printf("  GetWindowRect      (%ld,%ld)-(%ld,%ld)  %ldx%ld\n", window.left,
           window.top, window.right, window.bottom, window.right - window.left,
           window.bottom - window.top);
    printf("  DWM frame bounds   (%ld,%ld)-(%ld,%ld)  %ldx%ld\n", frame.left,
           frame.top, frame.right, frame.bottom, frame.right - frame.left,
           frame.bottom - frame.top);
    printf("  inset frame-window  left %ld top %ld right %ld bottom %ld\n",
           frame.left - window.left, frame.top - window.top,
           frame.right - window.right, frame.bottom - window.bottom);
    printf("  work area          (%ld,%ld)-(%ld,%ld)\n", mi.rcWork.left,
           mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom);
    printf("  monitor            (%ld,%ld)-(%ld,%ld)\n", mi.rcMonitor.left,
           mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom);
    printf("  cursor             (%ld,%ld)\n", cursor.x, cursor.y);
    printf("  GetDpiForWindow    %u\n", dpi);
    printf("  SM_CXSIZEFRAME %d + SM_CXPADDEDBORDER %d = %d\n",
           GetSystemMetrics(SM_CXSIZEFRAME), GetSystemMetrics(SM_CXPADDEDBORDER),
           GetSystemMetrics(SM_CXSIZEFRAME) +
               GetSystemMetrics(SM_CXPADDEDBORDER));
    printf("  for dpi %u: %d + %d = %d\n", dpi,
           GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi),
           GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi),
           GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
               GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi));

    DestroyWindow(hwnd);
    return 0;
}
