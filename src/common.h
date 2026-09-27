// Everything the modules share: the system headers they all need, the settings
// they all read, and a declaration for every function or global that crosses a
// file boundary.
//
// Each src/*.cpp includes only this header, which keeps every module a valid
// translation unit on its own (so an IDE resolves references and can check a
// single file), while `tools/bundle.py` concatenates them into the one
// mod.wh.cpp file Windhawk wants.
#pragma once

#include <windhawk_utils.h>

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

////////////////////////////////////////////////////////////////////////////////
// Unloading

// Set before anything is torn down. Everything that installs a hook or a
// subclass checks it (holding the same lock the teardown takes) so that
// nothing of ours is put back in place behind the teardown's back.
extern std::atomic<bool> g_uninitializing;

// How many hook procedures and worker threads of ours are running right now.
// UnhookWindowsHookEx does not wait for a hook procedure that is executing on
// another thread, so Wh_ModUninit waits for this to reach zero before letting
// the image go.
extern std::atomic<int> g_modRefCount;

struct ModRef {
    ModRef() { g_modRefCount++; }
    ~ModRef() { g_modRefCount--; }
    ModRef(const ModRef&) = delete;
    ModRef& operator=(const ModRef&) = delete;
};

////////////////////////////////////////////////////////////////////////////////
// Animation

// How far along an animation is, eased in and out (smoothstep) so that a short
// one reads as a movement rather than a jump. Every animation in the mod runs
// on this curve, so they all feel like the same piece of work.
inline double AnimationProgress(int elapsedMs, int durationMs) {
    if (durationMs <= 0 || elapsedMs >= durationMs) {
        return 1.0;
    }
    if (elapsedMs <= 0) {
        return 0.0;
    }
    double t = (double)elapsedMs / durationMs;
    return t * t * (3.0 - 2.0 * t);
}

////////////////////////////////////////////////////////////////////////////////
// Settings

enum class DragModifier { Win, Alt };
enum class MenuBarMode { Hide, KeepMenu, Skip };
// What a mouse gesture with the modifier held does to a window.
enum class WindowAction { None, ToggleMaximize, ToggleTitleBar, Close };
// Which drags fade the window they are dragging.
enum class DragTranslucency { Both, MoveOnly, Off };
// What a dragged window's edges are magnetic to.
enum class SnapMode { Both, Monitor, Windows, Off };

// Sentinel for "leave the DWM attribute alone" (not a valid DWM color value).
constexpr COLORREF kColorUntouched = 0xFFFFFFFD;
// And for "whatever the Windows accent color is". Kept as a sentinel rather
// than resolved when the settings are read, so that an accent color that
// changes afterwards is followed without reparsing anything.
constexpr COLORREF kColorAccent = 0xFFFFFFFC;

// The translucency of a dragged window, and how long it takes to get there
// and back. Also what a setting Windhawk has never written out means, since
// none of the three is usefully zero.
constexpr int kDefaultDragOpacity = 85;
constexpr int kDefaultDragFadeIn = 120;
constexpr int kDefaultDragFadeOut = 60;

// How long the border color takes to cross from one setting to the other when
// focus moves. Zero means the default here too.
constexpr int kDefaultBorderFade = 150;

// How close an edge has to come before it sticks, in units of a 96 dpi pixel
// so that the pull feels the same whatever a monitor is scaled to.
constexpr int kDefaultSnapDistance = 12;

// A parsed combination of modifiers and one key or mouse button. vk == 0
// means there is no binding. Small and trivially copyable, so the settings
// can hold one whole rather than a field at a time.
struct Hotkey {
    UINT vk = 0;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool win = false;
};

constexpr PCWSTR kDefaultHotkey = L"Ctrl+Alt+H";
constexpr PCWSTR kDefaultWindowShortcut = L"Win+MButton";

