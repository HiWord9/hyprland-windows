// Test harness for the mod. Compiles the *bundled* mod (so the bundler itself
// is covered too) with Windhawk's WH_EDITING stubs and exercises it against
// real windows on the desktop. Run build.ps1, which bundles first.
//
// Usage: harness.exe [--dpi-unaware] [--no-input] [--desktops]
//   --dpi-unaware   don't opt into per-monitor DPI awareness
//   --no-input      skip the tests that inject mouse input
//   --desktops      only the Win+Tab tests
#include "../build/hyprland-windows.wh.cpp"

#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

// What the mod keeps to itself, asked of it by the tests.
static bool IsBorderFading(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_borderMutex);
    return g_borderFades.count(hwnd) != 0;
}

static bool IsDragFading(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_fadeMutex);
    return g_fades.count(hwnd) != 0;
}

static bool IsDragSnapping(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_snapMutex);
    return g_snaps.count(hwnd) != 0;
}

// The last press on this thread, forgotten: the next one starts afresh.
static void ForgetLastPress() {
    g_lastPressTick = 0;
    g_lastPressRoot = nullptr;
}

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
// An application that does work of its own before DefWindowProc gets around
// to starting the loop, which is every application with anything to do. The
// release that ends a resize can be on its way during that gap, and the test
// window can stand in for such an application by waiting here.
static std::atomic<int> g_sizeCommandDelayMs;

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
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_SIZE && g_sizeCommandDelayMs > 0) {
                // Pumped, not just waited: an application that gets around to
                // its own queue here - a nested loop, a COM call, DoEvents -
                // retrieves the release that was posted for the loop that has
                // not started yet, and the mod then swallows it as its own.
                DWORD end = GetTickCount() + g_sizeCommandDelayMs;
                while ((int)(end - GetTickCount()) > 0) {
                    MSG m;
                    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                        ProcessRetrievedMessage(&m);
                        TranslateMessage(&m);
                        DispatchMessageW(&m);
                    }
                    Sleep(5);
                }
            }
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
    std::wstring paint = L"mspaint.exe";
    CHECK(ProgramEntryMatches(L"mspaint.exe", paint), "program list: a name");
    CHECK(ProgramEntryMatches(L" MSPaint.EXE ", paint),
          "program list: case and spaces don't matter");
    CHECK(ProgramEntryMatches(L"C:\\Program Files\\Paint\\mspaint.exe", paint),
          "program list: a full path is taken by its file name");
    CHECK(!ProgramEntryMatches(L"paint.exe", paint) &&
              !ProgramEntryMatches(L"", paint),
          "program list: a different or empty name is no match");
    CHECK(!ThisProgramName().empty(), "this program has a name (%ls)",
          ThisProgramName().c_str());
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
    CHECK(ParseBorderColor(L"accent") == kColorAccent, "color accent");
    CHECK(ParseBorderColor(L" ACCENT ") == kColorAccent,
          "color ' ACCENT ' (trim + case)");
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


    CHECK(AnimationProgress(0, 100) == 0.0, "an animation starts at zero");
    CHECK(AnimationProgress(100, 100) == 1.0, "and is done at its duration");
    CHECK(AnimationProgress(150, 100) == 1.0, "past which it stays done");
    CHECK(AnimationProgress(0, 0) == 1.0, "a zero-length one is done at once");
    double halfway = AnimationProgress(50, 100);
    CHECK(halfway > 0.49 && halfway < 0.51, "halfway is halfway (%.3f)",
          halfway);
    CHECK(AnimationProgress(25, 100) < 0.25 &&
              AnimationProgress(75, 100) > 0.75,
          "and it eases in and out");

    CHECK(BlendColor(RGB(0, 0, 0), RGB(255, 255, 255), 0.0) == RGB(0, 0, 0),
          "a blend starts on the first color");
    CHECK(BlendColor(RGB(0, 0, 0), RGB(255, 255, 255), 1.0) ==
              RGB(255, 255, 255),
          "and ends on the second");
    CHECK(BlendColor(RGB(0, 0, 0), RGB(200, 100, 50), 0.5) ==
              RGB(100, 50, 25),
          "every channel on its own (0x%06lX)",
          BlendColor(RGB(0, 0, 0), RGB(200, 100, 50), 0.5));
    CHECK(IsBlendableColor(RGB(1, 2, 3)), "a color can be blended");
    CHECK(!IsBlendableColor(DWMWA_COLOR_NONE), "\"no border\" cannot");
    CHECK(!IsBlendableColor(kColorUntouched), "neither can \"untouched\"");
    CHECK(!IsBlendableColor((COLORREF)DWMWA_COLOR_DEFAULT),
          "nor the system default");

    // "accent" is a color to fade through, unlike the two states above: it is
    // resolved to a real one every time it is used.
    COLORREF accent = AccentBorderColor();
    printf("  the accent color here is 0x%06lX\n", accent);
    CHECK(IsBlendableColor(accent), "the accent color resolves to a color");
    COLORREF wasActive = g_settings.borderActive;
    g_settings.borderActive = kColorAccent;
    CHECK(BorderColorFor(true) == accent, "and is what an accent border gets");
    CHECK(IsBlendableColor(BorderColorFor(true)),
          "so a fade to or from it works like any other color");
    g_settings.borderActive = wasActive;

    RECT rc{100, 100, 300, 300};
    CHECK(ResizeEdgeForPoint(rc, {120, 120}) == WMSZ_TOPLEFT, "corner top-left");
    CHECK(ResizeEdgeForPoint(rc, {280, 120}) == WMSZ_TOPRIGHT,
          "corner top-right");
    CHECK(ResizeEdgeForPoint(rc, {120, 280}) == WMSZ_BOTTOMLEFT,
          "corner bottom-left");
    CHECK(ResizeEdgeForPoint(rc, {280, 280}) == WMSZ_BOTTOMRIGHT,
          "corner bottom-right");

    CHECK(ParseDragTranslucency(L"") == DragTranslucency::Both,
          "translucency defaults to both drags");
    CHECK(ParseDragTranslucency(L"fade") == DragTranslucency::Both,
          "translucency fade");
    CHECK(ParseDragTranslucency(L"move") == DragTranslucency::MoveOnly,
          "translucency move only");
    CHECK(ParseDragTranslucency(L"opaque") == DragTranslucency::Off,
          "translucency opaque");
    CHECK(ParseDragTranslucency(L" OFF ") == DragTranslucency::Off,
          "translucency off");

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
    g_settings.hotkey = Hotkey{'H', false, false, false, false};
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

    g_settings.hotkey = Hotkey{'H', false, false, false, false};

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

// Starts a resize and lets the button go straight away, which is the case
// where the release can beat the loop it is supposed to end. Returns false if
// the loop had to be broken from outside - the loop was still running, with
// no button held, two seconds after the release.
static bool ResizeAndReleaseAtOnce(HWND hwnd, POINT start, DWORD holdMs) {
    // With the window standing in for an application that is busy for a
    // moment before the loop starts - without that, the release is never
    // early enough for the race to happen at all.
    g_sizeCommandDelayMs = 60;
    SetCursorPos(start.x, start.y);
    Sleep(50);
    SendMouse(MOUSEEVENTF_RIGHTDOWN);
    Sleep(20);
    MSG down;
    PeekMessageW(&down, nullptr, WM_RBUTTONDOWN, WM_RBUTTONDOWN, PM_REMOVE);
    g_swallowButtonUp[1] = true;  // what the consumed press leaves behind
    RequestDrag(hwnd, kDragResize, start);

    int enter0 = g_sizeMoveEnter, exit0 = g_sizeMoveExit;
    std::atomic<bool> rescued{false};
    std::thread breaker([&] {
        Sleep(holdMs);
        SendMouse(MOUSEEVENTF_RIGHTUP);
        DWORD deadline = GetTickCount() + 2000;
        while (g_sizeMoveExit == exit0 && (int)(deadline - GetTickCount()) > 0) {
            Sleep(20);
        }
        if (g_sizeMoveExit == exit0 && g_sizeMoveEnter != enter0) {
            // Stuck to the cursor. End it the way a click would, so the rest
            // of the tests can still run.
            rescued = true;
            POINT pt;
            GetCursorPos(&pt);
            PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
        }
    });

    DWORD t0 = GetTickCount();
    for (;;) {
        PumpRaw(50);
        DWORD spent = GetTickCount() - t0;
        bool entered = g_sizeMoveEnter != enter0;
        if (g_sizeMoveExit != exit0 || spent > 4000 ||
            (!entered && spent > 600)) {
            break;
        }
    }
    breaker.join();
    PumpRaw(150);
    g_sizeCommandDelayMs = 0;
    printf("  loop entered=%d, had to be broken=%d\n",
           g_sizeMoveEnter != enter0, (int)rescued);
    g_swallowButtonUp[0] = g_swallowButtonUp[1] = false;
    g_pendingRelease = false;
    return !rescued;
}

