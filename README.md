# Hyprland Windows

Window handling the way [Hyprland](https://hypr.land/) does it, on Windows.

![Hyprland Windows in action](https://raw.githubusercontent.com/HiWord9/hyprland-windows/extra/demo/1.1.0.gif)

* **Win + left mouse button** — drag a window from anywhere on it, not just by
  its title bar.
* **Win + right mouse button** — resize a window from anywhere on it, from
  whichever corner is closest to the cursor.
* **Win + double click** — maximize a window, or restore it. The title bar is
  gone, so the double click that used to be on it now works anywhere on the
  window; the settings offer other things for it to do.
* **Win + middle click** — close the window under the cursor. The button, the
  modifiers and the action are all settings; it can be a key instead, such as
  Win+Q — even one Windows already uses, like Win+W.
* **Ctrl+Alt+H** — hide the focused window's title bar; press it again to bring
  the title bar back.
* **Win+Tab** — go to the next virtual desktop that has windows on it, at once;
  Win+Shift+Tab goes back. Task View moves to Win+Ctrl+Tab.

Moving and resizing feel like the real thing: windows still snap to the screen
edges, dragging one to the top still offers the Windows 11 snap layouts, and
`Esc` cancels a drag in progress. A window you are dragging fades to slightly
translucent while you hold it, the way a Hyprland window does. Hold `Ctrl` as
well and its edges turn magnetic: bring one close to another window or to the
edge of the screen and it lines up with it. Hold `Shift` while resizing to
keep the window's proportions.

Title bars can also go away on their own: with hiding by default turned on,
every new window opens without one, except the programs on a list of
exceptions (Paint and the Snipping Tool to begin with).

Give windows a border color — your own, or `accent` to follow the Windows
accent color — and it fades from one color to the other as focus moves, the
way a Hyprland border does. That goes for every window with a frame, or only
for the ones whose title bar is hidden, whichever the settings say.

Everything is configurable in the settings — the hotkey, the modifier key, the
translucency, the magnet, the border, and how much of the frame goes away with
the title bar.

## Installing

1. Download `hyprland-windows.wh.cpp` from the
   [latest release](../../releases/latest).
2. In Windhawk, choose **Create new mod**.
3. Open the downloaded file, copy everything in it, and paste it into the
   editor, replacing what is already there.
4. Press **Compile** (<kbd>Ctrl</kbd>+<kbd>B</kbd>) and the mod is ready.

## Good to know

* Apps that draw their own title bar instead of using the system one (Chrome,
  Electron apps, VS Code, Office) are not affected by the hotkey.
* A window with a classic menu bar (File, Edit, …) can't keep that menu where it
  is once the title bar is gone, so by default it is hidden along with it and
  stays reachable from the keyboard with `Alt` or `F10`. The settings offer the
  other choices.
* `Win` + mouse doesn't work over content drawn with WinUI or XAML — Paint,
  Windows Terminal, the tabs and address bar of File Explorer: Windows hands
  those clicks to the app past the point where the mod sees them.
* Win+Tab switches desktops through a part of Windows that Microsoft doesn't
  publish. It is known for Windows 10 and for Windows 11 from 22H2 on; on a
  build the mod doesn't recognize, and with only the one desktop, Win+Tab stays
  Task View.

## Working on the source

Windhawk compiles one file per mod, so the mod is written as a set of modules
and assembled into that single file:

| path | what it is |
| --- | --- |
| `src/metadata.wh` | the Windhawk header: `@id`, `@name`, the mod's own readme and its settings |
| `src/common.h` | shared headers, the settings, and everything that crosses a file boundary |
| `src/settings.cpp` | reading and parsing the settings |
| `src/window_info.cpp` | which windows the mod may touch, plus window metrics |
| `src/frame_geometry.cpp` | non-client layout of a window with a hidden title bar |
| `src/decorations.cpp` | border color (and the fade between the two of them) and corner preference |
| `src/titlebar.cpp` | hiding/restoring a title bar and the per-window bookkeeping |
| `src/hotkey.cpp` | the key bindings: the title-bar hotkey, and the window shortcut when it is a key |
| `src/keyboard.cpp` | the mod's keyboard thread: keeping the Start menu shut after a `Win` + gesture, and the key bindings ahead of Windows' own shortcuts |
| `src/desktops.cpp` | `Win`+`Tab` through the virtual desktops |
| `src/drag.cpp` | `Win` + mouse move and resize |
| `src/drag_fade.cpp` | fading a window to translucent while it is dragged |
| `src/actions.cpp` | mouse gestures with the modifier held, and what they do |
| `src/snap.cpp` | magnetic edges while dragging, and the aspect ratio while resizing |
| `src/hooks.cpp` | the hooked APIs (messages, window creation) |
| `src/mod.cpp` | the Windhawk lifecycle entry points |
| `tools/bundle.py` | assembles all of the above into `build/hyprland-windows.wh.cpp` |

Every module includes only `src/common.h`, so each one is a valid translation
unit on its own — `.\build.ps1 -Check` compiles each file separately to prove
it. `build/` is generated and not committed.

Assembling the single file needs nothing but Python 3:

```powershell
python tools\bundle.py          # -> build\hyprland-windows.wh.cpp
```

`build.ps1` wraps that and builds the tests around it, which additionally needs
Windhawk installed for its bundled clang:

```powershell
.\build.ps1            # bundle + harness
.\build.ps1 -Bundle    # only build\hyprland-windows.wh.cpp
.\build.ps1 -Harness   # bundle + test\harness.exe
.\build.ps1 -Check     # syntax-check every src\*.cpp on its own
.\build.ps1 -Shell     # bundle + test\desktops_in_shell.exe/.dll
```

The mod itself is never compiled here: Windhawk compiles the `.wh.cpp` on the
user's machine, so a DLL is not something this project produces.

`test\harness.exe` includes the *bundled* mod (with Windhawk's editing stubs),
so the bundler is covered too, and drives it against real windows on the
desktop: non-client geometry, hit testing, maximize/restore, the unload path,
screenshots into `test\out\`, and the move/resize loops with injected mouse
input.

```powershell
.\test\harness.exe                 # everything (moves the mouse for a few seconds)
.\test\harness.exe --no-input      # skip the mouse-input tests
.\test\harness.exe --dpi-unaware   # run as a DPI-unaware process
.\test\harness.exe --desktops      # only the Win+Tab tests
```

What Win+Tab does is decided by the shell as much as by the mod, and a
harness is not the shell. `test\desktops_in_shell.exe` loads the bundled mod
into Explorer itself and presses the keys, counting how the desktops change —
or, with `--mod`, presses them for the copy Windhawk has compiled. It switches
the desktops of whoever runs it; run it elevated for the keys to reach elevated
windows as well.

```powershell
.\test\desktops_in_shell.exe burst 12 110   # Tab again and again, Win held
.\test\desktops_in_shell.exe taps 10 400    # Win+Tab again and again
.\test\desktops_in_shell.exe type 6         # keys typed right after a switch
```

The other `test\*_probe.cpp` files are standalone experiments that document
platform behaviour the mod relies on.
