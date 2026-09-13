# Builds mod.wh.cpp exactly the way Windhawk does (64-bit and 32-bit) to catch
# compile/link errors before pasting it into the Windhawk editor, and builds
# the test harness (test\harness.exe).
#
#   .\build.ps1            build everything
#   .\build.ps1 -Mod       only the mod
#   .\build.ps1 -Harness   only the harness
param([switch]$Mod, [switch]$Harness)
$ErrorActionPreference = 'Continue'
if (-not $Mod -and -not $Harness) { $Mod = $true; $Harness = $true }

$root = $PSScriptRoot
$compiler = 'C:\Program Files\Windhawk\Compiler'
$clang = "$compiler\bin\clang++.exe"
$engine = 'C:\Program Files\Windhawk\Engine\1.7.3'
$modId = 'hypr-frameless'
$modVersion = (Select-String -Path "$root\mod.wh.cpp" -Pattern '^// @version\s+(\S+)').Matches[0].Groups[1].Value

$common = @('-std=c++23', '-DUNICODE', '-D_UNICODE', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00',
    '-D_WIN32_IE=0x0A00', '-DNTDDI_VERSION=0x0A000008', '-D__USE_MINGW_ANSI_STDIO=0', '-DWH_MOD',
    '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers',
    '-Wno-cast-function-type-mismatch')
$libs = @('-lcomctl32', '-ldwmapi')
$failed = $false

Push-Location $compiler
try {
    if ($Mod) {
        New-Item -ItemType Directory -Force "$root\out" | Out-Null
        foreach ($arch in @(@{bits = '64'; target = 'x86_64-w64-mingw32' }, @{bits = '32'; target = 'i686-w64-mingw32' })) {
            Write-Host "=== mod: $($arch.target) ==="
            & $clang @common '-O2' '-shared' "-DWH_MOD_ID=L`"$modId`"" "-DWH_MOD_VERSION=L`"$modVersion`"" `
                "$engine\$($arch.bits)\windhawk.lib" '-include' 'windhawk_api.h' '-target' $arch.target `
                '-Wl,--export-all-symbols' '-x' 'c++' "$root\mod.wh.cpp" '-o' "$root\out\mod$($arch.bits).dll" @libs
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
