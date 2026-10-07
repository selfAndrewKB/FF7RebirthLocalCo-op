# version_match: carrying the mod's addresses to a new game build

The mod's source keeps the research build's IDA addresses (1.004). For each later build, these tools produce the
tables `src/platform/builds/build_<ver>.h` (generated) and `build_<ver>_replaced.h` (hand-found), which
`platform/build_translation` uses at run time. Plan and findings: `Rebirth/Documentation/PLAN-GAME-VERSIONS.md`.

Needs Python 3 with `pefile`, `capstone` 5 and `numpy`. The exes live in `Rebirth/builds/<ver>/ff7rebirth_.exe`.

| Script | What it does |
|---|---|
| `match.py <old exe> <new exe> <src> <out> --version-name <ver>` | harvests every address in the mod's code, matches it, writes `report.md`, `matches.json` and `build_<ver>.h` (~2 min) |
| `replaced_header.py <out> <ver> <header>` | turns the IDA agents' `replaced-*.json` into `build_<ver>_replaced.h` |
| `layout_diff.py <old dump> <new dump> <out.md>` | diffs reflected class layouts between two UE4SS object dumps (Ctrl+J in game) |
| `offset_census.py <src> <old dump> <new dump> <out.json> <out.md>` | lists the mod's integer constants with reflected-layout leads, for the offset audit |
| `vt_compare.py`, `vt_bytes.py <old vt> <new vt> <slots>` | map a vtable slot by slot between builds (by matched function, or by masked bytes) |
| `probe.py`, `probe2.py <addr>...` | debug one address: how its masked / structural pattern matches |

## A new game build, step by step

1. Before updating: copy the whole game folder (Steam cannot roll back).
2. Copy the new exe to `Rebirth/builds/<ver>/`, add the build (PE timestamp, image size, checksum) to
   `platform/build_translation.cpp` and a value to `BuildIndex` (`platform/build_value.h`).
3. `match.py`, copy `build_<ver>.h` into `src/platform/builds/`.
4. Re-find the unmatched addresses in IDA (agents; output `replaced-*.json`), then `replaced_header.py`.
5. Take an object dump on the new build, `layout_diff.py`, and audit the offset constants; a moved one becomes a
   `PerBuild` constant with both values.
6. Build, host tests, live check.
