// Hiding and restoring a window's title bar, and the per-window
// bookkeeping that goes with it. Runs on the window's own thread.
#include "common.h"

UINT g_msgFrameless;  // RegisterWindowMessage, set in Wh_ModInit

struct FramelessState {
    bool dwmTouched = false;
    // The title bar was hidden by "hide by default", not by the hotkey, so
    // turning that setting off brings it back.
    bool autoHidden = false;
    // Style bits removed from the window (KeepMenu mode), restored later.
    DWORD removedStyle = 0;
    // Hide mode: the menu bar is drawn for now, while the keyboard is in it.
    bool menuBarShown = false;
};
std::mutex g_windowsMutex;
std::unordered_map<HWND, FramelessState> g_windows;

bool IsFrameless(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_windowsMutex);
    return g_windows.count(hwnd) != 0;
}

void MarkDwmTouched(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_windowsMutex);
    auto it = g_windows.find(hwnd);
    if (it != g_windows.end()) {
        it->second.dwmTouched = true;
    }
}

bool IsDwmTouched(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_windowsMutex);
    auto it = g_windows.find(hwnd);
    return it != g_windows.end() && it->second.dwmTouched;
}


std::vector<HWND> SnapshotFramelessWindows() {
    std::lock_guard<std::mutex> lock(g_windowsMutex);
    std::vector<HWND> result;
    result.reserve(g_windows.size());
    for (const auto& [hwnd, state] : g_windows) {
        result.push_back(hwnd);
    }
    return result;
}

std::vector<HWND> SnapshotAutoHiddenWindows() {
    std::lock_guard<std::mutex> lock(g_windowsMutex);
    std::vector<HWND> result;
    for (const auto& [hwnd, state] : g_windows) {
        if (state.autoHidden) {
            result.push_back(hwnd);
        }
    }
    return result;
}
void RefreshFrame(HWND hwnd) {
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void HandleFramelessRequest(HWND hwnd, WPARAM action);

// A classic menu bar stays where the system lays it out, below where the
// caption was, which is now the client area. The system draws it whenever it
// sees fit - the frame painted on a resize or a focus change, Alt showing its
// underlines - over whatever the window has there. So it is drawn only while
// the keyboard is in it (Alt let go of, F10, Alt and a letter), and the window
// paints over it again once it is done. Of the frame, nothing else is lost:
// DWM draws what is left of it.

// Undocumented: what a themed window is sent to draw its menu bar - before
// the frame is painted, with the device context it is painted into; the bar
// itself; each item on it. Answering TRUE to the latter two says it was drawn.
constexpr UINT WM_UAHDRAWMENU = 0x0091;
constexpr UINT WM_UAHDRAWMENUITEM = 0x0092;
constexpr UINT WM_UAHINITMENU = 0x0093;
struct UAHMENU {
    HMENU menu;
    HDC dc;
    DWORD flags;
};

bool IsMenuBarShown(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_windowsMutex);
    auto it = g_windows.find(hwnd);
    return it != g_windows.end() && it->second.menuBarShown;
}

// Where the bar is drawn, in screen pixels; with a little room around it for
// the lines drawn along its edges, for keeping drawing off it.
bool MenuBarRect(HWND hwnd, RECT* rect, bool withEdges) {
    MENUBARINFO info{sizeof(info)};
    if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &info) ||
        IsRectEmpty(&info.rcBar)) {
        return false;
    }
    *rect = info.rcBar;
    if (withEdges) {
        InflateRect(rect, 0,
                    MulDiv(3, GetDpiForWindow(hwnd), USER_DEFAULT_SCREEN_DPI));
    }
    return true;
}

// The bar is cut out of the device context the frame is about to be painted
// into. That is how the background of a bar the app draws itself is kept out
// as well: the system fills that in, unasked.
void KeepFramePaintingOffMenuBar(HWND hwnd, HDC dc) {
    RECT strip, window;
    if (!dc || WindowFromDC(dc) != hwnd || !MenuBarRect(hwnd, &strip, true) ||
        !GetWindowRect(hwnd, &window)) {
        return;  // one that is only measured with, say
    }
    OffsetRect(&strip, -window.left, -window.top);
    ExcludeClipRect(dc, strip.left, strip.top, strip.right, strip.bottom);
}

