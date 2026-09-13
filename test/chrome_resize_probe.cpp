// Reproduces the "stale content while resizing" issue against a real window
// (e.g. Chrome) and compares how the client area repaints mid-resize under
// different SetWindowPos strategies. Screenshots are written mid-drag (before
// the "button" is released) so the stale area, if any, is captured.
//
// Usage: chrome_resize_probe.exe <class-substring>   (default: Chrome_WidgetWin)
#include <windows.h>

#include <cstdio>
#include <string>

static HWND g_found;
static std::string g_classWant;
static DWORD g_pidWant;

// Screenshots go to the "out" directory next to the probe executable.
static std::string OutDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir = path;
    dir.erase(dir.find_last_of("\\/") + 1);
    return dir + "out\\";
}

static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) {
        return TRUE;
    }
    RECT rc;
    GetWindowRect(hwnd, &rc);
    if (rc.right - rc.left < 400 || rc.bottom - rc.top < 300) {
        return TRUE;
    }
    if (g_pidWant) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != g_pidWant) {
            return TRUE;
        }
    }
    char cls[128];
    GetClassNameA(hwnd, cls, sizeof(cls));
    if (g_pidWant || g_classWant.empty() || strstr(cls, g_classWant.c_str())) {
        char title[256];
        GetWindowTextA(hwnd, title, sizeof(title));
        if (title[0]) {
            g_found = hwnd;
            printf("target: hwnd=%p class=%s title=%s\n", (void*)hwnd, cls,
                   title);
            return FALSE;
        }
    }
    return TRUE;
}

static bool SaveBmp(const char* path, int w, int h, const void* bgra) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    DWORD sz = (DWORD)w * h * 4;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + sz;
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bgra, sz, 1, f);
    fclose(f);
    return true;
}

static void Shot(HWND hwnd, const char* name) {
    RECT rc;
    GetWindowRect(hwnd, &rc);
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
    std::string path = OutDir() + name + ".bmp";
    printf("  shot %s (%dx%d): %s\n", name, w, h,
           SaveBmp(path.c_str(), w, h, bits) ? "ok" : "FAIL");
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// Grows the window bottom-right by `grow` px in steps, screenshots at the end
// of the grow (still "mid-drag"), then shrinks back. mode selects the flags.
static void GrowResize(HWND hwnd, const char* name, UINT extraFlags,
                       bool redraw, bool sizingMsg) {
    RECT r0;
    GetWindowRect(hwnd, &r0);
    int w0 = r0.right - r0.left, h0 = r0.bottom - r0.top;
    const int grow = 240, steps = 24;

    SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    for (int i = 1; i <= steps; i++) {
        int w = w0 + grow * i / steps;
        int h = h0 + grow * i / steps;
        if (sizingMsg) {
            RECT rc{r0.left, r0.top, r0.left + w, r0.top + h};
            SendMessageW(hwnd, WM_SIZING, WMSZ_BOTTOMRIGHT, (LPARAM)&rc);
        }
        SetWindowPos(hwnd, nullptr, 0, 0, w, h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | extraFlags);
        if (redraw) {
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
        Sleep(16);
    }
    Sleep(120);  // let any async present catch up a little
    Shot(hwnd, name);
    Sleep(300);
    SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);

    // restore
    SetWindowPos(hwnd, nullptr, 0, 0, w0, h0,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Sleep(400);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // arg: a class substring, or "pid:<n>" to target a specific process.
    std::string arg = argc > 1 ? argv[1] : "Chrome_WidgetWin";
    if (arg.rfind("pid:", 0) == 0) {
        g_pidWant = (DWORD)atoi(arg.c_str() + 4);
    } else {
        g_classWant = arg;
    }
    EnumWindows(EnumProc, 0);
    if (!g_found) {
        printf("no matching window found\n");
        return 1;
    }
    CreateDirectoryA(OutDir().c_str(), nullptr);
    SetForegroundWindow(g_found);
    Sleep(500);

    GrowResize(g_found, "chrome_copybits", 0, false, true);
    GrowResize(g_found, "chrome_nocopybits", SWP_NOCOPYBITS, false, true);
    GrowResize(g_found, "chrome_nocopy_redraw", SWP_NOCOPYBITS, true, true);
    GrowResize(g_found, "chrome_defer_novalidate", SWP_DEFERERASE, false, true);
    return 0;
}
