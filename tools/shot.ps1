param(
    [string]$Out = "$env:TEMP\lss-shot.png",
    [int]$Width = 1280,
    [string]$Title = "FINAL FANTASY VII REBIRTH",  # the window to capture; "" captures the primary monitor
    [string]$Display = "",                         # or a whole monitor, e.g. \\.\DISPLAY1
    [switch]$Focus                                 # bring the window to the front first; refuse if it cannot be
)
# Screenshot of the game window (by title), a monitor, or the primary monitor, downscaled to $Width (0: full size).
# The window must be visible on screen (borderless or windowed); its client area is what is captured. A covered window
# captures whatever covers it, so -Focus brings it to the front first (as tools/keys.ps1 does) and refuses otherwise.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class LssWindow {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    // Windows lets a process take the foreground right after it has sent input: an Alt tap, as tools/keys.ps1 does.
    public static bool Focus(IntPtr h) {
        ShowWindow(h, 9);
        keybd_event(0x12, 0, 0, UIntPtr.Zero);
        keybd_event(0x12, 0, 2, UIntPtr.Zero);
        return SetForegroundWindow(h);
    }
}
"@
# Per-monitor DPI aware for this thread (-4: DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), so the window's rect and the
# copy are in physical pixels: unaware, a 3840x2160 window on a monitor scaled to 175 % read as 2194x1234 and only its
# top-left 57 % was captured (every shot before 2026-10-24 evening).
[void][LssWindow]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))
if ($Display) {
    $bounds = ([System.Windows.Forms.Screen]::AllScreens | Where-Object DeviceName -eq $Display).Bounds
} elseif ($Title) {
    # By title, not the process's main window: with the UE4SS console open, Windows can report the console as the game
    # process's main window.
    $hwnd = [LssWindow]::FindWindow([NullString]::Value, $Title)
    if ($hwnd -eq [IntPtr]::Zero) { throw "no window titled '$Title'" }
    if ($Focus) {
        [void][LssWindow]::Focus($hwnd)
        Start-Sleep -Milliseconds 400
        if ([LssWindow]::GetForegroundWindow() -ne $hwnd) { throw "the game window is not in the foreground; no shot" }
    }
    $rect = New-Object LssWindow+RECT; $origin = New-Object LssWindow+POINT
    [void][LssWindow]::GetClientRect($hwnd, [ref]$rect)
    [void][LssWindow]::ClientToScreen($hwnd, [ref]$origin)
    $bounds = New-Object System.Drawing.Rectangle $origin.X, $origin.Y, $rect.Right, $rect.Bottom
} else {
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
}
$full = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$g = [System.Drawing.Graphics]::FromImage($full)
$g.CopyFromScreen($bounds.X, $bounds.Y, 0, 0, $bounds.Size)
if ($Width -le 0) { $Width = $bounds.Width }
$h = [int]($bounds.Height * $Width / $bounds.Width)
$small = New-Object System.Drawing.Bitmap $full, $Width, $h
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $full.Dispose(); $small.Dispose()
"$Out ($($bounds.Width)x$($bounds.Height) at $($bounds.X),$($bounds.Y) -> ${Width}x$h)"