// Windows reuses the IDs of threads that have ended. File Explorer opens each
// folder window on a thread of its own and ends it when the window closes, so
// a new window's thread often has the ID of one the mod hooked before - and
// that one's hooks went with it.
static void TestReusedThreadId() {
    printf("\n== a thread with a reused ID ==\n");

    DWORD firstId = 0;
    std::thread([&] {
        firstId = GetCurrentThreadId();
        InstallMessageHookForThread();
    }).join();

    bool reused = false, hooked = false;
    for (int i = 0; i < 5000 && !reused; i++) {
        std::thread([&] {
            if (GetCurrentThreadId() != firstId) {
                return;
            }
            reused = true;
            InstallMessageHookForThread();
            // The hook is what does the work: pumped without calling
            // ProcessRetrievedMessage, the hotkey hides the title bar only
            // if the hook is really there.
            HWND hwnd =
                CreateTestWindow(L"Hypr reused thread test", false, 200, 200);
            PumpRaw(200);
            PostMessageW(hwnd, WM_KEYDOWN, 'H', 1);
            PumpRaw(400);
            hooked = IsFrameless(hwnd);
            RequestFrameless(hwnd, kActionShow);
            PumpRaw(200);
            DestroyWindow(hwnd);
            PumpRaw(100);
        }).join();
    }
    if (!reused) {
        printf("  (no thread got the ID %lu again - nothing to check)\n",
               firstId);
        return;
    }
    CHECK(hooked, "a new thread with an old thread's ID gets a hook too");
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

    // Let go of the button the instant the resize starts. The release can
    // then be on its way before the loop exists, and a loop that never gets
    // one keeps following the cursor with no button held at all.
    RECT rq;
    GetWindowRect(hwnd, &rq);
    POINT corner{rq.right - 50, rq.bottom - 50};
    for (DWORD holdMs : {(DWORD)0, (DWORD)30, (DWORD)80}) {
        CHECK(ResizeAndReleaseAtOnce(hwnd, corner, holdMs),
              "a resize let go of after %lu ms ends on its own", holdMs);
    }

    // A window dims while the button is held, before it has moved at all: a
    // move loop does not count as a move/size loop until the cursor has gone
    // far enough to be a drag, and the fade cannot wait for that.
    SetCursorPos(rq.left + 140, rq.top + 140);
    PumpRaw(50);
    SendMouse(MOUSEEVENTF_LEFTDOWN);
    PumpRaw(30);
    BeginDragFade(hwnd, kDragMove);
    CHECK(IsDragFading(hwnd), "a move arms the fade");
    BYTE target = DragAlphaFor(255, g_settings.dragOpacity);
    BYTE dimmed = 255;
    bool sawLoop = false;
    for (int i = 0; i < 60 && dimmed > target; i++) {
        PumpRaw(20);
        sawLoop = sawLoop || IsInMoveSizeLoop();
        COLORREF k;
        BYTE a;
        DWORD f;
        if (GetLayeredWindowAttributes(hwnd, &k, &a, &f) && (f & LWA_ALPHA)) {
            dimmed = a;
        }
    }
    CHECK(!sawLoop, "with no move/size loop running at any point");
    CHECK(dimmed <= target, "the window dims on the press alone (alpha %d)",
          (int)dimmed);

    SendMouse(MOUSEEVENTF_LEFTUP);
    for (int i = 0; i < 40 && IsDragFading(hwnd); i++) {
        PumpRaw(50);
    }
    CHECK(!IsDragFading(hwnd), "and comes back when the button is let go of");
    CHECK(!(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED),
          "with the layered style off again");

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

    // The magnet, through a real drag. Against the work area rather than
    // another window, because that line is known exactly and no window
    // someone happens to have open can take the pull instead.
    g_settings.snap = SnapMode::Monitor;
    g_settings.snapMonitorGap = 0;
    UINT savedSnapModifier = g_settings.snapModifierVk;
    g_settings.snapModifierVk = 0;  // no key to hold: this is about the magnet
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT winBefore;
    GetWindowRect(hwnd, &winBefore);
    // Through the mod's own idea of the visible frame, which is the window
    // rectangle itself when DWM's numbers are in another coordinate space.
    RECT frameBefore = VisibleFrameOf(hwnd);
    POINT snapGrab{(winBefore.left + winBefore.right) / 2,
                   (winBefore.top + winBefore.bottom) / 2};
    // Aim its visible left edge a few pixels short of the work area's.
    POINT snapDelta{mi.rcWork.left + 6 - frameBefore.left, 0};
    DragWith(false, hwnd, snapGrab, snapDelta, kDragMove);
    PumpRaw(200);
    RECT frameAfter = VisibleFrameOf(hwnd);
    CHECK(frameAfter.left == mi.rcWork.left,
          "a dragged window sticks to the work area edge (%ld vs %ld)",
          frameAfter.left, mi.rcWork.left);
    CHECK(!IsDragSnapping(hwnd), "and the subclass comes off with the drag");
    g_settings.snap = SnapMode::Off;
    g_settings.snapModifierVk = savedSnapModifier;

    // The double-click gesture through the real message path, seen the way an
    // application sees it. With Alt as the modifier: holding Win here would
    // open the Start menu if anything about the mask went wrong, and that is
    // not what this is checking.
    g_settings.dragModifier = DragModifier::Alt;
    g_settings.doubleClickAction = WindowAction::ToggleMaximize;
    ForgetLastPress();
    RECT rGesture;
    GetWindowRect(hwnd, &rGesture);
    POINT mid{(rGesture.left + rGesture.right) / 2,
              (rGesture.top + rGesture.bottom) / 2};
    SetCursorPos(mid.x, mid.y);
    PumpRaw(50);

    INPUT alt{};
    alt.type = INPUT_KEYBOARD;
    alt.ki.wVk = VK_MENU;
    SendInput(1, &alt, sizeof(alt));
    std::thread clicker([] {
        Sleep(60);
        SendMouse(MOUSEEVENTF_LEFTDOWN);
        Sleep(60);
        SendMouse(MOUSEEVENTF_LEFTUP);
        Sleep(60);
        SendMouse(MOUSEEVENTF_LEFTDOWN);
        Sleep(60);
        SendMouse(MOUSEEVENTF_LEFTUP);
    });
    for (int i = 0; i < 14; i++) {
        PumpRaw(50);
    }
    clicker.join();
    alt.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &alt, sizeof(alt));
    PumpRaw(200);

    CHECK(IsZoomed(hwnd), "a double click with the modifier held maximized it");
    ShowWindow(hwnd, SW_RESTORE);
    PumpRaw(200);
    g_settings.dragModifier = DragModifier::Win;
    g_swallowButtonUp[0] = g_swallowButtonUp[1] = false;

    SetCursorPos(savedCursor.x, savedCursor.y);
    RemoveMessageHooks();
    DestroyWindow(hwnd);
    Pump(100);
}

////////////////////////////////////////////////////////////////////////////////
// The border color, and the fade between the two of them

// A message sent to a window, the way the hook on sent messages sees it.
static void SendThroughHook(HWND hwnd, UINT message, WPARAM wParam) {
    CWPSTRUCT sent{0, wParam, message, hwnd};
    CallWndProc(HC_ACTION, 0, (LPARAM)&sent);
}

