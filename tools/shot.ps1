param(
    [string]$Out = "$env:TEMP\lss-shot.png",
    [int]$Width = 1280,
    [string]$Title = "FINAL FANTASY VII REBIRTH",  # the window to capture; "" captures the primary monitor
    [string]$Display = ""                          # or a whole monitor, e.g. \\.\DISPLAY1
)
# Screenshot of the game window (by title), a monitor, or the primary monitor, downscaled to $Width.
# The window must be visible on screen (borderless or windowed); its client area is what is captured.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class LssWindow {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
}
"@
if ($Display) {
    $bounds = ([System.Windows.Forms.Screen]::AllScreens | Where-Object DeviceName -eq $Display).Bounds
} elseif ($Title) {
    $game = Get-Process | Where-Object { $_.MainWindowHandle -ne 0 -and $_.MainWindowTitle -eq $Title } | Select-Object -First 1
    if (-not $game) { throw "no window titled '$Title'" }
    $rect = New-Object LssWindow+RECT; $origin = New-Object LssWindow+POINT
    [void][LssWindow]::GetClientRect($game.MainWindowHandle, [ref]$rect)
    [void][LssWindow]::ClientToScreen($game.MainWindowHandle, [ref]$origin)
    $bounds = New-Object System.Drawing.Rectangle $origin.X, $origin.Y, $rect.Right, $rect.Bottom
} else {
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
}
$full = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$g = [System.Drawing.Graphics]::FromImage($full)
$g.CopyFromScreen($bounds.X, $bounds.Y, 0, 0, $bounds.Size)
$h = [int]($bounds.Height * $Width / $bounds.Width)
$small = New-Object System.Drawing.Bitmap $full, $Width, $h
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $full.Dispose(); $small.Dispose()
"$Out ($($bounds.Width)x$($bounds.Height) at $($bounds.X),$($bounds.Y) -> ${Width}x$h)"
