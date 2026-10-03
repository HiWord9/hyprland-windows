// DWM decorations (border color, corner preference) of frameless windows, and
// the fade from one border color to the other when focus moves.
//
// Anything set here has to be written back as the system default when the
// setting is cleared, not merely skipped: the window keeps whatever was last
// written to it, so a border that is no longer configured would otherwise stay
// on screen until its title bar comes back.
#include "common.h"

// The Windows accent color, as a real color to paint with.
//
// AccentColor in the registry is 0xAABBGGRR - a COLORREF with an alpha on top
// of it - so the alpha is all that has to come off. That it is that way round
// and not ARGB is measurable: ColorizationColor next to it holds the same
// color, is documented as ARGB, and the two values are byte-swapped copies of
// each other. DwmGetColorizationColor, the fallback here, is ARGB as well, so
// that one does need its red and blue exchanged.
COLORREF AccentBorderColor() {
    DWORD accent = 0;
    DWORD size = sizeof(accent);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                     L"AccentColor", RRF_RT_REG_DWORD, nullptr, &accent,
                     &size) == ERROR_SUCCESS) {
        return accent & 0x00FFFFFF;
    }

    DWORD colorization = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&colorization, &opaque))) {
        return RGB((colorization >> 16) & 0xFF, (colorization >> 8) & 0xFF,
                   colorization & 0xFF);
    }
    return kColorUntouched;  // no accent to be had; leave the border alone
}

// The color a window's border is meant to have, the way DWM wants it: a color,
// or DWMWA_COLOR_DEFAULT when the setting is empty.
COLORREF BorderColorFor(bool active) {
    COLORREF color =
        active ? g_settings.borderActive : g_settings.borderInactive;
    if (color == kColorAccent) {
        color = AccentBorderColor();
    }
    return color == kColorUntouched ? (COLORREF)DWMWA_COLOR_DEFAULT : color;
}

// Whether a color is one that can be faded through. The sentinels - "leave it
// alone", "system default", "no border" - are states rather than colors, and
// there is no halfway between a color and no border at all, so those switch
// at once. A real COLORREF has nothing in its top byte.
bool IsBlendableColor(COLORREF color) {
    return (color & 0xFF000000) == 0;
}

COLORREF BlendColor(COLORREF from, COLORREF to, double t) {
    auto channel = [&](int shift) {
        int a = (from >> shift) & 0xFF;
        int b = (to >> shift) & 0xFF;
        return (COLORREF)(int)(a + (b - a) * t + 0.5) << shift;
    };
    return channel(0) | channel(8) | channel(16);
}

////////////////////////////////////////////////////////////////////////////////
// Which windows carry a border color of ours
//
// Every window with a frame, or only the ones the mod has taken the title bar
// from when the setting says so. The bookkeeping lives here rather than with
// the title bars, because a window with a border color of ours is not
// necessarily one the mod has touched in any other way - and the color it
// last got is where a fade to the other one starts from.

std::mutex g_coloredMutex;
std::unordered_map<HWND, COLORREF> g_colored;

bool BorderColorsWanted() {
    return g_settings.borderActive != kColorUntouched ||
           g_settings.borderInactive != kColorUntouched;
}

bool IsBorderColorTarget(HWND hwnd) {
    if (!BorderColorsWanted()) {
        return false;
    }
    return g_settings.borderFramelessOnly ? IsFrameless(hwnd)
                                          : IsFrameWindow(hwnd);
}

COLORREF CurrentBorderColor(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_coloredMutex);
    auto it = g_colored.find(hwnd);
    return it == g_colored.end() ? kColorUntouched : it->second;
}

void ForgetBorderColor(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_coloredMutex);
    g_colored.erase(hwnd);
}

std::vector<HWND> SnapshotColoredWindows() {
    std::lock_guard<std::mutex> lock(g_coloredMutex);
    std::vector<HWND> result;
    result.reserve(g_colored.size());
    for (const auto& [hwnd, color] : g_colored) {
        result.push_back(hwnd);
    }
    return result;
}

void WriteBorderColor(HWND hwnd, COLORREF color) {
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color));
    if (color == (COLORREF)DWMWA_COLOR_DEFAULT) {
        ForgetBorderColor(hwnd);  // the border is the system's own again
        return;
    }
    std::lock_guard<std::mutex> lock(g_coloredMutex);
    g_colored[hwnd] = color;
}