static void TestBorderFade() {
    printf("\n== border color fade ==\n");

    const COLORREF kActive = RGB(0x33, 0xcc, 0xff);
    const COLORREF kInactive = RGB(0x20, 0x20, 0x20);
    g_settings.borderActive = kActive;
    g_settings.borderInactive = kInactive;
    g_settings.borderFadeDuration = 300;

    HWND hwnd = CreateTestWindow(L"Hypr border test", false, 220, 220);
    RequestFrameless(hwnd, kActionHide);
    Pump(300);
    CHECK(IsFrameless(hwnd), "the test window has a hidden title bar");
    CHECK(CurrentBorderColor(hwnd) != kColorUntouched,
          "and a border color of ours");
    CHECK(!IsBorderFading(hwnd),
          "taking the frame over sets the color outright");

    // Focus arriving crosses to the other color over time instead of
    // switching, and lands exactly on it.
    ApplyBorderColor(hwnd, false);
    CHECK(CurrentBorderColor(hwnd) == kInactive, "starting from inactive");
    OnWindowActivation(hwnd, true);
    CHECK(IsBorderFading(hwnd), "activation starts a fade");
    Sleep(120);
    COLORREF mid = CurrentBorderColor(hwnd);
    CHECK(mid != kInactive && mid != kActive,
          "which is somewhere in between on the way (0x%06lX)", mid);
    for (int i = 0; i < 40 && IsBorderFading(hwnd); i++) {
        Sleep(25);
    }
    CHECK(CurrentBorderColor(hwnd) == kActive,
          "and lands exactly on the active color (0x%06lX)",
          CurrentBorderColor(hwnd));

    // Focus leaving and coming back mid-fade turns the color around from
    // where it is, rather than jumping to the far end first.
    OnWindowActivation(hwnd, false);
    Sleep(100);
    COLORREF turning = CurrentBorderColor(hwnd);
    CHECK(turning != kActive && turning != kInactive,
          "a fade back starts where the color was (0x%06lX)", turning);
    OnWindowActivation(hwnd, true);
    Sleep(40);
    COLORREF returning = CurrentBorderColor(hwnd);
    CHECK(IsBorderFading(hwnd) && returning != kActive,
          "and is turned around again without a jump (0x%06lX)", returning);
    for (int i = 0; i < 40 && IsBorderFading(hwnd); i++) {
        Sleep(25);
    }
    CHECK(CurrentBorderColor(hwnd) == kActive,
          "landing on the color the last focus change asked for");

    // Instant when asked for.
    g_settings.borderFadeDuration = 0;
    OnWindowActivation(hwnd, false);
    CHECK(!IsBorderFading(hwnd) && CurrentBorderColor(hwnd) == kInactive,
          "the setting switches the color at once instead");
    g_settings.borderFadeDuration = 300;

    // And instant when there is nothing to fade through: "no border" and the
    // system default are states, not colors.
    ApplyBorderColor(hwnd, true);
    g_settings.borderInactive = DWMWA_COLOR_NONE;
    OnWindowActivation(hwnd, false);
    CHECK(!IsBorderFading(hwnd) &&
              CurrentBorderColor(hwnd) == (COLORREF)DWMWA_COLOR_NONE,
          "no border is not a color to fade to (0x%06lX)",
          CurrentBorderColor(hwnd));
    g_settings.borderInactive = kInactive;

    // An accent border is a real color by the time the fade sees it, so it
    // fades like any other - and it is resolved on the way, not when the
    // settings were read.
    g_settings.borderActive = kColorAccent;
    ApplyBorderColor(hwnd, false);
    // Read once, and compared against that: an accent color picked from the
    // wallpaper moves on its own, and the fade is heading for the one that
    // was there when focus changed.
    COLORREF accentThen = AccentBorderColor();
    OnWindowActivation(hwnd, true);
    CHECK(IsBorderFading(hwnd), "an accent border fades too");
    for (int i = 0; i < 40 && IsBorderFading(hwnd); i++) {
        Sleep(25);
    }
    CHECK(CurrentBorderColor(hwnd) == accentThen,
          "and lands on the accent color (0x%06lX, wanted 0x%06lX)",
          CurrentBorderColor(hwnd), accentThen);

    // The accent itself moving re-applies the color at once, without waiting
    // for focus to go anywhere. Both sides are the accent here, so it does
    // not matter which of them this window is entitled to.
    g_settings.borderInactive = kColorAccent;
    WriteBorderColor(hwnd, RGB(1, 2, 3));  // as if the accent had moved
    SendThroughHook(hwnd, WM_DWMCOLORIZATIONCOLORCHANGED, 0);
    CHECK(CurrentBorderColor(hwnd) == AccentBorderColor() &&
              !IsBorderFading(hwnd),
          "an accent change re-applies the border color at once");
    g_settings.borderActive = kActive;
    g_settings.borderInactive = kInactive;

    // A title bar coming back stops the fade: the restore writes the system
    // default, and a color landing after that would stay on the window.
    ApplyBorderColor(hwnd, false);
    OnWindowActivation(hwnd, true);
    CHECK(IsBorderFading(hwnd), "a fade is running");
    RequestFrameless(hwnd, kActionShow);
    Pump(200);
    CHECK(!IsFrameless(hwnd) && !IsBorderFading(hwnd),
          "restoring the title bar stops it");

    // So does the teardown, which waits for it before restoring anything.
    RequestFrameless(hwnd, kActionHide);
    Pump(300);
    ApplyBorderColor(hwnd, false);
    OnWindowActivation(hwnd, true);
    CHECK(IsBorderFading(hwnd), "a fade is running again");
    g_uninitializing = true;
    FinishBorderFades();
    CHECK(!IsBorderFading(hwnd), "and the teardown waits it out");
    g_uninitializing = false;

    RequestFrameless(hwnd, kActionShow);
    Pump(200);
    DestroyWindow(hwnd);
    Pump(100);
    g_settings.borderActive = kColorUntouched;
    g_settings.borderInactive = kColorUntouched;
    g_settings.borderFadeDuration = kDefaultBorderFade;
}

////////////////////////////////////////////////////////////////////////////////
// The Start menu mask, and where it lives
//
// The parts that need no Win key held. What happens at a real release - with
// the thread that armed it busy, or its process gone - is in
// test/mask_busy_probe.cpp, which pops the Start menu when it fails.

static void TestWinMask() {
    printf("\n== Start menu mask ==\n");

    ShutdownKeyboardServer();
    Sleep(100);
    CHECK(!IsShellProcess(), "the harness is not the shell");

    ArmWinMask(false);
    CHECK(!g_winMaskArmed, "a gesture without Win arms nothing");
    ArmWinMask(true);
    Sleep(100);
    CHECK(!g_winMaskArmed,
          "and one whose Win press is already over arms nothing either");

    // The server: a thread of its own with a window any process can find.
    HWND server = StartServer();
    CHECK(server != nullptr, "the mask server starts");
    DWORD pid = 0;
    DWORD serverThread = GetWindowThreadProcessId(server, &pid);
    CHECK(pid == GetCurrentProcessId() &&
              serverThread != GetCurrentThreadId(),
          "on a thread of its own, not the one that asked for it");
    CHECK(StartServer() == server, "and there is only ever one of them");
    bool findable = false;
    for (HWND found = nullptr; (found = FindWindowExW(
                                    HWND_MESSAGE, found, kKeyboardServerClass,
                                    nullptr));) {
        findable = findable || found == server;
    }
    CHECK(findable, "which another process can find by its class");

    // Another mask got to the release first, which is what a Win key-up of
    // our own looks like: stand down with it.
    g_winMaskArmed = true;
    KBDLLHOOKSTRUCT ours{};
    ours.vkCode = VK_LWIN;
    ours.dwExtraInfo = kInjectedMarker;
    LRESULT passed = LowLevelKeyboardProc(HC_ACTION, WM_KEYUP, (LPARAM)&ours);
    CHECK(passed == 0 && !g_winMaskArmed,
          "a release another mask made stands this one down");

    ShutdownKeyboardServer();
    for (int i = 0; i < 40 && IsWindow(server); i++) {
        Sleep(25);
    }
    CHECK(!IsWindow(server), "and the teardown takes the server away");
    {
        std::lock_guard<std::mutex> lock(g_serverMutex);
        CHECK(g_keyboardServer == nullptr && g_serverThreadId == 0,
              "leaving nothing behind for the next one to trip over");
        CHECK(g_keyboardHook == nullptr && g_foregroundHook == nullptr,
              "neither a keyboard hook nor a foreground subscription");
    }
}

////////////////////////////////////////////////////////////////////////////////
// Key bindings seen by the keyboard hook, ahead of Windows' own shortcuts

static std::wstring ForegroundProgram() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    WCHAR path[MAX_PATH] = L"?";
    DWORD len = MAX_PATH;
    if (h) {
        QueryFullProcessImageNameW(h, 0, path, &len);
        CloseHandle(h);
    }
    std::wstring s = path;
    return s.substr(s.find_last_of(L'\\') + 1);
}

static LRESULT KeyThroughHook(UINT vk, bool down) {
    KBDLLHOOKSTRUCT info{};
    info.vkCode = vk;
    return LowLevelKeyboardProc(HC_ACTION, down ? WM_KEYDOWN : WM_KEYUP,
                                (LPARAM)&info);
}

static void SendKey(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof(in));
}

// A key the mod's hooks let by, as they do its own mask: no copy of the mod
// running on this machine takes it.
static void SendMarkedKey(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    in.ki.dwExtraInfo = kInjectedMarker;
    SendInput(1, &in, sizeof(in));
}

