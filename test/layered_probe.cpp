// Probe: what does GetLayeredWindowAttributes report for a window in each of
// the states a layered window can be in? This is how the drag fade tells a
// window it may set a flat alpha on from one that paints its own
// transparency - and it also shows what putting a flat alpha on a window
// costs it: its own UpdateLayeredWindow fails from then on.
//
// Ran on Windows 11 26200:
//   plain window                       ok=0 err=87
//   just made layered                  ok=1 alpha=0   flags=0x0
//   after UpdateLayeredWindow          ok=0 err=0
//   after SetLayeredWindowAttributes   ok=1 alpha=200 flags=0x2 (LWA_ALPHA)
//   UpdateLayeredWindow again          failed, err=87
//   style removed                      ok=0 err=87
//   style added back                   ok=1 alpha=255 flags=0x2
#include <windows.h>

#include <cstdio>

static HWND g_hwnd;

static void Report(const char* label) {
    COLORREF key = 0xDEADBEEF;
    BYTE alpha = 123;
    DWORD flags = 0xFFFFFFFF;
    SetLastError(0);
    BOOL ok = GetLayeredWindowAttributes(g_hwnd, &key, &alpha, &flags);
    DWORD err = GetLastError();
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    printf("%-34s ok=%d err=%lu key=0x%08lX alpha=%d flags=0x%lX layered=%d\n",
           label, ok, err, (unsigned long)key, (int)alpha,
           (unsigned long)flags, (ex & WS_EX_LAYERED) ? 1 : 0);
    fflush(stdout);
}

static void PaintPerPixel() {
    // The UpdateLayeredWindow path: a 32-bit premultiplied bitmap with a real
    // per-pixel alpha channel.
    const int w = 200, h = 150;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp =
        CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto* px = (unsigned char*)bits;
    for (int i = 0; i < w * h; i++) {
        px[i * 4 + 0] = 40;   // premultiplied by alpha 128
        px[i * 4 + 1] = 20;
        px[i * 4 + 2] = 60;
        px[i * 4 + 3] = 128;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    POINT src{0, 0};
    SIZE size{w, h};
    POINT dst{300, 300};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    BOOL ok = UpdateLayeredWindow(g_hwnd, screen, &dst, &size, mem, &src, 0,
                                  &blend, ULW_ALPHA);
    printf("UpdateLayeredWindow ok=%d err=%lu\n", ok, GetLastError());
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

int main() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LayeredProbe";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, L"LayeredProbe", L"probe", WS_OVERLAPPEDWINDOW,
                             300, 300, 200, 150, nullptr, nullptr,
                             wc.hInstance, nullptr);
    ShowWindow(g_hwnd, SW_SHOW);
    Report("plain window");

    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE,
                      GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
    Report("just made layered");

    PaintPerPixel();
    Report("after UpdateLayeredWindow");

    SetLayeredWindowAttributes(g_hwnd, 0, 200, LWA_ALPHA);
    Report("after SetLayeredWindowAttributes");

    PaintPerPixel();
    Report("after UpdateLayeredWindow again");

    // And the state the mod leaves behind: the style off and on again.
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE,
                      GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) &
                          ~(LONG_PTR)WS_EX_LAYERED);
    Report("style removed");
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE,
                      GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
    Report("style added back");

    DestroyWindow(g_hwnd);
    return 0;
}