struct Settings {
    std::atomic<Hotkey> hotkey{};
    std::atomic<Hotkey> windowShortcut{};
    std::atomic<WindowAction> windowShortcutAction{WindowAction::Close};
    std::atomic<DragModifier> dragModifier{DragModifier::Win};
    std::atomic<bool> topEdgeResize{false};
    std::atomic<WindowAction> doubleClickAction{WindowAction::ToggleMaximize};
    std::atomic<int> doubleClickTime{0};  // 0: the Windows double-click speed
    std::atomic<DragTranslucency> dragTranslucency{DragTranslucency::Both};
    std::atomic<int> dragOpacity{kDefaultDragOpacity};
    std::atomic<int> dragFadeIn{kDefaultDragFadeIn};
    std::atomic<int> dragFadeOut{kDefaultDragFadeOut};
    std::atomic<MenuBarMode> menuBarMode{MenuBarMode::Hide};
    std::atomic<bool> hideByDefault{false};
    std::atomic<COLORREF> borderActive{kColorUntouched};
    std::atomic<COLORREF> borderInactive{kColorUntouched};
    std::atomic<bool> borderFade{true};
    // Off: the border colors apply to every window with a frame. On: only to
    // the ones the mod has taken the title bar from.
    std::atomic<bool> borderFramelessOnly{false};
    std::atomic<int> borderFadeDuration{kDefaultBorderFade};
    std::atomic<int> corners{DWMWCP_DEFAULT};
    std::atomic<SnapMode> snap{SnapMode::Both};
    std::atomic<int> snapDistance{kDefaultSnapDistance};
    std::atomic<int> snapWindowGap{0};
    std::atomic<int> snapMonitorGap{0};
    std::atomic<UINT> keepAspectVk{VK_SHIFT};   // 0: never keep the ratio
    std::atomic<UINT> snapModifierVk{VK_CONTROL};  // 0: no extra key at all
    std::atomic<bool> snapModifierHold{true};   // false: it suppresses instead
};

extern Settings g_settings;

std::wstring NormalizeSettingString(PCWSTR raw);
std::wstring ThisProgramName();
bool ProgramEntryMatches(PCWSTR entry, const std::wstring& program);
UINT ParseKeyName(PCWSTR raw);
// An empty setting means the documented default, which is not the same one
// for every binding - Windhawk hands out an empty string for a setting it has
// never written.
Hotkey ParseHotkey(PCWSTR raw, PCWSTR whenEmpty = kDefaultHotkey);
COLORREF ParseBorderColor(PCWSTR raw);
bool ParseBorderTransition(PCWSTR raw);
MenuBarMode ParseMenuBarMode(PCWSTR raw);
int ParseCorners(PCWSTR raw);
DragTranslucency ParseDragTranslucency(PCWSTR raw);
SnapMode ParseSnapMode(PCWSTR raw);
bool ParseSnapModifierHold(PCWSTR raw);
// A single modifier key, as a virtual key; 0 when the setting turns it off.
UINT ParseModifierVk(PCWSTR raw, UINT whenEmpty);
WindowAction ParseWindowAction(PCWSTR raw, WindowAction whenEmpty);
int ClampedSetting(int value, int fallback, int low, int high);
void LoadSettings();

////////////////////////////////////////////////////////////////////////////////
// Which windows we may touch, and their metrics

bool IsExcludedClass(HWND hwnd);
bool HasFrameStyles(LONG_PTR style);
bool IsFrameWindow(HWND hwnd);
bool IsAutoHideCandidate(HWND hwnd);
UINT WindowDpi(HWND hwnd);
int ResizeHandleHeight(HWND hwnd);

////////////////////////////////////////////////////////////////////////////////
// Non-client layout of a window with a hidden title bar

int MaximizedTopInset(HWND hwnd, const RECT& proposed);
LRESULT OnNcCalcSize(HWND hwnd, WPARAM wParam, LPARAM lParam);
LRESULT AdjustHitTest(HWND hwnd, LRESULT hit, LPARAM lParam);

////////////////////////////////////////////////////////////////////////////////
// DWM decorations

