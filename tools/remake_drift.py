"""The files this mod shares with the FF7 Remake mod (the sibling repository), and whether they have drifted.

The monitor row, the record of moved monitors and the setup app were taken from Remake's mod. Some files are kept
identical, byte for byte but for line endings; the others were adapted for this mod, each for a reason named here. This
script says which copies no longer match Remake's, and which adapted files have a Remake twin that changed since the
copy was taken, so a fix made on one side is carried to the other.

    python -I tools/remake_drift.py            exit 0 when nothing drifted; else the files, one per line
    python -I tools/remake_drift.py --diff     the same, with a unified diff per identical file that differs
"""
import difflib
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parents[1]
REMAKE = HERE.parents[1] / "Remake" / "LocalSplitScreen"

# The Remake commit the copies were taken from (the adapted files are compared with their twins as of this commit).
REMAKE_COMMIT = "9dcada2"

# Kept identical to Remake's file of the same path.
IDENTICAL = [
    "src/platform/text.h",
    "src/views/display_config.cpp",
    "src/views/display_config.h",
    "src/views/display_move.cpp",
    "src/views/display_move.h",
    "src/views/monitor_list.cpp",
    "src/views/monitor_list.h",
    "src/views/monitor_picks.cpp",
    "src/views/monitor_row.cpp",
    "src/views/record_file.cpp",
    "src/views/record_file.h",
    "src/views/span_desktop.cpp",
    "src/views/span_desktop.h",
    "src/views/display_align.h",
    "setup/app.ico",
    "setup/app.manifest",
    "setup/app.rc",
    "setup/guard.cpp",
    "setup/guard.h",
    "setup/setup_rules.h",
    "setup/setup_version.h.in",
    "setup/theme.h",
    "setup/tips.cpp",
    "setup/tips.h",
    "setup/view.h",
]

# Adapted: the path here, and why it differs from Remake's file of the same path.
ADAPTED = {
    "src/views/monitor_row.h": "one comment phrase: the column layout that takes the widths is not built yet",
    "src/views/monitor_picks.h": "the column count comes from views/columns.h",
    "src/views/row_journal.h": "a record that lists a monitor twice is damaged (to carry back to Remake)",
    "src/views/display_align.cpp": "the mod's folder from platform/module_folder.h",
    "src/views/game_window.h": "find and reshape only: Rebirth sizes its render image from the window (no resolution request)",
    "src/views/game_window.cpp": "the same",
    "src/views/window_span.h": "Rebirth's span: the columns follow the players, the monitor mode is Ctrl+F3",
    "src/views/window_span.cpp": "the same",
    "setup/app.h": "kOrderOption: the screen-order option waits for the column layout",
    "setup/app.cpp": "the screen-order cell is drawn and stepped only with an option to show",
    "setup/main.cpp": "the settings file read through ReadSettings",
    "setup/view.cpp": "the screen-order cell is drawn only with an option to show",
    "tests/monitor_row_test.cpp": "the test harness (tests/expect.h)",
    "tests/monitor_picks_test.cpp": "the test harness; a static_assert on the row's size",
    "tests/row_journal_test.cpp": "the test harness; the case of a monitor listed twice",
    "tests/setup_rules_test.cpp": "the test harness; the option stepped is two-player-split",
}


def text_of(data: bytes) -> bytes:
    return data.replace(b"\r\n", b"\n")


def remake_at_commit(path: str) -> bytes | None:
    done = subprocess.run(["git", "-C", str(REMAKE), "show", f"{REMAKE_COMMIT}:{path}"], capture_output=True)
    return done.stdout if done.returncode == 0 else None


def main() -> int:
    show_diff = "--diff" in sys.argv
    drifted = 0
    if not REMAKE.is_dir():
        print(f"Remake's mod is not at {REMAKE}; nothing compared")
        return 1
    for path in IDENTICAL:
        ours, theirs = HERE / path, REMAKE / path
        if not ours.exists() or not theirs.exists():
            print(f"{path}: missing on {'this' if not ours.exists() else 'the Remake'} side")
            drifted += 1
            continue
        a, b = text_of(ours.read_bytes()), text_of(theirs.read_bytes())
        if a != b:
            print(f"{path}: differs from Remake's file (kept identical by design)")
            drifted += 1
            if show_diff and path.endswith((".h", ".cpp", ".in", ".rc", ".manifest")):
                diff = difflib.unified_diff(b.decode("utf-8", "replace").splitlines(), a.decode("utf-8", "replace").splitlines(),
                                            f"remake/{path}", f"rebirth/{path}", lineterm="")
                print("\n".join(diff))
    for path, why in ADAPTED.items():
        if not (HERE / path).exists():
            print(f"{path}: missing here (adapted: {why})")
            drifted += 1
            continue
        then, now = remake_at_commit(path), (REMAKE / path)
        if then is None or not now.exists():
            continue  # a file Rebirth has and Remake does not (or had not at the commit): nothing to follow
        if text_of(then) != text_of(now.read_bytes()):
            print(f"{path}: Remake's file changed since {REMAKE_COMMIT}; re-read it (adapted here: {why})")
            drifted += 1
    print("nothing drifted" if drifted == 0 else f"{drifted} file(s) to look at")
    return 0 if drifted == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