////////////////////////////////////////////////////////////////////////////////
// The fade between the two colors
//
// One thread drives every window that is fading right now and stops as soon as
// there is nothing left to fade, so alt-tabbing through ten windows costs one
// thread rather than ten. A window that is already fading is retargeted rather
// than restarted: focus that comes back mid-fade turns the color around from
// where it is instead of jumping to the far end first.
//
// DwmSetWindowAttribute sends the window nothing, so it is safe to call from
// this thread. g_borderMutex is never held while it is called, and never while
// the window bookkeeping's own lock is taken.

struct BorderFade {
    COLORREF from;
    COLORREF to;
    DWORD startTick;
    int durationMs;
    // Which fade of this window's this is: a retarget gets a new one, so a
    // batch of writes that was in flight at the time knows not to end it.
    unsigned long long seq;
};

std::mutex g_borderMutex;
std::unordered_map<HWND, BorderFade> g_borderFades;
bool g_borderThreadRunning;        // guarded by g_borderMutex
unsigned long long g_borderSeq;    // guarded by g_borderMutex
// Set while the thread is handing a batch of colors to DWM, which it does
// outside the lock - the window bookkeeping has a lock of its own, and the
// two are never held at once.
std::atomic<bool> g_borderWriting;

constexpr DWORD kBorderStepMs = 16;    // ~60 Hz
constexpr int kBorderFinishWaitMs = 300;
constexpr int kBorderWriteWaitMs = 20;

// Stops the fade and waits for any color already on its way to the window, so
// that whatever the caller writes next is what stays on it.
void CancelBorderFade(HWND hwnd) {
    {
        std::lock_guard<std::mutex> lock(g_borderMutex);
        g_borderFades.erase(hwnd);
    }
    for (int waited = 0; waited < kBorderWriteWaitMs && g_borderWriting;
         waited++) {
        Sleep(1);
    }
}

DWORD WINAPI BorderFadeThread(LPVOID param) {
    bool more = true;
    while (more) {
        std::vector<std::pair<HWND, COLORREF>> writes;
        std::vector<std::pair<HWND, unsigned long long>> finished;
        {
            std::lock_guard<std::mutex> lock(g_borderMutex);
            if (g_uninitializing) {
                // The teardown writes the default back over every window of
                // ours, and a color landing after that would stay on it for
                // good.
                g_borderFades.clear();
            }
            DWORD now = GetTickCount();
            for (const auto& [hwnd, fade] : g_borderFades) {
                double t = AnimationProgress((int)(now - fade.startTick),
                                             fade.durationMs);
                bool alive = IsWindow(hwnd);
                if (alive) {
                    writes.emplace_back(hwnd, BlendColor(fade.from, fade.to, t));
                }
                if (!alive || t >= 1.0) {
                    finished.emplace_back(hwnd, fade.seq);
                }
            }
        }

        g_borderWriting = true;
        for (const auto& [hwnd, color] : writes) {
            WriteBorderColor(hwnd, color);
        }
        g_borderWriting = false;

        // Only now is a finished fade over. Taking it out any earlier would
        // say the window is done while its last color is still on its way,
        // which is exactly what the teardown and the restore wait for.
        {
            std::lock_guard<std::mutex> lock(g_borderMutex);
            for (const auto& [hwnd, seq] : finished) {
                auto it = g_borderFades.find(hwnd);
                if (it != g_borderFades.end() && it->second.seq == seq) {
                    g_borderFades.erase(it);
                }
            }
            more = !g_borderFades.empty();
            if (!more) {
                g_borderThreadRunning = false;
            }
        }
        if (more) {
            Sleep(kBorderStepMs);
        }
    }
    g_modRefCount--;  // the last thing this thread does in the mod's image
    return 0;
}

void ApplyBorderColor(HWND hwnd, bool active) {
    CancelBorderFade(hwnd);

    COLORREF target = BorderColorFor(active);
    if (target == (COLORREF)DWMWA_COLOR_DEFAULT &&
        CurrentBorderColor(hwnd) == kColorUntouched) {
        return;  // nothing to set, and nothing of ours to undo
    }
    WriteBorderColor(hwnd, target);
}

