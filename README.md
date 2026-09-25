# LocalSplitScreen

Two-player split-screen for FINAL FANTASY VII REBIRTH, as a UE4SS C++ mod.

- **Why it is built this way:** `../Documentation/SPLITSCREEN-REBUILD-PHILOSOPHY.md`
- **How it works, module by module:** `../Documentation/SPLITSCREEN-ARCHITECTURE.md`

## What it does

Press **Ctrl+F7** to add player 2 and **Ctrl+F8** to remove player 2. When a second local player
exists, the engine splits the screen by itself. This mod makes the split correct: each player's
view is rendered by the engine as its own view family, so the game's meshlet renderer never has
two views in one pass. With one player, the mod changes nothing.

While player 2 is in, the HUD is laid out in each half as it is in single player. This needs no
key; it follows the join and leave, whatever the switches below are set to.

| Keys | Switch |
|------|--------|
| Ctrl+F6 | Player 2's own camera, or player 1's |
| Ctrl+F5 | Top/bottom or side by side |
| Ctrl+F4 | Halves filled, or 16:9 halves with bars |
| Ctrl+F9 | Player 2's pad sticks on their member |

`tools/control.py -c "hud-band off"` turns the HUD layout off for diagnosis (the values are then
logged only); `hud-band on` turns it back on.

## How it is organised

```
src/
  mod.cpp        the UE4SS entry point: verify, install, wire the modules together
  platform/      the game image, the engine address table, the vtable entry, the log
  players/       player 2 joining and leaving, through UE's own local-player path
  renderer/      one view family per player
tests/           host-side tests of the logic that needs neither the game nor UE4SS
```

Every engine address lives in `src/platform/engine_addresses.h`, with the bytes the mod checks at
startup and the research document that established it. If any check fails, the mod installs
nothing and says why in `UE4SS.log`.

## Building

This is its own CMake root. It builds RE-UE4SS from the existing checkout at
`../_tools/UE4SS_CppMods/RE-UE4SS` (override with `-DUE4SS_ROOT=<path>`).

```
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Game__Shipping__Win64 --target LocalSplitScreen LocalSplitScreenTests
ctest --test-dir build -C Game__Shipping__Win64 --output-on-failure
```

The first build compiles UE4SS and takes a while; later builds only compile this mod.

## Installing

Copy `build/Game__Shipping__Win64/LocalSplitScreen.dll` to
`<game>/End/Binaries/Win64/ue4ss/Mods/LocalSplitScreen/dlls/main.dll`, and in `Mods/mods.txt`
enable `LocalSplitScreen : 1`. Enable only one split-screen mod at a time. Close the game first:
it holds the DLL open while running.

Do not deploy the `UE4SS.dll` this build produces; the game keeps its installed UE4SS.

## Not to be confused with

`../SplitScreen/` is RebirthLocalCoop, a separate project kept as evidence only. None of its code
is used here.
