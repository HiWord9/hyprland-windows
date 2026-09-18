// Test harness for the mod. Compiles the *bundled* mod (so the bundler itself
// is covered too) with Windhawk's WH_EDITING stubs and exercises it against
// real windows on the desktop. Run build.ps1, which bundles first.
//
// Usage: harness.exe [--dpi-unaware] [--no-input]
//   --dpi-unaware   don't opt into per-monitor DPI awareness
//   --no-input      skip the tests that inject mouse input
#include "../build/hyprland-windows.wh.cpp"

#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <thread>

static int g_failures = 0;
static int g_passes = 0;

#define CHECK(cond, ...)                          \
    do {                                          \
        if (cond) {                               \
            g_passes++;                           \
            printf("PASS: " __VA_ARGS__);         \
        } else {                                  \
            g_failures++;                         \
            printf("FAIL: " __VA_ARGS__);         \
        }                                         \
        printf("\n");                             \
        fflush(stdout);                           \
    } while (0)

// Screenshots go to the "out" directory next to harness.exe.
static std::string g_outDir;

static std::string OutDirNextToExe() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir = path;
    dir.erase(dir.find_last_of("\\/") + 1);
    return dir + "out\\";
}

// Pumps messages the way an application does, with the mod's hook logic
// applied to every retrieved message.
static void Pump(DWORD ms) {
    DWORD end = GetTickCount() + ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ProcessRetrievedMessage(&msg);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        DWORD now = GetTickCount();
        if ((int)(end - now) <= 0) {
            break;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, end - now, QS_ALLINPUT);
    }
}

static RECT ClientRectOnScreen(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    MapWindowPoints(hwnd, nullptr, (POINT*)&rc, 2);
    return rc;
}

static void PrintRects(const char* label, HWND hwnd) {
    RECT w, c = ClientRectOnScreen(hwnd);
    GetWindowRect(hwnd, &w);
    printf("  %s: window (%ld,%ld)-(%ld,%ld) client (%ld,%ld)-(%ld,%ld)\n",
           label, w.left, w.top, w.right, w.bottom, c.left, c.top, c.right,
           c.bottom);
}

static bool SaveBmp(const char* path, int w, int h, const void* bgra) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;  // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    DWORD imageSize = (DWORD)w * h * 4;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + imageSize;
    FILE* f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bgra, imageSize, 1, f);
    fclose(f);
    return true;
}

// Captures what DWM actually shows on screen for the given rect.
static void CaptureScreen(RECT rc, const char* name) {
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    std::string path = g_outDir + name + ".bmp";
    bool ok = SaveBmp(path.c_str(), w, h, bits);
    printf("  screenshot %s (%dx%d): %s\n", name, w, h, ok ? "saved" : "FAILED");
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

static void CaptureWindow(HWND hwnd, const char* name, int margin = 24) {
    RECT rc;
    GetWindowRect(hwnd, &rc);
    rc.left -= margin;
    rc.top -= margin;
    rc.right += margin;
    rc.bottom += margin;
    CaptureScreen(rc, name);
}

////////////////////////////////////////////////////////////////////////////////
// Test window

static int g_sizeMoveEnter, g_sizeMoveExit;
// Mouse moves the window got while a move/resize loop was running: the loop
// owns the mouse for as long as it lasts. Counted from WM_ENTERSIZEMOVE,
// because the request that starts a resize is answered asynchronously, so
// ordinary input still reaches the app until then.
static int g_mouseMovesSeen;
// Clicks the window got in its client area, which during a Win + mouse drag
// has to stay zero: neither the press the mod consumes nor the synthetic one
// it drives the resize loop with may reach the app.
static int g_clientClicksSeen;
// The translucency of a window being dragged, sampled from another thread:
// this one is inside the modal loop for as long as the drag lasts.
static std::atomic<bool> g_fadeSawLayered;
static std::atomic<int> g_fadeMinAlpha;

static LRESULT CALLBACK TestWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ENTERSIZEMOVE:
            g_sizeMoveEnter++;
            g_mouseMovesSeen = 0;
            break;
        case WM_EXITSIZEMOVE:
            g_sizeMoveExit++;
            break;
        case WM_MOUSEMOVE:
            g_mouseMovesSeen++;
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONUP:
            g_clientClicksSeen++;
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(RGB(230, 240, 255));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            // Red line on the very first client rows: shows where the client
            // area starts in the screenshots.
            RECT line{rc.left, rc.top, rc.right, rc.top + 2};
            HBRUSH red = CreateSolidBrush(RGB(220, 0, 0));
            FillRect(hdc, &line, red);
            DeleteObject(red);
            SetBkMode(hdc, TRANSPARENT);
            WCHAR text[128];
            swprintf(text, 128, L"client %ldx%ld", rc.right - rc.left,
                     rc.bottom - rc.top);
            TextOutW(hdc, 12, 12, text, (int)wcslen(text));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND CreateTestWindow(PCWSTR title, bool withMenu, int x, int y) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = TestWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"HyprlandWindowsTestWnd";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassW(&wc);
        registered = true;
    }

    HMENU menu = nullptr;
    if (withMenu) {
        menu = CreateMenu();
        HMENU file = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, 1, L"&Open");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"&File");
        AppendMenuW(menu, MF_STRING, 2, L"&Edit");
        AppendMenuW(menu, MF_STRING, 3, L"&Help");
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"HyprlandWindowsTestWnd", title,
                                WS_OVERLAPPEDWINDOW, x, y, 560, 360, nullptr,
                                menu, GetModuleHandleW(nullptr), nullptr);
    // A child control covering part of the client, like real apps have.
    CreateWindowExW(0, L"BUTTON", L"child button", WS_CHILD | WS_VISIBLE, 20,
                    60, 160, 32, hwnd, (HMENU)100, GetModuleHandleW(nullptr),
                    nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // If the real mod is installed in Windhawk it is injected into this
    // process too and may have auto-hidden the title bar already ("hide by
    // default"). Ask it to put the title bar back so the copy compiled into
    // the harness is the only one acting on the window.
    static const UINT realModMsgs[] = {
        RegisterWindowMessageW(L"HyprlandWindows_local@hyprland-windows"),
        RegisterWindowMessageW(L"HyprlandWindows_hyprland-windows"),
    };
    for (UINT m : realModMsgs) {
        PostMessageW(hwnd, m, kActionShow, 0);
    }
    Pump(200);
    return hwnd;
}

