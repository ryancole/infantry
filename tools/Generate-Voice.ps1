# Speaks the game's voice callouts into assets/sounds/ using the Windows speech
# engine (SAPI 5, over COM), then knocks the result about until it sounds like
# somebody on a battlefield instead of somebody reading one out.
# Usage: .\tools\Generate-Voice.ps1
#
# This is deliberately not part of Generate-Sounds.ps1. That script can promise
# byte-identical output from a fixed seed because it invents every sample it
# writes; this one asks Windows for a voice, and which voices a machine has is a
# fact about the machine. The same script on a box without David produces a
# different clip or none at all. So the .wav is committed and this file is the
# record of where it came from — the same deal trace_zone_map.py has with a
# minimap it can't redistribute — rather than a step anybody has to run.
#
# What SAPI hands back is a man calmly announcing a word, because reading is
# what every text-to-speech engine is built to do, and a callout is a yell. The
# processing below is the whole reason this is more than three lines, and none
# of it is polish: sped up, which lifts the pitch and drags the formants along
# with it so the voice reads as strained rather than merely hurried; driven into
# a soft clip, which is what a voice at the top of its range actually does; and
# rolled off underneath so it cuts through gunfire rather than sitting in the
# mud with the explosions. The silence SAPI pads the file with is cut off both
# ends, which matters for more than tidiness — Sound::Play3D applies its 3D
# transform once at spawn on the grounds that nothing here runs past about half
# a second, and a clip that is a third leading silence spends that budget on
# nothing.
#
# It is still not a shout. A person yelling into a phone would beat it outright,
# and this exists so there is a recognizable word to hear until somebody does
# that.
[CmdletBinding()]
param(
    [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'assets\sounds'),
    # Which installed voice to speak with, matched against the descriptions
    # SAPI reports. A soldier, so the male one; the first voice on the machine
    # if this matches nothing. Not $Voice, which is the same variable as the
    # $voice holding the engine below — PowerShell doesn't care about the case
    # and the [string] on it would quietly turn the COM object into its own
    # description.
    [string]$VoiceName = 'David'
)

$ErrorActionPreference = 'Stop'

# The callouts, and the only place the words are written down. A row here is a
# clip in the wave bank, named by the entry the game plays (Voice.h) — adding
# the next three is three more rows and nothing else.
$lines = @(
    @{ Name = 'medic'; Text = 'Medic!' }
)

$SampleRate = 22050
$SpeechRate = 2      # SAPI's own -10..10; a bark, not a sentence
$Speed      = 1.10   # resample factor: ~1.6 semitones up and 9% shorter
$Drive      = 2.2    # soft-clip amount, for the strain
$HighPass   = 180.0  # Hz; everything under this is chest, not word
# Not $Peak: the measured peaks below are $peak, which PowerShell considers the
# same variable, and a target that gets overwritten by the measurement it's
# divided by is a normalization that multiplies everything by one.
$TargetPeak = 0.80   # final normalization, matching the synthesized clips

New-Item -ItemType Directory -Force $OutDir | Out-Null

# --- The engine -----------------------------------------------------------
$voice = New-Object -ComObject SAPI.SpVoice
$chosen = $null
foreach ($token in $voice.GetVoices()) {
    if ($token.GetDescription() -like "*$VoiceName*") { $chosen = $token; break }
}
if (-not $chosen) {
    Write-Host "no voice matching '$VoiceName'; using the default" -ForegroundColor Yellow
} else {
    $voice.Voice = $chosen
}
Write-Host ("speaking as: " + $voice.Voice.GetDescription())
$voice.Rate = $SpeechRate
$voice.Volume = 100

