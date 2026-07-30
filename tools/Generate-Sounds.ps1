# Synthesizes the placeholder combat SFX into assets/sounds/. Deterministic
# (fixed RNG seed), so re-running produces byte-identical files. Replace the
# wavs with real recordings whenever; the game only cares about the filenames.
param(
    [string]$OutDir = (Join-Path $PSScriptRoot "..\assets\sounds")
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $OutDir | Out-Null

$SampleRate = 22050

function Write-Wav([string]$Path, [float[]]$Samples) {
    $writer = [System.IO.BinaryWriter]::new([System.IO.File]::Create($Path))
    try {
        $dataBytes = $Samples.Count * 2
        $writer.Write([char[]]"RIFF")
        $writer.Write([uint32](36 + $dataBytes))
        $writer.Write([char[]]"WAVE")
        $writer.Write([char[]]"fmt ")
        $writer.Write([uint32]16)
        $writer.Write([uint16]1)            # PCM
        $writer.Write([uint16]1)            # mono
        $writer.Write([uint32]$SampleRate)
        $writer.Write([uint32]($SampleRate * 2)) # byte rate
        $writer.Write([uint16]2)            # block align
        $writer.Write([uint16]16)           # bits per sample
        $writer.Write([char[]]"data")
        $writer.Write([uint32]$dataBytes)
        foreach ($s in $Samples) {
            $clamped = [Math]::Max(-1.0, [Math]::Min(1.0, $s))
            $writer.Write([int16]([Math]::Round($clamped * 32767)))
        }
    }
    finally {
        $writer.Dispose()
    }
    Write-Host "wrote $Path ($($Samples.Count) samples)"
}

$rng = [Random]::new(1337)

# fire: a low-passed noise burst over a short 140 Hz thump. Reads as a generic
# rifle crack; per-class character comes from runtime pitch shifting.
$n = [int](0.16 * $SampleRate)
$fire = [float[]]::new($n)
$lp = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $SampleRate
    $lp = 0.55 * $lp + 0.45 * (2.0 * $rng.NextDouble() - 1.0)
    $noise = $lp * [Math]::Exp(-$t * 28.0)
    $thump = 0.6 * [Math]::Sin(2.0 * [Math]::PI * 140.0 * $t) * [Math]::Exp(-$t * 18.0)
    $fire[$i] = 0.9 * $noise + $thump
}
Write-Wav (Join-Path $OutDir "fire.wav") $fire

# hit: a quick falling sine "thock" for a projectile connecting with a body.
$n = [int](0.12 * $SampleRate)
$hit = [float[]]::new($n)
$phase = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $SampleRate
    $freq = 700.0 - 4200.0 * $t
    $phase += 2.0 * [Math]::PI * $freq / $SampleRate
    $hit[$i] = 0.85 * [Math]::Sin($phase) * [Math]::Exp(-$t * 30.0)
}
Write-Wav (Join-Path $OutDir "hit.wav") $hit

# thud: a soft low bump for a bullet stopping in dirt or a wall. Quiet by
# design (amplitude baked in) so a stream of misses doesn't drown the mix.
$n = [int](0.09 * $SampleRate)
$thud = [float[]]::new($n)
$lp = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $SampleRate
    $lp = 0.82 * $lp + 0.18 * (2.0 * $rng.NextDouble() - 1.0)
    $noise = $lp * [Math]::Exp(-$t * 60.0)
    $bump = 0.5 * [Math]::Sin(2.0 * [Math]::PI * 90.0 * $t) * [Math]::Exp(-$t * 45.0)
    $thud[$i] = 0.35 * ($noise + $bump)
}
Write-Wav (Join-Path $OutDir "thud.wav") $thud

# explode: grenade detonation. A heavy overdriven sine sweeping down into the
# lows under a slow-decaying rumble of low-passed noise.
$n = [int](0.6 * $SampleRate)
$explode = [float[]]::new($n)
$phase = 0.0
$lp = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $SampleRate
    $freq = 150.0 * [Math]::Exp(-$t * 6.0) + 35.0
    $phase += 2.0 * [Math]::PI * $freq / $SampleRate
    $boom = [Math]::Tanh(3.0 * [Math]::Sin($phase)) * [Math]::Exp(-$t * 7.0)
    $lp = 0.88 * $lp + 0.12 * (2.0 * $rng.NextDouble() - 1.0)
    $rumble = $lp * 2.2 * [Math]::Exp(-$t * 5.0)
    $explode[$i] = 0.95 * $boom + 0.5 * $rumble
}
Write-Wav (Join-Path $OutDir "explode.wav") $explode

# death: a longer descending tone, slightly overdriven so it stands out from
# the hit sound even in a firefight.
$n = [int](0.5 * $SampleRate)
$death = [float[]]::new($n)
$phase = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $SampleRate
    $freq = 320.0 * [Math]::Exp(-$t * 3.0)
    $phase += 2.0 * [Math]::PI * $freq / $SampleRate
    $tone = [Math]::Tanh(2.5 * [Math]::Sin($phase))
    $death[$i] = 0.8 * $tone * [Math]::Exp(-$t * 4.0)
}
Write-Wav (Join-Path $OutDir "death.wav") $death
