# Display helper: list | modes <\\.\DISPLAYn> | set <\\.\DISPLAYn> <w> <h> <x> <y> <hz> | logical
param([string]$Cmd = 'list', [string]$Dev, [int]$W, [int]$H, [int]$X, [int]$Y, [int]$Hz = 60)
Add-Type @'
using System; using System.Runtime.InteropServices;
public class DC {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct DEVMODE { [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string dmDeviceName; public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; public int dmFields; public int x, y; public int dmDisplayOrientation, dmDisplayFixedOutput; public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate; [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string dmFormName; public short dmLogPixels; public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency, a,b,c,d,e,f,g,h; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool EnumDisplaySettingsW(string dev, int mode, ref DEVMODE dm);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int ChangeDisplaySettingsExW(string dev, ref DEVMODE dm, IntPtr hwnd, uint flags, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int ChangeDisplaySettingsExW(string dev, IntPtr dm, IntPtr hwnd, uint flags, IntPtr lp);
}
'@
function NewDM { $d = New-Object DC+DEVMODE; $d.dmSize = [Runtime.InteropServices.Marshal]::SizeOf($d); return $d }
switch ($Cmd) {
  'list' {
    foreach ($n in 1..6) { $d = NewDM; if ([DC]::EnumDisplaySettingsW("\\.\DISPLAY$n", -1, [ref]$d)) { "DISPLAY$n physical $($d.dmPelsWidth)x$($d.dmPelsHeight)@$($d.dmDisplayFrequency) at ($($d.x),$($d.y))" } }
  }
  'logical' {
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.Screen]::AllScreens | ForEach-Object { "$($_.DeviceName) $($_.Bounds.Width)x$($_.Bounds.Height) at ($($_.Bounds.X),$($_.Bounds.Y))" }
  }
  'modes' {
    $i = 0; $modes = @()
    while ($i -lt 2000) { $d = NewDM; if (-not [DC]::EnumDisplaySettingsW($Dev, $i, [ref]$d)) { break }; $modes += "$($d.dmPelsWidth)x$($d.dmPelsHeight)@$($d.dmDisplayFrequency)"; $i++ }
    "modes: $i"; $modes | Sort-Object -Unique | Where-Object { $_ -match '^(2560x1440|1920x1080|3840x2160|3440x1440)' }
  }
  'set' {
    $d = NewDM; [void][DC]::EnumDisplaySettingsW($Dev, -1, [ref]$d)
    "before: $($d.dmPelsWidth)x$($d.dmPelsHeight)@$($d.dmDisplayFrequency) at ($($d.x),$($d.y))"
    $d.dmPelsWidth = $W; $d.dmPelsHeight = $H; $d.x = $X; $d.y = $Y; $d.dmDisplayFrequency = $Hz
    $d.dmFields = 0x80000 -bor 0x100000 -bor 0x20 -bor 0x400000
    $r = [DC]::ChangeDisplaySettingsExW($Dev, [ref]$d, [IntPtr]::Zero, 0x00000001, [IntPtr]::Zero)
    "result: $r (0 ok, 1 restart, -1 failed, -2 badmode, -5 badparam)"
    Start-Sleep 3
    $d = NewDM; [void][DC]::EnumDisplaySettingsW($Dev, -1, [ref]$d)
    "after: $($d.dmPelsWidth)x$($d.dmPelsHeight)@$($d.dmDisplayFrequency) at ($($d.x),$($d.y))"
  }
}
