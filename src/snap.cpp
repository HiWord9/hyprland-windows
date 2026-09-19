// Magnetic edges while a window is being dragged, and the aspect ratio held
// while it is being resized.
//
// Both live in the same place: a subclass put on the window for the length of
// the drag, which edits the rectangle the system's loop is about to apply.
// The loop hands it over as WM_MOVING / WM_SIZING - sent messages, which the
// message hook never sees, so a subclass is the only way to them. It goes on
// in the drag request, where we are already on the window's own thread, and
// comes off at WM_EXITSIZEMOVE, which ends every drag including one cancelled
// with Esc.
//
// Two things about those loops are measured rather than assumed (see
// test/snap_probe.cpp):
//
//   * The size loop works out its rectangle from the cursor and the corner
//     that is staying put, so overriding it costs nothing: pull away from a
//     snapped edge and the window comes off it.
//   * The move loop instead takes the position it last applied and adds the
//     cursor's movement to it, so an override sticks forever - the window
//     would stay glued to the first line it touched. The position is
//     therefore worked out here from the cursor and the grab offset, which
//     takes that computation away from the loop entirely.
//
// Distances are in the coordinates of the *visible* frame rather than the
// window rectangle: a window carries an invisible resize border several
// pixels wide on three sides, so two windows whose window rectangles touch
// have a visible gap of twice that between them.
#include "common.h"

struct SnapState {
    WPARAM kind = kDragMove;
    POINT grabOffset{};  // cursor minus the window's top left, at the start
    SIZE grabSize{};     // the size that offset was taken at
    RECT startFrame{};   // the visible frame at the start, for the ratio
    RECT inset{};        // the visible frame minus the window rectangle
    std::vector<RECT> windows;   // the frames of the other windows
    std::vector<RECT> monitors;  // the work areas to snap to
};

std::mutex g_snapMutex;
std::unordered_map<HWND, SnapState> g_snaps;

constexpr UINT_PTR kSnapSubclassId = 0x48597073;  // 'Hyps'

////////////////////////////////////////////////////////////////////////////////
// Geometry

// How far the visible frame sits inside the window rectangle, which for a
// window with a resize border is several pixels on three sides.
//
// DWM is the only one who knows, and it answers in physical pixels whatever
// the process has been told about DPI - measured, see test/dpi_probe.cpp -
// while GetWindowRect, GetCursorPos and the work areas are all virtualized
// for a DPI-unaware process. In one of those, on a scaled monitor, the two
// are not the same coordinates at all: the inset came out as 211 pixels in
// the probe rather than 9. An inset that is not a small step inward is that
// disagreement showing, and then it is better to work in window rectangles
// than in wrong ones - windows line up a hair apart instead of exactly,
// which is what the system's own snapping does anyway.
constexpr int kMaxFrameInset = 32;

RECT FrameInsetOf(HWND hwnd) {
    RECT window{}, frame{};
    if (!GetWindowRect(hwnd, &window) ||
        FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame,
                                     sizeof(frame)))) {
        return RECT{};
    }
    RECT inset{frame.left - window.left, frame.top - window.top,
               frame.right - window.right, frame.bottom - window.bottom};
    bool sane = inset.left >= 0 && inset.left <= kMaxFrameInset &&
                inset.top >= 0 && inset.top <= kMaxFrameInset &&
                inset.right <= 0 && inset.right >= -kMaxFrameInset &&
                inset.bottom <= 0 && inset.bottom >= -kMaxFrameInset;
    return sane ? inset : RECT{};
}

RECT WindowToFrame(const RECT& rc, const RECT& inset) {
    return RECT{rc.left + inset.left, rc.top + inset.top,
                rc.right + inset.right, rc.bottom + inset.bottom};
}

RECT FrameToWindow(const RECT& rc, const RECT& inset) {
    return RECT{rc.left - inset.left, rc.top - inset.top,
                rc.right - inset.right, rc.bottom - inset.bottom};
}

// Where a window's visible frame is, in the coordinates this process sees.
RECT VisibleFrameOf(HWND hwnd) {
    RECT window{};
    if (!GetWindowRect(hwnd, &window)) {
        return RECT{};
    }
    return WindowToFrame(window, FrameInsetOf(hwnd));
}

bool RangesOverlap(int aLow, int aHigh, int bLow, int bHigh) {
    return aLow < bHigh && bLow < aHigh;
}

