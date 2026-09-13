// Test harness for mod.wh.cpp. Compiles the mod source with Windhawk's
// WH_EDITING stubs and exercises it against real windows on the desktop.
//
// Usage: harness.exe [--dpi-unaware] [--no-input]
//   --dpi-unaware   don't opt into per-monitor DPI awareness
//   --no-input      skip the tests that inject mouse input
#include "../mod.wh.cpp"

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

static int g_sizeMoveEnter, g_sizeMoveExit, g_mouseMovesSeen;

static LRESULT CALLBACK TestWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ENTERSIZEMOVE:
            g_sizeMoveEnter++;
            break;
        case WM_EXITSIZEMOVE:
            g_sizeMoveExit++;
            break;
        case WM_MOUSEMOVE:
            g_mouseMovesSeen++;
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
        wc.lpszClassName = L"HyprFramelessTestWnd";
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

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"HyprFramelessTestWnd", title,
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
        RegisterWindowMessageW(L"HyprFrameless_local@hypr-frameless"),
        RegisterWindowMessageW(L"HyprFrameless_hypr-frameless"),
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
        found = wcsstr(me.szModule, L"hypr-frameless") != nullptr;
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

    CHECK(ParseMenuBarMode(L"hide") == MenuBarMode::Hide, "menu mode hide");
    CHECK(ParseMenuBarMode(L"keepMenu") == MenuBarMode::KeepMenu,
          "menu mode keepMenu");
    CHECK(ParseMenuBarMode(L"skip") == MenuBarMode::Skip, "menu mode skip");
    CHECK(ParseMenuBarMode(L"") == MenuBarMode::Hide, "menu mode default");

    RECT rc{100, 100, 300, 300};
    CHECK(ResizeEdgeForPoint(rc, {120, 120}) == WMSZ_TOPLEFT, "edge top-left");
    CHECK(ResizeEdgeForPoint(rc, {280, 120}) == WMSZ_TOPRIGHT,
          "edge top-right");
    CHECK(ResizeEdgeForPoint(rc, {120, 280}) == WMSZ_BOTTOMLEFT,
          "edge bottom-left");
    CHECK(ResizeEdgeForPoint(rc, {280, 280}) == WMSZ_BOTTOMRIGHT,
          "edge bottom-right");
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
    CHECK(!IsFrameless(hwnd), "BeforeUninit restored the window");
    RECT cli4 = ClientRectOnScreen(hwnd);
    CHECK(EqualRect(&cli4, &cli0), "BeforeUninit: original client rect");
    CHECK(GetWindowLongPtrW(hwnd, GWL_STYLE) == style0,
          "BeforeUninit: original style");

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
// hook does (retrieved but not dispatched), runs the mod's move/resize entry
// point on this thread (which blocks in the modal loop) while another thread
// drags the mouse and releases the button. With cancel=true, Esc is pressed
// before the button is released.
static void DragWith(bool right, HWND hwnd, POINT start, POINT delta,
                     void (*entry)(HWND, HWND, POINT), bool cancel = false) {
    SetCursorPos(start.x, start.y);
    Sleep(50);
    SendMouse(right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN);
    Sleep(50);
    MSG down;
    UINT downMsg = right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
    bool gotDown = PeekMessageW(&down, nullptr, downMsg, downMsg, PM_REMOVE);
    CHECK(gotDown, "button-down message retrieved before the drag");

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
    DWORD t0 = GetTickCount();
    entry(hwnd, hwnd, start);
    DWORD elapsed = GetTickCount() - t0;
    mover.join();
    printf("  modal loop ran for %lu ms\n", (unsigned long)elapsed);
    CHECK(elapsed >= 150, "the loop blocked until the button was released");
    CHECK(g_sizeMoveEnter == enter0 + 1 && g_sizeMoveExit == exit0 + 1,
          "WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE delivered once");
    CHECK(g_mouseMovesSeen == 0, "app saw no WM_MOUSEMOVE during the drag");
    CHECK(GetCapture() == nullptr, "mouse capture released");
    Pump(50);
}