static bool RealModLoaded() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    MODULEENTRY32W me{sizeof(me)};
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok && !found;
         ok = Module32NextW(snap, &me)) {
        found = wcsstr(me.szModule, L"hyprland-windows") != nullptr;
    }
    CloseHandle(snap);
    return found;
}

////////////////////////////////////////////////////////////////////////////////
// Unit tests for the pure helpers

static void TestParsers() {
    printf("\n== parsers ==\n");
    CHECK(ParseKeyName(L"H") == 'H', "key 'H'");
    CHECK(ParseKeyName(L" h ") == 'H', "key ' h ' (trim + upper)");
    CHECK(ParseKeyName(L"7") == '7', "key '7'");
    CHECK(ParseKeyName(L"F12") == VK_F12, "key F12");
    CHECK(ParseKeyName(L"f1") == VK_F1, "key f1");
    CHECK(ParseKeyName(L"F25") == 0, "key F25 rejected");
    CHECK(ParseKeyName(L"Space") == VK_SPACE, "key Space");
    CHECK(ParseKeyName(L"PageUp") == VK_PRIOR, "key PageUp");
    CHECK(ParseKeyName(L"VK_INSERT") == VK_INSERT, "key VK_INSERT");
    CHECK(ParseKeyName(L"NumPad5") == VK_NUMPAD5, "key NumPad5");
    CHECK(ParseKeyName(L"0x48") == 0x48, "key 0x48");
    CHECK(ParseKeyName(L"") == 0, "key '' disabled");
    CHECK(ParseKeyName(L"nonsense") == 0, "key nonsense rejected");
    CHECK(ParseKeyName(nullptr) == 0, "key nullptr");

    CHECK(ParseBorderColor(L"") == kColorUntouched, "color '' untouched");
    CHECK(ParseBorderColor(L"default") == kColorUntouched,
          "color default untouched");
    CHECK(ParseBorderColor(L"none") == DWMWA_COLOR_NONE, "color none");
    CHECK(ParseBorderColor(L"#33ccff") == RGB(0x33, 0xcc, 0xff),
          "color #33ccff");
    CHECK(ParseBorderColor(L"33CCFF") == RGB(0x33, 0xcc, 0xff),
          "color 33CCFF (no #)");
    CHECK(ParseBorderColor(L"#fff") == RGB(255, 255, 255), "color #fff");
    CHECK(ParseBorderColor(L"#12345") == kColorUntouched,
          "color #12345 rejected");
    CHECK(ParseBorderColor(L"#gg0000") == kColorUntouched,
          "color #gg0000 rejected");

    CHECK(ParseCorners(L"round") == DWMWCP_ROUND, "corners round");
    CHECK(ParseCorners(L"roundsmall") == DWMWCP_ROUNDSMALL,
          "corners roundsmall");
    CHECK(ParseCorners(L"none") == DWMWCP_DONOTROUND, "corners none");
    CHECK(ParseCorners(L"default") == DWMWCP_DEFAULT, "corners default");

    // An unset setting must fall back to the default combo, or a fresh install
    // where Windhawk has not written the settings out would have no hotkey.
    Hotkey def = ParseHotkey(L"");
    CHECK(def.vk == 'H' && def.ctrl && def.alt && !def.shift && !def.win,
          "hotkey '' falls back to Ctrl+Alt+H");
    Hotkey explicitCombo = ParseHotkey(L"Ctrl+Alt+H");
    CHECK(explicitCombo.vk == 'H' && explicitCombo.ctrl &&
              explicitCombo.alt && !explicitCombo.shift && !explicitCombo.win,
          "hotkey Ctrl+Alt+H");
    Hotkey winShift = ParseHotkey(L" win + shift + f4 ");
    CHECK(winShift.vk == VK_F4 && winShift.win && winShift.shift &&
              !winShift.ctrl && !winShift.alt,
          "hotkey 'win + shift + f4' (spacing and case)");
    Hotkey supers = ParseHotkey(L"Super+Control+Space");
    CHECK(supers.vk == VK_SPACE && supers.win && supers.ctrl,
          "hotkey Super/Control aliases");
    CHECK(ParseHotkey(L"none").vk == 0, "hotkey 'none' disables");
    CHECK(ParseHotkey(L"OFF").vk == 0, "hotkey 'off' disables");
    CHECK(ParseHotkey(L"H").vk == 'H' && !ParseHotkey(L"H").ctrl,
          "hotkey with no modifiers");
    CHECK(ParseHotkey(L"Ctrl+").vk == 0 && ParseHotkey(L"Ctrl+").ctrl,
          "hotkey with a trailing + and no key");
    CHECK(ParseHotkey(L"Ctrl+Alt+nonsense").vk == 0,
          "hotkey with an unknown key name");

    CHECK(ParseMenuBarMode(L"hide") == MenuBarMode::Hide, "menu mode hide");
    CHECK(ParseMenuBarMode(L"keepMenu") == MenuBarMode::KeepMenu,
          "menu mode keepMenu");
    CHECK(ParseMenuBarMode(L"skip") == MenuBarMode::Skip, "menu mode skip");
    CHECK(ParseMenuBarMode(L"") == MenuBarMode::Hide, "menu mode default");

    RECT rc{100, 100, 300, 300};
    CHECK(ResizeEdgeForPoint(rc, {120, 120}) == WMSZ_TOPLEFT, "corner top-left");
    CHECK(ResizeEdgeForPoint(rc, {280, 120}) == WMSZ_TOPRIGHT,
          "corner top-right");
    CHECK(ResizeEdgeForPoint(rc, {120, 280}) == WMSZ_BOTTOMLEFT,
          "corner bottom-left");
    CHECK(ResizeEdgeForPoint(rc, {280, 280}) == WMSZ_BOTTOMRIGHT,
          "corner bottom-right");

    CHECK(ParseDragTranslucency(L""), "translucency defaults to the fade");
    CHECK(ParseDragTranslucency(L"fade"), "translucency fade");
    CHECK(!ParseDragTranslucency(L"opaque"), "translucency opaque");
    CHECK(!ParseDragTranslucency(L" OFF "), "translucency off");

    CHECK(ClampedSetting(0, 85, 10, 100) == 85,
          "a setting that was never written means the default");
    CHECK(ClampedSetting(50, 85, 10, 100) == 50, "a setting in range is kept");
    CHECK(ClampedSetting(400, 85, 10, 100) == 100, "a setting is clamped high");
    CHECK(ClampedSetting(3, 85, 10, 100) == 10, "a setting is clamped low");

    CHECK(DragAlphaFor(255, 85) == 216, "85%% of an opaque window (%d)",
          DragAlphaFor(255, 85));
    CHECK(DragAlphaFor(200, 50) == 100, "half of an already translucent one");
    CHECK(DragAlphaFor(255, 100) == 255, "100%% changes nothing");

    CHECK(FadeAlphaAt(255, 200, 100, 0) == 255, "a fade starts where it was");
    CHECK(FadeAlphaAt(255, 200, 100, 100) == 200, "and ends where it goes");
    CHECK(FadeAlphaAt(255, 200, 100, 150) == 200, "past its end it holds");
    CHECK(FadeAlphaAt(255, 200, 0, 0) == 200, "a zero-length fade is instant");
    BYTE half = FadeAlphaAt(255, 200, 100, 50);
    CHECK(half < 255 && half > 200, "halfway is in between (%d)", half);
    CHECK(FadeAlphaAt(255, 200, 100, 25) > FadeAlphaAt(255, 200, 100, 75),
          "and a fade only goes one way");
}

