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
Hotkey ParseHotkey(PCWSTR raw) {
    std::wstring spec = NormalizeSettingString(raw);
    if (spec.empty()) {
        spec = NormalizeSettingString(kDefaultHotkey);
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
    WindhawkUtils::StringSetting hotkey(Wh_GetStringSetting(L"hotkey"));
    Hotkey parsed = ParseHotkey(hotkey);
    g_settings.hotkeyVk = parsed.vk;
    g_settings.hotkeyCtrl = parsed.ctrl;
    g_settings.hotkeyAlt = parsed.alt;
    g_settings.hotkeyShift = parsed.shift;
    g_settings.hotkeyWin = parsed.win;

    WindhawkUtils::StringSetting modifier(Wh_GetStringSetting(L"dragModifier"));
    g_settings.dragModifier = NormalizeSettingString(modifier) == L"ALT"
                                  ? DragModifier::Alt
                                  : DragModifier::Win;

    g_settings.topEdgeResize = Wh_GetIntSetting(L"topEdgeResize") != 0;
    WindhawkUtils::StringSetting menuBar(Wh_GetStringSetting(L"menuBarWindows"));
    g_settings.menuBarMode = ParseMenuBarMode(menuBar);
    g_settings.hideByDefault = Wh_GetIntSetting(L"hideByDefault") != 0;

    WindhawkUtils::StringSetting active(Wh_GetStringSetting(L"borderActive"));
    WindhawkUtils::StringSetting inactive(
        Wh_GetStringSetting(L"borderInactive"));
    g_settings.borderActive = ParseBorderColor(active);
    g_settings.borderInactive = ParseBorderColor(inactive);

    WindhawkUtils::StringSetting corners(Wh_GetStringSetting(L"corners"));
    g_settings.corners = ParseCorners(corners);

    Wh_Log(L"Settings: hotkey vk=0x%02X ctrl=%d alt=%d shift=%d win=%d, "
           L"dragModifier=%s, topEdgeResize=%d, menuBar=%d, hideByDefault=%d",
           g_settings.hotkeyVk.load(), (int)g_settings.hotkeyCtrl,
           (int)g_settings.hotkeyAlt, (int)g_settings.hotkeyShift,
           (int)g_settings.hotkeyWin,
           g_settings.dragModifier == DragModifier::Alt ? L"alt" : L"win",
           (int)g_settings.topEdgeResize, (int)g_settings.menuBarMode.load(),
           (int)g_settings.hideByDefault);
}
