// Reading and parsing the mod's user settings.
#include "common.h"

Settings g_settings;

std::wstring NormalizeSettingString(PCWSTR raw) {
    std::wstring s = raw ? raw : L"";
    auto notSpace = [](wchar_t c) { return !std::iswspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    for (auto& c : s) {
        c = (wchar_t)std::towupper(c);
    }
    return s;
}

// The file name of this process's executable, e.g. "explorer.exe".
std::wstring ThisProgramName() {
    WCHAR path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!len || len >= MAX_PATH) {
        return L"";
    }
    PCWSTR slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

// Whether a program list entry names this program. Compared by file name and
// case-insensitively, so a full path pasted into the setting works as well.
bool ProgramEntryMatches(PCWSTR entry, const std::wstring& program) {
    std::wstring name = NormalizeSettingString(entry);
    size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        name = name.substr(slash + 1);
    }
    return !name.empty() && name == NormalizeSettingString(program.c_str());
}

bool ProgramInListSetting(PCWSTR name) {
    std::wstring program = ThisProgramName();
    for (int i = 0; i < 256; i++) {
        WindhawkUtils::StringSetting entry(
            Wh_GetStringSetting(L"%s[%d]", name, i));
        if (!*entry.get()) {
            break;  // the end of the list
        }
        if (ProgramEntryMatches(entry, program)) {
            return true;
        }
    }
    return false;
}

// Turns the "key" setting into a virtual key code, 0 if unusable.
UINT ParseKeyName(PCWSTR raw) {
    std::wstring s = NormalizeSettingString(raw);
    if (s.empty()) {
        return 0;
    }

    if (s.rfind(L"VK_", 0) == 0) {
        s = s.substr(3);
    }

    if (s.size() == 1) {
        wchar_t c = s[0];
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) {
            return c;
        }
        SHORT vk = VkKeyScanW(c);
        return vk == -1 ? 0 : (vk & 0xFF);
    }

    if (s.size() > 2 && s[0] == L'0' && s[1] == L'X') {
        return (UINT)wcstoul(s.c_str() + 2, nullptr, 16) & 0xFF;
    }

    if (s[0] == L'F' && s.size() <= 3 &&
        std::all_of(s.begin() + 1, s.end(),
                    [](wchar_t c) { return std::iswdigit(c) != 0; })) {
        int n = _wtoi(s.c_str() + 1);
        if (n >= 1 && n <= 24) {
            return VK_F1 + n - 1;
        }
        return 0;
    }

    static const struct {
        PCWSTR name;
        UINT vk;
    } kNames[] = {
        {L"SPACE", VK_SPACE},        {L"TAB", VK_TAB},
        {L"ENTER", VK_RETURN},       {L"RETURN", VK_RETURN},
        {L"ESC", VK_ESCAPE},         {L"ESCAPE", VK_ESCAPE},
        {L"BACKSPACE", VK_BACK},     {L"BACK", VK_BACK},
        {L"INSERT", VK_INSERT},      {L"INS", VK_INSERT},
        {L"DELETE", VK_DELETE},      {L"DEL", VK_DELETE},
        {L"HOME", VK_HOME},          {L"END", VK_END},
        {L"PAGEUP", VK_PRIOR},       {L"PRIOR", VK_PRIOR},
        {L"PAGEDOWN", VK_NEXT},      {L"NEXT", VK_NEXT},
        {L"UP", VK_UP},              {L"DOWN", VK_DOWN},
        {L"LEFT", VK_LEFT},          {L"RIGHT", VK_RIGHT},
        {L"PAUSE", VK_PAUSE},        {L"SCROLLLOCK", VK_SCROLL},
        {L"SCROLL", VK_SCROLL},      {L"PRINTSCREEN", VK_SNAPSHOT},
        {L"SNAPSHOT", VK_SNAPSHOT},  {L"CAPSLOCK", VK_CAPITAL},
        {L"CAPITAL", VK_CAPITAL},    {L"NUMLOCK", VK_NUMLOCK},
        {L"APPS", VK_APPS},          {L"MENU", VK_APPS},
        {L"NUMPAD0", VK_NUMPAD0},    {L"NUMPAD1", VK_NUMPAD1},
        {L"NUMPAD2", VK_NUMPAD2},    {L"NUMPAD3", VK_NUMPAD3},
        {L"NUMPAD4", VK_NUMPAD4},    {L"NUMPAD5", VK_NUMPAD5},
        {L"NUMPAD6", VK_NUMPAD6},    {L"NUMPAD7", VK_NUMPAD7},
        {L"NUMPAD8", VK_NUMPAD8},    {L"NUMPAD9", VK_NUMPAD9},
        {L"MULTIPLY", VK_MULTIPLY},  {L"ADD", VK_ADD},
        {L"SUBTRACT", VK_SUBTRACT},  {L"DECIMAL", VK_DECIMAL},
        {L"DIVIDE", VK_DIVIDE},
        // Mouse buttons, so that a binding can be a click as easily as a key.
        {L"MBUTTON", VK_MBUTTON},    {L"MIDDLE", VK_MBUTTON},
        {L"XBUTTON1", VK_XBUTTON1},  {L"X1", VK_XBUTTON1},
        {L"XBUTTON2", VK_XBUTTON2},  {L"X2", VK_XBUTTON2},
        {L"LBUTTON", VK_LBUTTON},    {L"RBUTTON", VK_RBUTTON},
    };
    for (const auto& entry : kNames) {
        if (s == entry.name) {
            return entry.vk;
        }
    }

    return 0;
}

