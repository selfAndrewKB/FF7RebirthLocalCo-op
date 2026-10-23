# LocalSplitScreen

Two-player split-screen for FINAL FANTASY VII REBIRTH, as a UE4SS C++ mod.

- **Plan and state:** `../Documentation/plan/README.md` (the front page; one file per topic in `plan/topics/`)
- **Why it is built this way:** `../Documentation/SPLITSCREEN-REBUILD-PHILOSOPHY.md`; the rules in
  `../Documentation/plan/rules.md`
- **How it works, module by module:** `../Documentation/SPLITSCREEN-ARCHITECTURE.md`; the systems reversed in
  `../Documentation/research/RE-*.md`

## What it does

Press **Ctrl+F7** to add player 2 and **Ctrl+F8** to remove player 2. When a second local player
exists, the engine splits the screen by itself. This mod makes the split correct: each player's
view is rendered by the engine as its own view family, so the game's meshlet renderer never has
two views in one pass. With one player, the mod changes nothing except the D3D12 residency manager
(see Performance).

Player 2 starts on their own camera and their own pad (sticks and movement), and the HUD is laid out
in each half as it is in single player, with player 2's half showing player 2's HUD. None of this
needs a key.

| Keys | Switch |
|------|--------|
| Ctrl+F5 | Top/bottom or side by side |
| Ctrl+F4 | Halves filled, or 16:9 halves with bars |
| Ctrl+F6 | For testing: player 2's half shows player 1's camera |
| Ctrl+F9 | For testing: every pad drives both players' sticks (the game's merge) |
| Ctrl+F10 | For comparison: the NPC population follows the leader alone instead of both players |
| Ctrl+F11 | For comparison: characters near player 2 take their detail from player 1's camera alone |
| Ctrl+F3 | A third full-size view in and out (the multi-monitor measurement) |
| Ctrl+F2 | The game window across its row of monitors, and back |

With the development tools built in, control commands switch the rest for diagnosis, e.g. `hud-band off`,
`hud-image off`, `hud-levers off`, `leak-fixes 0` (`help` lists them all, each with its reading): typed in the console
(`python tools/remote.py shell`, 127.0.0.1:4419) or sent with `-c` or `run <file>` (`src/control/script.h` has the
language).

## Performance

Split screen renders the scene twice, so the mod trims engine work that a second view makes redundant. Each change
was measured live and leaves the picture unchanged (`../Documentation/PLAN-P2-NATIVE-PLAY.md`, performance section).
A file in the mod's folder keeps the game's behaviour:

| Change | When | Keep the game's way |
|--------|------|---------------------|
| D3D12 residency manager off (one GPU wait per submission less) | whole session | `residency_on.txt` |
| GPU frame-time gap recorder off (timing method 1 for dynamic resolution) | player 2 in | — |
| Parallel passes without their own GPU submission | player 2 in | `submit_hints_on.txt` |
| Player 2's duplicate scenery-streaming work skipped | player 2 in | `stream-feedback each` (control) |
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
                 opacity; the held setting, the split layout and the single view over them
  player_two/    player 2's play, by game system: session/ (the join, the party member, the leave), scopes/ (one game
                 call made as player 2), camera/, input/ (pads, sticks, movement), feedback/ (rumble and sound),
                 battle/, traversal/, world/, rides/; the helpers every wrapper shares at the root
  views/         the view points, the whole-screen states, the streaming views, the third view, the window span
  hud/           the HUD in each half, by system: passes/ (player 2's HUD image), layout/ (the band), minimap/, field/
                 (prompt rows, markers, pins), battle/ (targets, gauges, popups); the roots, the live widgets, the
                 pure rules and the dev dumps at the root
  renderer/      one view family per player, and the renderer's performance levers
  control/       the console: the command language, the runner and the socket (built with the dev tools)
  dev/           the dev tools: the console's install, the watches' install and ticks
tests/           host-side tests of the logic that needs neither the game nor UE4SS
tools/           the control scripts, the regression harness, the version match
```

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

This is its own CMake root. It builds RE-UE4SS from the existing checkout at
`../_tools/UE4SS_CppMods/RE-UE4SS` (override with `-DUE4SS_ROOT=<path>`).

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
it holds the DLL open while running.

Do not deploy the `UE4SS.dll` this build produces; the game keeps its installed UE4SS.

## Not to be confused with

`../SplitScreen/` is RebirthLocalCoop, a separate project kept as evidence only. None of its code
is used here.
