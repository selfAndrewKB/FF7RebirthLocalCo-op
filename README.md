# LocalSplitScreen

Local split-screen co-op for up to three players in FINAL FANTASY VII REBIRTH, as a UE4SS C++ mod.

## Requirements

- FINAL FANTASY VII REBIRTH on Steam, version 1.005. Version 1.004 likely still works for the most part but has not been tested as thoroughly and is deprecated: its support will likely
  be removed in the future.
- [UE4SS from Nexus Mods](https://www.nexusmods.com/finalfantasy7rebirth/mods/267?tab=files): "UE4SS-Rebirth_FFVII_ReMenu (1.005 Compatible)". The mod is built against that loader's
  source for the time being.
- To build: Visual Studio 2022 with the C++ desktop workload, CMake 3.18 or later, a Rust toolchain (UE4SS builds part
  of itself with it), Python 3 and PowerShell 7.

## Installing

1. Unpack UE4SS (acquired from the [Nexus Mods page](https://www.nexusmods.com/finalfantasy7rebirth/mods/267?tab=files)) into `FINAL FANTASY VII REBIRTH\End\Binaries\Win64\`: placing the `dwmapi.dll` file and the `ue4ss` folder beside `ff7rebirth_.exe`
2. Unpack the mod's zip file into the same folder, replacing the `UE4SS-settings.ini` and `mods.txt` included in the UE4SS package. This ensures `GuiConsoleEnabled = 0` is set and that UE4SS's generic SplitScreen mod is disabled. Otherwise they are prone to conflicting and crashing Rebirth upon startup.
3. Optional, to play across multiple monitors: put `MultiMonitorSetup.exe` in `ue4ss\Mods\LocalSplitScreen\`.

The mod writes its settings to `LocalSplitScreen.ini` in that folder.

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

`tools/relaunch.ps1 -Deploy -Label <name>` installs a build: `LocalSplitScreen.dll` as
`ue4ss\Mods\LocalSplitScreen\dlls\main.dll` and `MultiMonitorSetup.exe` in that folder. It quits a running game first,
through the mod, and starts the game again. `-GameDir` names the game's `Win64` folder, `-Build` another DLL than the
development build's.

The default build is the development build. It adds a console on `127.0.0.1:4419` (`python tools/remote.py shell`;
`help` lists the commands) and extra logging. The players' build leaves those out:

```
cmake -S . -B build-release -G "Visual Studio 17 2022" -DLSS_DEV_TOOLS=OFF
python package.py
```

`package.py` runs the tests on the development build (so `build` must be configured too), builds the players' build
and zips the mod into `dist/`; `--nexus` leaves the setup app out.

## Game versions

The source is written for 1.005: every address in it is a 1.005 address, as IDA shows it. One DLL still serves 1.004:
its addresses are translated through the tables in `src/platform/builds/`, and offsets that moved between versions are
per-build values. Every address is checked against the game's code at start-up. If a check fails, that feature stays
off and the log says why. On an unknown version everything that does not match stays off. `tools/version_match/README.md`
covers adding a version.

The mod tells the versions apart by the PE header of `ff7rebirth_.exe` (`src/platform/build_translation.cpp`). The
Steam builds:

| Version | `ff7rebirth_.exe` | SHA-256 |
|---|---|---|
| 1.005 | 124,317,952 bytes | `752807180C4ED919667FF0EC163046410187E873888DD75248E78AAA72E363DC` |
| 1.004, deprecated | 126,263,624 bytes | `76BD4D539878A8759433F1CD7C7B47D5E3ABDA0EF41572A664D2D606A9F09DC8` |

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
tools/              build lock, deploy, console client, regression runner, version matching, releases
```
`tools/layout_doc.py` generates this map from the folders on disk and the descriptions written in the script. A host test fails when the map is out of date. After adding or moving a folder, add its description to the script and run `python tools/layout_doc.py`.

For what a module does, read the comment at the top of its header.

Name the mod’s headers `.h` (UE4SS’s use `.hpp`), and don’t reuse a UE4SS header’s name. The build looks in `src/` before UE4SS’s folders, so a mod header with the same name would be used in place of UE4SS’s. The exception is `src/ue4ss_compat/`, which replaces a few UE4SS headers on purpose.
