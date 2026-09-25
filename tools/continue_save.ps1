param(
    [int]$StartWaitS = 12,   # after relaunch.ps1's ready line: the title takes no input at first
    [int]$TitleWaitS = 8,    # after "press any button": the title menu fades in
    [int]$DialogWaitS = 7,   # after Continue: the "Resume playing?" dialog slides in
    [int]$LoadWaitS = 35     # after Yes: the field loads
)
# From the title screen right after tools/relaunch.ps1: press any button -> Continue (the default
# entry) -> Yes. Presses sent before a screen accepts input are dropped, and a stray press then
# lands on the next screen (Escape on the title menu opens "Quit the game?"), so the waits are
# generous. Check the result with tools/shot.ps1 before joining.
$ErrorActionPreference = "Stop"
$keys = Join-Path $PSScriptRoot "keys.ps1"
Start-Sleep -Seconds $StartWaitS
& $keys -Keys Enter | Out-Null
Start-Sleep -Seconds $TitleWaitS
& $keys -Keys Enter | Out-Null
Start-Sleep -Seconds $DialogWaitS
& $keys -Keys Enter | Out-Null
Start-Sleep -Seconds $LoadWaitS
"continue_save: pressed through title, Continue and Yes; waited ${LoadWaitS}s for the load"
