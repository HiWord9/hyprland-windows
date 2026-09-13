// ==WindhawkMod==
// @id              hypr-frameless
// @name            Hyprland-style Frameless Windows
// @description     Hotkey to hide a window's title bar completely, plus Win+LMB drag to move and Win+RMB drag to resize any window, like in Hyprland
// @version         0.0.1
// @author          hiword9
// @include         *
// @compilerOptions -lcomctl32 -ldwmapi
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Hyprland-style Frameless Windows

Brings the Hyprland window feel to Windows:

* **Hotkey** (default `Ctrl+Alt+H`) toggles the title bar of the focused
  window. The title bar is removed *completely* - no leftover frame strip at
  the top, the window content starts at the very first pixel. Snapping,
  maximize/restore animations, `Alt+Space`, shadows and rounded corners keep
  working because the window keeps its native styles; only the non-client
  layout is changed (the same technique Windows Terminal uses).
* **Win + left mouse button** anywhere on a window drags it. The click is not
  delivered to the application. Dragging a maximized window restores it first,
  like dragging its title bar would.
* **Win + right mouse button** anywhere on a window resizes it from the corner
  nearest to the cursor, exactly like `SUPER + RMB` in Hyprland.
* `Esc` during a drag cancels it and puts the window back.

Nothing is hidden by default - press the hotkey on a window to hide its title
bar, press it again to bring the title bar back. Disabling the mod restores
every window it touched.

## Notes

* Windows with a classic (non-client) menu bar: Windows lays that bar out
  from the title bar metrics, so it can't stay in place once the title bar is
  gone. By default it's hidden together with the title bar and stays reachable
  from the keyboard (`Alt`, `F10`, `Alt+letter`). The `Windows with a menu bar`
  setting can instead keep the menu bar (only the title bar is removed, a thin
  frame strip remains above the menu) or leave such windows alone.
* When the title bar is hidden the top edge of the window still works as a
  resize handle (can be disabled in the settings).
* Applications that draw their own title bar inside the client area (Chromium
  based browsers, Electron apps, VS Code, Office, UWP apps) cannot be fixed by
  any mod - tell those apps to use the native title bar instead.
* `Hide title bars by default` applies to windows created through
  `CreateWindowEx`; the hotkey works for every window.
* The border color / corner settings only affect windows whose title bar is
  hidden and are meant to mimic Hyprland's `col.active_border` /
  `col.inactive_border` / `rounding`.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- hotkey:
  - key: H
    $name: Key
    $description: >-
      A letter, a digit, F1-F24, or a key name such as Space, Enter, Insert,
      Home, PageUp, Up, NumPad5. Leave empty to disable the hotkey.
  - ctrl: true
    $name: Ctrl
  - alt: true
    $name: Alt
  - shift: false
    $name: Shift
  - win: false
    $name: Win
  $name: Toggle title bar hotkey
  $description: Hides or restores the title bar of the focused window
- dragModifier: win
  $name: Move/resize modifier key
  $description: >-
    Hold this key and drag with the left mouse button to move a window, or with
    the right mouse button to resize it
  $options:
  - win: Win (Super)
  - alt: Alt
- topEdgeResize: true
  $name: Resize from the top edge
  $description: >-
    When the title bar is hidden, the top few pixels of the window act as a
    resize handle, like the other edges
- menuBarWindows: hide
  $name: Windows with a menu bar
  $description: >-
    What to do with windows that have a classic menu bar below the title bar
  $options:
  - hide: Hide the menu bar too (keyboard access via Alt / F10 still works)
  - keepMenu: Keep the menu bar, remove only the title bar
  - skip: Don't touch such windows
- hideByDefault: false
  $name: Hide title bars by default
  $description: >-
    Automatically hide the title bar of every new window. Use the hotkey to
    bring it back for a specific window.
- border:
  - active: ""
    $name: Active window
    $description: '"#RRGGBB" for a color, "none" for no border, empty for the system default'
  - inactive: ""
    $name: Inactive window
    $description: '"#RRGGBB" for a color, "none" for no border, empty for the system default'
  $name: Border color of frameless windows
- corners: default
  $name: Corners of frameless windows
  $options:
  - default: System default
  - round: Rounded
  - roundsmall: Slightly rounded
  - none: Square
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

