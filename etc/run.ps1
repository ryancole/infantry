# Builds (incremental) and launches the game.
# Usage: .\etc\run.ps1 [-Config Debug|Release]
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug'
)

$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'build.ps1') -Config $Config
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$root = Split-Path $PSScriptRoot -Parent
& (Join-Path $root "build\$Config\infantry.exe")