// Hands the border back to the system, if the mod had it at all.
void RestoreBorderColor(HWND hwnd) {
    CancelBorderFade(hwnd);
    if (CurrentBorderColor(hwnd) == kColorUntouched) {
        return;
    }
    WriteBorderColor(hwnd, (COLORREF)DWMWA_COLOR_DEFAULT);
}

// Puts one window's border where the settings say it belongs: colored when it
// is a window they apply to, and the system's own when it is not one any more.
void RefreshBorderColor(HWND hwnd) {
    if (IsBorderColorTarget(hwnd)) {
        ApplyBorderColor(hwnd, GetForegroundWindow() == hwnd);
    } else {
        RestoreBorderColor(hwnd);
    }
}

// What a window's own thread reports through the hook on sent messages, which
// is the only place a window that the mod has not otherwise touched says
// anything about its focus - see hooks.cpp.
void OnWindowActivation(HWND hwnd, bool active) {
    if (IsBorderColorTarget(hwnd)) {
        AnimateBorderColor(hwnd, active);
    }
}

BOOL CALLBACK RefreshBorderColorProc(HWND hwnd, LPARAM lParam) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == (DWORD)lParam) {
        RefreshBorderColor(hwnd);
    }
    return TRUE;
}

// Every window of this process at once: at start-up, and whenever the
// settings change what the answer is.
void RefreshBorderColors() {
    for (HWND hwnd : SnapshotColoredWindows()) {
        if (!IsBorderColorTarget(hwnd)) {
            RestoreBorderColor(hwnd);
        }
    }
    EnumWindows(RefreshBorderColorProc, (LPARAM)GetCurrentProcessId());
}

// The teardown. DwmSetWindowAttribute sends the window nothing, so unlike a
// subclass this needs no round trip to anybody's thread.
void RestoreAllBorderColors() {
    for (HWND hwnd : SnapshotColoredWindows()) {
        RestoreBorderColor(hwnd);
    }
}

// The same color, reached over the configured time instead of at once. Used
// when focus moves; the frame being taken over, or the settings changing, sets
// the color outright - a screenful of windows fading in at once on start-up is
// not what anybody asked for.
void AnimateBorderColor(HWND hwnd, bool active) {
    COLORREF to = BorderColorFor(active);
    COLORREF from = CurrentBorderColor(hwnd);
    int durationMs = g_settings.borderFadeDuration;
    if (g_uninitializing || durationMs <= 0 || from == to ||
        !IsBlendableColor(from) || !IsBlendableColor(to)) {
        ApplyBorderColor(hwnd, active);  // nothing to fade between
        return;
    }

    MarkDwmTouched(hwnd);
    {
        std::lock_guard<std::mutex> lock(g_borderMutex);
        g_borderFades[hwnd] =
            BorderFade{from, to, GetTickCount(), durationMs, ++g_borderSeq};
        if (g_borderThreadRunning) {
            return;  // the thread picks this one up on its next step
        }
        g_borderThreadRunning = true;
    }

    if (!StartModThread(BorderFadeThread, nullptr)) {
        {
            std::lock_guard<std::mutex> lock(g_borderMutex);
            g_borderThreadRunning = false;
        }
        ApplyBorderColor(hwnd, active);
    }
}

// The teardown waits here, with g_uninitializing already set, before the
// windows get their defaults back.
void FinishBorderFades() {
    for (int waited = 0; waited < kBorderFinishWaitMs;
         waited += (int)kBorderStepMs) {
        {
            std::lock_guard<std::mutex> lock(g_borderMutex);
            if (!g_borderThreadRunning) {
                return;
            }
        }
        Sleep(kBorderStepMs);
    }
    Wh_Log(L"Border fades did not stop in time");
}

////////////////////////////////////////////////////////////////////////////////
// Corners

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
    RefreshBorderColor(hwnd);
    ApplyCorners(hwnd);
}

// The corners go back with the title bar; the border color is decided by
// whether the window is still one the colors apply to, which is not the same
// question any more.
void RestoreDwmAttributes(HWND hwnd) {
    int corners = DWMWCP_DEFAULT;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners,
                          sizeof(corners));
}