void ShowHiddenMenuBar(HWND hwnd, bool show) {
    {
        std::lock_guard<std::mutex> lock(g_windowsMutex);
        auto it = g_windows.find(hwnd);
        if (it == g_windows.end() || it->second.menuBarShown == show) {
            return;
        }
        it->second.menuBarShown = show;
    }
    RECT bar;
    if (!MenuBarRect(hwnd, &bar, !show)) {
        return;
    }
    if (show) {
        // Painting the frame would not reach it: what is painted of the frame
        // is what is outside the client area. So the frame is printed into
        // the window instead, onto just the bar.
        RECT window;
        GetWindowRect(hwnd, &window);
        OffsetRect(&bar, -window.left, -window.top);
        if (HDC dc = GetWindowDC(hwnd)) {
            IntersectClipRect(dc, bar.left, bar.top, bar.right, bar.bottom);
            DefWindowProcW(hwnd, WM_PRINT, reinterpret_cast<WPARAM>(dc),
                           PRF_NONCLIENT);
            ReleaseDC(hwnd, dc);
        }
        return;
    }
    MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&bar), 2);
    RedrawWindow(hwnd, &bar, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

LRESULT CALLBACK FramelessSubclassProc(HWND hwnd,
                                       UINT uMsg,
                                       WPARAM wParam,
                                       LPARAM lParam,
                                       UINT_PTR uIdSubclass,
                                       DWORD_PTR dwRefData) {
    ModRef ref;  // the image must not go away under this procedure

    bool keepMenu = (dwRefData & kRefKeepMenu) != 0;

    switch (uMsg) {
        case WM_NCCALCSIZE:
            if (keepMenu) {
                break;  // WS_CAPTION was removed instead; default layout
            }
            return OnNcCalcSize(hwnd, wParam, lParam);

        case WM_UAHINITMENU:
            if (!keepMenu && !IsMenuBarShown(hwnd) && lParam) {
                KeepFramePaintingOffMenuBar(
                    hwnd, reinterpret_cast<UAHMENU*>(lParam)->dc);
            }
            break;

        case WM_NCPAINT:
        case WM_NCACTIVATE:
            // Past the app: what an app paints of its frame itself is what it
            // adds to its bar (a dark one's edges, for one), and nothing else
            // of the frame is left to paint. DefWindowProc still has to: DWM
            // only draws the frame of a window it sees painted the usual way.
            if (!keepMenu && GetMenu(hwnd) && !IsMenuBarShown(hwnd)) {
                return DefWindowProcW(hwnd, uMsg, wParam, lParam);
            }
            break;

        case WM_UAHDRAWMENU:
        case WM_UAHDRAWMENUITEM:
            if (!keepMenu && !IsMenuBarShown(hwnd)) {
                return TRUE;
            }
            break;

        case WM_DRAWITEM:
            // Items of an owner-drawn bar, which the app draws itself.
            if (!keepMenu && !IsMenuBarShown(hwnd)) {
                auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
                if (item->CtlType == ODT_MENU &&
                    reinterpret_cast<HMENU>(item->hwndItem) == GetMenu(hwnd)) {
                    return TRUE;
                }
            }
            break;

        case WM_MENUSELECT:
            // One of the bar's own items, not the window menu Alt+Space opens
            // (which leaves the keyboard on the bar's window menu icon on its
            // way out) or a context menu.
            if (!keepMenu && lParam &&
                reinterpret_cast<HMENU>(lParam) == GetMenu(hwnd) &&
                HIWORD(wParam) != 0xFFFF && !(HIWORD(wParam) & MF_SYSMENU)) {
                ShowHiddenMenuBar(hwnd, true);
            }
            break;

        case WM_EXITMENULOOP:
            if (!keepMenu) {
                ShowHiddenMenuBar(hwnd, false);
            }
            break;

        case WM_NCHITTEST: {
            LRESULT hit = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            return keepMenu ? hit : AdjustHitTest(hwnd, hit, lParam);
        }

        case WM_NCDESTROY: {
            RemoveWindowSubclass(hwnd, FramelessSubclassProc, uIdSubclass);
            std::lock_guard<std::mutex> lock(g_windowsMutex);
            g_windows.erase(hwnd);
            break;
        }

        default:
            if (uMsg == g_msgFrameless) {
                // Sent (not posted) requests, e.g. the restore on unload.
                HandleFramelessRequest(hwnd, wParam);
                return 0;
            }
            break;
    }

    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

bool MakeFrameless(HWND hwnd, bool autoHidden) {
    if (!IsFrameWindow(hwnd)) {
        Wh_Log(L"Window %p is not eligible", hwnd);
        return false;
    }

    // A classic menu bar lives in the non-client area right below the
    // caption, laid out by the system from the title bar metrics. It can't
    // be kept there once the caption is gone, so either hide it along with
    // the title bar or fall back to plain WS_CAPTION removal.
    DWORD removedStyle = 0;
    if (GetMenu(hwnd)) {
        switch (g_settings.menuBarMode) {
            case MenuBarMode::Skip:
                Wh_Log(L"Window %p has a menu bar, skipping", hwnd);
                return false;
            case MenuBarMode::KeepMenu:
                // WS_CAPTION is WS_BORDER | WS_DLGFRAME; dropping only
                // WS_DLGFRAME removes the title bar but keeps the frame
                // metrics (and thus the client edges) unchanged.
                removedStyle =
                    (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_DLGFRAME;
                break;
            case MenuBarMode::Hide:
                break;
        }
    }

    if (!SetWindowSubclass(hwnd, FramelessSubclassProc, kSubclassId,
                           removedStyle ? kRefKeepMenu : 0)) {
        Wh_Log(L"SetWindowSubclass failed for %p", hwnd);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_windowsMutex);
        FramelessState state;
        state.autoHidden = autoHidden;
        state.removedStyle = removedStyle;
        g_windows[hwnd] = state;
    }

    if (removedStyle) {
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~(LONG_PTR)removedStyle);
    }

    ApplyDwmAttributes(hwnd);
    RefreshFrame(hwnd);
    Wh_Log(L"Title bar hidden for %p", hwnd);
    return true;
}

void RestoreFrame(HWND hwnd) {
    // Before the bookkeeping goes away: a fade still running would write a
    // color over the default this is about to put back.
    CancelBorderFade(hwnd);

    FramelessState state;
    {
        std::lock_guard<std::mutex> lock(g_windowsMutex);
        auto it = g_windows.find(hwnd);
        if (it == g_windows.end()) {
            return;
        }
        state = it->second;
        g_windows.erase(it);
    }

    RemoveWindowSubclass(hwnd, FramelessSubclassProc, kSubclassId);
    if (state.removedStyle) {
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        SetWindowLongPtrW(hwnd, GWL_STYLE, style | state.removedStyle);
    }
    if (state.dwmTouched) {
        RestoreDwmAttributes(hwnd);
    }
    RefreshBorderColor(hwnd);
    RefreshFrame(hwnd);
    Wh_Log(L"Title bar restored for %p", hwnd);
}

void HandleFramelessRequest(HWND hwnd, WPARAM action) {
    bool autoHide = action == kActionAutoHide;
    if (autoHide) {
        // Re-check now that the window is done being created. A window that
        // never becomes visible is left alone: toolkits create throwaway
        // top-level windows during start-up (to probe for a pixel format, for
        // instance) and touching those can break the app's initialization.
        if (!IsWindowVisible(hwnd) || !IsAutoHideCandidate(hwnd)) {
            return;
        }
        action = kActionHide;
    }

    bool frameless = IsFrameless(hwnd);
    if (action == kActionToggle) {
        action = frameless ? kActionShow : kActionHide;
    }
    if (action == kActionHide && !frameless) {
        if (g_uninitializing) {
            return;  // the title bars are being put back, not taken away
        }
        MakeFrameless(hwnd, autoHide);
    } else if (action == kActionShow && frameless) {
        RestoreFrame(hwnd);
    }
}

// Asks the window (on its own thread, possibly in another process that also
// runs this mod) to hide/restore its title bar.
void RequestFrameless(HWND hwnd, WPARAM action) {
    if (!hwnd) {
        return;
    }
    if (!PostMessageW(hwnd, g_msgFrameless, action, 0)) {
        Wh_Log(L"PostMessage to %p failed (%u)", hwnd, GetLastError());
    }
}

BOOL CALLBACK AutoHideEnumProc(HWND hwnd, LPARAM lParam) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == (DWORD)lParam && IsAutoHideCandidate(hwnd)) {
        RequestFrameless(hwnd, kActionAutoHide);
    }
    return TRUE;
}

void AutoHideExistingWindows() {
    EnumWindows(AutoHideEnumProc, (LPARAM)GetCurrentProcessId());
}

// "Hide by default" was turned off: the windows it took are handed back. Ones
// hidden with the hotkey stay as they are.
void RestoreAutoHiddenWindows() {
    for (HWND hwnd : SnapshotAutoHiddenWindows()) {
        RequestFrameless(hwnd, kActionShow);
    }
}
