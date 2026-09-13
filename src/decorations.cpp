// DWM decorations (border color, corner preference) of frameless windows.
#include "common.h"

void ApplyBorderColor(HWND hwnd, bool active) {
    COLORREF activeColor = g_settings.borderActive;
    COLORREF inactiveColor = g_settings.borderInactive;
    if (activeColor == kColorUntouched && inactiveColor == kColorUntouched) {
        return;
    }

    COLORREF color = active ? activeColor : inactiveColor;
    if (color == kColorUntouched) {
        color = DWMWA_COLOR_DEFAULT;
    }
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color));
    MarkDwmTouched(hwnd);
}

void ApplyCorners(HWND hwnd) {
    int corners = g_settings.corners;
    if (corners == DWMWCP_DEFAULT) {
        return;
    }
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners,
                          sizeof(corners));
    MarkDwmTouched(hwnd);
}

void ApplyDwmAttributes(HWND hwnd) {
    ApplyBorderColor(hwnd, GetForegroundWindow() == hwnd);
    ApplyCorners(hwnd);
}

void RestoreDwmAttributes(HWND hwnd) {
    COLORREF color = DWMWA_COLOR_DEFAULT;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color));
    int corners = DWMWCP_DEFAULT;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners,
                          sizeof(corners));
}
