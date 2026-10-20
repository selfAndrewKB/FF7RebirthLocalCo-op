# Span the game's window across the three-monitor row, the Resize Raccoon way: strip the border styles, MoveWindow.
param([int]$X = -1971, [int]$Y = 1440, [int]$W = 7680, [int]$H = 1440, [switch]$KeepBorder)
Add-Type @'
using System; using System.Runtime.InteropServices;
public class SW {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtrW(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern IntPtr SetWindowLongPtrW(IntPtr h, int i, IntPtr v);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int w, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int cy, bool r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
}
'@
$p = Get-Process ff7rebirth* -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Error 'no game window'; exit 1 }
$h = $p.MainWindowHandle
$r = New-Object SW+RECT; [void][SW]::GetWindowRect($h, [ref]$r); "before: window $($r.L),$($r.T)-$($r.R),$($r.B) ($($r.R-$r.L)x$($r.B-$r.T))"
if (-not $KeepBorder) {
  $GWL_STYLE = -16; $GWL_EXSTYLE = -20
  $style = [SW]::GetWindowLongPtrW($h, $GWL_STYLE).ToInt64()
  $style = $style -band (-bnot (0x00040000 -bor 0x00400000 -bor 0x00800000))   # WS_THICKFRAME | WS_DLGFRAME | WS_BORDER (= no WS_CAPTION)
  [void][SW]::SetWindowLongPtrW($h, $GWL_STYLE, [IntPtr]$style)
  $ex = [SW]::GetWindowLongPtrW($h, $GWL_EXSTYLE).ToInt64()
  $ex = $ex -band (-bnot (0x00000001 -bor 0x00000100 -bor 0x00000200 -bor 0x00020000))   # WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE
  [void][SW]::SetWindowLongPtrW($h, $GWL_EXSTYLE, [IntPtr]$ex)
  [void][SW]::SetWindowPos($h, [IntPtr]::Zero, 0, 0, 0, 0, 0x0001 -bor 0x0002 -bor 0x0004 -bor 0x0010 -bor 0x0020 -bor 0x0200 -bor 0x0400)  # NOSIZE NOMOVE NOZORDER NOACTIVATE FRAMECHANGED NOOWNERZORDER NOSENDCHANGING
}
# Move, then verify and re-apply (Resize Raccoon's watcher does the same): the first move right after the style
# strip came out 7680x2880 on 2026-10-20 (cause unknown); a second move with a changed size lands right.
for ($try = 1; $try -le 3; $try++) {
  if ($try -gt 1) { [void][SW]::MoveWindow($h, $X, $Y, $W, $H - 1, $true); Start-Sleep -Milliseconds 300 }
  $ok = [SW]::MoveWindow($h, $X, $Y, $W, $H, $true)
  Start-Sleep -Milliseconds 800
  [void][SW]::GetWindowRect($h, [ref]$r); $c = New-Object SW+RECT; [void][SW]::GetClientRect($h, [ref]$c)
  "try $try moved=$ok window $($r.L),$($r.T)-$($r.R),$($r.B) ($($r.R-$r.L)x$($r.B-$r.T)) client $($c.R)x$($c.B)"
  if ($r.L -eq $X -and $r.T -eq $Y -and ($r.R - $r.L) -eq $W -and ($r.B - $r.T) -eq $H) { break }
}
