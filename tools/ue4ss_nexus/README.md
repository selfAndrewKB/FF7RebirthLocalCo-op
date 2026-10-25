# The UE4SS build the mod is built against

Players get UE4SS from Nexus Mods: "UE4SS-Rebirth_FFVII_ReMenu (1.005 Compatible)" (2026-08-02), whose `UE4SS.log`
says `UE4SS - v3.0.1 Beta #0 - Git SHA #c0a8d4c`. That is upstream `UE4SS-RE/RE-UE4SS` main of 2025-02-06 (the
merge of PR #780), with its `deps/first/Unreal` submodule at `b4a6e7e4` and `deps/first/patternsleuth` at
`349cf44d`. The mod is built from that source so that it loads under that DLL and players install nothing else
(the decision: `Rebirth/Documentation/plan/versions.md`, "UE4SS builds").

That commit built with xmake only. The checkout the mod's CMake expects at `../_tools/UE4SS_CppMods/RE-UE4SS-c0a8d4c`
is a git worktree of the RE-UE4SS clone at `c0a8d4c` with upstream's later CMake build system added back
(`be331dea`, 2025-09-05, "Re-add cmake build system") and the few fixes that commit needs at the older sources.
Every source file is the commit's own; only build files are added. `cmake.patch` holds all of it.

## Making the checkout

From `Rebirth/_tools/UE4SS_CppMods` (the RE-UE4SS clone is owned by another Windows account, hence `safe.directory`):

```powershell
$p = "$PWD\RE-UE4SS"; $wt = "$PWD\RE-UE4SS-c0a8d4c"
git -c safe.directory=$p -C $p worktree add $wt c0a8d4c
git -c safe.directory=$wt -C $wt checkout -b nexus-c0a8d4c-cmake
# The submodules, from the clone's own stores (their remotes are SSH):
foreach ($s in @(@{path='deps/first/Unreal'; sha='b4a6e7e44cfb8c1aafb3ab5d13fec2aeecc035a3'},
                 @{path='deps/first/patternsleuth'; sha='349cf44df7d6f3cbb19322b88767a1558c7d4f6c'})) {
    $store = "$p\.git\modules\$($s.path -replace '/','\')"
    git -c safe.directory=$store clone --quiet --no-checkout $store "$wt\$($s.path -replace '/','\')"
    git -C "$wt\$($s.path -replace '/','\')" checkout --quiet $s.sha
}
$t = "$PWD\..\LocalSplitScreen\tools\ue4ss_nexus"
git -c safe.directory=$wt -C $wt apply --whitespace=nowarn "$t\cmake.patch"
Copy-Item "$t\Unreal-CMakeLists.txt" "$wt\deps\first\Unreal\CMakeLists.txt"
git -C "$wt\deps\first\Unreal" apply --whitespace=nowarn "$t\Unreal-compile.patch"
```

`Unreal-compile.patch` is the one source change: MSVC 14.44 rejects `find<ObjectClassType>(...)` in
`VirtualFunctionHelper.hpp` (a template body; upstream made the same change later). Everything else is build files.

Then the mod builds as `README.md` says (`cmake -S . -B build ...`); the first configure fetches the third-party
sources over https. If the configure cannot pick a Rust toolchain for corrosion, add
`-DRust_TOOLCHAIN=stable-x86_64-pc-windows-msvc`.

## What the patch changes against `be331dea`'s CMake

- `deps/third`: the versions xmake fetched at `c0a8d4c` (fmt 10.2.1, glaze v2.9.5, imgui v1.89, ImGuiTextEdit and
  IconFontCppHeaders as they stood on 2025-02-06, zydis v4.1.0, PolyHook `fd2a88f0`, raw_pdb `8c6a7146`), https
  URLs, a `glad` target (the loader's OpenGL backend uses it), corrosion at the commit the current upstream CMake uses
  (build glue only).
- `deps/first`: `String` (header-only) and the `fmt`/`String`/`Helpers` links xmake declared (`File`, `Helpers`,
  `DynamicOutput`, `LuaMadeSimple`, `SinglePassSigScanner`, `Unreal`, `UE4SS`); `patternsleuth_bind` as a
  `staticlib` crate (the newer manifest's line; corrosion imports only those).
- `UE4SS`: its sources by glob, as xmake built them (`src/**.cpp`); `UVTD` left out.
- The configuration list set before the dependencies (corrosion builds its copy step from it).
- `Unreal-CMakeLists.txt`: the submodule's own CMake file from its later commit `f55ddc7` (sources unchanged), plus
  the `String` link.

## The loader's own files

The Nexus zip's `UE4SS_Signatures` (FName and FText constructors, GNatives, GUObjectArray) are the ones this loader
needs: with the signature scripts written for `e31aaaa6`, `c0a8d4c` fails its scans ("did not return a valid address
for FName::FName") and the game stops at start-up. Both game folders hold the zip's set since 2026-10-25 (the old set
as `UE4SS_Signatures.e31aaaa6`); `VTableLayout.ini` and `UE4SS-settings.ini` stayed ours (the layout names the same
slots). C++ mods built against `e31aaaa6` (KismetDebugger, EventViewer, TowerHider, the old split-screen mods) do not
load under this DLL (error 0x7f, a missing import) and are logged as "not installable"; they need a rebuild from
this checkout's `cppmods` if wanted.

## The Nexus DLL against this source

The DLL's link time is 2025-02-08 04:45 UTC (its PE header; the PDB path `E:\Projects\22\Binaries\...` is a
developer's xmake tree, not CI). Its exports match a UE4SS.dll built from this checkout except for 17 it has that
this source lacks (`FFrame::StepCompiledIn`, `MallocAndMemsetNewFrame`, const overloads of
`FFieldClass::IsChildOf` and two `FProperty` copy helpers, `UKismetSystemLibrary::Self`, a `FFieldClassVariant`
constructor) and 4 non-const overloads this source has instead: that developer's Unreal submodule was a few
commits ahead of the pin (the script-hook branch, merged upstream in April 2025). The mod uses none of them; its
55 imports all resolve against the Nexus DLL (`check_exports.py`).

## Checking a build

`check_exports.py <mod main.dll> <UE4SS.dll>` lists every symbol the mod imports from UE4SS.dll that the Nexus DLL
does not export (none is the gate). The DLL itself: `Remake\UE4SS-Rebirth_FFVII_ReMenu (1.005 Compatible) ...\ue4ss\UE4SS.dll`
(the zip in `Downloads`).