static void TestKeyBindings(bool noInput) {
    printf("\n== key bindings through the keyboard hook ==\n");

    Hotkey savedShortcut = g_settings.windowShortcut;
    WindowAction savedAction = g_settings.windowShortcutAction;
    g_settings.windowShortcut = Hotkey{'W', false, false, false, true};
    g_settings.windowShortcutAction = WindowAction::ToggleMaximize;

    HWND hwnd = CreateTestWindow(L"Hypr key binding test", false, 280, 220);
    SetForegroundWindow(hwnd);
    Pump(300);

    CHECK(KeyThroughHook('W', true) == 0 && KeyThroughHook('W', false) == 0,
          "a plain W goes through untouched");

    if (!noInput) {
        // Win held for real, the way the hook sees it held.
        SendKey(VK_LWIN, false);
        Sleep(50);
        CHECK(KeyThroughHook('W', true) == 1, "Win+W is taken");
        CHECK(KeyThroughHook('W', true) == 1, "and so is its auto-repeat");
        CHECK(KeyThroughHook('W', false) == 1, "and its release");
        CHECK(KeyThroughHook('W', false) == 0, "but only that one release");
        SendKey(VK_LWIN, true);
        Pump(800);
        CHECK(IsZoomed(hwnd), "and the window in front did what it is bound to");
        std::wstring fg = ForegroundProgram();
        CHECK(_wcsicmp(fg.c_str(), L"StartMenuExperienceHost.exe") != 0 &&
                  _wcsicmp(fg.c_str(), L"SearchHost.exe") != 0,
              "with Start left shut after Win came up (%ls in front)",
              fg.c_str());
        ShowWindow(hwnd, SW_RESTORE);
        Pump(200);

        // With no window of that kind in front - a frameless popup stands in
        // for the desktop - the binding still keeps Windows' own shortcut
        // away, and does nothing.
        HWND popup = CreateWindowExW(
            0, L"STATIC", L"Hypr popup", WS_POPUP | WS_VISIBLE, 100, 100, 120,
            80, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetForegroundWindow(popup);
        Pump(200);
        CHECK(GetForegroundWindow() == popup && !IsFrameWindow(popup),
              "a window with no frame is in front");
        SendKey(VK_LWIN, false);
        Sleep(50);
        CHECK(KeyThroughHook('W', true) == 1 && KeyThroughHook('W', false) == 1,
              "and Win+W is still taken");
        SendKey(VK_LWIN, true);
        Pump(400);
        CHECK(IsWindow(popup) && !IsZoomed(hwnd),
              "with nothing done to any window");
        DestroyWindow(popup);
        SetForegroundWindow(hwnd);
        Pump(200);
    }

    // An elevated process keeps its hook while a window of its own is in
    // front, and only then.
    SetForegroundWindow(hwnd);
    Pump(200);
    FollowForeground(hwnd);
    CHECK(g_keepKeyboardHook && g_keyboardHook != nullptr,
          "a window of this process in front: the hook is in place");
    FollowForeground(FindWindowW(L"Shell_TrayWnd", nullptr));
    CHECK(!g_keepKeyboardHook && g_keyboardHook == nullptr,
          "someone else's window in front: it is gone again");

    g_settings.windowShortcut = savedShortcut;
    g_settings.windowShortcutAction = savedAction;
    DestroyWindow(hwnd);
    Pump(100);
}

////////////////////////////////////////////////////////////////////////////////
// Win+Tab through the virtual desktops

static GUID RegistryCurrentDesktop() {
    GUID g{};
    HKEY key;
    if (!RegOpenKeyExW(HKEY_CURRENT_USER,
                       L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer"
                       L"\\VirtualDesktops",
                       0, KEY_READ, &key)) {
        DWORD size = sizeof(g);
        RegQueryValueExW(key, L"CurrentVirtualDesktop", nullptr, nullptr,
                         (BYTE*)&g, &size);
        RegCloseKey(key);
    }
    return g;
}

static bool WaitForDesktop(bool changedFrom, GUID desktop, DWORD ms) {
    DWORD end = GetTickCount() + ms;
    while ((int)(end - GetTickCount()) > 0) {
        if ((RegistryCurrentDesktop() != desktop) == changedFrom) {
            return true;
        }
        Sleep(20);
    }
    return false;
}

static int DesktopIndex(GUID desktop) {
    GUID ids[32]{};
    DWORD size = sizeof(ids);
    HKEY key;
    if (!RegOpenKeyExW(HKEY_CURRENT_USER,
                       L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer"
                       L"\\VirtualDesktops",
                       0, KEY_READ, &key)) {
        RegQueryValueExW(key, L"VirtualDesktopIDs", nullptr, nullptr,
                         (BYTE*)ids, &size);
        RegCloseKey(key);
    }
    for (int i = 0; i < (int)(size / sizeof(GUID)); i++) {
        if (ids[i] == desktop) {
            return i + 1;
        }
    }
    return 0;
}

// What the shell's thread gets for Win+Tab: its hotkey's message.
static MSG WinTabHotkey(UINT modifiers = MOD_WIN) {
    MSG msg{};
    msg.message = WM_HOTKEY;
    msg.wParam = 11;
    msg.lParam = MAKELPARAM(modifiers, VK_TAB);
    return msg;
}

// Desktop changes, counted by a thread of their own while presses come.
struct DesktopWatch {
    std::atomic<bool> stop{false};
    std::atomic<int> changes{0};
    std::thread thread;
    void Start() {
        thread = std::thread([this] {
            GUID last = RegistryCurrentDesktop();
            while (!stop) {
                GUID now = RegistryCurrentDesktop();
                if (now != last) {
                    changes++;
                    last = now;
                }
                Sleep(2);
            }
        });
    }
    int Stop() {
        stop = true;
        thread.join();
        return changes;
    }
};

static std::string ForegroundClass() {
    char cls[64] = "";
    GetClassNameA(GetForegroundWindow(), cls, sizeof(cls));
    return GetForegroundWindow() == g_frontHolder ? "the mod's front holder" : cls;
}

static bool WindowOnCurrentDesktopInFront() {
    HWND fg = GetForegroundWindow();
    DWORD cloaked = 0;
    DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return fg && fg != g_frontHolder && !cloaked &&
           (IsFrameWindow(fg) || fg == GetShellWindow());
}

static bool WaitForFront(bool held, DWORD ms) {
    DWORD end = GetTickCount() + ms;
    while ((int)(end - GetTickCount()) > 0) {
        if ((GetForegroundWindow() == g_frontHolder) == held) {
            return true;
        }
        Sleep(10);
    }
    return false;
}

// An elevated window in front - Windhawk's own, for one - takes no keys from
// the harness: Windows keeps input sent from below away from it. The mod brings
// one forward like any other when it is on top of a desktop.
static bool ElevatedInFront() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return false;
    }
    bool elevated = true;  // a process whose token is not to be looked at
    HANDLE token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        elevated = GetTokenInformation(token, TokenElevation, &elevation,
                                       sizeof(elevation), &size) &&
                   elevation.TokenIsElevated;
        CloseHandle(token);
    }
    CloseHandle(process);
    return elevated;
}

static void NotCheckedForElevated(const char* what) {
    printf("  (not checked - an elevated window is in front, which takes no "
           "keys from the harness: %s)\n", what);
}

