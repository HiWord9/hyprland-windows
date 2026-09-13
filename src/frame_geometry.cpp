// Non-client layout of a window whose title bar is hidden: where the
// client area starts and which edges still resize.
#include "common.h"

// A maximized window is positioned so that its resize frame lies outside the
// monitor. Without a caption the client area would start in that invisible
// strip, so push it down to where the monitor's work area begins.
int MaximizedTopInset(HWND hwnd, const RECT& proposed) {
    if (!(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_MAXIMIZE)) {
        return 0;
    }

    int handle = ResizeHandleHeight(hwnd);
    HMONITOR monitor = MonitorFromRect(&proposed, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        return handle;
    }
    return (int)std::clamp<LONG>(info.rcWork.top - proposed.top, 0, handle);
}

LRESULT OnNcCalcSize(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    RECT* rc = wParam ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0]
                      : reinterpret_cast<RECT*>(lParam);
    const RECT proposed = *rc;

    // Let the app / DefWindowProc lay out the frame as usual, then take the
    // whole top part (frame + caption) away. Left, right and bottom frames
    // stay, so DWM keeps drawing the border, the shadow and rounded corners.
    LRESULT result = DefSubclassProc(hwnd, WM_NCCALCSIZE, wParam, lParam);

    LONG top = proposed.top + MaximizedTopInset(hwnd, proposed);
    if (top < rc->top && top < rc->bottom) {
        rc->top = top;
    }

    return result;
}

// With the caption gone, DefWindowProc reports HTCLIENT for the top edge.
// Optionally turn the topmost few pixels back into a resize handle.
LRESULT AdjustHitTest(HWND hwnd, LRESULT hit, LPARAM lParam) {
    if (!g_settings.topEdgeResize) {
        return hit;
    }
    if (hit != HTCLIENT && hit != HTLEFT && hit != HTRIGHT) {
        return hit;
    }

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (!(style & WS_THICKFRAME) || (style & WS_MAXIMIZE)) {
        return hit;
    }

    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) {
        return hit;
    }

    int handle = ResizeHandleHeight(hwnd);
    if (pt.y < rc.top || pt.y >= rc.top + handle) {
        return hit;
    }
    if (hit == HTLEFT || pt.x < rc.left + 2 * handle) {
        return HTTOPLEFT;
    }
    if (hit == HTRIGHT || pt.x >= rc.right - 2 * handle) {
        return HTTOPRIGHT;
    }
    return HTTOP;
}
