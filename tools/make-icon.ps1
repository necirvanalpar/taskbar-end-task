# Regenerates src\EndTask.ico: a white cross on a red rounded square, in every size Windows asks for.
# Usage:  powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$outFile = Join-Path (Split-Path $PSScriptRoot -Parent) 'src\EndTask.ico'

function New-IconBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # rounded square, half a pixel inside the bitmap
    $s = [single]$size
    $lo = [single]0.5
    $side = [single]($s - 1.0)
    $d = [single]($s * 0.48)       # corner diameter
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($lo, $lo, $d, $d, 180, 90)
    $path.AddArc($lo + $side - $d, $lo, $d, $d, 270, 90)
    $path.AddArc($lo + $side - $d, $lo + $side - $d, $d, $d, 0, 90)
    $path.AddArc($lo, $lo + $side - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $red = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 196, 43, 28))
    $g.FillPath($red, $path)

    # cross
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, [single]($s * 0.15))
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $a0 = [single]($s * 0.31)
    $a1 = [single]($s * 0.69)
    $g.DrawLine($pen, $a0, $a0, $a1, $a1)
    $g.DrawLine($pen, $a0, $a1, $a1, $a0)

    $pen.Dispose(); $red.Dispose(); $path.Dispose(); $g.Dispose()
    return $bmp
}

# One ICO image: PNG for 256 px, classic 32-bit DIB (colour rows bottom-up + empty AND mask) below that.
function Get-IconImageBytes([System.Drawing.Bitmap]$bmp) {
    $size = $bmp.Width
    $ms = New-Object System.IO.MemoryStream
    if ($size -ge 256) {
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        return ,$ms.ToArray()
    }
    $rect = New-Object System.Drawing.Rectangle(0, 0, $size, $size)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $pixels = New-Object byte[] ($size * $size * 4)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $pixels, 0, $pixels.Length)
    $bmp.UnlockBits($data)

    $maskRow = [int]([math]::Floor(($size + 31) / 32)) * 4
    $w = New-Object System.IO.BinaryWriter($ms)
    $w.Write([uint32]40); $w.Write([int32]$size); $w.Write([int32]($size * 2))
    $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]0)
    $w.Write([uint32]($pixels.Length + $maskRow * $size))
    $w.Write([int32]0); $w.Write([int32]0); $w.Write([uint32]0); $w.Write([uint32]0)
    for ($y = $size - 1; $y -ge 0; $y--) { $w.Write($pixels, $y * $size * 4, $size * 4) }
    $w.Write((New-Object byte[] ($maskRow * $size)))
    $w.Flush()
    return ,$ms.ToArray()
}

$images = @()
foreach ($size in $sizes) {
    $bmp = New-IconBitmap $size
    $images += ,@{ Size = $size; Bytes = (Get-IconImageBytes $bmp) }
    $bmp.Dispose()
}

$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($out)
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $dim = if ($img.Size -ge 256) { 0 } else { $img.Size }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$img.Bytes.Length); $bw.Write([uint32]$offset)
    $offset += $img.Bytes.Length
}
foreach ($img in $images) { $bw.Write([byte[]]$img.Bytes) }
$bw.Flush()
[System.IO.File]::WriteAllBytes($outFile, $out.ToArray())
"Wrote $outFile ($($out.Length) bytes, sizes: $($sizes -join ', '))"