# Reads back what SAPI wrote: it is always PCM at the format we asked for, so
# this only has to find the data chunk rather than understand a RIFF file.
function Read-Samples([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $pos = 12
    while ($pos + 8 -le $bytes.Length) {
        $id = [Text.Encoding]::ASCII.GetString($bytes, $pos, 4)
        $size = [BitConverter]::ToUInt32($bytes, $pos + 4)
        if ($id -eq 'data') {
            $n = [int]($size / 2)
            $out = [double[]]::new($n)
            for ($i = 0; $i -lt $n; $i++) {
                $out[$i] = [BitConverter]::ToInt16($bytes, $pos + 8 + $i * 2) / 32768.0
            }
            return $out
        }
        $pos += 8 + $size + ($size % 2)
    }
    throw "$path has no data chunk"
}

# Same header the synthesized clips are written with: mono 16-bit PCM, which is
# all make_wavebank.ps1 packs.
function Write-Wav([string]$Path, [double[]]$Samples) {
    $writer = [IO.BinaryWriter]::new([IO.File]::Create($Path))
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
        $writer.Write([uint32]($SampleRate * 2))
        $writer.Write([uint16]2)
        $writer.Write([uint16]16)
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
}

foreach ($line in $lines) {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("infantry_voice_" + $line.Name + ".wav")

    # SAPI writes straight to a file through a stream of its own; 22 is
    # SAFT22kHz16BitMono, which is the rate every other clip in the bank is at,
    # and 3 is "create for write".
    $stream = New-Object -ComObject SAPI.SpFileStream
    $stream.Format.Type = 22
    $stream.Open($tmp, 3, $false)
    $voice.AudioOutputStream = $stream
    $voice.Speak($line.Text, 0) | Out-Null
    $stream.Close()
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($stream)

    $raw = Read-Samples $tmp
    Remove-Item $tmp -Force

    # --- Trim: SAPI pads both ends, and the tail is worth keeping a little of
    # because the word decays into it. The threshold is relative to the loudest
    # thing in the clip, so it doesn't depend on how loud this voice happens to
    # be.
    $peak = 0.0
    foreach ($s in $raw) { $peak = [Math]::Max($peak, [Math]::Abs($s)) }
    $gate = $peak * 0.02
    $first = 0
    while ($first -lt $raw.Count -and [Math]::Abs($raw[$first]) -lt $gate) { $first++ }
    $last = $raw.Count - 1
    while ($last -gt $first -and [Math]::Abs($raw[$last]) -lt $gate) { $last-- }
    $first = [Math]::Max(0, $first - [int](0.005 * $SampleRate))
    $last = [Math]::Min($raw.Count - 1, $last + [int](0.030 * $SampleRate))
    $trimmed = $raw[$first..$last]

    # --- Speed: read out faster than it was written. Pitch and formants both
    # ride up with it, which is the part that matters — a voice pitched up
    # without its formants is a chipmunk, and a voice with both is a man
    # straining.
    $n = [int]([Math]::Floor($trimmed.Count / $Speed))
    $out = [double[]]::new($n)
    for ($i = 0; $i -lt $n; $i++) {
        $src = $i * $Speed
        $a = [int][Math]::Floor($src)
        $b = [Math]::Min($a + 1, $trimmed.Count - 1)
        $f = $src - $a
        $out[$i] = $trimmed[$a] * (1.0 - $f) + $trimmed[$b] * $f
    }

    # --- High-pass, then drive. In that order: clipping the low end first would
    # only make more of it, and what the roll-off is for is leaving room under
    # the voice for the explosions to have.
    $a = [Math]::Exp(-2.0 * [Math]::PI * $HighPass / $SampleRate)
    $prevIn = 0.0; $prevOut = 0.0
    for ($i = 0; $i -lt $n; $i++) {
        $x = $out[$i]
        $prevOut = $a * ($prevOut + $x - $prevIn)
        $prevIn = $x
        $out[$i] = [Math]::Tanh($Drive * $prevOut)
    }

    # --- Ends: a few milliseconds either side, so neither the start nor the
    # stop is a click. The tail is the longer of the two because a word ends by
    # dying away and a click at the end of one is the easier to hear.
    $fadeIn = [int](0.003 * $SampleRate)
    $fadeOut = [int](0.008 * $SampleRate)
    for ($i = 0; $i -lt $fadeIn -and $i -lt $n; $i++) { $out[$i] *= $i / $fadeIn }
    for ($i = 0; $i -lt $fadeOut -and $i -lt $n; $i++) {
        $out[$n - 1 - $i] *= $i / $fadeOut
    }

    $peak = 0.0
    foreach ($s in $out) { $peak = [Math]::Max($peak, [Math]::Abs($s)) }
    if ($peak -gt 0.0) {
        for ($i = 0; $i -lt $n; $i++) { $out[$i] = $out[$i] * $TargetPeak / $peak }
    }

    $path = Join-Path $OutDir ($line.Name + '.wav')
    Write-Wav $path $out
    Write-Host ("  {0}: `"{1}`" -> {2} samples, {3:F2}s" -f $line.Name, $line.Text, $n,
                ($n / $SampleRate))
}

[void][Runtime.InteropServices.Marshal]::ReleaseComObject($voice)
Write-Host "wrote $($lines.Count) callout(s) to $OutDir"
Write-Host "run etc\make_wavebank.ps1 to pack them"
