<#
.SYNOPSIS
    Builds the mod (and optionally runs the host tests) while holding a named mutex, so several
    sessions or agents can share build/ one at a time.

.DESCRIPTION
    Two MSBuild runs in one build tree collide on the same intermediate files and PDBs. Every build
    goes through this script instead; a second caller waits for the first to finish.

.EXAMPLE
    .\tools\build_locked.ps1 -Tests
#>
param(
    [switch]$Tests,
    [string]$Target = '',          # e.g. LocalSplitScreen; empty builds everything (the tests too; -Tests builds them anyway)
    [int]$WaitMinutes = 30
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build'
$mutex = New-Object System.Threading.Mutex($false, 'LocalSplitScreenBuild')
$waited = [Diagnostics.Stopwatch]::StartNew()
try {
    $owned = $mutex.WaitOne([TimeSpan]::FromMinutes($WaitMinutes))
} catch [System.Threading.AbandonedMutexException] {
    $owned = $true  # the previous holder exited without releasing: the build tree is ours now
}
if (-not $owned) { throw "the build lock was not free within $WaitMinutes minutes" }
try {
    if ($waited.Elapsed.TotalSeconds -ge 1) { "waited {0:N0} s for the build lock" -f $waited.Elapsed.TotalSeconds }
    $buildArgs = @('--build', $build, '--config', 'Game__Shipping__Win64')
    if ($Target) { $buildArgs += @('--target', $Target) }
    $output = & cmake @buildArgs 2>&1
    $failed = $LASTEXITCODE -ne 0
    # The errors and warnings, and the DLL line; on a failure also the output's last lines, whatever they say.
    $output | ForEach-Object { "$_" } | Select-String -Pattern 'error|warning C|LocalSplitScreen\.vcxproj ->' |
        ForEach-Object { $_.Line }
    if ($failed) { '--- the build output ends:'; $output | ForEach-Object { "$_" } | Select-Object -Last 25 }
    if ($failed) { throw 'the build failed' }
    if ($Tests) {
        # Every test executable first (ctest runs only what is built), then the tests.
        $testOutput = & cmake --build $build --config Game__Shipping__Win64 --target lss_tests 2>&1
        if ($LASTEXITCODE -ne 0) { $testOutput | ForEach-Object { "$_" } | Select-String -Pattern 'error' | ForEach-Object { $_.Line }; throw 'the tests did not build' }
        & ctest --test-dir $build -C Game__Shipping__Win64 --output-on-failure 2>&1 | Select-Object -Last 15
        if ($LASTEXITCODE -ne 0) { throw 'a host test failed' }
    }
} finally {
    $mutex.ReleaseMutex()
    $mutex.Dispose()
}
