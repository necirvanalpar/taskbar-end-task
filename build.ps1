# Builds build\EndTask.exe with Zig's bundled C compiler (no Visual Studio needed).
#
#   winget install zig.zig
#   powershell -ExecutionPolicy Bypass -File build.ps1
#
# -Target picks the CPU: x86_64 (default), x86 or aarch64.
[CmdletBinding()]
param(
    [ValidateSet('x86_64', 'x86', 'aarch64')]
    [string]$Target = 'x86_64',
    [string]$Zig = 'zig'
)
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$src = Join-Path $root 'src'
$out = Join-Path $root 'build'

# Zig installed by winget is not on PATH until the shell is restarted; look in winget's folders too.
if (-not (Get-Command $Zig -ErrorAction SilentlyContinue)) {
    $winget = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet'
    $found = @(Join-Path $winget 'Links\zig.exe') +
        @(Get-ChildItem (Join-Path $winget 'Packages\zig.zig_*\*\zig.exe') -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | ForEach-Object FullName) |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($found) { $Zig = $found }
    else { throw 'zig was not found. Install it with "winget install zig.zig" or pass -Zig <path to zig.exe>.' }
}

New-Item -ItemType Directory -Force $out | Out-Null
$res = Join-Path $out 'EndTask.res'
$exe = Join-Path $out 'EndTask.exe'

# 1) icon + manifest + version info
& $Zig rc /fo $res (Join-Path $src 'EndTask.rc')
if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }

# 2) compile and link
$libs = 'oleacc', 'oleaut32', 'ole32', 'dwmapi', 'version', 'shlwapi', 'shell32', 'advapi32', 'gdi32', 'user32' |
    ForEach-Object { "-l$_" }
& $Zig cc -target "$Target-windows-gnu" -std=c99 -O2 -s -Wall -Wextra '-Wl,--subsystem,windows' `
    (Join-Path $src 'EndTask.c') $res -o $exe @libs
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }

$file = Get-Item $exe
'{0}  ({1:N0} bytes)' -f $file.FullName, $file.Length
'SHA-256: ' + (Get-FileHash $exe -Algorithm SHA256).Hash