static void TestMoveResize() {
    printf("\n== move / resize loops ==\n");

    POINT savedCursor;
    GetCursorPos(&savedCursor);

    HWND hwnd = CreateTestWindow(L"Hypr drag test", false, 160, 160);
    Pump(300);
    RequestFrameless(hwnd, kActionHide);
    Pump(300);
    SetForegroundWindow(hwnd);
    Pump(100);

    RECT before;
    GetWindowRect(hwnd, &before);
    POINT center{(before.left + before.right) / 2,
                 (before.top + before.bottom) / 2};

    // Move.
    DragWith(false, hwnd, center, {140, 90}, StartMove);
    Pump(100);
    RECT after;
    GetWindowRect(hwnd, &after);
    PrintRects("after move", hwnd);
    CHECK(after.left == before.left + 140 && after.top == before.top + 90,
          "window moved by the drag delta (%ld,%ld)", after.left - before.left,
          after.top - before.top);
    CHECK(after.right - after.left == before.right - before.left,
          "size unchanged by move");

    // Resize from the bottom-right quadrant, starting well inside the window.
    RECT r0 = after;
    POINT p{r0.right - 80, r0.bottom - 60};
    DragWith(true, hwnd, p, {100, 70}, StartResize);
    Pump(100);
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
    DragWith(true, hwnd, q, {-50, -30}, StartResize);
    Pump(100);
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
    Pump(400);
    RECT rMax;
    GetWindowRect(hwnd, &rMax);
    POINT m{rMax.left + 300, rMax.top + 60};
    DragWith(false, hwnd, m, {80, 120}, StartMove);
    Pump(200);
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
    DragWith(false, hwnd, c, {90, 60}, StartMove, /*cancel=*/true);
    Pump(100);
    RECT r6;
    GetWindowRect(hwnd, &r6);
    PrintRects("after cancelled move", hwnd);
    CHECK(EqualRect(&r6, &r5), "Esc restored the original position");
    DragWith(true, hwnd, {r5.right - 40, r5.bottom - 40}, {60, 60},
             StartResize, /*cancel=*/true);
    Pump(100);
    GetWindowRect(hwnd, &r6);
    CHECK(EqualRect(&r6, &r5), "Esc restored the original size");

    // Button-up bookkeeping: the loop consumed the release, nothing left.
    CHECK(!g_swallowButtonUp[0] && !g_swallowButtonUp[1],
          "no orphaned button-up flagged after complete drags");
    // A drag that can't start (fixed-size window resize) leaves the button
    // down, so its release must be swallowed later.
    LONG_PTR st = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, st & ~WS_THICKFRAME);
    SetCursorPos(c.x, c.y);
    Sleep(50);
    SendMouse(MOUSEEVENTF_RIGHTDOWN);
    Sleep(50);
    MSG rd;
    CHECK(PeekMessageW(&rd, nullptr, WM_RBUTTONDOWN, WM_RBUTTONDOWN, PM_REMOVE),
          "button-down message retrieved (no-op case)");
    StartResize(hwnd, hwnd, c);
    g_swallowButtonUp[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    CHECK(g_swallowButtonUp[1], "no-op resize flags the pending button-up");
    SendMouse(MOUSEEVENTF_RIGHTUP);
    Sleep(50);
    MSG ru;
    if (PeekMessageW(&ru, nullptr, WM_RBUTTONUP, WM_RBUTTONUP, PM_REMOVE)) {
        ProcessRetrievedMessage(&ru);
        CHECK(ru.message == WM_NULL, "orphaned button-up swallowed");
    } else {
        CHECK(false, "button-up message retrieved");
    }
    CHECK(!g_swallowButtonUp[1], "swallow flag cleared");
    SetWindowLongPtrW(hwnd, GWL_STYLE, st);

    // Modifier gate: without the modifier held, clicks pass through.
    MSG click{hwnd, WM_LBUTTONDOWN, 0, 0, 0, center};
    ProcessRetrievedMessage(&click);
    CHECK(click.message == WM_LBUTTONDOWN,
          "click without modifier passes through");

    SetCursorPos(savedCursor.x, savedCursor.y);
    DestroyWindow(hwnd);
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
        printf("note: the real hypr-frameless mod is injected into this "
               "process; test windows ask it to restore their title bars\n");
    }

    Wh_ModInit();  // registers the message, settings come from stubs
    g_settings.topEdgeResize = true;
    g_settings.dragModifier = DragModifier::Win;

    TestParsers();
    TestFramelessGeometry(false, MenuBarMode::Hide);
    TestFramelessGeometry(true, MenuBarMode::Hide);
    TestFramelessGeometry(true, MenuBarMode::KeepMenu);
    if (!noInput) {
        TestMoveResize();
    }

    printf("\n%d passed, %d failed\n", g_passes, g_failures);
    return g_failures ? 1 : 0;
}