bool BorderColorsWanted();
bool IsBorderColorTarget(HWND hwnd);
COLORREF AccentBorderColor();
COLORREF BorderColorFor(bool active);
// The border color last written to a window, which is where a fade starts
// from. kColorUntouched when nothing of ours is on it.
COLORREF CurrentBorderColor(HWND hwnd);
bool IsBlendableColor(COLORREF color);
COLORREF BlendColor(COLORREF from, COLORREF to, double t);
void ApplyBorderColor(HWND hwnd, bool active);
void RestoreBorderColor(HWND hwnd);
void RefreshBorderColor(HWND hwnd);
void RefreshBorderColors();
void RestoreAllBorderColors();
std::vector<HWND> SnapshotColoredWindows();
void ForgetBorderColor(HWND hwnd);
// A window's own thread reporting that it has gained or lost focus.
void OnWindowActivation(HWND hwnd, bool active);
// The border color on a focus change, faded across instead of switched.
void AnimateBorderColor(HWND hwnd, bool active);
bool IsBorderFading(HWND hwnd);
void CancelBorderFade(HWND hwnd);
void FinishBorderFades();
void ApplyCorners(HWND hwnd);
void ApplyDwmAttributes(HWND hwnd);
void RestoreDwmAttributes(HWND hwnd);

////////////////////////////////////////////////////////////////////////////////
// Hiding / restoring the title bar

// Every request to hide/restore a title bar is posted to the window as
// `g_msgFrameless` and handled on the window's own thread, which is the only
// thread that may (un)subclass it.
// kActionAutoHide is the "hide by default" path. Unlike the explicit actions it
// is re-checked when it runs, because by then the window may have turned out to
// be something we should not touch.
enum FramelessAction : WPARAM {
    kActionToggle = 0,
    kActionHide = 1,
    kActionShow = 2,
    kActionAutoHide = 3,
};

extern UINT g_msgFrameless;  // RegisterWindowMessage, set in Wh_ModInit

constexpr UINT_PTR kSubclassId = 0x48597072;  // 'Hypr'

// dwRefData flag for the subclass: the frame layout was left to the system.
constexpr DWORD_PTR kRefKeepMenu = 1;

bool IsFrameless(HWND hwnd);
void MarkDwmTouched(HWND hwnd);
bool IsDwmTouched(HWND hwnd);
std::vector<HWND> SnapshotFramelessWindows();
std::vector<HWND> SnapshotAutoHiddenWindows();
void RefreshFrame(HWND hwnd);
bool MakeFrameless(HWND hwnd, bool autoHidden);
void RestoreFrame(HWND hwnd);
void HandleFramelessRequest(HWND hwnd, WPARAM action);
void RequestFrameless(HWND hwnd, WPARAM action);
void AutoHideExistingWindows();
void RestoreAutoHiddenWindows();

////////////////////////////////////////////////////////////////////////////////
// Hotkey

bool HandleHotkey(const MSG* msg);

////////////////////////////////////////////////////////////////////////////////
// Win + mouse: Start-menu suppression and the drags themselves

constexpr WORD kMaskVk = 0xE8;  // unassigned VK, only used as "a key was hit"
// Marks the keystrokes the mod injects to keep the Start menu shut, so its
// own low-level hook lets them through.
constexpr ULONG_PTR kInjectedMarker = 0x48797072;  // 'Hypr'

void ArmWinMask(bool usingWin);
void MaskModifierTap();
bool IsShellProcess();
bool IsElevatedProcess();
void StartKeyboardServer();
void StartKeyboardServerForWindow();
void ShutdownKeyboardServer();
bool HandleBindingKey(UINT vk, bool down);

// A drag is requested with this message, posted to the window that is to be
// moved or resized - see the comment at the top of drag.cpp.
extern UINT g_msgDrag;  // RegisterWindowMessage, set in Wh_ModInit

