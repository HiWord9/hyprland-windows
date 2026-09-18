// Fading a window to translucent while it is being dragged, the way a
// Hyprland window dims while you move it around.
//
// The fade is animated from a worker thread, because it has to keep going
// while the window's own thread is inside the system's move/size loop.
// SetLayeredWindowAttributes is safe to call from there - it sends the window
// nothing. Adding and removing WS_EX_LAYERED is not: SetWindowLongPtr sends
// WM_STYLECHANGED, and a cross-thread send waits for however long the
// application takes to answer, which a thread the unload waits for must not
// risk. So the style goes on in the drag request, which already runs on the
// window's own thread, and comes back off on that same thread, through a
// kDragUnfade request the worker posts once it is done.
#include "common.h"

// What a window looked like before the drag, so it can be put back exactly.
struct DragFade {
    bool addedLayered = false;  // the mod made it layered, and undoes that
    BYTE baseAlpha = 255;
    COLORREF baseKey = 0;
    DWORD baseFlags = 0;
};

std::mutex g_fadeMutex;
std::unordered_map<HWND, DragFade> g_fades;  // windows being dragged right now

// Everything a fade needs. The worker owns its copy, so nothing it reads can
// change or go away underneath it.
struct DragFadeWork {
    HWND root;
    DWORD threadId;  // whose move/size loop the fade follows
    int buttonVk;    // the physical button that holds this drag
    DragFade fade;
    BYTE target;
    int fadeIn;
    int fadeOut;
    bool loopSeen = false;  // the loop has been seen running at least once
};

constexpr DWORD kFadeStepMs = 8;       // ~120 Hz, about as fine as Sleep gets
constexpr DWORD kFadeHoldStepMs = 16;  // while waiting for the drag to end

bool IsDragFading(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_fadeMutex);
    return g_fades.count(hwnd) != 0;
}

// The alpha a window is dragged at. Relative to what it had, so a window that
// was already translucent keeps that much of a head start.
BYTE DragAlphaFor(BYTE baseAlpha, int opacityPercent) {
    return (BYTE)(baseAlpha * opacityPercent / 100);
}

// Smoothstep, so the fade eases in and out instead of starting and stopping
// abruptly - which is what makes a 100 ms fade read as a fade at all.
BYTE FadeAlphaAt(BYTE from, BYTE to, int durationMs, int elapsedMs) {
    if (durationMs <= 0 || elapsedMs >= durationMs) {
        return to;
    }
    if (elapsedMs <= 0) {
        return from;
    }
    double t = (double)elapsedMs / durationMs;
    t = t * t * (3.0 - 2.0 * t);
    return (BYTE)(from + (to - from) * t + 0.5);
}

void SetFadeAlpha(const DragFadeWork& work, BYTE alpha) {
    SetLayeredWindowAttributes(work.root, work.fade.baseKey, alpha,
                               work.fade.baseFlags | LWA_ALPHA);
}

// Whether the drag is still going. The loop is not what says so on its own: a
// move loop does not count as one until the cursor has moved far enough to be
// a drag, so before that there is nothing to see but the button - and a loop
// that has been running and is gone means the drag is over (cancelled with
// Esc, say) even though the button is still held.
bool DragStillHeld(DragFadeWork& work) {
    if (g_uninitializing || !IsWindow(work.root)) {
        return false;
    }
    if (IsThreadInMoveSizeLoop(work.threadId)) {
        work.loopSeen = true;
        return true;
    }
    if (work.loopSeen) {
        return false;
    }
    return (GetAsyncKeyState(work.buttonVk) & 0x8000) != 0;
}

// Walks the alpha from one value to the other and returns where it got to:
// `to`, unless `whileDragging` and the drag ended on the way - the fade back
// then starts from wherever the window was instead of jumping.
BYTE FadeOver(DragFadeWork& work,
              BYTE from,
              BYTE to,
              int durationMs,
              bool whileDragging) {
    BYTE alpha = from;
    DWORD start = GetTickCount();
    int elapsed = 0;
    while (elapsed < durationMs && !g_uninitializing &&
           (!whileDragging || DragStillHeld(work))) {
        alpha = FadeAlphaAt(from, to, durationMs, elapsed);
        SetFadeAlpha(work, alpha);
        Sleep(kFadeStepMs);
        elapsed = (int)(GetTickCount() - start);
    }
    if (!whileDragging || elapsed >= durationMs) {
        alpha = to;  // it ran to its end, or it has to land no matter what
        SetFadeAlpha(work, alpha);
    }
    return alpha;
}

