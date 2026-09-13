# Bundles src/ into build\<mod id>.wh.cpp - the single file Windhawk compiles -
# and builds the test harness around it.
#
# The mod itself is never compiled here: Windhawk builds it from the .wh.cpp on
# the user's machine, so a DLL is not something this project ships.
#
#   .\build.ps1             bundle + harness
#   .\build.ps1 -Bundle     only bundle (needs nothing but Python)
#   .\build.ps1 -Harness    bundle + test\harness.exe
#   .\build.ps1 -Check      syntax-check every src\*.cpp on its own, which is
#                           what keeps each module usable as a single file in
#                           an IDE (and catches a declaration missing from
#                           common.h that the bundle would happily hide)
param([switch]$Bundle, [switch]$Harness, [switch]$Check)
$ErrorActionPreference = 'Continue'
if (-not $Bundle -and -not $Harness -and -not $Check) { $Harness = $true }

$root = $PSScriptRoot
$compiler = 'C:\Program Files\Windhawk\Compiler'
$clang = "$compiler\bin\clang++.exe"

# Always bundle first: everything below compiles the generated file, so the
# tests exercise exactly what ships.
Write-Host "=== bundle ==="
& python "$root\tools\bundle.py"
if ($LASTEXITCODE -ne 0) { exit 1 }

$common = @('-std=c++23', '-DUNICODE', '-D_UNICODE', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00',
    '-D_WIN32_IE=0x0A00', '-DNTDDI_VERSION=0x0A000008', '-D__USE_MINGW_ANSI_STDIO=0', '-DWH_MOD',
    '-DWH_EDITING', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers',
    '-Wno-cast-function-type-mismatch')
$failed = $false

Push-Location $compiler
try {
    if ($Check) {
        foreach ($f in Get-ChildItem "$root\src\*.cpp") {
            & $clang @common '-fsyntax-only' '-include' 'windhawk_api.h' `
                '-target' 'x86_64-w64-mingw32' '-x' 'c++' $f.FullName
            if ($LASTEXITCODE -ne 0) { $failed = $true; Write-Host "FAIL $($f.Name)" }
            else { Write-Host "ok   $($f.Name)" }
        }
    }
    if ($Harness) {
        Write-Host "=== harness ==="
        & $clang @common '-O1' '-g' '-Wno-unused-function' '-include' 'windhawk_api.h' `
            '-target' 'x86_64-w64-mingw32' '-static' '-x' 'c++' "$root\test\harness.cpp" `
            '-o' "$root\test\harness.exe" '-lcomctl32' '-ldwmapi' '-lgdi32' '-luser32'
        if ($LASTEXITCODE -ne 0) { $failed = $true }
        Write-Host "exit: $LASTEXITCODE"
    }
}
finally { Pop-Location }

if ($failed) { exit 1 } else { exit 0 }