////////////////////////////////////////////////////////////////////////////////
// Geometry / rendering tests

static void TestFramelessGeometry(bool withMenu, MenuBarMode mode) {
    const char* modeName = mode == MenuBarMode::KeepMenu ? "keepMenu" : "hide";
    printf("\n== frameless geometry (%s%s) ==\n",
           withMenu ? "menu bar, mode=" : "no menu", withMenu ? modeName : "");
    std::string suffix = withMenu ? std::string("_menu_") + modeName : "";
    g_settings.menuBarMode = mode;
    bool keepMenu = withMenu && mode == MenuBarMode::KeepMenu;

    HWND hwnd = CreateTestWindow(withMenu ? L"Hypr test (menu)" : L"Hypr test",
                                 withMenu, 120, 120);
    Pump(400);

    RECT win0, cli0 = ClientRectOnScreen(hwnd);
    GetWindowRect(hwnd, &win0);
    LONG_PTR style0 = GetWindowLongPtrW(hwnd, GWL_STYLE);
    PrintRects("before", hwnd);
    UINT dpi = WindowDpi(hwnd);
    int handle = ResizeHandleHeight(hwnd);
    printf("  dpi=%u resize handle=%d caption+frame=%ld\n", dpi, handle,
           cli0.top - win0.top);
    CHECK(cli0.top > win0.top, "window has a title bar before hiding");
    CaptureWindow(hwnd, ("before" + suffix).c_str());

    // Hide via the same path the hotkey uses: a posted request handled by
    // the message hook on the window's thread.
    RequestFrameless(hwnd, kActionToggle);
    Pump(400);
    CHECK(IsFrameless(hwnd), "window is tracked as frameless");

    RECT win1, cli1 = ClientRectOnScreen(hwnd);
    GetWindowRect(hwnd, &win1);
    LONG_PTR style1 = GetWindowLongPtrW(hwnd, GWL_STYLE);
    PrintRects("hidden", hwnd);
    CHECK(EqualRect(&win0, &win1), "window rect unchanged after hiding");
    CHECK(cli1.left == cli0.left && cli1.right == cli0.right &&
              cli1.bottom == cli0.bottom,
          "left/right/bottom client edges unchanged");

    long midX = (win1.left + win1.right) / 2;
    long menuHeight = 0;
    if (keepMenu) {
        MENUBARINFO mbi{sizeof(mbi)};
        GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi);
        menuHeight = mbi.rcBar.bottom - mbi.rcBar.top;
        printf("  menu bar rect (%ld,%ld)-(%ld,%ld)\n", mbi.rcBar.left,
               mbi.rcBar.top, mbi.rcBar.right, mbi.rcBar.bottom);
        CHECK((style1 & WS_CAPTION) != WS_CAPTION, "keepMenu: WS_CAPTION removed");
        CHECK(menuHeight > 0, "keepMenu: menu bar still exists");
        CHECK(mbi.rcBar.top == win1.top + handle,
              "keepMenu: menu bar sits right below the frame strip (%ld vs %ld)",
              mbi.rcBar.top, win1.top + handle);
        RECT adjusted{};
        AdjustWindowRectExForDpi(&adjusted, (DWORD)style1, TRUE,
                                 (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE),
                                 dpi);
        CHECK(cli1.top == win1.top - adjusted.top,
              "keepMenu: client starts below frame + menu bar (%ld vs %ld)",
              cli1.top, win1.top - adjusted.top);
        CHECK(cli1.top - win1.top <= cli0.top - win0.top - handle,
              "keepMenu: the title bar height is gone");
        LRESULT menuHit = SendMessageW(hwnd, WM_NCHITTEST, 0,
                                       MAKELPARAM(midX, win1.top + handle + 5));
        CHECK(menuHit == HTMENU, "keepMenu: menu bar is HTMENU (%ld)",
              (long)menuHit);
        LRESULT stripHit = SendMessageW(hwnd, WM_NCHITTEST, 0,
                                        MAKELPARAM(midX, win1.top + 2));
        CHECK(stripHit == HTTOP, "keepMenu: frame strip is HTTOP (%ld)",
              (long)stripHit);
    } else {
        CHECK(style1 == style0, "window styles untouched");
        CHECK(cli1.top == win1.top,
              "client starts at the very top of the window (no strip)");
        LRESULT topHit = SendMessageW(hwnd, WM_NCHITTEST, 0,
                                      MAKELPARAM(midX, win1.top + 2));
        LRESULT cornerL = SendMessageW(
            hwnd, WM_NCHITTEST, 0, MAKELPARAM(win1.left + handle + 2, win1.top + 2));
        LRESULT cornerR = SendMessageW(
            hwnd, WM_NCHITTEST, 0, MAKELPARAM(win1.right - handle - 2, win1.top + 2));
        LRESULT below = SendMessageW(hwnd, WM_NCHITTEST, 0,
                                     MAKELPARAM(midX, win1.top + handle + 20));
        LRESULT menuBand = SendMessageW(hwnd, WM_NCHITTEST, 0,
                                        MAKELPARAM(midX, win1.top + 60));
        CHECK(topHit == HTTOP, "top strip hit test is HTTOP (%ld)", (long)topHit);
        CHECK(cornerL == HTTOPLEFT, "top-left corner is HTTOPLEFT (%ld)",
              (long)cornerL);
        CHECK(cornerR == HTTOPRIGHT, "top-right corner is HTTOPRIGHT (%ld)",
              (long)cornerR);
        CHECK(below == HTCLIENT, "below the strip is HTCLIENT (%ld)", (long)below);
        CHECK(menuBand == HTCLIENT,
              "former caption/menu band is plain HTCLIENT (%ld)", (long)menuBand);
        g_settings.topEdgeResize = false;
        LRESULT topHitOff = SendMessageW(hwnd, WM_NCHITTEST, 0,
                                         MAKELPARAM(midX, win1.top + 2));
        g_settings.topEdgeResize = true;
        CHECK(topHitOff == HTCLIENT,
              "top strip is HTCLIENT when topEdgeResize is off (%ld)",
              (long)topHitOff);
    }
    LRESULT leftEdge = SendMessageW(hwnd, WM_NCHITTEST, 0,
                                    MAKELPARAM(win1.left + 2, win1.top + 100));
    CHECK(leftEdge == HTLEFT, "left edge still resizes (%ld)", (long)leftEdge);

    CaptureWindow(hwnd, ("hidden" + suffix).c_str());

    // Maximize: the client must start at the work area, not above it.
    ShowWindow(hwnd, SW_MAXIMIZE);
    Pump(500);
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    RECT winMax, cliMax = ClientRectOnScreen(hwnd);
    GetWindowRect(hwnd, &winMax);
    PrintRects("maximized", hwnd);
    printf("  work area (%ld,%ld)-(%ld,%ld)\n", mi.rcWork.left, mi.rcWork.top,
           mi.rcWork.right, mi.rcWork.bottom);
    CHECK(IsZoomed(hwnd), "window is maximized");
    long expectedTop = mi.rcWork.top;
    if (keepMenu) {
        MENUBARINFO mbi{sizeof(mbi)};
        GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi);
        CHECK(mbi.rcBar.top == mi.rcWork.top,
              "maximized: menu bar starts at the work area top");
        // Frame strip is off-screen, so only the menu bar's NC height is left
        RECT adjusted{};
        AdjustWindowRectExForDpi(&adjusted,
                                 (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE),
                                 TRUE,
                                 (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE),
                                 dpi);
        expectedTop = winMax.top - adjusted.top;
    }
    CHECK(cliMax.top == expectedTop,
          "maximized: client top at work area top (+menu) (%ld vs %ld)",
          cliMax.top, expectedTop);
    CHECK(cliMax.left == mi.rcWork.left && cliMax.right == mi.rcWork.right &&
              cliMax.bottom == mi.rcWork.bottom,
          "maximized: other client edges match the work area");
    LRESULT maxHit = SendMessageW(
        hwnd, WM_NCHITTEST, 0,
        MAKELPARAM((mi.rcWork.left + mi.rcWork.right) / 2, mi.rcWork.top + 2));
    CHECK(maxHit == (keepMenu ? HTMENU : HTCLIENT),
          "maximized: top row hit test (%ld)", (long)maxHit);
    RECT shot{mi.rcWork.left, mi.rcMonitor.top,
              std::min(mi.rcWork.left + 500L, mi.rcWork.right),
              mi.rcWork.top + 160};
    CaptureScreen(shot, ("maximized" + suffix).c_str());

    ShowWindow(hwnd, SW_RESTORE);
    Pump(500);
    RECT cliRestored = ClientRectOnScreen(hwnd);
    RECT winRestored;
    GetWindowRect(hwnd, &winRestored);
    PrintRects("restored", hwnd);
    CHECK(EqualRect(&winRestored, &win0), "restored window rect");
    CHECK(cliRestored.top == cli1.top, "restored client top");

    // Show the title bar again.
    RequestFrameless(hwnd, kActionToggle);
    Pump(400);
    CHECK(!IsFrameless(hwnd), "window no longer tracked");
    RECT cli2 = ClientRectOnScreen(hwnd);
    PrintRects("shown", hwnd);
    CHECK(EqualRect(&cli2, &cli0), "client rect back to original");
    CHECK(GetWindowLongPtrW(hwnd, GWL_STYLE) == style0,
          "window style back to original");
    CaptureWindow(hwnd, ("shown" + suffix).c_str());

    // Hotkey path: plain key (no modifiers) so no real key state is needed.
    g_settings.hotkeyVk = 'H';
    g_settings.hotkeyCtrl = false;
    g_settings.hotkeyAlt = false;
    g_settings.hotkeyShift = false;
    g_settings.hotkeyWin = false;
    MSG key{hwnd, WM_KEYDOWN, 'H', 1, 0, {0, 0}};
    ProcessRetrievedMessage(&key);
    CHECK(key.message == WM_NULL, "hotkey keydown is swallowed");
    MSG repeat{hwnd, WM_KEYDOWN, 'H', (LPARAM)(1 | (1 << 30)), 0, {0, 0}};
    ProcessRetrievedMessage(&repeat);
    CHECK(repeat.message == WM_KEYDOWN, "auto-repeat keydown passes through");
    MSG other{hwnd, WM_KEYDOWN, 'J', 1, 0, {0, 0}};
    ProcessRetrievedMessage(&other);
    CHECK(other.message == WM_KEYDOWN, "other key passes through");
    Pump(400);
    CHECK(IsFrameless(hwnd), "hotkey hid the title bar");
    RECT cli3 = ClientRectOnScreen(hwnd);
    CHECK(cli3.top == cli1.top, "hotkey: same geometry as before");

    // Unload path: synchronous restore of every tracked window.
    Wh_ModBeforeUninit();
    Pump(200);
    // Uninit is one-way for the mod, but the harness carries on afterwards.
    g_uninitializing = false;
    CHECK(!IsFrameless(hwnd), "BeforeUninit restored the window");
    RECT cli4 = ClientRectOnScreen(hwnd);
    CHECK(EqualRect(&cli4, &cli0), "BeforeUninit: original client rect");
    CHECK(GetWindowLongPtrW(hwnd, GWL_STYLE) == style0,
          "BeforeUninit: original style");

    DestroyWindow(hwnd);
    Pump(100);
}

