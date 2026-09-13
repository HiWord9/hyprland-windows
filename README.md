# Hyprland Windows

Window handling the way [Hyprland](https://hypr.land/) does it, on Windows —
a mod for [Windhawk](https://windhawk.net/).

* **Win + left mouse button** — drag a window from anywhere on it, not just by
  its title bar.
* **Win + right mouse button** — resize a window from anywhere on it, from
  whichever corner is closest to the cursor.
* **Ctrl+Alt+H** — hide the focused window's title bar; press it again to bring
  the title bar back.

Moving and resizing feel like the real thing: windows still snap to the screen
edges, dragging one to the top still offers the Windows 11 snap layouts, and
`Esc` cancels a drag in progress.

The hotkey, the modifier key and how much of the frame goes away with the title
bar are all configurable in the mod's settings in Windhawk.

## Installing

1. Download `hyprland-windows.wh.cpp` from the
   [latest release](../../releases/latest).
2. Quit Windhawk (right-click its tray icon → Exit).
3. Copy the file into `C:\ProgramData\Windhawk\ModsSource\` — the folder needs
   administrator rights.
4. Start Windhawk. The mod appears with a warning that it has to be compiled —
   press **Compile** and it is ready.

Alternatively, skip the file copying: in Windhawk choose *Create new mod*,
paste the contents of the file into the editor and compile it there.

## Good to know

* Apps that draw their own title bar instead of using the system one (Chrome,
  Electron apps, VS Code, Office) are not affected by the hotkey.
* A window with a classic menu bar (File, Edit, …) can't keep that menu where it
  is once the title bar is gone, so by default it is hidden along with it and
  stays reachable from the keyboard with `Alt` or `F10`. The settings offer the
  other choices.

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
| `src/decorations.cpp` | border color and corner preference |
| `src/titlebar.cpp` | hiding/restoring a title bar and the per-window bookkeeping |
| `src/hotkey.cpp` | the title-bar hotkey |
| `src/startmenu.cpp` | keeping the Start menu shut after a `Win` + drag |
| `src/drag.cpp` | `Win` + mouse move and resize |
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
```

The other `test\*_probe.cpp` files are standalone experiments that document
platform behaviour the mod relies on.
