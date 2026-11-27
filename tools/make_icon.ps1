# XA Discord Explorer icon generator v0.2.0
# Produces the exact multi-resolution [XD] application identity used by the
# native executable. The style intentionally follows XA DevHub and Codex LB:
# dark navy rounded square, blue border, white first letter, blue second letter.
# Last Updated: 2026-08-16 09:57:14

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class XADiscordExplorerInkScan {
    public static int[] Box(Bitmap bitmap, int alphaMinimum) {
        Rectangle rectangle = new Rectangle(0, 0, bitmap.Width, bitmap.Height);
        BitmapData data = bitmap.LockBits(rectangle, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        int minX = int.MaxValue, maxX = -1, minY = int.MaxValue, maxY = -1;
        byte[] row = new byte[data.Stride];
        for (int y = 0; y < bitmap.Height; ++y) {
            Marshal.Copy(IntPtr.Add(data.Scan0, y * data.Stride), row, 0, data.Stride);
            for (int x = 0; x < bitmap.Width; ++x) {
                if (row[x * 4 + 3] < alphaMinimum) continue;
                minX = Math.Min(minX, x); maxX = Math.Max(maxX, x);
                minY = Math.Min(minY, y); maxY = Math.Max(maxY, y);
            }
        }
        bitmap.UnlockBits(data);
        return new int[] { minX, minY, maxX, maxY };
    }
}
'@

$resources = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\resources'))
$out = Join-Path $resources 'xa-discord-explorer.ico'
$preview = Join-Path $resources 'xa-discord-explorer-icon.png'

function New-RoundRect([single]$x, [single]$y, [single]$width, [single]$height, [single]$radius) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $diameter = 2 * $radius
    $path.AddArc($x, $y, $diameter, $diameter, 180, 90)
    $path.AddArc($x + $width - $diameter, $y, $diameter, $diameter, 270, 90)
    $path.AddArc($x + $width - $diameter, $y + $height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($x, $y + $height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

$backgroundTop = [System.Drawing.Color]::FromArgb(255, 0x22, 0x2f, 0x4a)
$backgroundBottom = [System.Drawing.Color]::FromArgb(255, 0x0d, 0x11, 0x17)
$accent = [System.Drawing.Color]::FromArgb(255, 0x4f, 0x8f, 0xf7)
$white = [System.Drawing.Color]::FromArgb(255, 0xe8, 0xed, 0xf5)
$sizes = 256, 128, 64, 48, 32, 24, 16
$entries = @()

foreach ($size in $sizes) {
    $bitmap = New-Object System.Drawing.Bitmap($size, $size)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $graphics.Clear([System.Drawing.Color]::Transparent)

    $scale = $size / 256.0
    $inset = [Math]::Max(1.0, 6.0 * $scale)
    $side = $size - 2 * $inset
    $radius = [Math]::Max(2.0, 46.0 * $scale)
    $field = New-RoundRect $inset $inset $side $side $radius
    $bounds = New-Object System.Drawing.RectangleF(0, 0, $size, $size)
    $background = New-Object System.Drawing.Drawing2D.LinearGradientBrush($bounds, $backgroundTop, $backgroundBottom, 65.0)
    $graphics.FillPath($background, $field)

    if ($size -ge 32) {
        $penWidth = [Math]::Max(2.0, 11.0 * $scale)
        $pen = New-Object System.Drawing.Pen($accent, $penWidth)
        $borderInset = $inset + $penWidth / 2.0
        $border = New-RoundRect $borderInset $borderInset ($size - 2 * $borderInset) ($size - 2 * $borderInset) ([Math]::Max(2.0, $radius - $penWidth / 2.0))
        $graphics.DrawPath($pen, $border)
        $pen.Dispose(); $border.Dispose()
    }

    $fontPixels = 112.0 * $scale
    if ($size -le 24) { $fontPixels = 126.0 * $scale }
    $secondColor = if ($size -le 24) { [System.Drawing.Color]::FromArgb(255, 0x8a, 0xb8, 0xff) } else { $accent }
    $supersample = 4
    $scratch = New-Object System.Drawing.Bitmap(($size * $supersample), ($size * $supersample), ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb))
    $scratchGraphics = [System.Drawing.Graphics]::FromImage($scratch)
    $scratchGraphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $scratchGraphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $scratchGraphics.Clear([System.Drawing.Color]::Transparent)
    $font = New-Object System.Drawing.Font('Segoe UI', ($fontPixels * $supersample), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $format = [System.Drawing.StringFormat]::GenericTypographic
    $firstSize = $scratchGraphics.MeasureString('X', $font, [System.Drawing.PointF]::Empty, $format)
    $padding = 2.0 * $supersample
    $firstBrush = New-Object System.Drawing.SolidBrush($white)
    $secondBrush = New-Object System.Drawing.SolidBrush($secondColor)
    $scratchGraphics.DrawString('X', $font, $firstBrush, $padding, $padding, $format)
    $scratchGraphics.DrawString('D', $font, $secondBrush, ($padding + $firstSize.Width), $padding, $format)
    $scratchGraphics.Dispose()

    $ink = [XADiscordExplorerInkScan]::Box($scratch, 24)
    if ($ink[2] -lt 0) { throw "Icon text rendered empty at size $size" }
    $inkCenterX = ($ink[0] + $ink[2] + 1) / 2.0
    $inkCenterY = ($ink[1] + $ink[3] + 1) / 2.0
    $offsetX = $size / 2.0 - $inkCenterX / $supersample
    $offsetY = $size / 2.0 - $inkCenterY / $supersample
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $destination = New-Object System.Drawing.RectangleF($offsetX, $offsetY, $size, $size)
    $graphics.DrawImage($scratch, $destination)

    $firstBrush.Dispose(); $secondBrush.Dispose(); $font.Dispose(); $scratch.Dispose()
    $background.Dispose(); $field.Dispose(); $graphics.Dispose()
    if ($size -eq 256) { $bitmap.Save($preview, [System.Drawing.Imaging.ImageFormat]::Png) }
    $memory = New-Object System.IO.MemoryStream
    $bitmap.Save($memory, [System.Drawing.Imaging.ImageFormat]::Png)
    $entries += , @{ Size = $size; Data = $memory.ToArray() }
    $memory.Dispose(); $bitmap.Dispose()
}

$stream = [System.IO.File]::Create($out)
$writer = New-Object System.IO.BinaryWriter($stream)
$writer.Write([uint16]0)
$writer.Write([uint16]1)
$writer.Write([uint16]$entries.Count)
$offset = 6 + 16 * $entries.Count
foreach ($entry in $entries) {
    $dimension = if ($entry.Size -ge 256) { 0 } else { $entry.Size }
    $writer.Write([byte]$dimension)
    $writer.Write([byte]$dimension)
    $writer.Write([byte]0)
    $writer.Write([byte]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]32)
    $writer.Write([uint32]$entry.Data.Length)
    $writer.Write([uint32]$offset)
    $offset += $entry.Data.Length
}
foreach ($entry in $entries) { $writer.Write($entry.Data) }
$writer.Close(); $stream.Close()

Write-Host "wrote $out ($((Get-Item $out).Length) bytes, $($entries.Count) sizes)"
Write-Host "wrote $preview ($((Get-Item $preview).Length) bytes)"