////////////////////////////////////////////////////////////////////////////////
// The WH_GETMESSAGE interception hook

// Pumps without calling ProcessRetrievedMessage, so only the mod's own hook
// can act on the messages.
static void PumpRaw(DWORD ms, UINT extraFlags = 0) {
    DWORD end = GetTickCount() + ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE | extraFlags)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        int left = (int)(end - GetTickCount());
        if (left <= 0) {
            break;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, left, QS_ALLINPUT);
    }
}

static void TestMessageHook() {
    printf("\n== WH_GETMESSAGE interception ==\n");

    // Messages are intercepted by a hook procedure instead of by hooking the
    // blocking GetMessage, so check the hook really is what does the work:
    // everything here is pumped without calling ProcessRetrievedMessage.
    HWND hwnd = CreateTestWindow(L"Hypr message hook test", false, 300, 300);
    PumpRaw(200);

    g_settings.hotkeyVk = 'H';
    g_settings.hotkeyCtrl = false;
    g_settings.hotkeyAlt = false;
    g_settings.hotkeyShift = false;
    g_settings.hotkeyWin = false;

    PostMessageW(hwnd, WM_KEYDOWN, 'H', 1);
    PumpRaw(300);
    CHECK(!IsFrameless(hwnd), "nothing happens before the hook is installed");

    InstallMessageHookForThread();
    PostMessageW(hwnd, WM_KEYDOWN, 'H', 1);
    PumpRaw(400);
    CHECK(IsFrameless(hwnd), "the hook picked the hotkey up and hid the bar");

    // A pump that filters with the PM_QS_* flags passes them in the hook's
    // wParam alongside PM_REMOVE, which must not stop the interception.
    PostMessageW(hwnd, WM_KEYDOWN, 'H', 1);
    PumpRaw(400, PM_QS_POSTMESSAGE);
    CHECK(!IsFrameless(hwnd), "a pump passing PM_QS_* flags is intercepted");

    RemoveMessageHooks();
    PostMessageW(hwnd, WM_KEYDOWN, 'H', 1);
    PumpRaw(300);
    CHECK(!IsFrameless(hwnd), "after removal the hotkey is ignored again");

    // A thread only tries once, so it takes a fresh image - which is what a
    // reloaded mod is - to get the hook back.
    g_messageHookAttempted = false;
    InstallMessageHookForThread();
    PostMessageW(hwnd, WM_KEYDOWN, 'H', 1);
    PumpRaw(400);
    CHECK(IsFrameless(hwnd), "a reloaded mod hooks the thread again");

    RemoveMessageHooks();
    RequestFrameless(hwnd, kActionShow);
    Pump(200);
    DestroyWindow(hwnd);
    Pump(100);
}