// "" / "default" -> untouched, "none" -> DWMWA_COLOR_NONE, "#RRGGBB" -> color.
COLORREF ParseBorderColor(PCWSTR raw) {
    std::wstring s = NormalizeSettingString(raw);
    if (s.empty() || s == L"DEFAULT") {
        return kColorUntouched;
    }
    if (s == L"NONE") {
        return DWMWA_COLOR_NONE;
    }
    if (s == L"ACCENT") {
        return kColorAccent;
    }
    if (s[0] == L'#') {
        s.erase(0, 1);
    }
    if (s.size() == 3) {
        s = {s[0], s[0], s[1], s[1], s[2], s[2]};
    }
    if (s.size() != 6 || !std::all_of(s.begin(), s.end(), [](wchar_t c) {
            return std::iswxdigit(c) != 0;
        })) {
        return kColorUntouched;
    }
    unsigned long v = wcstoul(s.c_str(), nullptr, 16);
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

MenuBarMode ParseMenuBarMode(PCWSTR raw) {
    std::wstring s = NormalizeSettingString(raw);
    if (s == L"KEEPMENU") {
        return MenuBarMode::KeepMenu;
    }
    if (s == L"SKIP") {
        return MenuBarMode::Skip;
    }
    return MenuBarMode::Hide;
}

// Anything unrecognized fades both drags, so a setting Windhawk has never
// written out - an empty string - means what the settings say it does.
DragTranslucency ParseDragTranslucency(PCWSTR raw) {
    std::wstring s = NormalizeSettingString(raw);
    if (s == L"OPAQUE" || s == L"OFF" || s == L"NONE") {
        return DragTranslucency::Off;
    }
    if (s == L"MOVE" || s == L"MOVEONLY") {
        return DragTranslucency::MoveOnly;
    }
    return DragTranslucency::Both;
}

SnapMode ParseSnapMode(PCWSTR raw) {
    std::wstring s = NormalizeSettingString(raw);
    if (s == L"OFF" || s == L"NONE") {
        return SnapMode::Off;
    }
    if (s == L"MONITOR" || s == L"SCREEN") {
        return SnapMode::Monitor;
    }
    if (s == L"WINDOWS") {
        return SnapMode::Windows;
    }
    return SnapMode::Both;
}

// "suppress" turns the extra key around: magnetic unless it is held.
bool ParseSnapModifierHold(PCWSTR raw) {
    return NormalizeSettingString(raw) != L"SUPPRESS";
}

UINT ParseModifierVk(PCWSTR raw, UINT whenEmpty) {
    std::wstring s = NormalizeSettingString(raw);
    if (s.empty()) {
        return whenEmpty;
    }
    if (s == L"SHIFT") {
        return VK_SHIFT;
    }
    if (s == L"CTRL" || s == L"CONTROL") {
        return VK_CONTROL;
    }
    if (s == L"ALT") {
        return VK_MENU;
    }
    return 0;  // "off", and anything else nobody can hold down
}

// A setting Windhawk has never written out reads as zero, which is why zero
// means "the documented default" rather than a value of its own.
int ClampedSetting(int value, int fallback, int low, int high) {
    return value <= 0 ? fallback : std::clamp(value, low, high);
}

int ParseCorners(PCWSTR raw) {
    std::wstring s = NormalizeSettingString(raw);
    if (s == L"ROUND") {
        return DWMWCP_ROUND;
    }
    if (s == L"ROUNDSMALL") {
        return DWMWCP_ROUNDSMALL;
    }
    if (s == L"NONE") {
        return DWMWCP_DONOTROUND;
    }
    return DWMWCP_DEFAULT;
}

// Splits "Ctrl+Alt+H" into the key and its modifiers. An empty setting means
// "use the default", so the hotkey works even before Windhawk has written the
// settings out; "none" is how you turn it off.
Hotkey ParseHotkey(PCWSTR raw, PCWSTR whenEmpty) {
    std::wstring spec = NormalizeSettingString(raw);
    if (spec.empty()) {
        spec = NormalizeSettingString(whenEmpty);
    }
    if (spec == L"NONE" || spec == L"OFF" || spec == L"-") {
        return {};
    }

    Hotkey hotkey{};
    size_t pos = 0;
    while (pos <= spec.size()) {
        size_t plus = spec.find(L'+', pos);
        std::wstring token =
            spec.substr(pos, plus == std::wstring::npos ? plus : plus - pos);
        pos = plus == std::wstring::npos ? spec.size() + 1 : plus + 1;

        token = NormalizeSettingString(token.c_str());
        if (token.empty()) {
            continue;  // a stray or trailing "+"
        }
        if (token == L"CTRL" || token == L"CONTROL") {
            hotkey.ctrl = true;
        } else if (token == L"ALT") {
            hotkey.alt = true;
        } else if (token == L"SHIFT") {
            hotkey.shift = true;
        } else if (token == L"WIN" || token == L"SUPER" || token == L"META") {
            hotkey.win = true;
        } else {
            hotkey.vk = ParseKeyName(token.c_str());
        }
    }
    return hotkey;
}

void LoadSettings() {
    WindhawkUtils::StringSetting hotkey(Wh_GetStringSetting(L"titleBar.hotkey"));
    g_settings.hotkey = ParseHotkey(hotkey, kDefaultHotkey);

    WindhawkUtils::StringSetting shortcut(
        Wh_GetStringSetting(L"gestures.shortcut"));
    g_settings.windowShortcut = ParseHotkey(shortcut, kDefaultWindowShortcut);
    WindhawkUtils::StringSetting shortcutAction(
        Wh_GetStringSetting(L"gestures.shortcutAction"));
    g_settings.windowShortcutAction =
        ParseWindowAction(shortcutAction, WindowAction::Close);

    WindhawkUtils::StringSetting modifier(Wh_GetStringSetting(L"drag.modifier"));
    g_settings.dragModifier = NormalizeSettingString(modifier) == L"ALT"
                                  ? DragModifier::Alt
                                  : DragModifier::Win;

    g_settings.topEdgeResize = Wh_GetIntSetting(L"titleBar.topEdgeResize") != 0;

    WindhawkUtils::StringSetting doubleClick(
        Wh_GetStringSetting(L"gestures.doubleClickAction"));
    g_settings.doubleClickAction =
        ParseWindowAction(doubleClick, WindowAction::ToggleMaximize);
    // Zero is a value of its own here - "whatever the mouse settings say" -
    // so it survives the clamp.
    g_settings.doubleClickTime =
        ClampedSetting(Wh_GetIntSetting(L"gestures.doubleClickTime"), 0, 100, 2000);

    WindhawkUtils::StringSetting translucency(
        Wh_GetStringSetting(L"drag.translucency"));
    g_settings.dragTranslucency = ParseDragTranslucency(translucency);
    g_settings.dragOpacity = ClampedSetting(Wh_GetIntSetting(L"drag.opacity"),
                                            kDefaultDragOpacity, 10, 100);
    g_settings.dragFadeIn = ClampedSetting(Wh_GetIntSetting(L"drag.fadeIn"),
                                           kDefaultDragFadeIn, 1, 2000);
    g_settings.dragFadeOut = ClampedSetting(Wh_GetIntSetting(L"drag.fadeOut"),
                                            kDefaultDragFadeOut, 1, 2000);

    WindhawkUtils::StringSetting menuBar(Wh_GetStringSetting(L"titleBar.menuBar"));
    g_settings.menuBarMode = ParseMenuBarMode(menuBar);
    // Decided once for the whole process: the mod runs inside the program,
    // so a program on the list simply never hides anything by default.
    g_settings.hideByDefault = Wh_GetIntSetting(L"titleBar.hideByDefault") != 0 &&
                               !ProgramInListSetting(L"titleBar.exclude");

    WindhawkUtils::StringSetting active(Wh_GetStringSetting(L"border.active"));
    WindhawkUtils::StringSetting inactive(
        Wh_GetStringSetting(L"border.inactive"));
    g_settings.borderActive = ParseBorderColor(active);
    g_settings.borderInactive = ParseBorderColor(inactive);

    g_settings.borderFramelessOnly =
        Wh_GetIntSetting(L"border.framelessOnly") != 0;
    // Zero is a value of its own here - no fade - so it is not the fallback
    // other settings make of it.
    g_settings.borderFadeDuration =
        std::clamp(Wh_GetIntSetting(L"border.fadeDuration"), 0, 2000);

    g_settings.desktopWinTab = Wh_GetIntSetting(L"desktops.winTab") != 0;

    WindhawkUtils::StringSetting corners(Wh_GetStringSetting(L"titleBar.corners"));
    g_settings.corners = ParseCorners(corners);

    WindhawkUtils::StringSetting snap(Wh_GetStringSetting(L"snap.mode"));
    g_settings.snap = ParseSnapMode(snap);
    g_settings.snapDistance = ClampedSetting(Wh_GetIntSetting(L"snap.distance"),
                                             kDefaultSnapDistance, 1, 64);
    // Zero is a gap of its own here, so it survives the clamp.
    g_settings.snapWindowGap =
        ClampedSetting(Wh_GetIntSetting(L"snap.windowGap"), 0, 0, 64);
    g_settings.snapMonitorGap =
        ClampedSetting(Wh_GetIntSetting(L"snap.monitorGap"), 0, 0, 64);
    WindhawkUtils::StringSetting keepAspect(
        Wh_GetStringSetting(L"drag.keepAspect"));
    g_settings.keepAspectVk = ParseModifierVk(keepAspect, VK_SHIFT);
    WindhawkUtils::StringSetting snapModifier(
        Wh_GetStringSetting(L"snap.modifier"));
    g_settings.snapModifierVk = ParseModifierVk(snapModifier, VK_CONTROL);
    WindhawkUtils::StringSetting snapModifierWhen(
        Wh_GetStringSetting(L"snap.modifierWhen"));
    g_settings.snapModifierHold = ParseSnapModifierHold(snapModifierWhen);

    Hotkey key = g_settings.hotkey;
    Hotkey shortcutKey = g_settings.windowShortcut;
    Wh_Log(L"Settings: hotkey vk=0x%02X ctrl=%d alt=%d shift=%d win=%d, "
           L"dragModifier=%s, topEdgeResize=%d, menuBar=%d, hideByDefault=%d, "
           L"dragTranslucency=%d, dragOpacity=%d",
           key.vk, (int)key.ctrl, (int)key.alt, (int)key.shift, (int)key.win,
           g_settings.dragModifier == DragModifier::Alt ? L"alt" : L"win",
           (int)g_settings.topEdgeResize, (int)g_settings.menuBarMode.load(),
           (int)g_settings.hideByDefault,
           (int)g_settings.dragTranslucency.load(),
           g_settings.dragOpacity.load());
    Wh_Log(L"Settings: doubleClickAction=%d, doubleClickTime=%d (%d ms), "
           L"windowShortcut vk=0x%02X ctrl=%d alt=%d shift=%d win=%d -> %d",
           (int)g_settings.doubleClickAction.load(),
           g_settings.doubleClickTime.load(), DoubleClickTimeMs(),
           shortcutKey.vk, (int)shortcutKey.ctrl, (int)shortcutKey.alt,
           (int)shortcutKey.shift, (int)shortcutKey.win,
           (int)g_settings.windowShortcutAction.load());
}
