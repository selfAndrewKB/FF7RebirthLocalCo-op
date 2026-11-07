<#
.SYNOPSIS
    Quits the game through the mod, optionally deploys a new build, and launches the game again.

.DESCRIPTION
    1. If the game is running: player 2 leaves (if in), then the mod's `quit` command, and the game
       is waited on until it exits by itself. It is never killed.
    2. With -Deploy: the installed main.dll is kept as main.dll.bak-<Label> and the build is copied
       in; both hashes are printed and must match. The setup app built beside the DLL goes next to the
       settings file (the mod starts it as the guard that puts moved monitors back); an open copy is set
       aside by renaming, since Windows will not overwrite a running exe.
    3. The last session's UE4SS.log is kept under ue4ss\logs (the newest ten), since each launch
       starts a new one.
    4. The game is launched through Steam (or, with -Direct, its exe is started directly: the 1.004
       copy must never go through Steam, which starts the live game), and this returns once the mod
       has logged its ready line for this launch (or reports that it did not). With -NoLaunch it is
       left closed. -GameDir names another game folder (the 1.004 copy's End\Binaries\Win64).
    Title and load menus are left to keys.ps1 and shot.ps1, so each one is checked on screen first.

.EXAMPLE
    .\tools\relaunch.ps1 -Deploy -Label v0.6.3
#>
param(
    [switch]$Deploy,
    [switch]$NoLaunch,  # quit, deploy and keep the log, but leave the game closed
    [string]$Label = '',
    [string]$Build = "$PSScriptRoot\..\build\Game__Shipping__Win64\LocalSplitScreen.dll",
    [int]$ReadyTimeoutSeconds = 150,
    [string]$GameDir = 'G:\_SSD Games\SteamLibrary\steamapps\common\FINAL FANTASY VII REBIRTH\End\Binaries\Win64',
    [switch]$Direct  # start ff7rebirth_.exe from $GameDir instead of Steam's -applaunch (the 1.004 copy)
)

$ErrorActionPreference = 'Stop'
$gameDir = $GameDir
$modDir  = "$gameDir\ue4ss\Mods\LocalSplitScreen"
$log     = "$gameDir\ue4ss\UE4SS.log"
# The mod's console (a dev build) on 127.0.0.1:4419: one per machine, so one game at a time (the 1.004 copy and the
# live game would share the port; the second's console never opens).
$remote  = "$PSScriptRoot\remote.py"

$game = Get-Process ff7rebirth_ -ErrorAction SilentlyContinue
if ($game) {
    $status = python $remote --timeout 20 -c 'get p2.in'
    if ($LASTEXITCODE -eq 2) {
        # No console answers (a release build): closed by its window, as a player's close. Never a kill.
        'no console (a release build?): closing the game by its window'
        [void]$game.CloseMainWindow()
    } else {
        if ($status -match 'value p2\.in off') {
            'player 2 is out'
        } else {
            python $remote --timeout 60 -c 'leave' -c 'wait-until p2.in == off 900' | Select-Object -Last 1
        }
        python $remote --timeout 30 -c 'quit' | Select-Object -Last 1
    }
    if (-not $game.WaitForExit(120000)) { throw 'the game did not exit within 2 minutes; not deploying' }
    'game exited'
}

if ($Deploy) {
    if (-not $Label) { throw '-Deploy needs -Label for the backup name' }
    if (Get-Process ff7rebirth_ -ErrorAction SilentlyContinue) { throw 'the game is still running' }
    $dll = "$modDir\dlls\main.dll"
    Copy-Item $dll "$dll.bak-$Label"
    Copy-Item $Build $dll
    $installed = (Get-FileHash $dll).Hash
    $built     = (Get-FileHash $Build).Hash
    "installed $installed (backup main.dll.bak-$Label)"
    if ($installed -ne $built) { throw "the installed DLL does not match the build ($built)" }
    # The setup app, named by the mod's own line (the build names the exe from the same one).
    $nameLine = Select-String -Path "$PSScriptRoot\..\src\views\display_move.h" -Pattern 'kSetupExe = L"([^"]+)"'
    $setup    = if ($nameLine) { Join-Path (Split-Path $Build -Parent) $nameLine.Matches[0].Groups[1].Value } else { '' }
    if ($setup -and (Test-Path $setup)) {
        $target = Join-Path $modDir (Split-Path $setup -Leaf)
        foreach ($stale in Get-ChildItem $modDir -Filter "$(Split-Path $setup -Leaf).in-use-*" -ErrorAction SilentlyContinue) {
            try { [IO.File]::Delete($stale.FullName) } catch { }  # an earlier deploy's set-aside copy, once that app has closed
        }
        try {
            Copy-Item $setup $target -ErrorAction Stop
            "installed $(Split-Path $setup -Leaf)"
        } catch {
            $aside = "$target.in-use-$Label"
            [IO.File]::Move($target, $aside)
            Copy-Item $setup $target
            "installed $(Split-Path $setup -Leaf) (the app was open: its old file is set aside as $(Split-Path $aside -Leaf))"
        }
    } else {
        'the setup app is not built: the mod has no guard to put moved monitors back after a crash'
    }
}

# UE4SS starts a new log on every launch: the last session's is kept first (the newest ten).
if (Test-Path $log) {
    $kept = "$gameDir\ue4ss\logs"
    New-Item -ItemType Directory -Force $kept | Out-Null
    $stamp = (Get-Item $log).LastWriteTime.ToString('yyyyMMdd-HHmmss')
    Move-Item $log "$kept\UE4SS-$stamp.log" -Force  # moved: the ready check below never reads the old session
    Get-ChildItem $kept -Filter 'UE4SS-*.log' | Sort-Object Name -Descending | Select-Object -Skip 10 | Remove-Item
    "previous log kept as logs\UE4SS-$stamp.log"
}

if ($NoLaunch) { 'not launched (-NoLaunch)'; return }

$start = Get-Date
if ($Direct) {
    Start-Process "$gameDir\ff7rebirth_.exe" -WorkingDirectory $gameDir
} else {
    Start-Process 'c:/program files (x86)/steam/steam.exe' -ArgumentList '-applaunch', '2909400'
}
for ($i = 0; $i -lt $ReadyTimeoutSeconds; $i++) {
    Start-Sleep 1
    if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -gt $start -and
        (Select-String -Path $log -Pattern 'ready: every engine check passed' -SimpleMatch -Quiet)) {
        Select-String -Path $log -Pattern 'LocalSplitScreen\] (v\d|check failed|standing aside|HUD layout)' |
            ForEach-Object { $_.Line.Substring([Math]::Min(29, $_.Line.Length)) }
        return
    }
}
"no ready line within $ReadyTimeoutSeconds s; last lines:"
Get-Content $log -Tail 5
