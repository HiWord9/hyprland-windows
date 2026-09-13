// Tests driving the *native* SC_SIZE loop of another window with a synthetic
// held left mouse button, so a resize can be driven by (here: simulated) real
// cursor movement and torn down on demand. If the native loop reflows the
// target live (Chrome etc.), this is the path the mod should use for Win+RMB.
//
// Usage: nativesize_probe.exe <pid>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

static HWND g_target;
static DWORD g_pid;

// Screenshots go to the "out" directory next to the probe executable.
static std::string OutDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir = path;
    dir.erase(dir.find_last_of("\\/") + 1);
    return dir + "out\\";
}

static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != g_pid) return TRUE;
    RECT rc;
    GetWindowRect(hwnd, &rc);
    if (rc.right - rc.left < 300 || rc.bottom - rc.top < 200) return TRUE;
    char t[256];
    GetWindowTextA(hwnd, t, sizeof(t));
    if (!t[0]) return TRUE;
    g_target = hwnd;
    printf("target hwnd=%p title=%s\n", (void*)hwnd, t);
    return FALSE;
}

static void Mouse(DWORD f) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = f;
    in.mi.dwExtraInfo = 0x48797072;
    SendInput(1, &in, sizeof(in));
}

static bool SaveBmp(const char* path, int w, int h, const void* p) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = -h;
    ih.biPlanes = 1; ih.biBitCount = 32;
    DWORD sz = (DWORD)w * h * 4;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + sz;
    FILE* f = fopen(path, "wb"); if (!f) return false;
    fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f); fwrite(p, sz, 1, f); fclose(f);
    return true;
}
static void Shot(HWND hwnd, const char* name) {
    RECT rc; GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HDC sc = GetDC(nullptr), mem = CreateCompatibleDC(sc);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, sc, rc.left, rc.top, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    std::string path = OutDir() + name + ".bmp";
    printf("  shot %s (%dx%d): %s\n", name, w, h, SaveBmp(path.c_str(), w, h, bits) ? "ok" : "FAIL");
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, sc);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 2) { printf("usage: nativesize_probe <pid>\n"); return 1; }
    g_pid = (DWORD)atoi(argv[1]);
    EnumWindows(EnumProc, 0);
    if (!g_target) { printf("no window for pid %lu\n", g_pid); return 1; }
    CreateDirectoryA(OutDir().c_str(), nullptr);

    RECT r0; GetWindowRect(g_target, &r0);
    int w0 = r0.right - r0.left, h0 = r0.bottom - r0.top;
    printf("orig rect %ld,%ld %dx%d\n", r0.left, r0.top, w0, h0);

    // Grab point near the bottom-right corner, inside the window.
    POINT start{r0.right - 60, r0.bottom - 60};
    SetForegroundWindow(g_target);
    SetCursorPos(start.x, start.y);
    Sleep(300);

    // Hold a synthetic left button, then ask the target to run its native
    // bottom-right sizing loop (it tracks the left button we now hold down).
    Mouse(MOUSEEVENTF_LEFTDOWN);
    Sleep(30);
    PostMessageW(g_target, WM_SYSCOMMAND, SC_SIZE + WMSZ_BOTTOMRIGHT,
                 MAKELPARAM(start.x, start.y));

    // Drag the cursor to grow the window; the native loop should follow live.
    const int grow = 260, steps = 26;
    for (int i = 1; i <= steps; i++) {
        SetCursorPos(start.x + grow * i / steps, start.y + grow * i / steps);
        Sleep(16);
    }
    Sleep(120);
    Shot(g_target, "native_midresize");  // still mid-drag, button held
    Sleep(200);

    // Release: end the native loop by lifting the (synthetic) left button.
    Mouse(MOUSEEVENTF_LEFTUP);
    Sleep(300);

    RECT r1; GetWindowRect(g_target, &r1);
    printf("after rect %ld,%ld %dx%d (grew W%+d H%+d)\n", r1.left, r1.top,
           r1.right - r1.left, r1.bottom - r1.top,
           (r1.right - r1.left) - w0, (r1.bottom - r1.top) - h0);

    // Restore original size.
    SetWindowPos(g_target, nullptr, 0, 0, w0, h0,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
}
