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

# reload: a dry mechanical clack for a magazine leaving or entering the well.
# One clip covers both ends of the reload — the game plays it flat when the mag
# drops and pitched up when the fresh one seats, so the pair brackets the wait.
# Short and quiet: it fires twice per magazine, so it has to sit under the
# gunfire rather than announce itself.
$n = [int](0.09 * $SampleRate)
$reload = [float[]]::new($n)
$phase = 0.0
$hp = 0.0
$prev = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $SampleRate
    # High-passed noise: the rattle of the catch, with none of the body a
    # low-passed burst would give it (that's the thud).
    $white = 2.0 * $rng.NextDouble() - 1.0
    $hp = 0.7 * ($hp + $white - $prev)
    $prev = $white
    $click = $hp * [Math]::Exp(-$t * 90.0)
    # A short metallic ring under it, so it reads as a part seating in metal.
    $freq = 900.0 * [Math]::Exp(-$t * 8.0) + 240.0
    $phase += 2.0 * [Math]::PI * $freq / $SampleRate
    $ring = 0.5 * [Math]::Sin($phase) * [Math]::Exp(-$t * 55.0)
    $reload[$i] = 0.5 * ($click + $ring)
}
Write-Wav (Join-Path $OutDir "reload.wav") $reload

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

# swing: the pass of air off a melee swing. Band-passed noise under an envelope
# that swells through the middle of the arc and dies with it, so it reads as
# something travelling rather than something starting. There's no impact in it
# on purpose: connecting plays the hit clip over the top, and a miss is meant to
# sound like a miss.
#
# New clips go on the end of this file rather than beside a related one: every
# generator draws from the same seeded RNG in the order it's written, so
# inserting one in the middle silently rewrites every clip after it.
$n = [int](0.18 * $SampleRate)
$swing = [float[]]::new($n)
$lp = 0.0
$hp = 0.0
$prev = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $SampleRate
    $white = 2.0 * $rng.NextDouble() - 1.0
    $lp = 0.72 * $lp + 0.28 * $white   # off the top: hiss, not a cymbal
    $hp = 0.85 * ($hp + $lp - $prev)   # off the bottom: air, not a thud
    $prev = $lp
    $env = [Math]::Sin([Math]::PI * $t / 0.18)
    $swing[$i] = 1.1 * $hp * $env * $env
}
Write-Wav (Join-Path $OutDir "swing.wav") $swing

# heal: the medic's field dressing going on. An injector hiss off the top of a
# soft two-note rise, so it reads as something being given rather than something
# fired — nothing in it clicks, cracks or seats, which is what keeps it clear of
# the reload it will often be heard next to. It plays once, at the start of the
# dressing: what says the dressing is still running is the ring on the floor and
# the health bar climbing, and a sustained tone under a firefight would be one
# more thing to hear over rather than one more thing heard.
$n = [int](0.34 * $SampleRate)
$heal = [float[]]::new($n)
$phase = 0.0
$lp = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $SampleRate
    # Two notes a fifth apart, the second arriving a third of the way in, both
    # swelling rather than striking.
    $tone = [Math]::Sin(2.0 * [Math]::PI * 520.0 * $t)
    if ($t -gt 0.11) { $tone += [Math]::Sin(2.0 * [Math]::PI * 780.0 * ($t - 0.11)) }
    $swell = [Math]::Min(1.0, $t / 0.05) * [Math]::Exp(-$t * 5.5)
    # The hiss of the injector itself: low-passed noise, gone before the notes
    # are, so it sits at the front of the sound like a plunger going down.
    $lp = 0.6 * $lp + 0.4 * (2.0 * $rng.NextDouble() - 1.0)
    $hiss = $lp * [Math]::Exp(-$t * 16.0)
    $heal[$i] = 0.32 * $tone * $swell + 0.22 * $hiss
}
Write-Wav (Join-Path $OutDir "heal.wav") $heal

