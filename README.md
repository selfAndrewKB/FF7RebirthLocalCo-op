# LocalSplitScreen

Split-screen for up to three players for FINAL FANTASY VII REBIRTH, as a UE4SS C++ mod.

- **Plan and state:** `../Documentation/plan/README.md` (the front page; one file per topic in `plan/topics/`)
- **Why it is built this way:** `../Documentation/SPLITSCREEN-REBUILD-PHILOSOPHY.md`; the rules in
  `../Documentation/plan/rules.md`
- **How it works, module by module:** `../Documentation/SPLITSCREEN-ARCHITECTURE.md`; the systems reversed in
  `../Documentation/research/RE-*.md`

## What it does

**Joining with a controller:** press **Start** on a controller that plays no one to join on it (player 2, then player
3). Hold **View** (Back) for a second to open your own window in your view: **LB**/**RB** move your view one screen left
or right (the order is saved), **A** closes it, holding **B** for a second leaves the game (player 1 stays). On the title
screen, the controller that presses a button plays player 1. A controller that plays no one never pauses the game.
Change characters through the game's own Party menu. Players 2 and 3: click **L3** twice to be brought back beside
player 1 in the field (the game's own catch-up warp; not in a menu, a scene, a battle, or when already beside).

**Co-op settings in the game:** the pause menu has a **Co-op** row above System. It opens the game's own Options screen
with the mod's settings (monitors, screen order, layouts); a change applies at once and is saved to
`LocalSplitScreen.ini`.

Press **Ctrl+F7** to add a player from the keyboard (player 2, then player 3, on pads 2 and 3) and **Ctrl+F8** to remove
the last one in. When another local player exists, the engine splits the screen by itself (two halves; with three
players one large view and two, the layout a setting). This mod makes the split correct: each player's
view is rendered by the engine as its own view family, so the game's meshlet renderer never has
two views in one pass. With one player, the mod changes nothing except the D3D12 residency manager
(see Performance).

Players 2 and 3 each start on their own camera and their own pad (sticks and movement) and play their slot of the
party order (player 2 the 2nd, player 3 the 3rd), and the HUD is laid out in each view as it is in single player, with
each player's view showing their own HUD. None of this needs a key.

| Keys | Switch |
|------|--------|
| Ctrl+F5 | Top/bottom or side by side |
| Ctrl+F4 | Halves filled, or 16:9 halves with bars |
| Ctrl+F6 | For testing: players 2 and 3's views show player 1's camera |
| Ctrl+F9 | For testing: every pad drives every player's sticks (the game's merge) |
| Ctrl+F10 | For comparison: the NPC population follows the leader alone instead of every player |
| Ctrl+F11 | For comparison: characters near player 2 or 3 take their detail from player 1's camera alone |
| Ctrl+F3 | The three-player monitor mode on and off: the window across the row of monitors, player 1's picture in every column no player has |
| Ctrl+F2 | The game window across its row of monitors, and back |

**Several monitors:** one full view per player, each on its own monitor.

1. With the game closed, run **Multi Monitor Setup** (`MultiMonitorSetup.exe` in the mod's folder,
   `Mods/LocalSplitScreen`).
2. Click two or three monitors in the order you want them, left to right. Each is marked with the player it will show.
3. Set **Multiple Monitors** to On and press **Save**.
4. Start the game as always, in windowed fullscreen (borderless) or windowed mode. The app need not stay open.

The monitors need not stand side by side in Windows, and they may differ in size and scaling: each view fills its own
monitor edge to edge, a wider monitor showing more to the sides at the same scale and a shorter one its own height.
While the game runs they are moved into a row, and they go back when it ends, even after a crash. Windows' own arrangement is never changed, so
a restart of the PC always returns your layout. If a game ever ends without putting them back, the app shows a
**Restore Monitors** button. A monitor you picked that is not connected is waited for, not replaced: with two of three
connected the game uses those two, with fewer it stays in one window. A virtual monitor made by a streaming program
(Apollo, Vibepollo) works like any other and is recognised by its name, so give each device you stream to a name of its
own. With no monitors picked, the game takes the longest row of monitors of one height it finds. Leave
`MultiMonitorSetup.exe` where it is and under that name: the mod starts it in the background to put your monitors back
if the game dies.

**Settings file:** the mod writes `Mods/LocalSplitScreen/LocalSplitScreen.ini` at every start from its own list of
options, each with its words, its default and a line of help, and reads it once at the next start: change a value after
an `=` to make it the default (`multi-monitor = on` holds the three-player monitor mode from the first steady frame;
`two-player-split = vertical` puts two players side by side from the first join; `split-fov.horizontal` sets the filled
halves' FOV weight). The keys above change an option for the session only. The repository ships no copy: the file is
the code's own.

With the development tools built in, control commands switch the rest for diagnosis, e.g. `hud-band off`,
`hud-image off`, `hud-levers off`, `leak-fixes 0` (`help` lists them all, each with its reading; every option of the
settings file is a command of the same name): typed in the console (`python tools/remote.py shell`, 127.0.0.1:4419)
or sent with `-c` or `run <file>` (`src/control/script.h` has the language).

## Performance

Split screen renders the scene twice, so the mod trims engine work that a second view makes redundant. Each change
was measured live and leaves the picture unchanged (`../Documentation/PLAN-P2-NATIVE-PLAY.md`, performance section).
A file in the mod's folder keeps the game's behaviour:

| Change | When | Keep the game's way |
|--------|------|---------------------|
| D3D12 residency manager off (one GPU wait per submission less) | whole session | `residency_on.txt` |
| GPU frame-time gap recorder off (timing method 1 for dynamic resolution) | player 2 or 3 in | — |
| Parallel passes without their own GPU submission | player 2 or 3 in | `submit_hints_on.txt` |
| Players 2 and 3's duplicate scenery-streaming work skipped | player 2 or 3 in | `stream-feedback each` (control) |
| D3D12 async compute (measured slower; off) | opt-in | `async_compute_on.txt` turns it on |

`watch_only.txt` runs the game unmodified for comparisons. `perf_log.txt` turns the perf log on from launch, and
`census.txt` (or `census on`) adds a count of every GPU submission per frame to it.

## How it is organised

```
src/
  mod.cpp        the UE4SS entry point: verify, install, wire the modules together
  modules.cpp    the module table (installed in order, removed in reverse), the two tick phases, the shared actions
  platform/      the game image, the fact checks, the redirects, the log, the dev switch
  facts/         the engine facts two or more modules share, one declaration per address
  players/       the engine's own paths: local players, reflection calls, console and settings access, a widget's
                 opacity; the held setting, the split layout and the single view over them; each player's member
                 generation, the log descriptions and the field reads every module shares
  session/       players 2 and 3's join, party member and leave
  scopes/        one game call made as player 2 or 3
  camera/        each player's cameras
  input/         pads, sticks and movement
  feedback/      rumble and sound
  battle/        players 2 and 3's battle: menus, targets, the enemies' coin, synergies
  traversal/     climbing, vaulting and the rest of traversal
  world/         the world around each player: crowds, triggers, pickups, shops
  rides/         chocobos, the wheelie and the shared rides
  views/         the view points, the whole-screen states, the streaming views, the columns and mirror views, the window span
  hud/           the HUD in each view, by system: passes/ (players 2 and 3's HUD images), layout/ (the band),
                 minimap/, field/ (prompt rows, markers, pins), battle/ (targets, gauges, popups); the roots, the
                 live widgets, the pure rules and the dev dumps at the root
  renderer/      one view family per player, and the renderer's performance levers
  control/       the console: the command language, the runner and the socket (built with the dev tools)
  dev/           the dev tools: the console's install, the watches' install and ticks
tests/           host-side tests of the logic that needs neither the game nor UE4SS
tools/           the control scripts, the regression harness, the version match
```

`src/` is searched before UE4SS's include folders, and Windows ignores case, so a mod header named like a UE4SS one
under a folder of the same name (`src/input/Handler.hpp` against UE4SS's `<Input/Handler.hpp>`) would replace it;
and a file in `src/hud/` that includes `"battle/x.h"` finds `src/hud/battle/x.h` before `src/battle/x.h`. Keep the
mod's headers `.h` and their names distinct from those.

Engine facts (addresses, vtable slots, field offsets) are written as IDA shows them for the research
build, each with the bytes that prove it and the research document that established it. The ones two
or more modules share live in `src/facts/`, one declaration per address, grouped by engine object
(`facts/engine.h` is the stock engine's); a fact one module reads stays in that module. The startup
facts (`facts::kStartupFacts`) are checked before anything is installed: if any fails, the mod installs
nothing and says why in `UE4SS.log`. Every other fact is checked where its feature installs
(`platform/verified_code.h`), and that feature alone stands aside.

## Game versions

One DLL serves every supported game version (1.004 and 1.005). At startup `GameImage` reads the
exe's PE header and logs the version it found.
- The source keeps the research build's (1.004) addresses. For other builds,
  `platform/build_translation` maps them through `platform/builds/build_<ver>.h` (generated by
  `tools/version_match/`) and `build_<ver>_replaced.h` (found by hand in IDA).
- Field offsets that moved are `PerBuild` constants (`platform/build_value.h`).
- An unknown build gets a warning. Its checks then fail, and those features stand aside.

Adding a new build is described step by step in `tools/version_match/README.md`.

## Building

This is its own CMake root. It builds RE-UE4SS from the checkout at
`../_tools/UE4SS_CppMods/RE-UE4SS-c0a8d4c` (override with `-DUE4SS_ROOT=<path>`): the source of the UE4SS build
players get from Nexus Mods (Git SHA `c0a8d4c`), so the mod loads under that loader and players install nothing
else. `tools/ue4ss_nexus/README.md` makes that checkout and names what it holds; `tools/ue4ss_nexus/check_exports.py`
checks a built DLL against the Nexus loader's exports. If the configure cannot pick a Rust toolchain for corrosion,
add `-DRust_TOOLCHAIN=stable-x86_64-pc-windows-msvc`.

```
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Game__Shipping__Win64
cmake --build build --config Game__Shipping__Win64 --target lss_tests
ctest --test-dir build -C Game__Shipping__Win64 --output-on-failure
```

The first build compiles UE4SS and takes a while; later builds only compile this mod. Each test is
its own executable (`tests/<name>_test.cpp`, one `lss_test()` line in `CMakeLists.txt`); the
`lss_tests` target builds them all, since ctest runs only what is built. `tools/build_locked.ps1
-Tests` does both under the build lock. One test is a script: `tests/facts_once.py` walks `src/`
and fails if an engine address is declared in two files, or a module checks a function's bytes
itself instead of declaring a `FunctionFact` and finding it (the facts rule).

The development tools are built in by default: control scripts, the HUD trace, the position probe
and the dump commands (`src/dev/dev_tools.h`). A player's build leaves them out:

```
cmake -S . -B build-release -G "Visual Studio 17 2022" -DLSS_DEV_TOOLS=OFF
cmake --build build-release --config Game__Shipping__Win64 --target LocalSplitScreen
```

## Installing

Copy `build/Game__Shipping__Win64/LocalSplitScreen.dll` to
`<game>/End/Binaries/Win64/ue4ss/Mods/LocalSplitScreen/dlls/main.dll`, and in `Mods/mods.txt`
enable `LocalSplitScreen : 1`. Enable only one split-screen mod at a time. Close the game first:
it holds the DLL open while running. Copy `build/Game__Shipping__Win64/MultiMonitorSetup.exe` (the setup app, built
with the mod; also the guard that puts moved monitors back) to `Mods/LocalSplitScreen/`, beside the settings file.
`tools/relaunch.ps1 -Deploy -Label <name>` does both and sets an open copy of the app aside.

The monitor row, the record of moved monitors and the setup app are shared with the FF7 Remake mod: `tools/remake_drift.py`
lists the files kept identical and the adapted ones, and says which have drifted from Remake's.

Do not deploy the `UE4SS.dll` this build produces; the game keeps its installed UE4SS, which is the Nexus Mods
"UE4SS-Rebirth_FFVII_ReMenu (1.005 Compatible)" upload with its own `UE4SS_Signatures` (`tools/ue4ss_nexus/README.md`:
the loader refuses the signature scripts written for newer builds).

## Not to be confused with

`../SplitScreen/` is RebirthLocalCoop, a separate project kept as evidence only. None of its code
is used here.