////////////////////////////////////////////////////////////////////////////////
// Settings

enum class DragModifier { Win, Alt };
enum class MenuBarMode { Hide, KeepMenu, Skip };

// Sentinel for "leave the DWM attribute alone" (not a valid DWM color value).
constexpr COLORREF kColorUntouched = 0xFFFFFFFD;

struct Settings {
    std::atomic<UINT> hotkeyVk{0};
    std::atomic<bool> hotkeyCtrl{true};
    std::atomic<bool> hotkeyAlt{true};
    std::atomic<bool> hotkeyShift{false};
    std::atomic<bool> hotkeyWin{false};
    std::atomic<DragModifier> dragModifier{DragModifier::Win};
    std::atomic<bool> topEdgeResize{true};
    std::atomic<MenuBarMode> menuBarMode{MenuBarMode::Hide};
    std::atomic<bool> hideByDefault{false};
    std::atomic<COLORREF> borderActive{kColorUntouched};
    std::atomic<COLORREF> borderInactive{kColorUntouched};
    std::atomic<int> corners{DWMWCP_DEFAULT};
} g_settings;

std::wstring NormalizeSettingString(PCWSTR raw) {
    std::wstring s = raw ? raw : L"";
    auto notSpace = [](wchar_t c) { return !std::iswspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    for (auto& c : s) {
        c = (wchar_t)std::towupper(c);
    }
    return s;
}

// Turns the "key" setting into a virtual key code, 0 if unusable.
UINT ParseKeyName(PCWSTR raw) {
    std::wstring s = NormalizeSettingString(raw);
    if (s.empty()) {
        return 0;
    }

    if (s.rfind(L"VK_", 0) == 0) {
        s = s.substr(3);
    }

    if (s.size() == 1) {
        wchar_t c = s[0];
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) {
            return c;
        }
        SHORT vk = VkKeyScanW(c);
        return vk == -1 ? 0 : (vk & 0xFF);
    }

    if (s.size() > 2 && s[0] == L'0' && s[1] == L'X') {
        return (UINT)wcstoul(s.c_str() + 2, nullptr, 16) & 0xFF;
    }

    if (s[0] == L'F' && s.size() <= 3 &&
        std::all_of(s.begin() + 1, s.end(),
                    [](wchar_t c) { return std::iswdigit(c) != 0; })) {
        int n = _wtoi(s.c_str() + 1);
        if (n >= 1 && n <= 24) {
            return VK_F1 + n - 1;
        }
        return 0;
    }

    static const struct {
        PCWSTR name;
        UINT vk;
    } kNames[] = {
        {L"SPACE", VK_SPACE},        {L"TAB", VK_TAB},
        {L"ENTER", VK_RETURN},       {L"RETURN", VK_RETURN},
        {L"ESC", VK_ESCAPE},         {L"ESCAPE", VK_ESCAPE},
        {L"BACKSPACE", VK_BACK},     {L"BACK", VK_BACK},
        {L"INSERT", VK_INSERT},      {L"INS", VK_INSERT},
        {L"DELETE", VK_DELETE},      {L"DEL", VK_DELETE},
        {L"HOME", VK_HOME},          {L"END", VK_END},
        {L"PAGEUP", VK_PRIOR},       {L"PRIOR", VK_PRIOR},
        {L"PAGEDOWN", VK_NEXT},      {L"NEXT", VK_NEXT},
        {L"UP", VK_UP},              {L"DOWN", VK_DOWN},
        {L"LEFT", VK_LEFT},          {L"RIGHT", VK_RIGHT},
        {L"PAUSE", VK_PAUSE},        {L"SCROLLLOCK", VK_SCROLL},
        {L"SCROLL", VK_SCROLL},      {L"PRINTSCREEN", VK_SNAPSHOT},
        {L"SNAPSHOT", VK_SNAPSHOT},  {L"CAPSLOCK", VK_CAPITAL},
        {L"CAPITAL", VK_CAPITAL},    {L"NUMLOCK", VK_NUMLOCK},
        {L"APPS", VK_APPS},          {L"MENU", VK_APPS},
        {L"NUMPAD0", VK_NUMPAD0},    {L"NUMPAD1", VK_NUMPAD1},
        {L"NUMPAD2", VK_NUMPAD2},    {L"NUMPAD3", VK_NUMPAD3},
        {L"NUMPAD4", VK_NUMPAD4},    {L"NUMPAD5", VK_NUMPAD5},
        {L"NUMPAD6", VK_NUMPAD6},    {L"NUMPAD7", VK_NUMPAD7},
        {L"NUMPAD8", VK_NUMPAD8},    {L"NUMPAD9", VK_NUMPAD9},
        {L"MULTIPLY", VK_MULTIPLY},  {L"ADD", VK_ADD},
        {L"SUBTRACT", VK_SUBTRACT},  {L"DECIMAL", VK_DECIMAL},
        {L"DIVIDE", VK_DIVIDE},
    };
    for (const auto& entry : kNames) {
        if (s == entry.name) {
            return entry.vk;
        }
    }

    return 0;
}

