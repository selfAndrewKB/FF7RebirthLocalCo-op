# LocalSplitScreen

Local split-screen co-op for up to three players in FINAL FANTASY VII REBIRTH, as a UE4SS C++ mod.

## Requirements

- FINAL FANTASY VII REBIRTH on Steam, version 1.005 or 1.004.
- UE4SS from Nexus Mods: "UE4SS-Rebirth_FFVII_ReMenu (1.005 Compatible)". The mod is built against that loader's
  source and needs no other.
- To build: Visual Studio 2022 with the C++ desktop workload, CMake 3.18 or later, a Rust toolchain (UE4SS builds part
  of itself with it), Python 3 and PowerShell 7.

## Building

The build compiles UE4SS from the source of the Nexus loader, which needs a prepared checkout. By default it is
expected at `../_tools/UE4SS_CppMods/RE-UE4SS-c0a8d4c`; `tools/ue4ss_nexus/README.md` says how to make it. Point
`-DUE4SS_ROOT=<path>` at it if it lives elsewhere.

```
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Game__Shipping__Win64
cmake --build build --config Game__Shipping__Win64 --target lss_tests
ctest --test-dir build -C Game__Shipping__Win64 --output-on-failure
```

This gives `LocalSplitScreen.dll` and `MultiMonitorSetup.exe` in `build/Game__Shipping__Win64/`. The first build
compiles UE4SS and takes a while; later ones only rebuild the mod. If CMake cannot pick a Rust toolchain, add
`-DRust_TOOLCHAIN=stable-x86_64-pc-windows-msvc` to the first line.

`tools/build_locked.ps1 -Tests` builds and runs the tests in one step, and makes parallel builds wait their turn.

The default build is the development build. It adds a console on `127.0.0.1:4419` (`python tools/remote.py shell`;
`help` lists the commands) and extra logging. The players' build leaves those out:

```
cmake -S . -B build-release -G "Visual Studio 17 2022" -DLSS_DEV_TOOLS=OFF
python package.py
```

`package.py` runs the tests, builds the players' build and zips the mod folder into `dist/`.

## Installing

1. Copy `LocalSplitScreen.dll` to `<game>\End\Binaries\Win64\ue4ss\Mods\LocalSplitScreen\dlls\main.dll`.
2. Copy `MultiMonitorSetup.exe` into `Mods\LocalSplitScreen\`.
3. Add `LocalSplitScreen : 1` to `Mods\mods.txt`, or put an empty `enabled.txt` in the mod's folder.

`tools/relaunch.ps1 -Deploy -Label <name>` copies both files and starts the game. The build also makes a `UE4SS.dll`,
which is only there to link the mod against; the game keeps the Nexus one.

The mod writes its settings to `LocalSplitScreen.ini` in its folder.

## Game versions

One DLL serves 1.004 and 1.005. The source holds 1.004's addresses; for 1.005 they are translated through
`src/platform/builds/`, and offsets that moved between versions are per-build values. Every address is checked against
the game's code at start-up. If a check fails, that feature stays off and the log says why. On an unknown version
everything that does not match stays off. `tools/version_match/README.md` covers adding a version.

## Project map

```
src/
  engine_tick       the mod's call once per frame
  mod.cpp           the entry point
  modules           every module, in install order
  platform/         game versions and address checks, game-code patches, safe memory access, the log, settings
  facts/            addresses and offsets that several modules share
  players/          shared engine access: local players, calls into the engine, the split layout
  session/          joining and leaving, and the party member each player plays
  scopes/           running one game call as player 2 or 3
  camera/           each player's camera
  input/            controllers, sticks and movement
  feedback/         rumble and sound
  battle/           command menus, targeting, enemy attention, synergies, battle options
  traversal/        climbing, vaulting and the rest of traversal
  world/            crowds, triggers, pickups, shops, levers, catch-up warps
  rides/            chocobos and other rides
  views/            screen layouts, full-screen moments, multiple monitors
  hud/              each player's HUD
  renderer/         one render pass per player, and frame-rate work
  menu/             the Co-op options page
  control/          the development console
  dev/              development tools and watches
  ue4ss_compat/     header shims for the UE4SS version the mod builds against
setup/              the Multi Monitor Setup app
tests/              host tests, run without the game
tools/              build lock, deploy, console client, regression runner, version matching
```

`tools/layout_doc.py` writes this map, and a test fails when it is out of date. Each module's header comment says what
it does. Keep the mod's headers `.h` and unlike UE4SS's names: `src/` is searched first, so a same-named header would
replace UE4SS's.
