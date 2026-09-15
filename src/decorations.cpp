// DWM decorations (border color, corner preference) of frameless windows.
//
// Anything set here has to be written back as the system default when the
// setting is cleared, not merely skipped: the window keeps whatever was last
// written to it, so a border that is no longer configured would otherwise stay
// on screen until its title bar comes back.
#include "common.h"

void ApplyBorderColor(HWND hwnd, bool active) {
    COLORREF activeColor = g_settings.borderActive;
    COLORREF inactiveColor = g_settings.borderInactive;
    bool anyColor =
        activeColor != kColorUntouched || inactiveColor != kColorUntouched;
    if (!anyColor && !IsDwmTouched(hwnd)) {
        return;  // nothing to set, and nothing of ours to undo
    }

    COLORREF color = active ? activeColor : inactiveColor;
    if (color == kColorUntouched) {
        color = DWMWA_COLOR_DEFAULT;
    }
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color));
    if (anyColor) {
        MarkDwmTouched(hwnd);
    }
}

void ApplyCorners(HWND hwnd) {
    int corners = g_settings.corners;
    if (corners == DWMWCP_DEFAULT && !IsDwmTouched(hwnd)) {
        return;
    }
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners,
                          sizeof(corners));
    if (corners != DWMWCP_DEFAULT) {
        MarkDwmTouched(hwnd);
    }
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
