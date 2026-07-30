# Bakes a system font into a .spritefont binary (the MakeSpriteFont format
# DirectXTK12's SpriteFont loads): "DXTKfont" magic, glyph table, then a
# premultiplied-alpha BGRA glyph atlas. Rasterization uses GDI+, the same
# engine the original MakeSpriteFont tool uses.
# Usage: .\etc\make_font.ps1 [-FontName Consolas] [-SizePx 64] [-PreviewPng path]
[CmdletBinding()]
param(
    [string]$FontName = 'Consolas',
    [int]$SizePx = 64,
    [string]$OutFile = (Join-Path (Split-Path $PSScriptRoot -Parent) 'assets\fonts\hud.spritefont'),
    [string]$PreviewPng = ''
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$firstChar = 32   # space
$lastChar = 126   # tilde
$defaultChar = 63 # '?'
$atlasWidth = 512
$pad = 1          # atlas padding between glyphs

$font = New-Object System.Drawing.Font($FontName, $SizePx,
    [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
if ($font.Name -ne $FontName) {
    Write-Host "FAIL: font '$FontName' not found (GDI+ substituted '$($font.Name)')" -ForegroundColor Red
    exit 1
}

# Typographic measuring: no GDI+ padding, and count trailing spaces.
$fmt = [System.Drawing.StringFormat]::GenericTypographic.Clone()
$fmt.FormatFlags = $fmt.FormatFlags -bor [System.Drawing.StringFormatFlags]::MeasureTrailingSpaces

# Scratch surface each glyph is rendered into before tight-cropping.
$cell = $SizePx * 3
$scratch = New-Object System.Drawing.Bitmap($cell, $cell, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$gfx = [System.Drawing.Graphics]::FromImage($scratch)
$gfx.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
$lineSpacing = $font.GetHeight($gfx)

# Rasterizes one character; returns its tight alpha bitmap and metrics.
function Get-GlyphData([char]$ch) {
    $gfx.Clear([System.Drawing.Color]::Transparent)
    $gfx.DrawString([string]$ch, $font, [System.Drawing.Brushes]::White, $SizePx, $SizePx, $fmt)

    # Advance width via the double-measure trick (kerningless for a pair of
    # identical glyphs in any sane font; exact for monospace).
    $one = $gfx.MeasureString([string]$ch, $font, [System.Drawing.PointF]::Empty, $fmt).Width
    $two = $gfx.MeasureString(([string]$ch + $ch), $font, [System.Drawing.PointF]::Empty, $fmt).Width
    $advance = $two - $one

    # Tight bounds from the alpha channel.
    $bits = $scratch.LockBits(
        (New-Object System.Drawing.Rectangle(0, 0, $cell, $cell)),
        [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $raw = New-Object byte[] ($bits.Stride * $cell)
    [System.Runtime.InteropServices.Marshal]::Copy($bits.Scan0, $raw, 0, $raw.Length)
    $scratch.UnlockBits($bits)

    $minX = $cell; $minY = $cell; $maxX = -1; $maxY = -1
    for ($y = 0; $y -lt $cell; $y++) {
        $row = $y * $bits.Stride
        for ($x = 0; $x -lt $cell; $x++) {
            if ($raw[$row + $x * 4 + 3] -ne 0) {
                if ($x -lt $minX) { $minX = $x }
                if ($x -gt $maxX) { $maxX = $x }
                if ($y -lt $minY) { $minY = $y }
                if ($y -gt $maxY) { $maxY = $y }
            }
        }
    }

    if ($maxX -lt 0) {
        # Whitespace: empty subrect, all width in the advance.
        return @{ Alpha = $null; W = 0; H = 0; XOff = 0.0; YOff = 0.0; XAdv = $advance }
    }

    $w = $maxX - $minX + 1
    $h = $maxY - $minY + 1
    $alpha = New-Object byte[] ($w * $h)
    for ($y = 0; $y -lt $h; $y++) {
        $row = ($minY + $y) * $bits.Stride
        for ($x = 0; $x -lt $w; $x++) {
            $alpha[$y * $w + $x] = $raw[$row + ($minX + $x) * 4 + 3]
        }
    }
    # Offsets are relative to the pen at the top-left of the line cell; the
    # glyph was drawn with its cell origin at ($SizePx, $SizePx). DirectXTK
    # advances the pen by XOff + subrect width + XAdv per glyph.
    return @{
        Alpha = $alpha; W = $w; H = $h
        XOff = [float]($minX - $SizePx)
        YOff = [float]($minY - $SizePx)
        XAdv = [float]($advance - ($minX - $SizePx) - $w)
    }
}

Write-Host "Rasterizing $FontName ${SizePx}px..."
$glyphs = @{}
for ($c = $firstChar; $c -le $lastChar; $c++) {
    $glyphs[$c] = Get-GlyphData ([char]$c)
}

# Shelf-pack tallest-first into a fixed-width atlas.
$order = ($firstChar..$lastChar) | Sort-Object { -$glyphs[$_].H }
$shelfX = 0; $shelfY = 0; $shelfH = 0
foreach ($c in $order) {
    $g = $glyphs[$c]
    if ($g.W -eq 0) { $g.X = 0; $g.Y = 0; continue }
    if ($shelfX + $g.W + $pad -gt $atlasWidth) {
        $shelfY += $shelfH + $pad
        $shelfX = 0
        $shelfH = 0
    }
    $g.X = $shelfX
    $g.Y = $shelfY
    $shelfX += $g.W + $pad
    if ($g.H -gt $shelfH) { $shelfH = $g.H }
}
$atlasHeight = $shelfY + $shelfH
$atlasHeight = ($atlasHeight + 3) -band -bnot 3

# Compose the premultiplied BGRA atlas: white glyphs, so every channel is
# just the coverage value.
$atlas = New-Object byte[] ($atlasWidth * $atlasHeight * 4)
foreach ($c in $firstChar..$lastChar) {
    $g = $glyphs[$c]
    for ($y = 0; $y -lt $g.H; $y++) {
        for ($x = 0; $x -lt $g.W; $x++) {
            $a = $g.Alpha[$y * $g.W + $x]
            $o = (($g.Y + $y) * $atlasWidth + $g.X + $x) * 4
            $atlas[$o] = $a; $atlas[$o + 1] = $a; $atlas[$o + 2] = $a; $atlas[$o + 3] = $a
        }
    }
}

New-Item -ItemType Directory -Force (Split-Path $OutFile -Parent) | Out-Null
$stream = [System.IO.File]::Create($OutFile)
$writer = New-Object System.IO.BinaryWriter($stream)
$writer.Write([System.Text.Encoding]::ASCII.GetBytes('DXTKfont'))
$writer.Write([uint32]($lastChar - $firstChar + 1))
foreach ($c in $firstChar..$lastChar) {
    $g = $glyphs[$c]
    $writer.Write([uint32]$c)
    $writer.Write([int32]$g.X)               # Subrect.left
    $writer.Write([int32]$g.Y)               # Subrect.top
    $writer.Write([int32]($g.X + $g.W))      # Subrect.right
    $writer.Write([int32]($g.Y + $g.H))      # Subrect.bottom
    $writer.Write([float]$g.XOff)
    $writer.Write([float]$g.YOff)
    $writer.Write([float]$g.XAdv)
}
$writer.Write([float]$lineSpacing)
$writer.Write([uint32]$defaultChar)
$writer.Write([uint32]$atlasWidth)
$writer.Write([uint32]$atlasHeight)
$writer.Write([uint32]87)                    # DXGI_FORMAT_B8G8R8A8_UNORM
$writer.Write([uint32]($atlasWidth * 4))     # stride
$writer.Write([uint32]$atlasHeight)          # rows
$writer.Write($atlas)
$writer.Dispose()
$stream.Dispose()

if ($PreviewPng) {
    $bmp = New-Object System.Drawing.Bitmap($atlasWidth, $atlasHeight, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $bits = $bmp.LockBits((New-Object System.Drawing.Rectangle(0, 0, $atlasWidth, $atlasHeight)),
        [System.Drawing.Imaging.ImageLockMode]::WriteOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    [System.Runtime.InteropServices.Marshal]::Copy($atlas, 0, $bits.Scan0, $atlas.Length)
    $bmp.UnlockBits($bits)
    $bmp.Save($PreviewPng, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "preview: $PreviewPng"
}

$gfx.Dispose(); $scratch.Dispose(); $font.Dispose()
Write-Host "wrote $OutFile ($((Get-Item $OutFile).Length) bytes, ${atlasWidth}x${atlasHeight} atlas)"
