# hyprland-windows

**Hyprland Windows** — a [Windhawk](https://windhawk.net/) mod that brings the
Hyprland window feel to Windows 11: `Win + LMB` moves any window from anywhere
on it, `Win + RMB` resizes it from the nearest corner, and a hotkey hides a
window's title bar completely.

## Layout

Windhawk compiles one file per mod, but the mod is written as a set of modules
and assembled into that one file:

| path | what it is |
| --- | --- |
| `src/metadata.wh` | the Windhawk header: `@id`, `@name`, the mod's own readme and its settings schema |
| `src/common.h` | shared headers, the settings struct, and a declaration for everything that crosses a file boundary |
| `src/settings.cpp` | reading and parsing the user settings |
| `src/window_info.cpp` | which windows the mod may touch, plus window metrics |
| `src/frame_geometry.cpp` | non-client layout of a window with a hidden title bar |
| `src/decorations.cpp` | DWM border color and corner preference |
| `src/titlebar.cpp` | hiding/restoring a title bar and the per-window bookkeeping |
| `src/hotkey.cpp` | the title-bar toggle hotkey |
| `src/startmenu.cpp` | keeping the Start menu shut after a `Win` + drag |
| `src/drag.cpp` | `Win` + mouse move and resize |
| `src/hooks.cpp` | the hooked APIs (messages, window creation) |
| `src/mod.cpp` | the Windhawk lifecycle entry points |
| `tools/bundle.py` | assembles the above into `build/hyprland-windows.wh.cpp` |

Every module includes only `src/common.h`, so each one is a valid translation
unit on its own: an IDE resolves references across the project, and
`.\build.ps1 -Check` compiles each file separately to prove it.

`build/` is generated and not committed — `hyprland-windows.wh.cpp` is a build
artifact, not a source file.

## Building

Assembling the single file needs nothing but Python 3:

```powershell
python tools\bundle.py          # -> build\hyprland-windows.wh.cpp
```

`build.ps1` wraps that and builds the tests around it, which additionally
needs [Windhawk](https://windhawk.net/) installed for its bundled clang:

```powershell
.\build.ps1            # bundle + harness
.\build.ps1 -Bundle    # only build\hyprland-windows.wh.cpp
.\build.ps1 -Harness   # bundle + test\harness.exe
.\build.ps1 -Check     # syntax-check every src\*.cpp on its own
```

The mod itself is never compiled here: Windhawk compiles the `.wh.cpp` on the
user's machine, so a DLL is not something this project produces.

`compile_flags.txt` gives editors/clangd the include paths for the Windhawk
toolchain; if Windhawk is not in `C:\Program Files\Windhawk`, adjust the two
`-isystem` lines.

## Tests

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
platform behaviour the mod relies on: `sizemove_probe` (how the system's
`SC_MOVE`/`SC_SIZE` loop behaves when started from code),
`nativesize_inproc_probe` (driving the native resize loop from the right mouse
button), `startmenu_probe` and `winmask_probe` (what actually stops the Start
menu from opening), `chrome_resize_probe` (repaint during a resize, against a
real window).
