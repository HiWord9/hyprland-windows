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
// Settings

enum class DragModifier { Win, Alt };
enum class MenuBarMode { Hide, KeepMenu, Skip };
// Which drags fade the window they are dragging.
enum class DragTranslucency { Both, MoveOnly, Off };

// Sentinel for "leave the DWM attribute alone" (not a valid DWM color value).
constexpr COLORREF kColorUntouched = 0xFFFFFFFD;

// The translucency of a dragged window, and how long it takes to get there
// and back. Also what a setting Windhawk has never written out means, since
// none of the three is usefully zero.
constexpr int kDefaultDragOpacity = 85;
constexpr int kDefaultDragFadeIn = 120;
constexpr int kDefaultDragFadeOut = 60;

struct Settings {
    std::atomic<UINT> hotkeyVk{0};
    std::atomic<bool> hotkeyCtrl{true};
    std::atomic<bool> hotkeyAlt{true};
    std::atomic<bool> hotkeyShift{false};
    std::atomic<bool> hotkeyWin{false};
    std::atomic<DragModifier> dragModifier{DragModifier::Win};
    std::atomic<bool> topEdgeResize{false};
    std::atomic<DragTranslucency> dragTranslucency{DragTranslucency::Both};
    std::atomic<int> dragOpacity{kDefaultDragOpacity};
    std::atomic<int> dragFadeIn{kDefaultDragFadeIn};
    std::atomic<int> dragFadeOut{kDefaultDragFadeOut};
    std::atomic<MenuBarMode> menuBarMode{MenuBarMode::Hide};
    std::atomic<bool> hideByDefault{false};
    std::atomic<COLORREF> borderActive{kColorUntouched};
    std::atomic<COLORREF> borderInactive{kColorUntouched};
    std::atomic<int> corners{DWMWCP_DEFAULT};
};

extern Settings g_settings;

// A parsed key combination. vk == 0 means "no hotkey".
struct Hotkey {
    UINT vk = 0;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool win = false;
};

constexpr PCWSTR kDefaultHotkey = L"Ctrl+Alt+H";

std::wstring NormalizeSettingString(PCWSTR raw);
UINT ParseKeyName(PCWSTR raw);
Hotkey ParseHotkey(PCWSTR raw);
COLORREF ParseBorderColor(PCWSTR raw);
MenuBarMode ParseMenuBarMode(PCWSTR raw);
int ParseCorners(PCWSTR raw);
DragTranslucency ParseDragTranslucency(PCWSTR raw);
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

void ApplyBorderColor(HWND hwnd, bool active);
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

void ArmWinMask();
void ShutdownWinMask();

// A drag is requested with this message, posted to the window that is to be
// moved or resized - see the comment at the top of drag.cpp.
extern UINT g_msgDrag;  // RegisterWindowMessage, set in Wh_ModInit

enum DragKind : WPARAM {
    kDragMove = 0,
    kDragResize = 1,
    // Not a drag: the drag is over, and the window's own thread is the only
    // one that may take the mod's WS_EX_LAYERED back off - see drag_fade.cpp.
    kDragUnfade = 2,
};

// How long the threads that watch a drag wait for its loop to start. The
// request that starts it is posted, so the loop is a moment away - and
// nothing either of them does is any use before it is running.
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