void RunDragFade(DragFadeWork& work) {
    // Straight into the fade, with nothing waited for first: the window dims
    // when the button goes down, which for a move is well before the loop
    // that moves it has anything to show.
    BYTE alpha =
        FadeOver(work, work.fade.baseAlpha, work.target, work.fadeIn, true);
    while (DragStillHeld(work)) {
        Sleep(kFadeHoldStepMs);
    }
    // Snapped back rather than faded when the mod is on its way out: the
    // unload is waiting for this thread to be done.
    FadeOver(work, alpha, work.fade.baseAlpha,
             g_uninitializing ? 0 : work.fadeOut, false);

    // Only the window's own thread may take WS_EX_LAYERED back off. When the
    // request cannot be handed over - the window is gone, or the mod is being
    // unloaded and the hook that would pick it up is not there any more - the
    // window keeps a layered style at full opacity, which nothing can see and
    // which goes away with the window.
    bool handed = !g_uninitializing && IsWindow(work.root) &&
                  PostMessageW(work.root, g_msgDrag, kDragUnfade, 0);
    if (!handed) {
        std::lock_guard<std::mutex> lock(g_fadeMutex);
        g_fades.erase(work.root);
    }
}

DWORD WINAPI DragFadeThread(LPVOID param) {
    {
        std::unique_ptr<DragFadeWork> work(static_cast<DragFadeWork*>(param));
        RunDragFade(*work);
    }
    g_modRefCount--;  // the last thing this thread does in the mod's image
    return 0;
}

// Called from the drag request, on the window's own thread, once a move or
// size loop is about to start.
void BeginDragFade(HWND root, WPARAM kind) {
    DragTranslucency mode = g_settings.dragTranslucency;
    int opacity = g_settings.dragOpacity;
    if (g_uninitializing || mode == DragTranslucency::Off || opacity >= 100) {
        return;
    }
    // A resize is the one drag you may want to watch reflow as it happens,
    // rather than through the window.
    if (mode == DragTranslucency::MoveOnly && kind == kDragResize) {
        return;
    }

    DragFade fade;
    LONG_PTR exStyle = GetWindowLongPtrW(root, GWL_EXSTYLE);
    if (exStyle & WS_EX_LAYERED) {
        // Two states to stay out of, and the same reason for both: a window
        // that has nothing to read either composites itself with
        // UpdateLayeredWindow already (which reports no attributes at all) or
        // is layered without having said how yet (which reports none set),
        // and once a flat alpha has been put on a window, its own
        // UpdateLayeredWindow fails from then on.
        if (!GetLayeredWindowAttributes(root, &fade.baseKey, &fade.baseAlpha,
                                        &fade.baseFlags) ||
            !fade.baseFlags) {
            return;
        }
        if (!(fade.baseFlags & LWA_ALPHA)) {
            fade.baseAlpha = 255;  // color-keyed only; no alpha to keep
        }
    } else {
        fade.addedLayered = true;
    }

    BYTE target = DragAlphaFor(fade.baseAlpha, opacity);
    if (target >= fade.baseAlpha) {
        return;  // nothing anyone could see
    }

    {
        std::lock_guard<std::mutex> lock(g_fadeMutex);
        // An earlier drag of this window is still fading back. It owns the
        // window's state until it is done, so this drag goes without.
        if (!g_fades.emplace(root, fade).second) {
            return;
        }
    }

    auto* work = new DragFadeWork{root,
                                  GetWindowThreadProcessId(root, nullptr),
                                  PhysicalButtonVk(kind == kDragResize),
                                  fade,
                                  target,
                                  g_settings.dragFadeIn,
                                  g_settings.dragFadeOut};
    if (fade.addedLayered) {
        // Layered but unchanged, so nothing flashes before the fade starts.
        SetWindowLongPtrW(root, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
        SetLayeredWindowAttributes(root, 0, fade.baseAlpha, LWA_ALPHA);
    }

    g_modRefCount++;
    HANDLE thread = CreateThread(nullptr, 0, DragFadeThread, work, 0, nullptr);
    if (!thread) {
        Wh_Log(L"CreateThread failed (%u)", GetLastError());
        g_modRefCount--;
        delete work;
        EndDragFade(root);  // on the window's own thread, so undo it here
        return;
    }
    CloseHandle(thread);
}

// The window's thread is asked to take the mod's WS_EX_LAYERED back off, the
// one part of the fade that only it may do. This is the window being put back
// the way it was, so it runs during the teardown as well.
void EndDragFade(HWND hwnd) {
    DragFade fade;
    {
        std::lock_guard<std::mutex> lock(g_fadeMutex);
        auto it = g_fades.find(hwnd);
        if (it == g_fades.end()) {
            return;
        }
        fade = it->second;
        g_fades.erase(it);
    }

    if (fade.addedLayered) {
        LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                          exStyle & ~(LONG_PTR)WS_EX_LAYERED);
    } else {
        SetLayeredWindowAttributes(hwnd, fade.baseKey, fade.baseAlpha,
                                   fade.baseFlags);
    }
}