////////////////////////////////////////////////////////////////////////////////
// "Hide by default"

static void TestAutoHide() {
    printf("\n== auto-hide (hide by default) ==\n");

    // Auto-hide is deferred until the window pumps messages and re-checked
    // when it runs, so a window that never becomes visible - like the
    // throwaway top-level windows toolkits create while starting up - is left
    // alone. Hiding those mid-creation stops some apps from starting.
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"HyprlandWindowsTestWnd",
                                L"Hypr auto-hide test", WS_OVERLAPPEDWINDOW,
                                240, 240, 420, 280, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    CHECK(hwnd && !IsWindowVisible(hwnd), "created an invisible window");
    CHECK(IsAutoHideCandidate(hwnd), "it otherwise qualifies for auto-hide");

    RequestFrameless(hwnd, kActionAutoHide);
    Pump(300);
    CHECK(!IsFrameless(hwnd), "auto-hide leaves an invisible window alone");

    ShowWindow(hwnd, SW_SHOW);
    Pump(200);
    RequestFrameless(hwnd, kActionAutoHide);
    Pump(300);
    CHECK(IsFrameless(hwnd), "auto-hide takes the window once it is shown");
    RECT win, cli = ClientRectOnScreen(hwnd);
    GetWindowRect(hwnd, &win);
    CHECK(cli.top == win.top, "and removes the whole title bar");

    // An explicit request - what the hotkey sends - is not filtered that way.
    RequestFrameless(hwnd, kActionShow);
    Pump(300);
    CHECK(!IsFrameless(hwnd), "explicit show restores it");
    ShowWindow(hwnd, SW_HIDE);
    Pump(100);
    RequestFrameless(hwnd, kActionHide);
    Pump(300);
    CHECK(IsFrameless(hwnd), "an explicit hide still works while hidden");

    RequestFrameless(hwnd, kActionShow);
    Pump(200);
    DestroyWindow(hwnd);
    Pump(100);
}