// The closest of the lines to this edge, or the edge itself when none of them
// is near enough.
int SnappedEdge(int edge, const std::vector<int>& lines, int limit) {
    int best = edge;
    int bestDistance = limit + 1;
    for (int line : lines) {
        int distance = abs(line - edge);
        if (distance <= limit && distance < bestDistance) {
            best = line;
            bestDistance = distance;
        }
    }
    return best;
}

// Where an edge of ours may land: against a neighbour with the gap between
// them, flush with the same edge of one, or at the edge of a work area. Only
// neighbours that overlap us on the other axis count - a window far above is
// not something this one is lining up with.
void CollectLines(const SnapState& state,
                  const RECT& frame,
                  bool horizontal,
                  std::vector<int>* forLow,
                  std::vector<int>* forHigh) {
    int windowGap = g_settings.snapWindowGap;
    int monitorGap = g_settings.snapMonitorGap;
    SnapMode mode = g_settings.snap;
    int acrossLow = horizontal ? frame.top : frame.left;
    int acrossHigh = horizontal ? frame.bottom : frame.right;

    if (mode == SnapMode::Both || mode == SnapMode::Windows) {
        for (const RECT& other : state.windows) {
            int theirLow = horizontal ? other.left : other.top;
            int theirHigh = horizontal ? other.right : other.bottom;
            int theirAcrossLow = horizontal ? other.top : other.left;
            int theirAcrossHigh = horizontal ? other.bottom : other.right;
            if (!RangesOverlap(acrossLow, acrossHigh, theirAcrossLow,
                               theirAcrossHigh)) {
                continue;
            }
            forLow->push_back(theirHigh + windowGap);  // against its far side
            forLow->push_back(theirLow);               // or flush with it
            forHigh->push_back(theirLow - windowGap);
            forHigh->push_back(theirHigh);
        }
    }

    if (mode == SnapMode::Both || mode == SnapMode::Monitor) {
        for (const RECT& work : state.monitors) {
            int workLow = horizontal ? work.left : work.top;
            int workHigh = horizontal ? work.right : work.bottom;
            forLow->push_back(workLow + monitorGap);
            forHigh->push_back(workHigh - monitorGap);
        }
    }
}

// A move shifts the whole window, so each axis takes the one correction that
// suits it best: moving a single edge here would resize the window instead.
void SnapMovedFrame(const SnapState& state, int limit, RECT* frame) {
    for (int axis = 0; axis < 2; axis++) {
        bool horizontal = axis == 0;
        std::vector<int> forLow, forHigh;
        CollectLines(state, *frame, horizontal, &forLow, &forHigh);

        int low = horizontal ? frame->left : frame->top;
        int high = horizontal ? frame->right : frame->bottom;
        int shiftLow = SnappedEdge(low, forLow, limit) - low;
        int shiftHigh = SnappedEdge(high, forHigh, limit) - high;
        int shift = shiftLow;
        if (shiftLow == 0) {
            shift = shiftHigh;
        } else if (shiftHigh != 0 && abs(shiftHigh) < abs(shiftLow)) {
            shift = shiftHigh;
        }

        if (horizontal) {
            frame->left += shift;
            frame->right += shift;
        } else {
            frame->top += shift;
            frame->bottom += shift;
        }
    }
}

bool EdgeMovesLeft(UINT edge) {
    return edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT;
}
bool EdgeMovesRight(UINT edge) {
    return edge == WMSZ_RIGHT || edge == WMSZ_TOPRIGHT ||
           edge == WMSZ_BOTTOMRIGHT;
}
bool EdgeMovesTop(UINT edge) {
    return edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT;
}
bool EdgeMovesBottom(UINT edge) {
    return edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT ||
           edge == WMSZ_BOTTOMRIGHT;
}

// A resize moves the edges the user has hold of and leaves the rest where
// they are, so each of those edges snaps on its own.
void SnapSizedFrame(const SnapState& state, UINT edge, int limit, RECT* frame) {
    for (int axis = 0; axis < 2; axis++) {
        bool horizontal = axis == 0;
        std::vector<int> forLow, forHigh;
        CollectLines(state, *frame, horizontal, &forLow, &forHigh);

        bool lowMoves = horizontal ? EdgeMovesLeft(edge) : EdgeMovesTop(edge);
        bool highMoves =
            horizontal ? EdgeMovesRight(edge) : EdgeMovesBottom(edge);
        LONG& low = horizontal ? frame->left : frame->top;
        LONG& high = horizontal ? frame->right : frame->bottom;
        if (lowMoves) {
            low = SnappedEdge(low, forLow, limit);
        }
        if (highMoves) {
            high = SnappedEdge(high, forHigh, limit);
        }
    }
}