static void TestDesktops(bool noInput) {
    printf("\n== Win+Tab through the virtual desktops ==\n");
    if (noInput) {
        return;
    }
    g_settings.desktopWinTab = true;

    MSG winTab = WinTabHotkey();
    CHECK(!HandleDesktopHotkey(&winTab),
          "with no desktop thread, Win+Tab is left to the shell");
    // The harness stands in for the shell: the desktop thread runs here, and
    // the shell's Win+Tab is handed to it the way the shell's thread does.
    StartDesktopThread();
    CHECK(g_desktopThreadId != 0, "the desktop thread is up");
    // With only the one desktop, Win+Tab stays the shell's: Task View.
    GUID ids[32]{};
    DWORD idsSize = sizeof(ids);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops",
                 L"VirtualDesktopIDs", RRF_RT_REG_BINARY, nullptr, ids, &idsSize);
    int desktopCount = (int)(idsSize / sizeof(GUID));
    CHECK(SeveralDesktops() == (desktopCount > 1),
          "the desktops are counted where the shell keeps them (%d)", desktopCount);
    MSG other = WinTabHotkey(MOD_WIN | MOD_SHIFT);
    CHECK(!HandleDesktopHotkey(&other), "a hotkey other than Win+Tab is left alone");

    GUID home = RegistryCurrentDesktop();
    CHECK(HandleDesktopHotkey(&winTab), "Win+Tab is taken");
    if (!WaitForDesktop(true, home, 1000)) {
        printf("  (no other desktop with windows on it - switching not "
               "checked)\n");
    } else {
        CHECK(g_layout != nullptr,
              "it goes to another desktop, through the shell's own desktop "
              "manager");
        // A key that is not for switching brings the window on top forward
        // at once, ahead of the key itself; one held for a chord does not.
        Sleep(100);
        if (GetForegroundWindow() == g_frontHolder) {
            BringDesktopForwardForKey(VK_LSHIFT);
            CHECK(GetForegroundWindow() == g_frontHolder,
                  "Shift, which may be for the next switch, leaves the front "
                  "held");
            BringDesktopForwardForKey('A');
            for (int i = 0; i < 50 && !WindowOnCurrentDesktopInFront(); i++) {
                Sleep(10);
            }
            CHECK(WindowOnCurrentDesktopInFront(),
                  "any other key brings the window on top forward at once (%s)",
                  ForegroundClass().c_str());
        } else {
            printf("  (the front was not held here, by a window not the "
                   "shell's - keys not checked)\n");
        }
        WaitForFront(false, 1500);
        Sleep(400);

        // The rest goes through Win+Shift+Tab: a hotkey, the way Win+Tab is
        // one in the shell, and that is what lets the mod hold the front -
        // Windows lets whoever got the last input bring a window forward.
        // The harness holds it, or a copy of the mod in the shell does. Keys
        // stop reaching anything once an elevated window comes forward.
        auto pressTab = [] {
            SendMarkedKey(VK_TAB, false);
            Sleep(40);
            SendMarkedKey(VK_TAB, true);
        };
        bool keysReach = !ElevatedInFront();
        auto checkKeys = [&](bool ok, const char* what) {
            if (!ok && ElevatedInFront()) {
                NotCheckedForElevated(what);
                keysReach = false;
            } else {
                CHECK(ok, "%s", what);
            }
        };
        if (!keysReach) {
            NotCheckedForElevated("Win+Shift+Tab and the presses after it");
        }
        if (keysReach) {
            GUID before = RegistryCurrentDesktop();
            SendMarkedKey(VK_LWIN, false);
            SendMarkedKey(VK_SHIFT, false);
            pressTab();
            bool switched = WaitForDesktop(true, before, 1000);
            Sleep(50);
            // The mod's window holds the front now - unless the shell takes
            // it, which it does from a window that is not its own: the one in
            // the shell is, which test/desktops_in_shell.cpp sees to.
            printf("  in front while Win is down: %s\n", ForegroundClass().c_str());
            SendMarkedKey(VK_SHIFT, true);
            SendMarkedKey(VK_LWIN, true);
            checkKeys(switched, "Win+Shift+Tab goes the other way");
            bool broughtForward =
                WaitForFront(false, 1500) && WindowOnCurrentDesktopInFront();
            CHECK(broughtForward,
                  "and the window on top of that desktop brought forward once "
                  "it is up (%s)", ForegroundClass().c_str());
            keysReach = keysReach && !ElevatedInFront();
            Sleep(400);
        }

        if (keysReach) {
            // A burst with Win held: every press switches, at once.
            GUID before = RegistryCurrentDesktop();
            DesktopWatch watch;
            watch.Start();
            constexpr int kPresses = 12;
            SendMarkedKey(VK_LWIN, false);
            SendMarkedKey(VK_SHIFT, false);
            for (int i = 0; i < kPresses; i++) {
                pressTab();
                Sleep(70);
            }
            Sleep(200);
            printf("  in front after a burst, Win still down: %s\n",
                   ForegroundClass().c_str());
            SendMarkedKey(VK_SHIFT, true);
            SendMarkedKey(VK_LWIN, true);
            bool broughtForward = WaitForFront(false, 1500);
            Sleep(1000);  // anything the shell might still do
            int changes = watch.Stop();
            printf("  %d presses from desktop %d: %d desktop changes, ended on %d\n",
                   kPresses, DesktopIndex(before), changes,
                   DesktopIndex(RegistryCurrentDesktop()));
            // Whether the shell undoes any of them is for the shell to say,
            // and the harness is not the shell: that is what
            // test/desktops_in_shell.cpp is for. Here, only what the mod does.
            checkKeys(changes >= kPresses,
                      "a burst of presses with Win held: every press switched");
            CHECK(broughtForward && WindowOnCurrentDesktopInFront(),
                  "and once Win was up, the window on top was brought forward");
            keysReach = keysReach && !ElevatedInFront();
            Sleep(400);
        }

        if (keysReach) {
            // Presses each with a Win of its own, the way one taps
            // Win+Shift+Tab again and again.
            GUID before = RegistryCurrentDesktop();
            DesktopWatch tapWatch;
            tapWatch.Start();
            constexpr int kTaps = 8;
            for (int i = 0; i < kTaps; i++) {
                SendMarkedKey(VK_LWIN, false);
                SendMarkedKey(VK_SHIFT, false);
                pressTab();
                Sleep(40);
                SendMarkedKey(VK_SHIFT, true);
                SendMarkedKey(VK_LWIN, true);
                Sleep(150 + 60 * (i % 4));
            }
            WaitForFront(false, 1500);
            Sleep(1000);
            int changes = tapWatch.Stop();
            printf("  %d taps from desktop %d: %d desktop changes\n", kTaps,
                   DesktopIndex(before), changes);
            checkKeys(changes >= kTaps, "taps one after another: every one switched");
            CHECK(WindowOnCurrentDesktopInFront(),
                  "with the window on top brought forward after the last one");
            Sleep(400);
        }

        // Home again by the shell's Win+Tab, which needs no keys.
        for (int i = 0; i < 6 && RegistryCurrentDesktop() != home; i++) {
            GUID at = RegistryCurrentDesktop();
            HandleDesktopHotkey(&winTab);
            WaitForDesktop(true, at, 1000);
            WaitForFront(false, 1500);
            Sleep(400);
        }
        CHECK(RegistryCurrentDesktop() == home, "back where the harness started");
    }

    // Task View is on Win+Ctrl+Tab, which opens it and closes it again.
    auto pressCtrlWinTab = [] {
        SendMarkedKey(VK_LWIN, false);
        SendMarkedKey(VK_CONTROL, false);
        SendMarkedKey(VK_TAB, false);
        SendMarkedKey(VK_TAB, true);
        SendMarkedKey(VK_CONTROL, true);
        SendMarkedKey(VK_LWIN, true);
    };
    if (ElevatedInFront()) {
        NotCheckedForElevated("Win+Ctrl+Tab");
    } else {
        pressCtrlWinTab();
        bool opened = false;
        for (int i = 0; i < 150 && !opened; i++) {
            Sleep(10);
            opened = ShellViewUp();
        }
        CHECK(opened, "Win+Ctrl+Tab opens Task View");
        if (opened) {
            Sleep(500);
            CHECK(!HandleDesktopHotkey(&winTab),
                  "while it is up, Win+Tab is left to the shell, which closes it");
            pressCtrlWinTab();
            for (int i = 0; i < 150 && ShellViewUp(); i++) {
                Sleep(10);
            }
            CHECK(!ShellViewUp(), "and Win+Ctrl+Tab again closes it");
            Sleep(300);
        }
    }

    // With one of the shell's views up - a window of Task View's class
    // stands in for it - the desktops are left alone.
    WNDCLASSW viewClass{};
    viewClass.lpfnWndProc = DefWindowProcW;
    viewClass.hInstance = GetModuleHandleW(nullptr);
    viewClass.lpszClassName = L"XamlExplorerHostIslandWindow";
    RegisterClassW(&viewClass);
    HWND view = CreateWindowExW(WS_EX_TOOLWINDOW, L"XamlExplorerHostIslandWindow",
                                L"Task View", WS_POPUP | WS_VISIBLE, 0, 0, 1, 1,
                                nullptr, nullptr, viewClass.hInstance, nullptr);
    Pump(100);
    CHECK(ShellViewUp(), "a shell view is seen to be up");
    CHECK(!HandleDesktopHotkey(&winTab), "and Win+Tab is left to the shell");
    GUID before = RegistryCurrentDesktop();
    PostThreadMessageW(g_desktopThreadId, WM_APP + 2, 1, 0);
    CHECK(!WaitForDesktop(true, before, 800),
          "and a step that comes in all the same switches nothing");
    DestroyWindow(view);
    UnregisterClassW(L"XamlExplorerHostIslandWindow", viewClass.hInstance);
    Pump(100);
    CHECK(!ShellViewUp(), "and once it has gone, the desktops are free again");

    // The real Task View, opened through the shell: its windows are not
    // listed where the harness's own are, and it is in front a moment before
    // it is shown.
    ShellExecuteW(nullptr, L"open",
                  L"shell:::{3080F90E-D7AD-11D9-BD98-0000947B0257}", nullptr,
                  nullptr, SW_SHOWNORMAL);
    bool inFront = false, seenInFront = true, shown = false;
    for (int i = 0; i < 300 && !shown; i++) {
        HWND fg = GetForegroundWindow();
        WCHAR cls[64] = L"";
        if (GetClassNameW(fg, cls, ARRAYSIZE(cls)) &&
            !wcscmp(cls, L"XamlExplorerHostIslandWindow")) {
            inFront = true;
            seenInFront = seenInFront && ShellViewUp();
            shown = IsWindowVisible(fg);
        }
        Sleep(5);
    }
    if (!inFront) {
        printf("  (Task View did not come up - not checked)\n");
    } else {
        CHECK(seenInFront && shown,
              "the real Task View is seen, from the moment it is in front");
        SendKey(VK_ESCAPE, false);
        SendKey(VK_ESCAPE, true);
        for (int i = 0; i < 100 && ShellViewUp(); i++) {
            Sleep(20);
        }
        CHECK(!ShellViewUp(), "and no longer once it has closed");
    }

    auto previousHotkeyFree = [] {
        if (!RegisterHotKey(nullptr, 1, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT,
                            VK_TAB)) {
            return false;
        }
        UnregisterHotKey(nullptr, 1);
        return true;
    };

    // Turned off in the settings, Win+Tab is the shell's again and the
    // mod's two hotkeys are let go of; back on, they are taken again.
    g_settings.desktopWinTab = false;
    DesktopSettingsChanged();
    Sleep(200);
    CHECK(!HandleDesktopHotkey(&winTab), "turned off, Win+Tab is left to the shell");
    bool freeWhenOff = previousHotkeyFree();
    g_settings.desktopWinTab = true;
    DesktopSettingsChanged();
    Sleep(200);
    if (freeWhenOff) {
        CHECK(!previousHotkeyFree(),
              "and Win+Shift+Tab is free while it is off, and taken back after");
    } else {
        printf("  (Win+Shift+Tab is held by a copy of the mod in the shell)\n");
    }

    bool heldBefore = !previousHotkeyFree();
    ShutdownDesktopThread();
    for (int i = 0; i < 40 && g_desktopThreadId; i++) {
        Sleep(25);
    }
    CHECK(!g_desktopThreadId && !g_desktopManager && !g_internalManager &&
              !g_frontHolder,
          "the desktop thread goes, and lets go of the shell and its window");
    if (previousHotkeyFree()) {
        CHECK(heldBefore, "and of the hotkeys it held");
    } else {
        printf("  (Win+Shift+Tab is held by a copy of the mod in the shell)\n");
    }
}