////////////////////////////////////////////////////////////////////////////////
// Move / resize loop tests (inject mouse input)

static void SendMouse(DWORD flags, DWORD data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    SendInput(1, &in, sizeof(in));
}

static void MoveCursorGradually(POINT from, POINT to, int steps) {
    for (int i = 1; i <= steps; i++) {
        SetCursorPos(from.x + (to.x - from.x) * i / steps,
                     from.y + (to.y - from.y) * i / steps);
        Sleep(15);
    }
}

// Presses the button, retrieves the button-down message the way the message
// hook does (retrieved but not dispatched), asks the mod for the drag, and
// pumps messages the way an application does - which is where the system's
// move/resize loop is entered from - while another thread drags the mouse and
// releases the button. With cancel=true, Esc is pressed before the button is
// released.
static void DragWith(bool right, HWND hwnd, POINT start, POINT delta,
                     WPARAM kind, bool cancel = false) {
    SetCursorPos(start.x, start.y);
    Sleep(50);
    SendMouse(right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN);
    Sleep(50);
    MSG down;
    UINT downMsg = right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
    bool gotDown = PeekMessageW(&down, nullptr, downMsg, downMsg, PM_REMOVE);
    CHECK(gotDown, "button-down message retrieved before the drag");
    if (right) {
        // What HandleModifierButtonDown does for the press it consumes.
        g_swallowButtonUp[1] = true;
    }
    RequestDrag(hwnd, kind, start);

    g_fadeSawLayered = false;
    g_fadeMinAlpha = 255;
    std::atomic<bool> sampling{true};
    std::thread sampler([&] {
        while (sampling) {
            if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) {
                g_fadeSawLayered = true;
                COLORREF key;
                BYTE alpha;
                DWORD flags;
                if (GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags) &&
                    (flags & LWA_ALPHA) && alpha < g_fadeMinAlpha) {
                    g_fadeMinAlpha = alpha;
                }
            }
            Sleep(5);
        }
    });

    std::thread mover([=] {
        Sleep(200);
        POINT end{start.x + delta.x, start.y + delta.y};
        MoveCursorGradually(start, end, 12);
        Sleep(120);
        if (cancel) {
            INPUT k[2]{};
            k[0].type = INPUT_KEYBOARD;
            k[0].ki.wVk = VK_ESCAPE;
            k[1] = k[0];
            k[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, k, sizeof(INPUT));
            Sleep(150);
        }
        SendMouse(right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP);
    });

    int enter0 = g_sizeMoveEnter, exit0 = g_sizeMoveExit;
    g_mouseMovesSeen = 0;
    g_clientClicksSeen = 0;
    DWORD t0 = GetTickCount();
    // The loop runs inside DispatchMessage, so this is what blocks in it.
    for (int i = 0; i < 60 && g_sizeMoveExit == exit0; i++) {
        PumpRaw(50);
    }
    DWORD elapsed = GetTickCount() - t0;
    mover.join();
    sampling = false;
    sampler.join();
    printf("  modal loop ran for %lu ms\n", (unsigned long)elapsed);
    CHECK(elapsed >= 150, "the loop blocked until the button was released");
    CHECK(g_sizeMoveEnter == enter0 + 1 && g_sizeMoveExit == exit0 + 1,
          "WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE delivered once");
    // A single move can still slip through as the loop takes the mouse over.
    // What matters is that the drag itself - a dozen of them - does not.
    CHECK(g_mouseMovesSeen <= 1, "app saw no mouse moves during the drag (%d)",
          g_mouseMovesSeen);
    CHECK(g_clientClicksSeen == 0, "app saw no click of its own during it");
    CHECK(GetCapture() == nullptr, "mouse capture released");
    PumpRaw(100);
    if (!cancel) {
        CHECK(!g_swallowButtonUp[0] && !g_swallowButtonUp[1],
              "no button-up left flagged afterwards");
    }
    // A cancelled drag can end with the cursor outside the window, and its
    // release then lands on whatever is there instead.
    g_swallowButtonUp[0] = g_swallowButtonUp[1] = false;
}

