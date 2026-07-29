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

cmake -S $root -B $buildDir -A x64
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