////////////////////////////////////////////////////////////////////////////////
// Which windows the border colors reach

static void TestBorderScope() {
    printf("\n== border color scope ==\n");

    const COLORREF kActive = RGB(0x11, 0x99, 0x44);
    g_settings.borderActive = kActive;
    g_settings.borderInactive = RGB(0x22, 0x22, 0x22);
    g_settings.borderFramelessOnly = false;

    HWND plain = CreateTestWindow(L"Hypr border scope", false, 280, 200);
    CHECK(!IsFrameless(plain), "the window still has its title bar");
    CHECK(IsBorderColorTarget(plain),
          "and the colors apply to it all the same");

    OnWindowActivation(plain, true);
    for (int i = 0; i < 40 && IsBorderFading(plain); i++) {
        Sleep(25);
    }
    CHECK(CurrentBorderColor(plain) == kActive,
          "focus paints it the active color (0x%06lX)",
          CurrentBorderColor(plain));

    // Only the windows the mod has taken the title bar from, when asked.
    g_settings.borderFramelessOnly = true;
    CHECK(!IsBorderColorTarget(plain), "the setting takes it back out again");
    RefreshBorderColors();
    CHECK(CurrentBorderColor(plain) == kColorUntouched,
          "and its border goes back to the system's own");
    OnWindowActivation(plain, false);
    CHECK(CurrentBorderColor(plain) == kColorUntouched,
          "with focus no longer painting it either");

    RequestFrameless(plain, kActionHide);
    Pump(300);
    CHECK(IsFrameless(plain) && IsBorderColorTarget(plain),
          "a window with its title bar hidden is still one of them");
    OnWindowActivation(plain, true);
    for (int i = 0; i < 40 && IsBorderFading(plain); i++) {
        Sleep(25);
    }
    CHECK(CurrentBorderColor(plain) == kActive, "and is painted again");

    // The title bar coming back takes the color with it, in this mode.
    RequestFrameless(plain, kActionShow);
    Pump(300);
    CHECK(!IsFrameless(plain) && CurrentBorderColor(plain) == kColorUntouched,
          "and loses it when the title bar comes back");

    // The teardown hands every colored window back, wherever it came from.
    g_settings.borderFramelessOnly = false;
    OnWindowActivation(plain, true);
    for (int i = 0; i < 40 && IsBorderFading(plain); i++) {
        Sleep(25);
    }
    CHECK(CurrentBorderColor(plain) == kActive, "painted once more");
    CHECK(SnapshotColoredWindows().size() >= 1, "and counted as painted");
    RestoreAllBorderColors();
    CHECK(CurrentBorderColor(plain) == kColorUntouched &&
              SnapshotColoredWindows().empty(),
          "which the teardown undoes for all of them at once");

    // A window that goes away is forgotten about.
    OnWindowActivation(plain, true);
    for (int i = 0; i < 40 && IsBorderFading(plain); i++) {
        Sleep(25);
    }
    CHECK(CurrentBorderColor(plain) != kColorUntouched, "painted again");
    SendThroughHook(plain, WM_NCDESTROY, 0);
    CHECK(CurrentBorderColor(plain) == kColorUntouched,
          "and dropped when the window is destroyed");

    g_settings.borderActive = kColorUntouched;
    g_settings.borderInactive = kColorUntouched;
    CHECK(!BorderColorsWanted(),
          "with no colors set there is nothing to listen for");
    RestoreAllBorderColors();
    DestroyWindow(plain);
    Pump(100);
}

////////////////////////////////////////////////////////////////////////////////
// Magnetic edges, and the shape kept while resizing
//
// On a made-up desktop: a neighbour and a work area, with no real windows
// involved, so the rules can be checked exactly rather than against whatever
// happens to be on screen.

static void TestSnapGeometry() {
    printf("\n== magnetic edges ==\n");

    CHECK(ParseSnapMode(L"") == SnapMode::Both, "snap defaults to both");
    CHECK(ParseSnapMode(L"windows") == SnapMode::Windows, "snap windows only");
    CHECK(ParseSnapMode(L"monitor") == SnapMode::Monitor, "snap monitor only");
    CHECK(ParseSnapMode(L"none") == SnapMode::Off, "snap none");
    CHECK(ParseModifierVk(L"", VK_SHIFT) == VK_SHIFT,
          "the shape key defaults to Shift");
    CHECK(ParseModifierVk(L"ctrl", VK_SHIFT) == VK_CONTROL, "shape key ctrl");
    CHECK(ParseModifierVk(L"none", VK_SHIFT) == 0, "shape key off");

    std::vector<int> lines{100, 300};
    CHECK(SnappedEdge(104, lines, 12) == 100, "an edge takes the nearest line");
    CHECK(SnappedEdge(296, lines, 12) == 300, "from either side of it");
    CHECK(SnappedEdge(150, lines, 12) == 150, "and stays put when none is near");

    RECT inset{9, 0, -9, -9};
    RECT window{100, 100, 500, 400};
    RECT frame = WindowToFrame(window, inset);
    CHECK(frame.left == 109 && frame.top == 100 && frame.right == 491 &&
              frame.bottom == 391,
          "the visible frame sits inside the window rectangle");
    RECT back = FrameToWindow(frame, inset);
    CHECK(EqualRect(&back, &window), "and converts back to it exactly");

    SnapMode savedMode = g_settings.snap;
    int savedWindowGap = g_settings.snapWindowGap;
    int savedMonitorGap = g_settings.snapMonitorGap;
    g_settings.snap = SnapMode::Both;
    g_settings.snapWindowGap = 0;
    g_settings.snapMonitorGap = 0;

    SnapState desktop;
    desktop.windows.push_back(RECT{500, 200, 900, 600});
    desktop.monitors.push_back(RECT{0, 0, 1920, 1040});

    RECT moved{294, 300, 494, 500};  // its right edge 6 short of the neighbour
    SnapMovedFrame(desktop, 12, &moved);
    CHECK(moved.right == 500 && moved.left == 300,
          "a moved window lines up with the one beside it (%ld,%ld)", moved.left,
          moved.right);

    g_settings.snapWindowGap = 8;
    RECT gapped{294, 300, 494, 500};
    SnapMovedFrame(desktop, 12, &gapped);
    CHECK(gapped.right == 492, "and leaves the gap it is asked for (%ld)",
          gapped.right);
    g_settings.snapWindowGap = 0;

    RECT apart{294, 30, 494, 120};  // beside it on paper, but nowhere near it
    SnapMovedFrame(desktop, 12, &apart);
    CHECK(apart.right == 494,
          "a window that does not overlap it is left alone (%ld)", apart.right);

    RECT distant{200, 300, 400, 500};
    SnapMovedFrame(desktop, 12, &distant);
    CHECK(distant.right == 400, "and so is one that is simply too far away");

    RECT beside{294, 206, 494, 406};  // put next to it, its top 6 lower
    SnapMovedFrame(desktop, 12, &beside);
    CHECK(beside.right == 500 && beside.top == 200,
          "one put beside it lines up top to top (%ld,%ld)", beside.right,
          beside.top);

    RECT below{506, 606, 706, 806};  // under it, both edges a little off
    SnapMovedFrame(desktop, 12, &below);
    CHECK(below.left == 500 && below.top == 600,
          "and one put under it, left to left (%ld,%ld)", below.left,
          below.top);

    g_settings.snap = SnapMode::Monitor;
    RECT edge{6, 500, 206, 700};
    SnapMovedFrame(desktop, 12, &edge);
    CHECK(edge.left == 0 && edge.right == 200,
          "the edge of the screen pulls it too (%ld)", edge.left);
    RECT neighbourOnly{294, 300, 494, 500};
    SnapMovedFrame(desktop, 12, &neighbourOnly);
    CHECK(neighbourOnly.right == 494,
          "and with windows switched off, another window does not");

    // A resize moves the edges being pulled and no others.
    g_settings.snap = SnapMode::Both;
    RECT sized{100, 300, 494, 500};
    SnapSizedFrame(desktop, WMSZ_RIGHT, 12, &sized);
    CHECK(sized.right == 500 && sized.left == 100,
          "a resize sticks the edge being pulled (%ld,%ld)", sized.left,
          sized.right);
    RECT other{494, 300, 800, 500};
    SnapSizedFrame(desktop, WMSZ_RIGHT, 12, &other);
    CHECK(other.left == 494, "and leaves the one that is not (%ld)", other.left);

    // The shape, when the key for it is held.
    RECT start{0, 0, 400, 200};  // two to one
    RECT corner{0, 0, 600, 260};
    ApplyAspectRatio(WMSZ_BOTTOMRIGHT, start, &corner);
    // The nearest size on the shape's own line to the 600x260 asked for,
    // rather than the one axis or the other taken whole.
    CHECK(corner.right == 584 && corner.bottom == 292,
          "a corner takes the nearest size that keeps the shape (%ldx%ld)",
          corner.right - corner.left, corner.bottom - corner.top);
    CHECK(corner.left == 0 && corner.top == 0,
          "with the corner that is not being held left where it was");

    RECT side{-100, 0, 400, 200};
    ApplyAspectRatio(WMSZ_LEFT, start, &side);
    CHECK(side.left == -100 && side.right == 400 && side.bottom == 250,
          "a side takes the other axis with it (%ldx%ld)",
          side.right - side.left, side.bottom - side.top);

    RECT tall{0, 0, 400, 400};
    ApplyAspectRatio(WMSZ_TOP, start, &tall);
    CHECK(tall.right == 800 && tall.top == 0,
          "and so does the top, from the bottom edge (%ldx%ld)",
          tall.right - tall.left, tall.bottom - tall.top);

    // The size has to move smoothly as the cursor does. Deciding it by
    // whichever axis moved further reads the same most of the time and jumps
    // wherever the two swap places, which is a diagonal line right through
    // the middle of where a corner drag goes.
    int worstJump = 0;
    double worstShape = 2.0;
    RECT previous{};
    for (int step = 0; step <= 40; step++) {
        // A path that crosses that diagonal: mostly downward at first, then
        // mostly rightward.
        RECT proposed{0, 0, 400 + step * 5, 200 + (40 - step) * 5};
        ApplyAspectRatio(WMSZ_BOTTOMRIGHT, start, &proposed);
        double shape = (double)(proposed.right - proposed.left) /
                       (proposed.bottom - proposed.top);
        if (abs(shape - 2.0) > abs(worstShape - 2.0)) {
            worstShape = shape;
        }
        if (step > 0) {
            worstJump = std::max(
                worstJump, (int)abs((proposed.right - proposed.left) -
                                    (previous.right - previous.left)));
        }
        previous = proposed;
    }
    CHECK(worstShape > 1.98 && worstShape < 2.02,
          "the shape is kept at every step of a corner drag (%.3f)",
          worstShape);
    CHECK(worstJump <= 12,
          "and the size follows the cursor without jumping (worst %d px)",
          worstJump);

    // The extra key that decides whether the magnet applies at all.
    UINT savedSnapVk = g_settings.snapModifierVk;
    bool savedHold = g_settings.snapModifierHold;
    CHECK(ParseSnapModifierHold(L""), "the extra key holds by default");
    CHECK(ParseSnapModifierHold(L"hold"), "extra key hold");
    CHECK(!ParseSnapModifierHold(L"suppress"), "extra key suppress");
    g_settings.snap = SnapMode::Both;
    g_settings.snapModifierVk = VK_CONTROL;
    g_settings.snapModifierHold = true;
    CHECK(SnapAllowedWith(true) && !SnapAllowedWith(false),
          "the magnet waits for the extra key when it is set to hold");
    g_settings.snapModifierHold = false;
    CHECK(!SnapAllowedWith(true) && SnapAllowedWith(false),
          "and the key turns it off instead when it is set to suppress");
    g_settings.snapModifierVk = 0;
    CHECK(SnapAllowedWith(false) && SnapAllowedWith(true),
          "with no extra key it is always on");
    g_settings.snap = SnapMode::Off;
    CHECK(!SnapAllowedWith(true), "unless it is switched off altogether");
    g_settings.snapModifierVk = savedSnapVk;
    g_settings.snapModifierHold = savedHold;

    g_settings.snap = savedMode;
    g_settings.snapWindowGap = savedWindowGap;
    g_settings.snapMonitorGap = savedMonitorGap;
}

