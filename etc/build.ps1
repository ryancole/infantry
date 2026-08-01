# Configures (if needed) and builds the game.
# Usage: .\etc\build.ps1 [-Config Debug|Release]
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $root 'build'

# DirectXTK12 compiles its shaders with fxc.exe from the Windows SDK, which is
# only on PATH in a VS developer prompt; find it ourselves everywhere else.
if (-not (Get-Command fxc.exe -ErrorAction SilentlyContinue)) {
    $fxc = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\10.*\x64\fxc.exe' -ErrorAction SilentlyContinue |
        Sort-Object FullName | Select-Object -Last 1
    if ($fxc) { $env:Path = "$($fxc.DirectoryName);$env:Path" }
}

# -A only applies to a fresh configure. Re-passing it at an existing build
# directory is what CMake compares against the cache, and any cache configured
# without it (a bare `cmake -S . -B build`, or an IDE that configured for us)
# stores an empty platform and fails the comparison — even though VS 2019 and
# up already default to x64. So it's set once, when there's nothing to conflict
# with, and left out of every reconfigure after that.
$fresh = -not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))
if ($fresh) { cmake -S $root -B $buildDir -A x64 } else { cmake -S $root -B $buildDir }

# A cache can still be unusable — a moved source tree, a Visual Studio that has
# been upgraded out from under it — and that's not something to make the user
# diagnose: the build directory is entirely derived, so it's thrown away and
# configured from scratch. Only on a reconfigure; a fresh configure that fails
# has a real problem in it, and retrying would just fail the same way.
if ($LASTEXITCODE -ne 0 -and -not $fresh) {
    Write-Host "`nConfigure failed against the existing cache; reconfiguring from scratch." -ForegroundColor Yellow
    Remove-Item -Recurse -Force (Join-Path $buildDir 'CMakeCache.txt'), (Join-Path $buildDir 'CMakeFiles') -ErrorAction SilentlyContinue
    cmake -S $root -B $buildDir -A x64
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# DirectXTK12's CMake launches CompileShaders.cmd via `cmake -E env`, which
# modern CMake refuses to spawn (.cmd scripts need cmd.exe). Pre-compile the
# shaders here; the custom build step then sees up-to-date outputs and skips.
$tkShaderSrc = Join-Path $buildDir '_deps\directxtk12-src\Src\Shaders'
$tkShaderOut = Join-Path $buildDir '_deps\directxtk12-build\Shaders\Compiled'
$tkSentinel = Join-Path $tkShaderOut 'SpriteEffect_SpriteVertexShader.inc'
if ((Test-Path $tkShaderSrc) -and -not (Test-Path $tkSentinel)) {
    Write-Host 'Pre-compiling DirectXTK12 shaders...'
    New-Item -ItemType Directory -Force $tkShaderOut | Out-Null
    $env:CompileShadersOutput = $tkShaderOut
    cmd /c "cd /d `"$tkShaderSrc`" && `"$tkShaderSrc\CompileShaders.cmd`"" > (Join-Path $tkShaderOut 'compileshaders.log') 2>&1
    if (-not (Test-Path $tkSentinel)) {
        Write-Host "DirectXTK12 shader pre-compile failed; see $tkShaderOut\compileshaders.log" -ForegroundColor Red
        exit 1
    }
}

cmake --build $buildDir --config $Config
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "`nBuilt: $buildDir\$Config\infantry.exe" -ForegroundColor Green