// "" / "default" -> untouched, "none" -> DWMWA_COLOR_NONE, "#RRGGBB" -> color.
COLORREF ParseBorderColor(PCWSTR raw) {
    std::wstring s = NormalizeSettingString(raw);
    if (s.empty() || s == L"DEFAULT") {
        return kColorUntouched;
    }
    if (s == L"NONE") {
        return DWMWA_COLOR_NONE;
    }
    if (s[0] == L'#') {
        s.erase(0, 1);
    }
    if (s.size() == 3) {
        s = {s[0], s[0], s[1], s[1], s[2], s[2]};
    }
    if (s.size() != 6 || !std::all_of(s.begin(), s.end(), [](wchar_t c) {
            return std::iswxdigit(c) != 0;
        })) {
        return kColorUntouched;
    }
    unsigned long v = wcstoul(s.c_str(), nullptr, 16);
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

MenuBarMode ParseMenuBarMode(PCWSTR raw) {
    std::wstring s = NormalizeSettingString(raw);
    if (s == L"KEEPMENU") {
        return MenuBarMode::KeepMenu;
    }
    if (s == L"SKIP") {
        return MenuBarMode::Skip;
    }
    return MenuBarMode::Hide;
}

int ParseCorners(PCWSTR raw) {
    std::wstring s = NormalizeSettingString(raw);
    if (s == L"ROUND") {
        return DWMWCP_ROUND;
    }
    if (s == L"ROUNDSMALL") {
        return DWMWCP_ROUNDSMALL;
    }
    if (s == L"NONE") {
        return DWMWCP_DONOTROUND;
    }
    return DWMWCP_DEFAULT;
}

void LoadSettings() {
    WindhawkUtils::StringSetting key(Wh_GetStringSetting(L"hotkey.key"));
    g_settings.hotkeyVk = ParseKeyName(key);
    g_settings.hotkeyCtrl = Wh_GetIntSetting(L"hotkey.ctrl") != 0;
    g_settings.hotkeyAlt = Wh_GetIntSetting(L"hotkey.alt") != 0;
    g_settings.hotkeyShift = Wh_GetIntSetting(L"hotkey.shift") != 0;
    g_settings.hotkeyWin = Wh_GetIntSetting(L"hotkey.win") != 0;

    WindhawkUtils::StringSetting modifier(Wh_GetStringSetting(L"dragModifier"));
    g_settings.dragModifier = NormalizeSettingString(modifier) == L"ALT"
                                  ? DragModifier::Alt
                                  : DragModifier::Win;

    g_settings.topEdgeResize = Wh_GetIntSetting(L"topEdgeResize") != 0;
    WindhawkUtils::StringSetting menuBar(Wh_GetStringSetting(L"menuBarWindows"));
    g_settings.menuBarMode = ParseMenuBarMode(menuBar);
    g_settings.hideByDefault = Wh_GetIntSetting(L"hideByDefault") != 0;

    WindhawkUtils::StringSetting active(Wh_GetStringSetting(L"border.active"));
    WindhawkUtils::StringSetting inactive(
        Wh_GetStringSetting(L"border.inactive"));
    g_settings.borderActive = ParseBorderColor(active);
    g_settings.borderInactive = ParseBorderColor(inactive);

    WindhawkUtils::StringSetting corners(Wh_GetStringSetting(L"corners"));
    g_settings.corners = ParseCorners(corners);

    Wh_Log(L"Settings: hotkey vk=0x%02X ctrl=%d alt=%d shift=%d win=%d, "
           L"dragModifier=%s, topEdgeResize=%d, menuBar=%d, hideByDefault=%d",
           g_settings.hotkeyVk.load(), (int)g_settings.hotkeyCtrl,
           (int)g_settings.hotkeyAlt, (int)g_settings.hotkeyShift,
           (int)g_settings.hotkeyWin,
           g_settings.dragModifier == DragModifier::Alt ? L"alt" : L"win",
           (int)g_settings.topEdgeResize, (int)g_settings.menuBarMode.load(),
           (int)g_settings.hideByDefault);
}

////////////////////////////////////////////////////////////////////////////////
// Frameless window bookkeeping

// Every request to hide/restore a title bar is posted to the window as this
// message and handled on the window's own thread, which is the only thread
// that may (un)subclass it.
enum FramelessAction : WPARAM {
    kActionToggle = 0,
    kActionHide = 1,
    kActionShow = 2,
};

UINT g_msgFrameless;  // RegisterWindowMessage, set in Wh_ModInit

constexpr UINT_PTR kSubclassId = 0x48597072;  // 'Hypr'

struct FramelessState {
    bool dwmTouched = false;
    // Style bits removed from the window (KeepMenu mode), restored later.
    DWORD removedStyle = 0;
};

// dwRefData flag for the subclass: the frame layout was left to the system.
constexpr DWORD_PTR kRefKeepMenu = 1;

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

std::vector<HWND> SnapshotFramelessWindows() {
    std::lock_guard<std::mutex> lock(g_windowsMutex);
    std::vector<HWND> result;
    result.reserve(g_windows.size());
    for (const auto& [hwnd, state] : g_windows) {
        result.push_back(hwnd);
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////
// Window classification

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

////////////////////////////////////////////////////////////////////////////////
// Non-client geometry

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

////////////////////////////////////////////////////////////////////////////////
// DWM decorations (border color, corners)

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

////////////////////////////////////////////////////////////////////////////////
// Hiding / restoring the title bar (window thread only)

void RefreshFrame(HWND hwnd) {
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void HandleFramelessRequest(HWND hwnd, WPARAM action);

LRESULT CALLBACK FramelessSubclassProc(HWND hwnd,
                                       UINT uMsg,
                                       WPARAM wParam,
                                       LPARAM lParam,
                                       UINT_PTR uIdSubclass,
                                       DWORD_PTR dwRefData) {
    bool keepMenu = (dwRefData & kRefKeepMenu) != 0;

    switch (uMsg) {
        case WM_NCCALCSIZE:
            if (keepMenu) {
                break;  // WS_CAPTION was removed instead; default layout
            }
            return OnNcCalcSize(hwnd, wParam, lParam);

        case WM_NCHITTEST: {
            LRESULT hit = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            return keepMenu ? hit : AdjustHitTest(hwnd, hit, lParam);
        }

        case WM_NCACTIVATE:
            ApplyBorderColor(hwnd, wParam != FALSE);
            break;

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

bool MakeFrameless(HWND hwnd) {
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
    RefreshFrame(hwnd);
    Wh_Log(L"Title bar restored for %p", hwnd);
}

void HandleFramelessRequest(HWND hwnd, WPARAM action) {
    bool frameless = IsFrameless(hwnd);
    if (action == kActionToggle) {
        action = frameless ? kActionShow : kActionHide;
    }
    if (action == kActionHide && !frameless) {
        MakeFrameless(hwnd);
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
        RequestFrameless(hwnd, kActionHide);
    }
    return TRUE;
}

void AutoHideExistingWindows() {
    EnumWindows(AutoHideEnumProc, (LPARAM)GetCurrentProcessId());
}

////////////////////////////////////////////////////////////////////////////////
// Hotkey

bool HandleHotkey(const MSG* msg) {
    if (msg->lParam & (1 << 30)) {
        return false;  // auto-repeat
    }

    UINT vk = g_settings.hotkeyVk;
    if (!vk || msg->wParam != vk) {
        return false;
    }

    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    bool alt = GetKeyState(VK_MENU) < 0;
    bool shift = GetKeyState(VK_SHIFT) < 0;
    bool win = GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0;
    if (ctrl != g_settings.hotkeyCtrl || alt != g_settings.hotkeyAlt ||
        shift != g_settings.hotkeyShift || win != g_settings.hotkeyWin) {
        return false;
    }

    HWND target = msg->hwnd ? GetAncestor(msg->hwnd, GA_ROOT)
                            : GetForegroundWindow();
    Wh_Log(L"Hotkey: toggling %p", target);
    RequestFrameless(target, kActionToggle);
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Modifier + mouse: move / resize

bool IsDragModifierDown() {
    if (g_settings.dragModifier == DragModifier::Alt) {
        return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    }
    return ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) !=
           0;
}

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

constexpr WORD kMaskVk = 0xE8;  // unassigned VK, only used as a "a key was hit"
constexpr ULONG_PTR kInjectedMarker = 0x48797072;  // 'Hypr'

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

using PeekMessageW_t = decltype(&PeekMessageW);
PeekMessageW_t PeekMessageW_Original;

int PhysicalButtonVk(bool right) {
    bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    return (right != swapped) ? VK_RBUTTON : VK_LBUTTON;
}

struct DragSession {
    HWND root;        // window being moved/resized
    HWND capture;     // window (on this thread) that receives the mouse input
    POINT anchor;     // cursor position when the drag started
    RECT original;    // window rect when the drag started
    UINT edge;        // 0 = move, otherwise WMSZ_* corner to resize from
    UINT buttonUp;    // WM_LBUTTONUP / WM_RBUTTONUP that ends the drag
    UINT ncButtonUp;  // WM_NCLBUTTONUP / WM_NCRBUTTONUP
    int buttonVk;     // physical button to watch (VK_LBUTTON / VK_RBUTTON)
};

LPCWSTR CursorForEdge(UINT edge) {
    switch (edge) {
        case WMSZ_TOPLEFT:
        case WMSZ_BOTTOMRIGHT:
            return IDC_SIZENWSE;
        case WMSZ_TOPRIGHT:
        case WMSZ_BOTTOMLEFT:
            return IDC_SIZENESW;
        default:
            return IDC_SIZEALL;
    }
}

RECT DragRectForPoint(const DragSession& s, POINT pt, const MINMAXINFO& mmi) {
    RECT rc = s.original;
    int dx = pt.x - s.anchor.x;
    int dy = pt.y - s.anchor.y;
    if (!s.edge) {
        OffsetRect(&rc, dx, dy);
        return rc;
    }

    bool left = s.edge == WMSZ_LEFT || s.edge == WMSZ_TOPLEFT ||
                s.edge == WMSZ_BOTTOMLEFT;
    bool right = s.edge == WMSZ_RIGHT || s.edge == WMSZ_TOPRIGHT ||
                 s.edge == WMSZ_BOTTOMRIGHT;
    bool top = s.edge == WMSZ_TOP || s.edge == WMSZ_TOPLEFT ||
               s.edge == WMSZ_TOPRIGHT;
    bool bottom = s.edge == WMSZ_BOTTOM || s.edge == WMSZ_BOTTOMLEFT ||
                  s.edge == WMSZ_BOTTOMRIGHT;
    if (left) rc.left += dx;
    if (right) rc.right += dx;
    if (top) rc.top += dy;
    if (bottom) rc.bottom += dy;

    LONG minW = std::max(1L, mmi.ptMinTrackSize.x);
    LONG minH = std::max(1L, mmi.ptMinTrackSize.y);
    LONG maxW = std::max(minW, mmi.ptMaxTrackSize.x);
    LONG maxH = std::max(minH, mmi.ptMaxTrackSize.y);
    LONG w = std::clamp(rc.right - rc.left, minW, maxW);
    LONG h = std::clamp(rc.bottom - rc.top, minH, maxH);
    if (left) rc.left = rc.right - w; else rc.right = rc.left + w;
    if (top) rc.top = rc.bottom - h; else rc.bottom = rc.top + h;
    return rc;
}

// Modal move/resize loop, like the system's own one behind a caption drag but
// driven by whichever mouse button started it (the system loop only ends on
// the left button) and independent of the thread that owns the root window.
void RunDragLoop(DragSession& s) {
    UINT dpi = WindowDpi(s.root);
    MINMAXINFO mmi{};
    mmi.ptMinTrackSize = {GetSystemMetricsForDpi(SM_CXMINTRACK, dpi),
                          GetSystemMetricsForDpi(SM_CYMINTRACK, dpi)};
    mmi.ptMaxTrackSize = {GetSystemMetricsForDpi(SM_CXMAXTRACK, dpi),
                          GetSystemMetricsForDpi(SM_CYMAXTRACK, dpi)};
    SendMessageW(s.root, WM_GETMINMAXINFO, 0, (LPARAM)&mmi);

    HCURSOR cursor = LoadCursorW(nullptr, CursorForEdge(s.edge));
    // Bypass our own hook while pumping here.
    PeekMessageW_t peekMessage =
        PeekMessageW_Original ? PeekMessageW_Original : PeekMessageW;

    SendMessageW(s.root, WM_ENTERSIZEMOVE, 0, 0);
    SetCapture(s.capture);
    SetCursor(cursor);

    RECT current = s.original;
    bool cancelled = false;
    bool done = false;
    while (!done && GetCapture() == s.capture) {
        MSG msg;
        while (!done && peekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage((int)msg.wParam);
                done = true;
                break;
            }
            if (msg.message == s.buttonUp || msg.message == s.ncButtonUp) {
                done = true;
                break;
            }

            switch (msg.message) {
                case WM_MOUSEMOVE:
                case WM_NCMOUSEMOVE: {
                    RECT rc = DragRectForPoint(s, msg.pt, mmi);
                    if (!EqualRect(&rc, &current)) {
                        if (s.edge) {
                            SendMessageW(s.root, WM_SIZING, s.edge,
                                         (LPARAM)&rc);
                        } else {
                            SendMessageW(s.root, WM_MOVING, 0, (LPARAM)&rc);
                        }
                        SetWindowPos(s.root, nullptr, rc.left, rc.top,
                                     rc.right - rc.left, rc.bottom - rc.top,
                                     SWP_NOZORDER | SWP_NOACTIVATE |
                                         (s.edge ? 0 : SWP_NOSIZE));
                        current = rc;
                    }
                    SetCursor(cursor);
                    continue;
                }

                case WM_KEYDOWN:
                case WM_SYSKEYDOWN:
                    if (msg.wParam == VK_ESCAPE) {
                        cancelled = true;
                        done = true;
                        continue;
                    }
                    break;

                // Other mouse buttons pressed mid-drag: not for the app.
                case WM_LBUTTONDOWN:
                case WM_LBUTTONDBLCLK:
                case WM_RBUTTONDOWN:
                case WM_RBUTTONDBLCLK:
                case WM_MBUTTONDOWN:
                case WM_MBUTTONDBLCLK:
                case WM_XBUTTONDOWN:
                case WM_XBUTTONDBLCLK:
                case WM_NCLBUTTONDOWN:
                case WM_NCRBUTTONDOWN:
                case WM_NCMBUTTONDOWN:
                    continue;
            }

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (done) {
            break;
        }

        // The release may never arrive as a message (e.g. the capture went to
        // a window that isn't in the foreground), so also watch the physical
        // button state instead of blocking forever.
        if (!(GetAsyncKeyState(s.buttonVk) & 0x8000)) {
            break;
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
    }

    if (GetCapture() == s.capture) {
        ReleaseCapture();
    }
    if (cancelled && !EqualRect(&current, &s.original)) {
        SetWindowPos(s.root, nullptr, s.original.left, s.original.top,
                     s.original.right - s.original.left,
                     s.original.bottom - s.original.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | (s.edge ? 0 : SWP_NOSIZE));
    }
    SendMessageW(s.root, WM_EXITSIZEMOVE, 0, 0);
}

void StartMove(HWND root, HWND capture, POINT pt) {
    if (IsIconic(root)) {
        return;
    }

    // Hand the move to the window's own thread so the *system* runs its native
    // caption-drag loop. That gives everything a title-bar drag would: Aero
    // Snap at the screen edges, the Windows 11 snap-layouts flyout when the
    // cursor reaches the top, and automatic restore of a maximized window.
    LPARAM lp = MAKELPARAM(pt.x, pt.y);
    if (GetWindowThreadProcessId(root, nullptr) == GetCurrentThreadId()) {
        DefWindowProcW(root, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, lp);
    } else {
        PostMessageW(root, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, lp);
    }
}

// Hyprland resizes from the corner nearest to the cursor.
UINT ResizeEdgeForPoint(const RECT& rc, POINT pt) {
    bool left = pt.x < (rc.left + rc.right) / 2;
    bool top = pt.y < (rc.top + rc.bottom) / 2;
    if (top) {
        return left ? WMSZ_TOPLEFT : WMSZ_TOPRIGHT;
    }
    return left ? WMSZ_BOTTOMLEFT : WMSZ_BOTTOMRIGHT;
}

void StartResize(HWND root, HWND capture, POINT pt) {
    if (IsIconic(root) || IsZoomed(root)) {
        return;
    }
    if (!(GetWindowLongPtrW(root, GWL_STYLE) & WS_THICKFRAME)) {
        return;  // fixed-size window
    }

    DragSession s{root, capture,       pt,
                  {},   0,             WM_RBUTTONUP,
                  WM_NCRBUTTONUP,      PhysicalButtonVk(true)};
    if (!GetWindowRect(root, &s.original)) {
        return;
    }
    s.edge = ResizeEdgeForPoint(s.original, pt);
    RunDragLoop(s);
}

// Per-thread: a button-up to swallow because we swallowed its button-down.
thread_local bool g_swallowButtonUp[2];  // [0] = left, [1] = right

bool HandleModifierButtonDown(const MSG* msg, bool right) {
    if (!IsDragModifierDown()) {
        return false;
    }

    HWND root = GetAncestor(msg->hwnd, GA_ROOT);
    if (!root) {
        root = msg->hwnd;
    }
    if (!IsFrameWindow(root)) {
        return false;  // desktop, taskbar, menus... - normal click
    }

    Wh_Log(L"%s %p", right ? L"Resize" : L"Move", root);
    ArmWinMask();

    // Mouse capture must go to a window of this thread; the clicked window
    // always is, the root usually.
    HWND capture =
        GetWindowThreadProcessId(root, nullptr) == GetCurrentThreadId()
            ? root
            : msg->hwnd;
    if (right) {
        StartResize(root, capture, msg->pt);
    } else {
        StartMove(root, capture, msg->pt);
    }

    // If the move/size loop consumed the button release, nothing is left to
    // swallow; otherwise make sure the app doesn't see an orphaned button-up.
    g_swallowButtonUp[right] =
        (GetAsyncKeyState(PhysicalButtonVk(right)) & 0x8000) != 0;
    return true;
}

bool HandleButtonUp(bool right) {
    if (!g_swallowButtonUp[right]) {
        return false;
    }
    g_swallowButtonUp[right] = false;
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Message loop hooks

// Called for every message an application removes from its queue. Returns
// with the message replaced by WM_NULL if it was consumed by the mod.
void ProcessRetrievedMessage(MSG* msg) {
    bool consumed = false;

    switch (msg->message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            consumed = HandleHotkey(msg);
            break;

        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
            consumed = HandleModifierButtonDown(msg, false);
            break;

        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
        case WM_NCRBUTTONDOWN:
        case WM_NCRBUTTONDBLCLK:
            consumed = HandleModifierButtonDown(msg, true);
            break;

        case WM_LBUTTONUP:
        case WM_NCLBUTTONUP:
            consumed = HandleButtonUp(false);
            break;

        case WM_RBUTTONUP:
        case WM_NCRBUTTONUP:
            consumed = HandleButtonUp(true);
            break;

        default:
            if (msg->message == g_msgFrameless && msg->hwnd) {
                HandleFramelessRequest(msg->hwnd, msg->wParam);
                consumed = true;
            }
            break;
    }

    if (consumed) {
        msg->message = WM_NULL;
        msg->wParam = 0;
        msg->lParam = 0;
    }
}

using GetMessageW_t = decltype(&GetMessageW);
GetMessageW_t GetMessageW_Original;
BOOL WINAPI GetMessageW_Hook(LPMSG lpMsg,
                             HWND hWnd,
                             UINT wMsgFilterMin,
                             UINT wMsgFilterMax) {
    BOOL ret = GetMessageW_Original(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax);
    if (ret > 0) {
        ProcessRetrievedMessage(lpMsg);
    }
    return ret;
}

using GetMessageA_t = decltype(&GetMessageA);
GetMessageA_t GetMessageA_Original;
BOOL WINAPI GetMessageA_Hook(LPMSG lpMsg,
                             HWND hWnd,
                             UINT wMsgFilterMin,
                             UINT wMsgFilterMax) {
    BOOL ret = GetMessageA_Original(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax);
    if (ret > 0) {
        ProcessRetrievedMessage(lpMsg);
    }
    return ret;
}

BOOL WINAPI PeekMessageW_Hook(LPMSG lpMsg,
                              HWND hWnd,
                              UINT wMsgFilterMin,
                              UINT wMsgFilterMax,
                              UINT wRemoveMsg) {
    BOOL ret = PeekMessageW_Original(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax,
                                     wRemoveMsg);
    if (ret && (wRemoveMsg & PM_REMOVE)) {
        ProcessRetrievedMessage(lpMsg);
    }
    return ret;
}

using PeekMessageA_t = decltype(&PeekMessageA);
PeekMessageA_t PeekMessageA_Original;
BOOL WINAPI PeekMessageA_Hook(LPMSG lpMsg,
                              HWND hWnd,
                              UINT wMsgFilterMin,
                              UINT wMsgFilterMax,
                              UINT wRemoveMsg) {
    BOOL ret = PeekMessageA_Original(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax,
                                     wRemoveMsg);
    if (ret && (wRemoveMsg & PM_REMOVE)) {
        ProcessRetrievedMessage(lpMsg);
    }
    return ret;
}

////////////////////////////////////////////////////////////////////////////////
// Window creation hooks ("hide by default")

void OnWindowCreated(HWND hwnd, DWORD dwStyle) {
    if (!hwnd || (dwStyle & WS_CHILD) || !g_settings.hideByDefault) {
        return;
    }
    if (IsAutoHideCandidate(hwnd)) {
        // We're on the creating thread, and the window isn't visible yet, so
        // do it right away instead of posting: no title bar flicker.
        MakeFrameless(hwnd);
    }
}

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t CreateWindowExW_Original;
HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle,
                                 LPCWSTR lpClassName,
                                 LPCWSTR lpWindowName,
                                 DWORD dwStyle,
                                 int X,
                                 int Y,
                                 int nWidth,
                                 int nHeight,
                                 HWND hWndParent,
                                 HMENU hMenu,
                                 HINSTANCE hInstance,
                                 LPVOID lpParam) {
    HWND hwnd = CreateWindowExW_Original(dwExStyle, lpClassName, lpWindowName,
                                         dwStyle, X, Y, nWidth, nHeight,
                                         hWndParent, hMenu, hInstance, lpParam);
    OnWindowCreated(hwnd, dwStyle);
    return hwnd;
}

using CreateWindowExA_t = decltype(&CreateWindowExA);
CreateWindowExA_t CreateWindowExA_Original;
HWND WINAPI CreateWindowExA_Hook(DWORD dwExStyle,
                                 LPCSTR lpClassName,
                                 LPCSTR lpWindowName,
                                 DWORD dwStyle,
                                 int X,
                                 int Y,
                                 int nWidth,
                                 int nHeight,
                                 HWND hWndParent,
                                 HMENU hMenu,
                                 HINSTANCE hInstance,
                                 LPVOID lpParam) {
    HWND hwnd = CreateWindowExA_Original(dwExStyle, lpClassName, lpWindowName,
                                         dwStyle, X, Y, nWidth, nHeight,
                                         hWndParent, hMenu, hInstance, lpParam);
    OnWindowCreated(hwnd, dwStyle);
    return hwnd;
}

////////////////////////////////////////////////////////////////////////////////
// Mod lifecycle

BOOL Wh_ModInit() {
    Wh_Log(L"Init");

    g_msgFrameless = RegisterWindowMessageW(L"HyprFrameless_" WH_MOD_ID);
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

    // A drag may have armed a low-level keyboard hook that hasn't seen its Win
    // key-up yet; remove it before the DLL goes away.
    g_winMaskArmed = false;
    RemoveAllMaskHooks();

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