////////////////////////////////////////////////////////////////////////////////
// Gestures: what a double click is, and what an action does

static void TestGestures() {
    printf("\n== gestures ==\n");

    CHECK(ParseWindowAction(L"", WindowAction::ToggleMaximize) ==
              WindowAction::ToggleMaximize,
          "the default action is maximize");
    CHECK(ParseWindowAction(L"maximize", WindowAction::ToggleMaximize) ==
              WindowAction::ToggleMaximize,
          "action maximize");
    CHECK(ParseWindowAction(L"titleBar", WindowAction::ToggleMaximize) ==
              WindowAction::ToggleTitleBar,
          "action titleBar");
    CHECK(ParseWindowAction(L"close", WindowAction::ToggleMaximize) ==
              WindowAction::Close, "action close");
    CHECK(ParseWindowAction(L"none", WindowAction::ToggleMaximize) ==
              WindowAction::None, "action none");
    CHECK(ParseWindowAction(L" OFF ", WindowAction::ToggleMaximize) ==
              WindowAction::None, "action off");

    // A binding is a key or a mouse button, with the same parser behind both.
    Hotkey mid = ParseHotkey(L"", kDefaultWindowShortcut);
    CHECK(mid.vk == VK_MBUTTON && mid.win && !mid.ctrl && !mid.alt,
          "an unwritten shortcut means Win+MButton");
    CHECK(ParseHotkey(L"Alt+XButton1", kDefaultWindowShortcut).vk ==
              VK_XBUTTON1,
          "shortcut Alt+XButton1");
    CHECK(ParseHotkey(L"Win+Q", kDefaultWindowShortcut).vk == 'Q',
          "shortcut Win+Q");
    CHECK(ParseHotkey(L"none", kDefaultWindowShortcut).vk == 0,
          "shortcut none turns it off");
    CHECK(IsMouseButtonVk(VK_MBUTTON) && IsMouseButtonVk(VK_XBUTTON2),
          "the mouse buttons are known as buttons");
    CHECK(!IsMouseButtonVk('Q'), "and a key is not one");

    CHECK(ButtonVkForMessage(WM_MBUTTONDOWN, 0) == VK_MBUTTON,
          "a middle press is the middle button");
    CHECK(ButtonVkForMessage(WM_NCMBUTTONUP, HTCAPTION) == VK_MBUTTON,
          "and so is one on the frame");
    CHECK(ButtonVkForMessage(WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON2)) ==
              VK_XBUTTON2,
          "the side buttons are told apart");
    CHECK(ButtonVkForMessage(WM_NCXBUTTONDOWN, MAKEWPARAM(HTCLIENT, XBUTTON1)) ==
              VK_XBUTTON1,
          "on the frame as well");
    CHECK(ButtonVkForMessage(WM_MOUSEMOVE, 0) == 0,
          "and a mouse move is no button at all");

    // The whole button path, as an application's message pump sees it. No
    // modifier is held here, so a bare middle click is left alone.
    HWND target = CreateTestWindow(L"Hypr shortcut test", false, 300, 240);
    Hotkey savedShortcut = g_settings.windowShortcut;
    WindowAction savedAction = g_settings.windowShortcutAction;
    g_settings.windowShortcut = Hotkey{VK_MBUTTON, false, false, false, false};
    g_settings.windowShortcutAction = WindowAction::ToggleMaximize;

    MSG mdown{target, WM_MBUTTONDOWN, 0, 0, 0, {}};
    ProcessRetrievedMessage(&mdown);
    CHECK(mdown.message == WM_NULL, "a bound middle press is taken");
    MSG mup{target, WM_MBUTTONUP, 0, 0, 0, {}};
    ProcessRetrievedMessage(&mup);
    CHECK(mup.message == WM_NULL, "and so is the release that follows it");
    MSG mup2{target, WM_MBUTTONUP, 0, 0, 0, {}};
    ProcessRetrievedMessage(&mup2);
    CHECK(mup2.message == WM_MBUTTONUP, "but only that one release");
    Pump(400);
    CHECK(IsZoomed(target), "and the action ran on the window under it");

    g_settings.windowShortcut = Hotkey{VK_MBUTTON, true, false, false, false};
    MSG bare{target, WM_MBUTTONDOWN, 0, 0, 0, {}};
    ProcessRetrievedMessage(&bare);
    CHECK(bare.message == WM_MBUTTONDOWN,
          "a press without the modifiers passes through");
    g_settings.windowShortcutAction = WindowAction::None;
    g_settings.windowShortcut = Hotkey{VK_MBUTTON, false, false, false, false};
    MSG off{target, WM_MBUTTONDOWN, 0, 0, 0, {}};
    ProcessRetrievedMessage(&off);
    CHECK(off.message == WM_MBUTTONDOWN, "so does one with no action bound");

    g_settings.windowShortcut = savedShortcut;
    g_settings.windowShortcutAction = savedAction;
    DestroyWindow(target);
    Pump(100);

    int savedTime = g_settings.doubleClickTime;
    g_settings.doubleClickTime = 0;
    CHECK(DoubleClickTimeMs() == (int)GetDoubleClickTime(),
          "zero follows the Windows double-click speed (%d ms)",
          DoubleClickTimeMs());
    g_settings.doubleClickTime = 250;
    CHECK(DoubleClickTimeMs() == 250, "a time of its own is used as it is");

    // The recognizer runs on given ticks, so its rules can be checked without
    // waiting out a double-click time for each of them.
    HWND one = (HWND)0x1111, other = (HWND)0x2222;
    POINT pt{500, 500};
    ForgetLastPress();
    CHECK(!IsDoubleClickAt(one, pt, 1000), "one press is not a double click");
    CHECK(IsDoubleClickAt(one, pt, 1200),
          "a second one soon after and in the same place is");
    CHECK(!IsDoubleClickAt(one, pt, 1300),
          "a third click starts over instead of counting again");

    ForgetLastPress();
    IsDoubleClickAt(one, pt, 2000);
    CHECK(!IsDoubleClickAt(one, pt, 2251), "too late is not a double click");

    ForgetLastPress();
    IsDoubleClickAt(one, pt, 3000);
    POINT away{pt.x + GetSystemMetrics(SM_CXDOUBLECLK), pt.y};
    CHECK(!IsDoubleClickAt(one, away, 3100), "and neither is too far away");

    ForgetLastPress();
    IsDoubleClickAt(one, pt, 4000);
    CHECK(!IsDoubleClickAt(other, pt, 4100),
          "nor a click that lands on another window");

    ForgetLastPress();
    IsDoubleClickAt(one, pt, 5000);
    ForgetLastPress();
    CHECK(!IsDoubleClickAt(one, pt, 5100), "a forgotten press counts for none");
    g_settings.doubleClickTime = savedTime;

    // And the actions themselves, asked for the way a gesture asks: posted to
    // the window, carried out on its own thread.
    HWND hwnd = CreateTestWindow(L"Hypr gesture test", false, 260, 260);
    CHECK(!IsZoomed(hwnd), "the window starts out restored");
    RequestWindowAction(hwnd, WindowAction::ToggleMaximize);
    Pump(400);
    CHECK(IsZoomed(hwnd), "the maximize action maximizes it");
    RequestWindowAction(hwnd, WindowAction::ToggleMaximize);
    Pump(400);
    CHECK(!IsZoomed(hwnd), "and the same action restores it again");

    RequestWindowAction(hwnd, WindowAction::ToggleTitleBar);
    Pump(400);
    CHECK(IsFrameless(hwnd), "the title bar action hides the title bar");
    RequestWindowAction(hwnd, WindowAction::ToggleTitleBar);
    Pump(400);
    CHECK(!IsFrameless(hwnd), "and brings it back");

    RequestWindowAction(hwnd, WindowAction::Close);
    Pump(400);
    CHECK(!IsWindow(hwnd), "the close action closes the window");
    Pump(100);
}