// Keeps the shape the window started the resize with. The corner the user is
// not holding stays where it is, and on a corner drag the axis that moved
// further decides - so the window follows the cursor instead of fighting it.
void ApplyAspectRatio(UINT edge, const RECT& start, RECT* rc) {
    int startWidth = start.right - start.left;
    int startHeight = start.bottom - start.top;
    if (startWidth <= 0 || startHeight <= 0) {
        return;
    }
    double ratio = (double)startWidth / startHeight;
    int width = rc->right - rc->left;
    int height = rc->bottom - rc->top;

    bool horizontalOnly = edge == WMSZ_LEFT || edge == WMSZ_RIGHT;
    bool verticalOnly = edge == WMSZ_TOP || edge == WMSZ_BOTTOM;
    if (horizontalOnly || (!verticalOnly && abs(width - startWidth) >=
                                                abs(height - startHeight))) {
        height = (int)(width / ratio + 0.5);
    } else {
        width = (int)(height * ratio + 0.5);
    }

    if (EdgeMovesLeft(edge)) {
        rc->left = rc->right - width;
    } else {
        rc->right = rc->left + width;
    }
    if (EdgeMovesTop(edge)) {
        rc->top = rc->bottom - height;
    } else {
        rc->bottom = rc->top + height;
    }
}

////////////////////////////////////////////////////////////////////////////////
// What the loop hands over

int SnapDistancePx(HWND hwnd) {
    return MulDiv(g_settings.snapDistance, (int)WindowDpi(hwnd), 96);
}

bool KeepAspectHeld() {
    UINT vk = g_settings.keepAspectVk;
    return vk != 0 && GetKeyState((int)vk) < 0;
}

void AdjustMove(HWND hwnd, const SnapState& state, RECT* rc) {
    // The loop's own proposal is thrown away: it works the position out from
    // the rectangle it last applied, so anything changed here would be built
    // on from then on and the window would never come off the line again.
    POINT cursor;
    if (!GetCursorPos(&cursor)) {
        return;
    }
    int width = rc->right - rc->left;
    int height = rc->bottom - rc->top;

    POINT offset = state.grabOffset;
    if (state.grabSize.cx > 0 && state.grabSize.cy > 0 &&
        (width != state.grabSize.cx || height != state.grabSize.cy)) {
        // The loop resized the window mid-drag: a maximized one being
        // restored, or a move onto a monitor with a different scale. Keep the
        // cursor where it was on the window rather than where it was in
        // pixels.
        offset.x = MulDiv(offset.x, width, state.grabSize.cx);
        offset.y = MulDiv(offset.y, height, state.grabSize.cy);
    }

    RECT window{cursor.x - offset.x, cursor.y - offset.y, 0, 0};
    window.right = window.left + width;
    window.bottom = window.top + height;

    RECT frame = WindowToFrame(window, state.inset);
    if (g_settings.snap != SnapMode::Off) {
        SnapMovedFrame(state, SnapDistancePx(hwnd), &frame);
    }
    *rc = FrameToWindow(frame, state.inset);
}

void AdjustSize(HWND hwnd, const SnapState& state, UINT edge, RECT* rc) {
    if (KeepAspectHeld()) {
        // The ratio and the magnet want different rectangles, and the one the
        // user is holding a key down for wins.
        ApplyAspectRatio(edge, state.startFrame, rc);
        return;
    }
    if (g_settings.snap == SnapMode::Off) {
        return;
    }
    RECT frame = WindowToFrame(*rc, state.inset);
    SnapSizedFrame(state, edge, SnapDistancePx(hwnd), &frame);
    *rc = FrameToWindow(frame, state.inset);
}

////////////////////////////////////////////////////////////////////////////////
// The neighbours to line up with

struct NeighbourSearch {
    HWND dragged;
    SnapState* state;
};

BOOL CALLBACK CollectWindowProc(HWND hwnd, LPARAM lParam) {
    auto* search = (NeighbourSearch*)lParam;
    if (hwnd == search->dragged || !IsWindowVisible(hwnd) || IsIconic(hwnd) ||
        !IsFrameWindow(hwnd)) {
        return TRUE;
    }
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked))) &&
        cloaked) {
        return TRUE;  // another virtual desktop, or a suspended app
    }

    RECT frame = VisibleFrameOf(hwnd);
    if (frame.right > frame.left && frame.bottom > frame.top) {
        search->state->windows.push_back(frame);
    }
    return TRUE;
}

