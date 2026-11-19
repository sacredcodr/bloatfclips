# Renders assets/bloatfclips.ico: the tray icon design (light ring, dark slate disc, red record dot)
# at 16..256 px with anti-aliasing, packed as PNG-compressed ICO entries.
# Usage: powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1
param([string]$Out = (Join-Path $PSScriptRoot "..\assets\bloatfclips.ico"))
Add-Type -AssemblyName System.Drawing

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$pngs = foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)
    $c = $s / 2.0
    # Same proportions as CreateRecorderTrayIcon in src/main.cpp (radii sqrt(56), 6, sqrt(14) at 16 px).
    foreach ($layer in @(@(0.4677, 'D0D6DC'), @(0.375, '20242A'), @(0.2339, 'E53935'))) {
        $r = $s * $layer[0]
        $brush = New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml('#' + $layer[1]))
        $g.FillEllipse($brush, [float]($c - $r), [float]($c - $r), [float](2 * $r), [float](2 * $r))
        $brush.Dispose()
    }
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}

New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$fs = [System.IO.File]::Create($Out)
$w = New-Object System.IO.BinaryWriter $fs
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Dispose()
"wrote $Out ($offset bytes)"