# step: one boot going down on dirt. Grit off the top of a short low thump, all
# of it over in well under a tenth of a second — a footfall is an event, not a
# noise. The amplitude is baked in low (the way the thud's is) and for a harder
# reason: every soldier on the field plays this twice a stride, so it has to be
# the floor of the mix rather than anything that competes for the middle of it.
# What varies per step is left to the game — a little detune and the volume of
# the walk it belongs to — because two identical footfalls in a row are what
# makes a stride sound like a machine.
$n = [int](0.075 * $SampleRate)
$step = [float[]]::new($n)
$lp = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $SampleRate
    $lp = 0.62 * $lp + 0.38 * (2.0 * $rng.NextDouble() - 1.0)
    $grit = $lp * [Math]::Exp(-$t * 75.0)
    $weight = 0.7 * [Math]::Sin(2.0 * [Math]::PI * 120.0 * $t) * [Math]::Exp(-$t * 55.0)
    $step[$i] = 0.22 * ($grit + $weight)
}
Write-Wav (Join-Path $OutDir "step.wav") $step

# bodyfall: a soldier arriving on the ground. Deeper and slacker than the thud
# a round makes stopping in dirt, with the rustle of what they were wearing over
# the top of it and gone first. It plays when the ragdoll actually lands rather
# than when the soldier died, which is the whole point of having it: the death
# is the tone, and this is the second, later sound of the body catching up.
$n = [int](0.28 * $SampleRate)
$bodyfall = [float[]]::new($n)
$phase = 0.0
$lp = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $SampleRate
    $freq = 95.0 * [Math]::Exp(-$t * 12.0) + 48.0
    $phase += 2.0 * [Math]::PI * $freq / $SampleRate
    $thump = [Math]::Sin($phase) * [Math]::Exp(-$t * 14.0)
    # Brighter noise than the thump wants, so it reads as cloth on the way down
    # rather than as more of the same low end.
    $lp = 0.5 * $lp + 0.5 * (2.0 * $rng.NextDouble() - 1.0)
    $cloth = $lp * [Math]::Exp(-$t * 22.0)
    $bodyfall[$i] = 0.55 * $thump + 0.18 * $cloth
}
Write-Wav (Join-Path $OutDir "bodyfall.wav") $bodyfall

# spawn: a fresh soldier standing on the field. A tone that climbs a fifth and
# settles, over the whump of boots taking their own weight — rising because it
# is the one sound in here that reports something good, and short because it is
# heard at the moment the player gets their hands back and nothing should be
# playing over the top of the first thing they do with them.
$n = [int](0.45 * $SampleRate)
$spawn = [float[]]::new($n)
$phase = 0.0
$lp = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $SampleRate
    $freq = 300.0 + 200.0 * [Math]::Min(1.0, $t / 0.15)
    $phase += 2.0 * [Math]::PI * $freq / $SampleRate
    $tone = [Math]::Sin($phase) * [Math]::Min(1.0, $t / 0.04) * [Math]::Exp(-$t * 4.5)
    $whump = [Math]::Sin(2.0 * [Math]::PI * 70.0 * $t) * [Math]::Exp(-$t * 16.0)
    $lp = 0.55 * $lp + 0.45 * (2.0 * $rng.NextDouble() - 1.0)
    $air = $lp * [Math]::Exp(-$t * 9.0)
    $spawn[$i] = 0.34 * $tone + 0.30 * $whump + 0.12 * $air
}
Write-Wav (Join-Path $OutDir "spawn.wav") $spawn

# throw: a grenade leaving the hand. The same trick as the blade's swing —
# band-passed noise under a travelling envelope — pitched down into cloth and
# given a hard front edge, because a throw starts with the arm already moving
# and ends with the thing gone. Nothing in it cracks or reports: a grenade going
# out is deliberately the quietest way a soldier can attack, and the loud part
# of it arrives three seconds later somewhere else.
$n = [int](0.14 * $SampleRate)
$throw = [float[]]::new($n)
$lp = 0.0
$hp = 0.0
$prev = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $SampleRate
    $white = 2.0 * $rng.NextDouble() - 1.0
    $lp = 0.80 * $lp + 0.20 * $white  # duller than the blade's: cloth, not edge
    $hp = 0.70 * ($hp + $lp - $prev)  # still off the bottom, so it isn't a thud
    $prev = $lp
    $env = [Math]::Min(1.0, $t / 0.025) * [Math]::Exp(-$t * 20.0)
    $throw[$i] = 2.6 * $hp * $env
}
Write-Wav (Join-Path $OutDir "throw.wav") $throw