enum DragKind : WPARAM {
    kDragMove = 0,
    kDragResize = 1,
    // Not a drag: the drag is over, and the window's own thread is the only
    // one that may take the mod's WS_EX_LAYERED back off - see drag_fade.cpp.
    kDragUnfade = 2,
    // Nor is this one: a gesture that asks the window to do something to
    // itself, with the WindowAction in lParam - see actions.cpp.
    kDragAction = 3,
    // Sent, not posted, and answered by the drag subclass rather than by the
    // message hook: the teardown taking that subclass off - see snap.cpp.
    kDragUnsnap = 4,
};

// How long the thread that ends a resize waits for its loop to start. The
// request that starts it is posted, so the loop is a moment away - and the
// release it posts is no use at all before the loop is running.
constexpr int kMoveSizeStartWaitMs = 500;

bool IsDragModifierDown();
bool IsThreadInMoveSizeLoop(DWORD threadId);
bool IsInMoveSizeLoop();
int PhysicalButtonVk(bool right);
void ForceLeftButtonDown();
UINT ResizeEdgeForPoint(const RECT& rc, POINT pt);
void RequestDrag(HWND root, WPARAM kind, POINT pt);

// Runs on the target window's thread and rewrites the request into the system
// command that starts a move or size loop. Returns false if there is nothing
// to start, which means the message is to be swallowed.

// Per-thread: a button-up to swallow because we swallowed its button-down.
extern thread_local bool g_swallowButtonUp[2];  // [0] = left, [1] = right
bool HandleDragRequest(MSG* msg);
bool HandleModifierButtonDown(const MSG* msg, bool right);
bool HandleButtonUp(bool right);
bool HandleLeftButtonUp();

////////////////////////////////////////////////////////////////////////////////
// Gestures with the modifier held, and what they do to a window

// A binding matches when its key or button is the one that just arrived and
// the modifiers are held.
bool IsMouseButtonVk(UINT vk);
UINT ButtonVkForMessage(UINT message, WPARAM wParam);
bool MatchesShortcut(const Hotkey& binding, UINT vk, bool now = false);
bool HandleShortcutButton(const MSG* msg);
bool HandleShortcutButtonUp(const MSG* msg);

int DoubleClickTimeMs();
// Whether this press and the one before it on this thread are a double click.
// Takes the tick instead of reading the clock, so the rules are testable.
bool IsDoubleClickAt(HWND root, POINT pt, DWORD tick);
void ForgetLastPress();
void DoWindowAction(HWND hwnd, WindowAction action);
void RequestWindowAction(HWND root, WindowAction action);

////////////////////////////////////////////////////////////////////////////////
// Magnetic edges, and the aspect ratio while resizing

RECT VisibleFrameOf(HWND hwnd);
bool SnapAllowedWith(bool modifierDown);
bool SnapAllowed();
void BeginDragSnap(HWND root, WPARAM kind, POINT pt);
void EndDragSnap(HWND hwnd);
bool IsDragSnapping(HWND hwnd);
std::vector<HWND> SnapshotSnappedWindows();

////////////////////////////////////////////////////////////////////////////////
// Translucency while dragging

bool IsDragFading(HWND hwnd);
BYTE DragAlphaFor(BYTE baseAlpha, int opacityPercent);
BYTE FadeAlphaAt(BYTE from, BYTE to, int durationMs, int elapsedMs);
void BeginDragFade(HWND root, WPARAM kind);
void EndDragFade(HWND hwnd);

////////////////////////////////////////////////////////////////////////////////
// The hooked APIs

void ProcessRetrievedMessage(MSG* msg);
void OnWindowCreated(HWND hwnd, DWORD dwStyle);

// Message interception lives in a WH_GETMESSAGE hook, one per pumping thread -
// see the comment at the top of hooks.cpp for why it is not an API hook.
void InstallMessageHookForThread();
void InstallMessageHooks();
void RemoveMessageHooks();
void RefreshCallWndProcHooks();

using CreateWindowExW_t = decltype(&CreateWindowExW);
using CreateWindowExA_t = decltype(&CreateWindowExA);

extern CreateWindowExW_t CreateWindowExW_Original;
extern CreateWindowExA_t CreateWindowExA_Original;

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
                                 LPVOID lpParam);
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
                                 LPVOID lpParam);
