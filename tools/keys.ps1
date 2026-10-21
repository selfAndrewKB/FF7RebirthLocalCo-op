param(
    [Parameter(Mandatory)] [string[]]$Keys,   # e.g. Enter, Escape, Up, Down, Left, Right, Space, Ctrl+F7
    [int]$HoldMs = 120,
    [int]$GapMs = 600,
    [string]$Title = "FINAL FANTASY VII REBIRTH"
)
# Brings the game window to the front and presses keys by scan code (games reading raw input ignore
# virtual-key-only events). Refuses if the game window cannot be made the foreground window.
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class LssKeys {
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)] struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; [FieldOffset(8)] public long pad0; [FieldOffset(16)] public long pad1; [FieldOffset(24)] public long pad2; [FieldOffset(32)] public long pad3; }
    [DllImport("user32.dll", SetLastError=true)] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    // Windows lets a process take the foreground right after it has sent input; an Alt tap is the
    // least disruptive input to send (it opens no menu in a game window).
    public static bool Focus(IntPtr hwnd) {
        keybd_event(0x12, 0, 0, UIntPtr.Zero);
        keybd_event(0x12, 0, 2, UIntPtr.Zero);
        return SetForegroundWindow(hwnd);
    }
    public static uint Key(ushort scan, bool extended, bool up) {
        var input = new INPUT[1];
        input[0].type = 1;
        input[0].ki.wScan = scan;
        input[0].ki.dwFlags = 0x0008u | (extended ? 0x0001u : 0u) | (up ? 0x0002u : 0u);
        return SendInput(1, input, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@
$scan = @{ Enter = @(0x1C, $false); Escape = @(0x01, $false); Space = @(0x39, $false); Tab = @(0x0F, $false)
           Up = @(0x48, $true); Down = @(0x50, $true); Left = @(0x4B, $true); Right = @(0x4D, $true)
           C = @(0x2E, $false); Q = @(0x10, $false); E = @(0x12, $false)   # C: commands menu; Q/E: its party switch
           G = @(0x22, $false)                                            # G: call the chocobo (field)
           F2 = @(0x3C, $false); F3 = @(0x3D, $false)
           F4 = @(0x3E, $false); F5 = @(0x3F, $false); F6 = @(0x40, $false); F7 = @(0x41, $false); F8 = @(0x42, $false)
           F9 = @(0x43, $false); F10 = @(0x44, $false); F11 = @(0x57, $false) }
$ctrl = @(0x1D, $false)  # left Ctrl; "Ctrl+<key>" holds it around the key (the mod's switches are Ctrl+F2..F11)
# By title, not the process's main window: with the UE4SS console open, Windows can report the
# console as the game process's main window.
$hwnd = [LssKeys]::FindWindow([NullString]::Value, $Title)  # $null would reach it as "" (a class name)
if ($hwnd -eq [IntPtr]::Zero) { throw "no window titled '$Title'" }
[void][LssKeys]::ShowWindow($hwnd, 9)
[void][LssKeys]::Focus($hwnd)
Start-Sleep -Milliseconds 300
if ([LssKeys]::GetForegroundWindow() -ne $hwnd) { throw "the game window is not in the foreground; no key sent" }
foreach ($k in $Keys) {
    $withCtrl = $k.StartsWith('Ctrl+')
    $name = if ($withCtrl) { $k.Substring(5) } else { $k }
    if (-not $scan.ContainsKey($name)) { throw "unknown key $k" }
    $code, $ext = $scan[$name]
    if ($withCtrl) {
        if ([LssKeys]::Key($ctrl[0], $ctrl[1], $false) -ne 1) { throw "SendInput failed for Ctrl" }
        Start-Sleep -Milliseconds 50
    }
    if ([LssKeys]::Key($code, $ext, $false) -ne 1) {
        if ($withCtrl) { [void][LssKeys]::Key($ctrl[0], $ctrl[1], $true) }
        throw "SendInput failed for $k"
    }
    Start-Sleep -Milliseconds $HoldMs
    [void][LssKeys]::Key($code, $ext, $true)
    if ($withCtrl) {
        Start-Sleep -Milliseconds 50
        [void][LssKeys]::Key($ctrl[0], $ctrl[1], $true)
    }
    "pressed $k"
    Start-Sleep -Milliseconds $GapMs
}
