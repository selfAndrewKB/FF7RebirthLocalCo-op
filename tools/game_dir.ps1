<#
.SYNOPSIS
    Prints the game's End\Binaries\Win64 folder: FF7REBIRTH_GAME_DIR when set, else the Steam library that holds the
    game (Steam's own folder from the registry key Steam writes, the other libraries from its libraryfolders.vdf).
    With -Steam, prints steam.exe instead. The one place the tools learn where the game is.
#>
param([switch]$Steam)

$ErrorActionPreference = 'Stop'
$steamDir = try { (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath -replace '/', '\' }
            catch { 'C:\Program Files (x86)\Steam' }
if ($Steam) { Join-Path $steamDir 'steam.exe'; return }
if ($env:FF7REBIRTH_GAME_DIR) { $env:FF7REBIRTH_GAME_DIR; return }

$subdir = 'steamapps\common\FINAL FANTASY VII REBIRTH\End\Binaries\Win64'
$libraries = @($steamDir)
$vdf = Join-Path $steamDir 'steamapps\libraryfolders.vdf'
if (Test-Path $vdf) {
    $libraries += [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"') |
        ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' }
}
foreach ($library in $libraries) {
    $dir = Join-Path $library $subdir
    if (Test-Path (Join-Path $dir 'ff7rebirth_.exe')) { $dir; return }
}
throw "the game was not found in any Steam library: set FF7REBIRTH_GAME_DIR to its End\Binaries\Win64 folder"
