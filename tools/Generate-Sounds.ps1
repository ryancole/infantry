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

# medic: a soldier shouting for one. The only clip here that is trying to be a
# voice, and it's the one most obviously waiting to be replaced by somebody
# saying the word into a microphone — a formant synthesizer can make a mouth
# shape but it can't make a person.
#
# What it does have is the shape of the call, which is most of what carries at
# forty units: two syllables of about the right lengths, a closure between them,
# and a stop cutting the second one off. The source is an impulse train at a
# falling pitch — a shout starts high and comes down — run through three
# resonators standing in for the first three formants, whose centers glide
# between the vowels rather than stepping, because a mouth moves and a step
# clicks. /i:/ is the wide pair, a low F1 under a high F2; the second vowel
# closes the gap between them. The two consonants that survive are the ones the
# ear needs: the /d/ release, which is a noise burst through the same
# resonators, and the /k/, which is nothing at all — the sound just stops, and
# stopping is what makes it a word instead of a note.
$n = [int](0.45 * $SampleRate)
$medic = [float[]]::new($n)
$phase = 0.0
$f1 = 300.0; $f2 = 1100.0; $f3 = 2400.0  # the formants as they currently stand
$amp = 0.0                               # and the loudness, glided for the same reason
$src = 0.0
$y1a = 0.0; $y1b = 0.0; $y2a = 0.0; $y2b = 0.0; $y3a = 0.0; $y3b = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $SampleRate
    # What the mouth is doing this instant. The noise column is the plosive:
    # everywhere else it's a whisper of breath under the voice.
    #                             F1      F2      F3    amp   noise
    if     ($t -lt 0.035) { $tf1 = 300; $tf2 = 1100; $tf3 = 2400; $tAmp = 0.35; $nz = 0.00 } # /m/
    elseif ($t -lt 0.215) { $tf1 = 320; $tf2 = 2250; $tf3 = 3000; $tAmp = 1.00; $nz = 0.05 } # /i:/
    elseif ($t -lt 0.265) { $tf1 = 350; $tf2 = 2000; $tf3 = 2800; $tAmp = 0.00; $nz = 0.00 } # closure
    elseif ($t -lt 0.285) { $tf1 = 420; $tf2 = 1800; $tf3 = 2600; $tAmp = 0.00; $nz = 1.00 } # /d/
    elseif ($t -lt 0.400) { $tf1 = 430; $tf2 = 1850; $tf3 = 2600; $tAmp = 0.95; $nz = 0.04 } # vowel
    else                  { $tf1 = 430; $tf2 = 1850; $tf3 = 2600; $tAmp = 0.00; $nz = 0.00 } # /k/
    $f1 += 0.006 * ($tf1 - $f1)
    $f2 += 0.006 * ($tf2 - $f2)
    $f3 += 0.006 * ($tf3 - $f3)
    $amp += 0.030 * ($tAmp - $amp)

    # The glottis: one impulse per period, softened by a one-pole so the source
    # rolls off the way a voice does instead of buzzing like a flat spectrum.
    # The pitch falls through the call and wobbles, because a held note is a
    # sung one and this is meant to be shouted.
    $f0 = 180.0 - 55.0 * $t / 0.45 + 3.0 * [Math]::Sin(2.0 * [Math]::PI * 5.5 * $t)
    $phase += $f0 / $SampleRate
    $pulse = 0.0
    if ($phase -ge 1.0) { $phase -= 1.0; $pulse = 1.0 }
    $src += 0.70 * ($pulse - $src)
    # One draw per sample whatever the mouth is doing: the RNG's sequence is
    # this file's determinism, and it can't depend on which branch above ran.
    $white = 2.0 * $rng.NextDouble() - 1.0
    $x = $amp * $src + $nz * 0.25 * $white

    $w = 2.0 * [Math]::PI * $f1 / $SampleRate
    $r = 0.9870
    $o1 = (1.0 - $r) * $x + 2.0 * $r * [Math]::Cos($w) * $y1a - $r * $r * $y1b
    $y1b = $y1a; $y1a = $o1
    $w = 2.0 * [Math]::PI * $f2 / $SampleRate
    $r = 0.9845
    $o2 = (1.0 - $r) * $x + 2.0 * $r * [Math]::Cos($w) * $y2a - $r * $r * $y2b
    $y2b = $y2a; $y2a = $o2
    $w = 2.0 * [Math]::PI * $f3 / $SampleRate
    $r = 0.9760
    $o3 = (1.0 - $r) * $x + 2.0 * $r * [Math]::Cos($w) * $y3a - $r * $r * $y3b
    $y3b = $y3a; $y3a = $o3

    # Softly overdriven: a shout is a voice at the top of its range, and the
    # clipping is where that lives. F1 is mixed *under* F2 rather than over it,
    # which looks wrong next to a chart of a real vowel and isn't: a two-pole
    # resonator has far more gain down where its poles crowd together, so an
    # even mix comes out as a boom with a vowel somewhere inside it. These
    # weights are what makes the three bands land at roughly the levels the
    # numbers say.
    $medic[$i] = [Math]::Tanh(2.2 * (0.55 * $o1 + 1.00 * $o2 + 0.45 * $o3))
}
# Normalized rather than scaled by a chosen number: what three resonators come
# out at is not something to predict by hand, and the level this wants is the
# same as every other clip's — loud enough to carry, quiet enough to sit under
# the gunfire it will be shouted through.
$peak = 0.0
foreach ($s in $medic) { $peak = [Math]::Max($peak, [Math]::Abs($s)) }
if ($peak -gt 0.0) {
    for ($i = 0; $i -lt $n; $i++) { $medic[$i] = $medic[$i] * 0.80 / $peak }
}
Write-Wav (Join-Path $OutDir "medic.wav") $medic