BOOL CALLBACK CollectMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM lParam) {
    auto* state = (SnapState*)lParam;
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(monitor, &info)) {
        state->monitors.push_back(info.rcWork);
    }
    return TRUE;
}

// Once per drag: windows do not move while one is going on, and asking for
// them again at every step would be forty times a second.
void CollectNeighbours(HWND dragged, SnapState* state) {
    NeighbourSearch search{dragged, state};
    EnumWindows(CollectWindowProc, (LPARAM)&search);
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitorProc, (LPARAM)state);
}

////////////////////////////////////////////////////////////////////////////////
// The subclass

LRESULT CALLBACK SnapSubclassProc(HWND hwnd,
                                  UINT uMsg,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  UINT_PTR uIdSubclass,
                                  DWORD_PTR dwRefData) {
    ModRef ref;  // the image must not go away under this procedure

    switch (uMsg) {
        case WM_MOVING:
        case WM_SIZING: {
            // Ours first and the application's afterwards: whatever it wants
            // to do to the rectangle - a terminal rounding it to whole rows,
            // a window with rules of its own - gets the last word.
            std::lock_guard<std::mutex> lock(g_snapMutex);
            auto it = g_snaps.find(hwnd);
            if (it == g_snaps.end()) {
                break;
            }
            if (uMsg == WM_MOVING) {
                AdjustMove(hwnd, it->second, (RECT*)lParam);
            } else {
                AdjustSize(hwnd, it->second, (UINT)wParam, (RECT*)lParam);
            }
            break;
        }

        case WM_DPICHANGED: {
            // The invisible border is measured in pixels, and the window has
            // just changed how big a pixel is.
            std::lock_guard<std::mutex> lock(g_snapMutex);
            auto it = g_snaps.find(hwnd);
            if (it != g_snaps.end()) {
                it->second.inset = FrameInsetOf(hwnd);
            }
            break;
        }

        case WM_EXITSIZEMOVE:
        case WM_NCDESTROY:
            EndDragSnap(hwnd);
            break;

        default:
            if (uMsg == g_msgDrag && wParam == kDragUnsnap) {
                // Sent rather than posted: the teardown taking the subclass
                // off before the image goes away.
                EndDragSnap(hwnd);
                return 0;
            }
            break;
    }

    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

std::vector<HWND> SnapshotSnappedWindows() {
    std::lock_guard<std::mutex> lock(g_snapMutex);
    std::vector<HWND> result;
    result.reserve(g_snaps.size());
    for (const auto& [hwnd, state] : g_snaps) {
        result.push_back(hwnd);
    }
    return result;
}

bool IsDragSnapping(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_snapMutex);
    return g_snaps.count(hwnd) != 0;
}

// Called from the drag request, on the window's own thread, once a move or
// size loop is about to start.
void BeginDragSnap(HWND root, WPARAM kind, POINT pt) {
    bool wantSnap = g_settings.snap != SnapMode::Off;
    bool wantRatio = kind == kDragResize && g_settings.keepAspectVk != 0;
    if (g_uninitializing || (!wantSnap && !wantRatio)) {
        return;
    }

    RECT window{};
    if (!GetWindowRect(root, &window)) {
        return;
    }

    SnapState state;
    state.kind = kind;
    state.inset = FrameInsetOf(root);
    state.startFrame = WindowToFrame(window, state.inset);

    state.grabOffset = POINT{pt.x - window.left, pt.y - window.top};
    state.grabSize =
        SIZE{window.right - window.left, window.bottom - window.top};
    if (wantSnap) {
        CollectNeighbours(root, &state);
    }

    {
        std::lock_guard<std::mutex> lock(g_snapMutex);
        if (!g_snaps.emplace(root, std::move(state)).second) {
            return;  // a drag of this window is somehow still going on
        }
    }

    if (!SetWindowSubclass(root, SnapSubclassProc, kSnapSubclassId, 0)) {
        Wh_Log(L"SetWindowSubclass failed for %p", root);
        std::lock_guard<std::mutex> lock(g_snapMutex);
        g_snaps.erase(root);
    }
}

// On the window's own thread: at the end of the drag, when the window dies,
// or when the teardown asks for it.
void EndDragSnap(HWND hwnd) {
    bool had = false;
    {
        std::lock_guard<std::mutex> lock(g_snapMutex);
        had = g_snaps.erase(hwnd) != 0;
    }
    if (had) {
        RemoveWindowSubclass(hwnd, SnapSubclassProc, kSnapSubclassId);
    }
}
