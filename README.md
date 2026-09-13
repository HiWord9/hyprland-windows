# hyprland-windows

**Hyprland Windows** — a [Windhawk](https://windhawk.net/) mod that brings the
Hyprland window feel to Windows 11: `Win + LMB` moves any window from anywhere
on it, `Win + RMB` resizes it from the nearest corner, and a hotkey hides a
window's title bar completely.

The mod itself is the single file `mod.wh.cpp` (readme and settings are inside
it, as Windhawk expects). Paste it into the Windhawk editor and compile.

## Building outside Windhawk

`build.ps1` compiles the mod with Windhawk's own toolchain, exactly like the
editor does (64-bit and 32-bit), and builds the test harness:

```powershell
.\build.ps1            # mod + harness
.\build.ps1 -Mod       # only the mod (out\mod64.dll, out\mod32.dll)
.\build.ps1 -Harness   # only test\harness.exe
```

## Tests

`test\harness.exe` includes `mod.wh.cpp` directly (with Windhawk's editing
stubs) and drives it against real windows on the desktop: it checks the
non-client geometry, hit testing, maximize/restore, the unload path, takes
screenshots into `test\out\`, and runs the move/resize loops with injected
mouse input.

```powershell
.\test\harness.exe                 # everything (moves the mouse for a few seconds)
.\test\harness.exe --no-input      # skip the mouse-input tests
.\test\harness.exe --dpi-unaware   # run as a DPI-unaware process
```

`test\sizemove_probe.cpp` is a standalone experiment documenting how the
system's `SC_MOVE` / `SC_SIZE` modal loop behaves when started from code.