static void TestMoveResize() {
    printf("\n== move / resize loops ==\n");

    POINT savedCursor;
    GetCursorPos(&savedCursor);

    // With the mod's own hook in place, so that the messages the system's
    // loops pump are seen exactly the way they are in an application.
    g_messageHookAttempted = false;
    InstallMessageHookForThread();

    HWND hwnd = CreateTestWindow(L"Hypr drag test", false, 160, 160);
    PumpRaw(300);
    RequestFrameless(hwnd, kActionHide);
    PumpRaw(300);
    SetForegroundWindow(hwnd);
    PumpRaw(100);

    RECT before;
    GetWindowRect(hwnd, &before);
    POINT center{(before.left + before.right) / 2,
                 (before.top + before.bottom) / 2};

    // Move.
    DragWith(false, hwnd, center, {140, 90}, kDragMove);
    PumpRaw(100);
    RECT after;
    GetWindowRect(hwnd, &after);
    PrintRects("after move", hwnd);
    CHECK(after.left == before.left + 140 && after.top == before.top + 90,
          "window moved by the drag delta (%ld,%ld)", after.left - before.left,
          after.top - before.top);
    CHECK(after.right - after.left == before.right - before.left,
          "size unchanged by move");

    // Translucency while it was being dragged, and the window handed back
    // exactly as it was once the drag is over.
    CHECK(g_fadeSawLayered, "the dragged window was made layered");
    CHECK(g_fadeMinAlpha <= DragAlphaFor(255, g_settings.dragOpacity),
          "and faded to the configured opacity (alpha %d)",
          (int)g_fadeMinAlpha);
    PumpRaw(400);
    CHECK(!(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED),
          "the layered style is gone once the drag ends");
    CHECK(!IsDragFading(hwnd), "and the window is no longer tracked");

    // Resize from the bottom-right quadrant, starting well inside the window.
    RECT r0 = after;
    POINT p{r0.right - 80, r0.bottom - 60};
    DragWith(true, hwnd, p, {100, 70}, kDragResize);
    PumpRaw(100);
    RECT r1;
    GetWindowRect(hwnd, &r1);
    POINT cur;
    GetCursorPos(&cur);
    PrintRects("after resize (BR)", hwnd);
    CHECK(r1.left == r0.left && r1.top == r0.top,
          "bottom-right resize keeps the top-left corner");
    CHECK(r1.right == r0.right + 100 && r1.bottom == r0.bottom + 70,
          "bottom-right resize grew by the drag delta (%ld,%ld)",
          r1.right - r0.right, r1.bottom - r0.bottom);
    CHECK(cur.x == p.x + 100 && cur.y == p.y + 70,
          "cursor was not warped to the corner (%ld,%ld vs %ld,%ld)", cur.x,
          cur.y, p.x + 100, p.y + 70);

    // Resize from the top-left quadrant.
    RECT r2 = r1;
    POINT q{r2.left + 60, r2.top + 40};
    DragWith(true, hwnd, q, {-50, -30}, kDragResize);
    PumpRaw(100);
    RECT r3;
    GetWindowRect(hwnd, &r3);
    PrintRects("after resize (TL)", hwnd);
    CHECK(r3.right == r2.right && r3.bottom == r2.bottom,
          "top-left resize keeps the bottom-right corner");
    CHECK(r3.left == r2.left - 50 && r3.top == r2.top - 30,
          "top-left resize moved the top-left corner by the delta (%ld,%ld)",
          r3.left - r2.left, r3.top - r2.top);

    // Drag a maximized window: it should restore and follow the cursor.
    ShowWindow(hwnd, SW_MAXIMIZE);
    PumpRaw(400);
    RECT rMax;
    GetWindowRect(hwnd, &rMax);
    POINT m{rMax.left + 300, rMax.top + 60};
    DragWith(false, hwnd, m, {80, 120}, kDragMove);
    PumpRaw(200);
    RECT r4;
    GetWindowRect(hwnd, &r4);
    PrintRects("after maximized drag", hwnd);
    CHECK(!IsZoomed(hwnd), "maximized window was restored by the drag");
    POINT endCursor{m.x + 80, m.y + 120};
    CHECK(PtInRect(&r4, endCursor), "restored window is under the cursor");
    CHECK(r4.right - r4.left == r3.right - r3.left,
          "restored window kept its normal size");

    // Esc cancels a drag and puts the window back.
    RECT r5 = r4;
    POINT c{(r5.left + r5.right) / 2, (r5.top + r5.bottom) / 2};
    DragWith(false, hwnd, c, {90, 60}, kDragMove, /*cancel=*/true);
    PumpRaw(100);
    RECT r6;
    GetWindowRect(hwnd, &r6);
    PrintRects("after cancelled move", hwnd);
    CHECK(EqualRect(&r6, &r5), "Esc restored the original position");
    DragWith(true, hwnd, {r5.right - 40, r5.bottom - 40}, {60, 60},
             kDragResize, /*cancel=*/true);
    PumpRaw(100);
    GetWindowRect(hwnd, &r6);
    CHECK(EqualRect(&r6, &r5), "Esc restored the original size");
    PumpRaw(400);
    CHECK(!(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED),
          "a cancelled drag hands the window back too");

    // Requests that must not start anything. A move needs the button to
    // still be down (nothing is held here), and a fixed-size window has
    // nothing to resize - in which case no synthetic button may be left held
    // either.
    int enter0 = g_sizeMoveEnter;
    RequestDrag(hwnd, kDragMove, c);
    PumpRaw(300);
    CHECK(g_sizeMoveEnter == enter0,
          "a move request with the button already up starts no loop");
    CHECK(!IsDragFading(hwnd), "and fades nothing");

    LONG_PTR st = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, st & ~WS_THICKFRAME);
    RequestDrag(hwnd, kDragResize, c);
    PumpRaw(300);
    CHECK(g_sizeMoveEnter == enter0,
          "a resize request for a fixed-size window starts no loop");
    CHECK(!(GetAsyncKeyState(VK_LBUTTON) & 0x8000),
          "and holds no mouse button of its own");
    SetWindowLongPtrW(hwnd, GWL_STYLE, st);

    // A pending (consumed) right button-up is swallowed exactly once.
    g_swallowButtonUp[1] = true;
    MSG ru{hwnd, WM_RBUTTONUP, 0, 0, 0, c};
    ProcessRetrievedMessage(&ru);
    CHECK(ru.message == WM_NULL, "flagged button-up swallowed");
    CHECK(!g_swallowButtonUp[1], "swallow flag cleared");
    MSG ru2{hwnd, WM_RBUTTONUP, 0, 0, 0, c};
    ProcessRetrievedMessage(&ru2);
    CHECK(ru2.message == WM_RBUTTONUP, "next button-up passes through");

    // A flag left over from a press whose release went elsewhere must not
    // swallow the release of the next one.
    g_swallowButtonUp[1] = true;
    MSG rd{hwnd, WM_RBUTTONDOWN, 0, 0, 0, c};
    ProcessRetrievedMessage(&rd);
    CHECK(rd.message == WM_RBUTTONDOWN && !g_swallowButtonUp[1],
          "a fresh press clears a stale swallow flag");

    // Modifier gate: without the modifier held, clicks pass through.
    MSG click{hwnd, WM_LBUTTONDOWN, 0, 0, 0, center};
    ProcessRetrievedMessage(&click);
    CHECK(click.message == WM_LBUTTONDOWN,
          "click without modifier passes through");

    SetCursorPos(savedCursor.x, savedCursor.y);
    RemoveMessageHooks();
    DestroyWindow(hwnd);
    Pump(100);
}

////////////////////////////////////////////////////////////////////////////////
// Translucency while dragging, without a drag: what the fade does to a window
// on its own, including the windows it has to leave alone.

