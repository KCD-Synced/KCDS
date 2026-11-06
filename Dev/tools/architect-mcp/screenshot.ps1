# A picture of a KCDS editor's game window for the MCP server's editor_screenshot:
# the window of the process -ProcessId drawn with PrintWindow (PW_RENDERFULLCONTENT, which the
# game's DirectX window needs), scaled to at most -Width pixels across, saved as a PNG at -Out.
#
# A burst (-Count above 1): -Count pictures -EveryMs apart put in one contact sheet
# of -Columns across, each tile -Width wide with its time since the first; -ViewOnly crops each to
# the editor's world view (without its panels), so motion can be read in one picture.
param(
  [Parameter(Mandatory = $true)][int]$ProcessId,
  [Parameter(Mandatory = $true)][string]$Out,
  [int]$Width = 1280,
  [int]$Count = 1,
  [int]$EveryMs = 100,
  [int]$Columns = 4,
  [switch]$ViewOnly
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class KcdsWindow {
  public delegate bool EnumProc(IntPtr window, IntPtr data);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr data);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint id);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out RECT rect);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public static IntPtr Find(uint process) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((window, data) => {
      uint owner;
      GetWindowThreadProcessId(window, out owner);
      if (owner == process && IsWindowVisible(window)) { found = window; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
'@
$window = [KcdsWindow]::Find([uint32]$ProcessId)
if ($window -eq [IntPtr]::Zero) { throw "no window of process $ProcessId" }

# One picture of the window, scaled to $tileWidth across (cropped to the world view with
# -ViewOnly: the shell's view is 0.172 to 0.8 across and 0.078 to 0.713 down the window).
function Get-Shot([int]$tileWidth) {
  $rect = New-Object KcdsWindow+RECT
  [void][KcdsWindow]::GetClientRect($window, [ref]$rect)
  $bitmap = New-Object System.Drawing.Bitmap ($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top)
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  $dc = $graphics.GetHdc()
  [void][KcdsWindow]::PrintWindow($window, $dc, 3)
  $graphics.ReleaseHdc($dc)
  $source = New-Object System.Drawing.Rectangle 0, 0, $bitmap.Width, $bitmap.Height
  if ($ViewOnly) {
    $source = New-Object System.Drawing.Rectangle ([int]($bitmap.Width * 0.172)), ([int]($bitmap.Height * 0.078)), ([int]($bitmap.Width * 0.628)), ([int]($bitmap.Height * 0.635))
  }
  $scale = [Math]::Min(1.0, $tileWidth / $source.Width)
  $small = New-Object System.Drawing.Bitmap ([int]($source.Width * $scale)), ([int]($source.Height * $scale))
  $draw = [System.Drawing.Graphics]::FromImage($small)
  $draw.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $target = New-Object System.Drawing.Rectangle 0, 0, $small.Width, $small.Height
  $draw.DrawImage($bitmap, $target, $source, [System.Drawing.GraphicsUnit]::Pixel)
  $draw.Dispose()
  $graphics.Dispose()
  $bitmap.Dispose()
  return $small
}

if ($Count -le 1) {
  $shot = Get-Shot $Width
  $shot.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
  exit 0
}

$clock = [System.Diagnostics.Stopwatch]::StartNew()
$shots = @()
$times = @()
for ($index = 0; $index -lt $Count; $index++) {
  $due = $index * $EveryMs
  while ($clock.ElapsedMilliseconds -lt $due) { Start-Sleep -Milliseconds 5 }
  $times += $clock.ElapsedMilliseconds
  $shots += Get-Shot $Width
}
$columns = [Math]::Max(1, [Math]::Min($Columns, $Count))
$rows = [Math]::Ceiling($Count / $columns)
$tileWidth = $shots[0].Width
$tileHeight = $shots[0].Height
$sheet = New-Object System.Drawing.Bitmap ($tileWidth * $columns), ($tileHeight * $rows)
$paint = [System.Drawing.Graphics]::FromImage($sheet)
$font = New-Object System.Drawing.Font 'Segoe UI', 11
$back = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(170, 0, 0, 0))
for ($index = 0; $index -lt $Count; $index++) {
  $x = ($index % $columns) * $tileWidth
  $y = [Math]::Floor($index / $columns) * $tileHeight
  $paint.DrawImage($shots[$index], $x, $y)
  $label = "{0}  {1:0.00} s" -f ($index + 1), ($times[$index] / 1000.0)
  $paint.FillRectangle($back, $x, $y, 92, 20)
  $paint.DrawString($label, $font, [System.Drawing.Brushes]::Yellow, $x + 3, $y + 1)
}
$sheet.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