////////////////////////////////////////////////////////////////////////////////
// The release that ends a resize

static void TestResizeRelease() {
    printf("\n== resize release ==\n");

    HWND hwnd = CreateTestWindow(L"Hypr release test", false, 260, 200);

    // The watcher thread with no loop to end and no button held - which is
    // what a resize looks like for the moment between the request and the
    // loop it starts. A release posted in that moment is lost, or is
    // retrieved by the application and swallowed as the mod's own, and the
    // loop is then left following the cursor with nothing to stop it.
    int refs0 = g_modRefCount;
    CHECK(StartResizeRelease(hwnd), "the resize watcher starts");
    MSG up;
    bool posted = false;
    for (int i = 0; i < 12 && !posted; i++) {
        Sleep(25);
        posted = PeekMessageW(&up, hwnd, WM_LBUTTONUP, WM_LBUTTONUP,
                              PM_REMOVE) != 0;
    }
    CHECK(!posted, "and posts no release while no loop is running");

    // It gives up after kMoveSizeStartWaitMs and lets the image go.
    for (int i = 0; i < 40 && g_modRefCount > refs0; i++) {
        Sleep(50);
    }
    CHECK(g_modRefCount == refs0, "and lets go of the mod when it gives up");
    CHECK(!PeekMessageW(&up, hwnd, WM_LBUTTONUP, WM_LBUTTONUP, PM_REMOVE),
          "with nothing posted on the way out either");

    // A flag left over from a resize whose loop never started must not eat
    // the release of the next click.
    g_pendingRelease = true;
    POINT pt{300, 240};
    MSG down{hwnd, WM_LBUTTONDOWN, 0, 0, 0, pt};
    ProcessRetrievedMessage(&down);
    CHECK(!g_pendingRelease, "a fresh press clears a stale pending release");
    MSG release{hwnd, WM_LBUTTONUP, 0, 0, 0, pt};
    ProcessRetrievedMessage(&release);
    CHECK(release.message == WM_LBUTTONUP, "so the release passes through");

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
    BeginDragFade(hwnd, kDragMove);
    CHECK(IsDragFading(hwnd), "a drag arms the fade");
    CHECK(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED,
          "and makes the window layered");
    CHECK(GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags) &&
              (flags & LWA_ALPHA),
          "with an alpha of its own to animate (0x%lX)", flags);

    // No button is held here, so the drag is over before it began and the
    // window's thread - this one - is asked to take the style back off.
    for (int i = 0; i < 20 && IsDragFading(hwnd); i++) {
        Pump(100);
    }
    CHECK(!IsDragFading(hwnd), "a drag with no button held ends the fade");
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
    BeginDragFade(translucent, kDragMove);
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
    BeginDragFade(perPixel, kDragMove);
    CHECK(!IsDragFading(perPixel),
          "which the fade stays out of - the app may be about to paint it");
    PaintPerPixel(perPixel);
    CHECK(!GetLayeredWindowAttributes(perPixel, &key, &alpha, &flags),
          "and once it has, there is nothing to read at all");
    BeginDragFade(perPixel, kDragMove);
    CHECK(!IsDragFading(perPixel), "which the fade stays out of as well");
    DestroyWindow(perPixel);
    Pump(100);

    HWND modes = CreateTestWindow(L"Hypr fade test 4", false, 320, 320);
    g_settings.dragTranslucency = DragTranslucency::Off;
    BeginDragFade(modes, kDragMove);
    CHECK(!IsDragFading(modes), "the setting turns the fade off");

    g_settings.dragTranslucency = DragTranslucency::MoveOnly;
    BeginDragFade(modes, kDragResize);
    CHECK(!IsDragFading(modes), "move-only leaves a resize opaque");

    g_settings.dragTranslucency = DragTranslucency::Both;
    g_settings.dragOpacity = 100;
    BeginDragFade(modes, kDragMove);
    CHECK(!IsDragFading(modes), "and so does an opacity of 100%%");
    CHECK(!(GetWindowLongPtrW(modes, GWL_EXSTYLE) & WS_EX_LAYERED),
          "none of them touches the window");

    // Move-only still fades a move, which is the point of it.
    g_settings.dragOpacity = kDefaultDragOpacity;
    g_settings.dragTranslucency = DragTranslucency::MoveOnly;
    BeginDragFade(modes, kDragMove);
    CHECK(IsDragFading(modes), "move-only still fades a move");
    for (int i = 0; i < 20 && IsDragFading(modes); i++) {
        Pump(100);
    }
    g_settings.dragTranslucency = DragTranslucency::Both;
    DestroyWindow(modes);
    Pump(100);
}

////////////////////////////////////////////////////////////////////////////////

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("harness start\n");
    bool dpiUnaware = false, noInput = false, onlyDesktops = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dpi-unaware")) dpiUnaware = true;
        if (!strcmp(argv[i], "--no-input")) noInput = true;
        if (!strcmp(argv[i], "--desktops")) onlyDesktops = true;
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
    // Off for the tests that check where a drag put a window: whatever else
    // is on this desktop would otherwise have a say in it.
    g_settings.snap = SnapMode::Off;

    if (onlyDesktops) {
        TestDesktops(false);
        printf("\n%d passed, %d failed\n", g_passes, g_failures);
        return g_failures ? 1 : 0;
    }
    TestParsers();
    TestFramelessGeometry(false, MenuBarMode::Hide);
    TestFramelessGeometry(true, MenuBarMode::Hide);
    TestFramelessGeometry(true, MenuBarMode::KeepMenu);
    TestMessageHook();
    TestReusedThreadId();
    TestAutoHide();
    TestBorderFade();
    TestBorderScope();
    TestWinMask();
    TestKeyBindings(noInput);
    TestDesktops(noInput);
    TestSnapGeometry();
    TestGestures();
    TestResizeRelease();
    TestDragFade();
    if (!noInput) {
        TestMoveResize();
    }

    printf("\n%d passed, %d failed\n", g_passes, g_failures);
    return g_failures ? 1 : 0;
}
