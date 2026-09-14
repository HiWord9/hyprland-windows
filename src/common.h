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
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

////////////////////////////////////////////////////////////////////////////////
// Settings

enum class DragModifier { Win, Alt };
enum class MenuBarMode { Hide, KeepMenu, Skip };

// Sentinel for "leave the DWM attribute alone" (not a valid DWM color value).
constexpr COLORREF kColorUntouched = 0xFFFFFFFD;

struct Settings {
    std::atomic<UINT> hotkeyVk{0};
    std::atomic<bool> hotkeyCtrl{true};
    std::atomic<bool> hotkeyAlt{true};
    std::atomic<bool> hotkeyShift{false};
    std::atomic<bool> hotkeyWin{false};
    std::atomic<DragModifier> dragModifier{DragModifier::Win};
    std::atomic<bool> topEdgeResize{true};
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
std::vector<HWND> SnapshotFramelessWindows();
void RefreshFrame(HWND hwnd);
bool MakeFrameless(HWND hwnd);
void RestoreFrame(HWND hwnd);
void HandleFramelessRequest(HWND hwnd, WPARAM action);
void RequestFrameless(HWND hwnd, WPARAM action);
void AutoHideExistingWindows();

////////////////////////////////////////////////////////////////////////////////
// Hotkey

bool HandleHotkey(const MSG* msg);

////////////////////////////////////////////////////////////////////////////////
// Win + mouse: Start-menu suppression and the drags themselves

constexpr WORD kMaskVk = 0xE8;  // unassigned VK, only used as "a key was hit"
constexpr ULONG_PTR kInjectedMarker = 0x48797072;  // 'Hypr'

void ArmWinMask();
void ShutdownWinMask();

bool IsDragModifierDown();
int PhysicalButtonVk(bool right);
void InjectMouseButton(DWORD flags);
void DrainInjectedLeftButton();
UINT ResizeCornerForPoint(const RECT& rc, POINT pt);
void StartMove(HWND root, POINT pt);
void StartResize(HWND root, POINT pt);

// Per-thread: a button-up to swallow because we swallowed its button-down.
extern thread_local bool g_swallowButtonUp[2];  // [0] = left, [1] = right
bool HandleModifierButtonDown(const MSG* msg, bool right);
bool HandleButtonUp(bool right);

////////////////////////////////////////////////////////////////////////////////
// The hooked APIs

void ProcessRetrievedMessage(MSG* msg);
void OnWindowCreated(HWND hwnd, DWORD dwStyle);

using GetMessageW_t = decltype(&GetMessageW);
using GetMessageA_t = decltype(&GetMessageA);
using PeekMessageW_t = decltype(&PeekMessageW);
using PeekMessageA_t = decltype(&PeekMessageA);
using CreateWindowExW_t = decltype(&CreateWindowExW);
using CreateWindowExA_t = decltype(&CreateWindowExA);

extern GetMessageW_t GetMessageW_Original;
extern GetMessageA_t GetMessageA_Original;
extern PeekMessageW_t PeekMessageW_Original;
extern PeekMessageA_t PeekMessageA_Original;
extern CreateWindowExW_t CreateWindowExW_Original;
extern CreateWindowExA_t CreateWindowExA_Original;

BOOL WINAPI GetMessageW_Hook(LPMSG lpMsg,
                             HWND hWnd,
                             UINT wMsgFilterMin,
                             UINT wMsgFilterMax);
BOOL WINAPI GetMessageA_Hook(LPMSG lpMsg,
                             HWND hWnd,
                             UINT wMsgFilterMin,
                             UINT wMsgFilterMax);
BOOL WINAPI PeekMessageW_Hook(LPMSG lpMsg,
                              HWND hWnd,
                              UINT wMsgFilterMin,
                              UINT wMsgFilterMax,
                              UINT wRemoveMsg);
BOOL WINAPI PeekMessageA_Hook(LPMSG lpMsg,
                              HWND hWnd,
                              UINT wMsgFilterMin,
                              UINT wMsgFilterMax,
                              UINT wRemoveMsg);
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
