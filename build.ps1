# Bundles src/ into build\<mod id>.wh.cpp, then compiles it exactly the way
# Windhawk does (64-bit and 32-bit) to catch compile/link errors before pasting
# it into the Windhawk editor, and builds the test harness (test\harness.exe).
#
#   .\build.ps1             bundle + mod + harness
#   .\build.ps1 -Bundle     only bundle
#   .\build.ps1 -Mod        bundle + the mod DLLs
#   .\build.ps1 -Harness    bundle + the harness
#   .\build.ps1 -Check      syntax-check every src\*.cpp on its own, which is
#                           what keeps each module usable as a single file in
#                           an IDE (and catches a declaration missing from
#                           common.h that the bundle would happily hide)
param([switch]$Bundle, [switch]$Mod, [switch]$Harness, [switch]$Check)
$ErrorActionPreference = 'Continue'
if (-not $Bundle -and -not $Mod -and -not $Harness -and -not $Check) { $Mod = $true; $Harness = $true }

$root = $PSScriptRoot
$compiler = 'C:\Program Files\Windhawk\Compiler'
$clang = "$compiler\bin\clang++.exe"
$engine = 'C:\Program Files\Windhawk\Engine\1.7.3'

$meta = Get-Content "$root\src\metadata.wh" -Raw
$modId = [regex]::Match($meta, '(?m)^//\s*@id\s+(\S+)\s*$').Groups[1].Value
$modVersion = [regex]::Match($meta, '(?m)^//\s*@version\s+(\S+)\s*$').Groups[1].Value
if (-not $modId -or -not $modVersion) { Write-Host 'could not read @id/@version from src\metadata.wh'; exit 1 }
$bundled = "$root\build\$modId.wh.cpp"

# Always bundle first: everything below compiles the generated file, so the
# tests exercise exactly what ships.
Write-Host "=== bundle ==="
& python "$root\tools\bundle.py"
if ($LASTEXITCODE -ne 0) { exit 1 }

$common = @('-std=c++23', '-DUNICODE', '-D_UNICODE', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00',
    '-D_WIN32_IE=0x0A00', '-DNTDDI_VERSION=0x0A000008', '-D__USE_MINGW_ANSI_STDIO=0', '-DWH_MOD',
    '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers',
    '-Wno-cast-function-type-mismatch')
$libs = @('-lcomctl32', '-ldwmapi')
$failed = $false

Push-Location $compiler
try {
    if ($Check) {
        foreach ($f in Get-ChildItem "$root\src\*.cpp") {
            & $clang @common '-DWH_EDITING' '-fsyntax-only' '-include' 'windhawk_api.h' `
                '-target' 'x86_64-w64-mingw32' '-x' 'c++' $f.FullName
            if ($LASTEXITCODE -ne 0) { $failed = $true; Write-Host "FAIL $($f.Name)" }
            else { Write-Host "ok   $($f.Name)" }
        }
    }
    if ($Mod) {
        New-Item -ItemType Directory -Force "$root\out" | Out-Null
        foreach ($arch in @(@{bits = '64'; target = 'x86_64-w64-mingw32' }, @{bits = '32'; target = 'i686-w64-mingw32' })) {
            Write-Host "=== mod: $($arch.target) ==="
            & $clang @common '-O2' '-shared' "-DWH_MOD_ID=L`"$modId`"" "-DWH_MOD_VERSION=L`"$modVersion`"" `
                "$engine\$($arch.bits)\windhawk.lib" '-include' 'windhawk_api.h' '-target' $arch.target `
                '-Wl,--export-all-symbols' '-x' 'c++' $bundled '-o' "$root\out\mod$($arch.bits).dll" @libs
            if ($LASTEXITCODE -ne 0) { $failed = $true }
            Write-Host "exit: $LASTEXITCODE"
        }
    }
    if ($Harness) {
        Write-Host "=== harness ==="
        & $clang @common '-O1' '-g' '-DWH_EDITING' '-Wno-unused-function' '-include' 'windhawk_api.h' `
            '-target' 'x86_64-w64-mingw32' '-static' '-x' 'c++' "$root\test\harness.cpp" `
            '-o' "$root\test\harness.exe" @libs '-lgdi32' '-luser32'
        if ($LASTEXITCODE -ne 0) { $failed = $true }
        Write-Host "exit: $LASTEXITCODE"
    }
}
finally { Pop-Location }

if ($failed) { exit 1 } else { exit 0 }
