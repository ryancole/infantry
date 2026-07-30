# Packs the PCM .wav files in assets/sounds into a single XACT wave bank
# (sounds.xwb) in the in-memory format DirectXTK12's WaveBank loads: WBND
# header v44 with bank data, entry metadata, entry names, and wave data
# segments. Replaces the XWBTool utility, which the vendored DirectXTK12
# checkout doesn't ship.
# Usage: .\etc\make_wavebank.ps1
[CmdletBinding()]
param(
    [string]$SoundsDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'assets\sounds'),
    [string]$OutFile = ''
)

$ErrorActionPreference = 'Stop'
if (-not $OutFile) { $OutFile = Join-Path $SoundsDir 'sounds.xwb' }

$wavs = Get-ChildItem (Join-Path $SoundsDir '*.wav') | Sort-Object Name
if (-not $wavs) {
    Write-Host "FAIL: no .wav files in $SoundsDir" -ForegroundColor Red
    exit 1
}

# Parse a RIFF/WAVE file: PCM format fields plus the raw data chunk.
function Read-Wav([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    if ([Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne 'RIFF' -or
        [Text.Encoding]::ASCII.GetString($bytes, 8, 4) -ne 'WAVE') {
        throw "$path is not a RIFF/WAVE file"
    }
    $wav = @{}
    $pos = 12
    while ($pos + 8 -le $bytes.Length) {
        $id = [Text.Encoding]::ASCII.GetString($bytes, $pos, 4)
        $size = [BitConverter]::ToUInt32($bytes, $pos + 4)
        if ($id -eq 'fmt ') {
            $wav.Tag = [BitConverter]::ToUInt16($bytes, $pos + 8)
            $wav.Channels = [BitConverter]::ToUInt16($bytes, $pos + 10)
            $wav.Rate = [BitConverter]::ToUInt32($bytes, $pos + 12)
            $wav.Bits = [BitConverter]::ToUInt16($bytes, $pos + 22)
        }
        elseif ($id -eq 'data') {
            $data = New-Object byte[] $size
            [Array]::Copy($bytes, $pos + 8, $data, 0, $size)
            $wav.Data = $data
        }
        $pos += 8 + $size + ($size % 2) # chunks are word-aligned
    }
    if ($wav.Tag -ne 1) { throw "$path is not PCM (format tag $($wav.Tag))" }
    if ($wav.Bits -ne 8 -and $wav.Bits -ne 16) { throw "$path must be 8- or 16-bit PCM" }
    if ($wav.Rate -ge (1 -shl 18)) { throw "$path sample rate too high for the miniformat" }
    $wav
}

$entries = foreach ($f in $wavs) {
    $w = Read-Wav $f.FullName
    $w.Name = $f.BaseName.ToLowerInvariant()
    Write-Host ("  {0}: {1} ch, {2} Hz, {3}-bit, {4} bytes" -f $w.Name, $w.Channels, $w.Rate, $w.Bits, $w.Data.Length)
    $w
}

$count = @($entries).Count
$align = 4
$headerSize = 4 + 4 + 4 + 5 * 8   # signature, versions, five segment regions
$bankDataSize = 4 + 4 + 64 + 4 + 4 + 4 + 4 + 8
$metaOffset = $headerSize + $bankDataSize
$namesOffset = $metaOffset + 24 * $count
$waveOffset = $namesOffset + 64 * $count
$waveOffset = ($waveOffset + $align - 1) -band -bnot ($align - 1)

# Lay out wave data with per-entry alignment.
$dataOffsets = @(); $cursor = 0
foreach ($e in $entries) {
    $cursor = ($cursor + $align - 1) -band -bnot ($align - 1)
    $dataOffsets += $cursor
    $cursor += $e.Data.Length
}
$waveBytes = $cursor

$stream = [IO.File]::Create($OutFile)
$writer = New-Object IO.BinaryWriter($stream)

# HEADER: 'WBND', tool version, format version, segment table.
$writer.Write([Text.Encoding]::ASCII.GetBytes('WBND'))
$writer.Write([uint32]44)
$writer.Write([uint32]44)
$writer.Write([uint32]$headerSize);  $writer.Write([uint32]$bankDataSize)      # BANKDATA
$writer.Write([uint32]$metaOffset);  $writer.Write([uint32](24 * $count))      # ENTRYMETADATA
$writer.Write([uint32]$namesOffset); $writer.Write([uint32]0)                  # SEEKTABLES (none: PCM)
$writer.Write([uint32]$namesOffset); $writer.Write([uint32](64 * $count))      # ENTRYNAMES
$writer.Write([uint32]$waveOffset);  $writer.Write([uint32]$waveBytes)         # ENTRYWAVEDATA

# BANKDATA: in-memory bank with entry names.
$writer.Write([uint32]0x00010000)  # FLAGS_ENTRYNAMES | TYPE_BUFFER
$writer.Write([uint32]$count)
$bankName = [Text.Encoding]::ASCII.GetBytes('infantry')
$writer.Write($bankName); $writer.Write((New-Object byte[] (64 - $bankName.Length)))
$writer.Write([uint32]24)          # entry metadata element size
$writer.Write([uint32]64)          # entry name element size
$writer.Write([uint32]$align)
$writer.Write([uint32]0)           # compact format (unused)
$writer.Write([uint64]0)           # build time (unused by the loader)

# ENTRYMETADATA: flags+duration, miniformat, play region, loop region.
for ($i = 0; $i -lt $count; $i++) {
    $e = $entries[$i]
    $blockAlign = $e.Channels * ($e.Bits / 8)
    $duration = [uint32]($e.Data.Length / $blockAlign)
    $writer.Write([uint32]($duration -shl 4))
    $mini = [uint32]0                             # wFormatTag = PCM
    $mini = $mini -bor ([uint32]$e.Channels -shl 2)
    $mini = $mini -bor ([uint32]$e.Rate -shl 5)
    $mini = $mini -bor ([uint32]$blockAlign -shl 23)
    if ($e.Bits -eq 16) { $mini = $mini -bor ([uint32]1 -shl 31) }
    $writer.Write([uint32]$mini)
    $writer.Write([uint32]$dataOffsets[$i])       # play region, segment-relative
    $writer.Write([uint32]$e.Data.Length)
    $writer.Write([uint32]0); $writer.Write([uint32]0)  # loop region
}

# ENTRYNAMES: fixed 64-byte, null-padded.
foreach ($e in $entries) {
    $name = [Text.Encoding]::ASCII.GetBytes($e.Name)
    $writer.Write($name); $writer.Write((New-Object byte[] (64 - $name.Length)))
}

# ENTRYWAVEDATA, aligned.
$writer.Write((New-Object byte[] ($waveOffset - $namesOffset - 64 * $count)))
for ($i = 0; $i -lt $count; $i++) {
    $pad = $dataOffsets[$i] - ($writer.BaseStream.Position - $waveOffset)
    if ($pad -gt 0) { $writer.Write((New-Object byte[] $pad)) }
    $writer.Write($entries[$i].Data)
}

$writer.Dispose(); $stream.Dispose()
Write-Host "wrote $OutFile ($((Get-Item $OutFile).Length) bytes, $count entries)"