// Makes a window composite itself per pixel, the way an app that draws its
// own transparency does, so the fade has something real to stay out of.
static void PaintPerPixel(HWND hwnd) {
    const int side = 8;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = side;
    bi.bmiHeader.biHeight = side;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp =
        CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memset(bits, 0x80, side * side * 4);  // premultiplied, half transparent
    HGDIOBJ old = SelectObject(mem, bmp);
    POINT src{0, 0};
    SIZE size{side, side};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    CHECK(UpdateLayeredWindow(hwnd, screen, nullptr, &size, mem, &src, 0,
                              &blend, ULW_ALPHA),
          "the window paints its own per-pixel transparency");
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

static void TestDragFade() {
    printf("\n== drag translucency ==\n");

    COLORREF key = 0;
    BYTE alpha = 0;
    DWORD flags = 0;

    HWND hwnd = CreateTestWindow(L"Hypr fade test", false, 200, 200);
    LONG_PTR ex0 = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    BeginDragFade(hwnd);
    CHECK(IsDragFading(hwnd), "a drag arms the fade");
    CHECK(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED,
          "and makes the window layered");
    CHECK(GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags) &&
              alpha == 255,
          "still opaque until the loop it follows starts (alpha %d)",
          (int)alpha);

    // No loop ever starts here, so the fade gives up waiting and asks the
    // window's thread - this one - to take the style back off.
    for (int i = 0; i < 20 && IsDragFading(hwnd); i++) {
        Pump(100);
    }
    CHECK(!IsDragFading(hwnd), "a loop that never starts ends the fade");
    CHECK(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) == ex0,
          "and leaves the window's styles as they were");
    DestroyWindow(hwnd);
    Pump(100);

    // A window that is already translucent keeps its own alpha and its own
    // layered style; the fade is relative to what it had.
    HWND translucent = CreateTestWindow(L"Hypr fade test 2", false, 240, 240);
    SetWindowLongPtrW(
        translucent, GWL_EXSTYLE,
        GetWindowLongPtrW(translucent, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(translucent, 0, 180, LWA_ALPHA);
    Pump(100);
    BeginDragFade(translucent);
    CHECK(IsDragFading(translucent), "an already translucent window fades too");
    CHECK(DragAlphaFor(180, g_settings.dragOpacity) < 180,
          "to less than it was");
    for (int i = 0; i < 20 && IsDragFading(translucent); i++) {
        Pump(100);
    }
    CHECK(GetLayeredWindowAttributes(translucent, &key, &alpha, &flags) &&
              alpha == 180,
          "and gets its own alpha back afterwards (%d)", (int)alpha);
    CHECK(GetWindowLongPtrW(translucent, GWL_EXSTYLE) & WS_EX_LAYERED,
          "with the layered style it brought itself left alone");
    DestroyWindow(translucent);
    Pump(100);

    // A window that paints its own transparency is not touched at all: a flat
    // alpha replaces what it drew, and makes its next UpdateLayeredWindow
    // fail on top of that.
    HWND perPixel = CreateTestWindow(L"Hypr fade test 3", false, 280, 280);
    SetWindowLongPtrW(perPixel, GWL_EXSTYLE,
                      GetWindowLongPtrW(perPixel, GWL_EXSTYLE) | WS_EX_LAYERED);
    CHECK(GetLayeredWindowAttributes(perPixel, &key, &alpha, &flags) && !flags,
          "a freshly layered window reports no attributes set (0x%lX)", flags);
    BeginDragFade(perPixel);
    CHECK(!IsDragFading(perPixel),
          "which the fade stays out of - the app may be about to paint it");
    PaintPerPixel(perPixel);
    CHECK(!GetLayeredWindowAttributes(perPixel, &key, &alpha, &flags),
          "and once it has, there is nothing to read at all");
    BeginDragFade(perPixel);
    CHECK(!IsDragFading(perPixel), "which the fade stays out of as well");
    DestroyWindow(perPixel);
    Pump(100);

    HWND off = CreateTestWindow(L"Hypr fade test 4", false, 320, 320);
    g_settings.dragTranslucency = false;
    BeginDragFade(off);
    CHECK(!IsDragFading(off), "the setting turns the fade off");
    g_settings.dragTranslucency = true;
    g_settings.dragOpacity = 100;
    BeginDragFade(off);
    CHECK(!IsDragFading(off), "and so does an opacity of 100%%");
    CHECK(!(GetWindowLongPtrW(off, GWL_EXSTYLE) & WS_EX_LAYERED),
          "neither of them touches the window");
    g_settings.dragOpacity = kDefaultDragOpacity;
    DestroyWindow(off);
    Pump(100);
}

////////////////////////////////////////////////////////////////////////////////

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("harness start\n");
    bool dpiUnaware = false, noInput = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dpi-unaware")) dpiUnaware = true;
        if (!strcmp(argv[i], "--no-input")) noInput = true;
    }
    if (!dpiUnaware) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    g_outDir = OutDirNextToExe();
    CreateDirectoryA(g_outDir.c_str(), nullptr);
    if (dpiUnaware) {
        g_outDir += "unaware_";
    }

    if (RealModLoaded()) {
        printf("note: the real hyprland-windows mod is injected into this "
               "process; test windows ask it to restore their title bars\n");
    }

    Wh_ModInit();  // registers the message, settings come from stubs
    g_settings.topEdgeResize = true;
    g_settings.dragModifier = DragModifier::Win;

    TestParsers();
    TestFramelessGeometry(false, MenuBarMode::Hide);
    TestFramelessGeometry(true, MenuBarMode::Hide);
    TestFramelessGeometry(true, MenuBarMode::KeepMenu);
    TestMessageHook();
    TestAutoHide();
    TestDragFade();
    if (!noInput) {
        TestMoveResize();
    }

    printf("\n%d passed, %d failed\n", g_passes, g_failures);
    return g_failures ? 1 : 0;
}
