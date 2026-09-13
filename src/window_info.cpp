// Deciding which windows the mod may touch, and basic window metrics.
#include "common.h"

bool IsExcludedClass(HWND hwnd) {
    WCHAR cls[64];
    if (!GetClassNameW(hwnd, cls, ARRAYSIZE(cls))) {
        return false;
    }

    static constexpr PCWSTR kExcluded[] = {
        L"Progman",                      // desktop
        L"WorkerW",                      // desktop (wallpaper host)
        L"Shell_TrayWnd",                // taskbar
        L"Shell_SecondaryTrayWnd",       // taskbar on other monitors
        L"#32768",                       // popup menus
        L"tooltips_class32",             // tooltips
        L"Windows.UI.Core.CoreWindow",   // Start menu, search, etc.
        L"Xaml_WindowedPopupClass",      // XAML popups
        L"SysShadow",                    // menu shadows
    };
    for (PCWSTR excluded : kExcluded) {
        if (_wcsicmp(cls, excluded) == 0) {
            return true;
        }
    }
    return false;
}

bool HasFrameStyles(LONG_PTR style) {
    return (style & WS_CAPTION) == WS_CAPTION || (style & WS_THICKFRAME);
}

// Top-level windows with a real frame: those may be moved/resized with the
// modifier key and may have their title bar hidden.
bool IsFrameWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (style & WS_CHILD) {
        return false;
    }
    if (!HasFrameStyles(style)) {
        return false;
    }
    return !IsExcludedClass(hwnd);
}

// Stricter rule for "hide by default": only ordinary windows with a real
// title bar, no tool windows and no non-activatable helper windows.
bool IsAutoHideCandidate(HWND hwnd) {
    if (!IsFrameWindow(hwnd)) {
        return false;
    }
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) != WS_CAPTION) {
        return false;
    }
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) {
        return false;
    }
    return true;
}
UINT WindowDpi(HWND hwnd) {
    UINT dpi = GetDpiForWindow(hwnd);
    return dpi ? dpi : 96;
}

// Thickness of the (invisible) resize frame around a window.
int ResizeHandleHeight(HWND hwnd) {
    UINT dpi = WindowDpi(hwnd);
    return GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
           GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}
