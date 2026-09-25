"""Send a control script to the running mod and print its result.

    python tools/control.py script.txt            submit a file and wait for its result
    python tools/control.py -c "join" -c "wait-until playing"
                                                  submit commands given on the command line
    python tools/control.py script.txt --no-wait  submit and return at once

The mod's command channel (src/control/command_channel.h) takes *.txt scripts from
Mods/LocalSplitScreen/control/inbox/ and writes done/<name>.result.txt, whose last line starts
with "finished:". The script is written under .tmp and renamed, so the mod never reads half a file.

Exit status: 0 finished ok, 1 failed or rejected, 2 timed out waiting (the game may be paused,
loading or closed; the result so far is printed).
"""
import argparse
import os
import pathlib
import sys
import time

DEFAULT_MOD = pathlib.Path(
    r"G:\_SSD Games\SteamLibrary\steamapps\common\FINAL FANTASY VII REBIRTH"
    r"\End\Binaries\Win64\ue4ss\Mods\LocalSplitScreen")


def submit(control: pathlib.Path, name: str, text: str) -> pathlib.Path:
    inbox = control / "inbox"
    if not inbox.is_dir():
        sys.exit(f"no inbox at {inbox} (has the mod run with the command channel yet?)")
    stem = f"{time.strftime('%Y%m%d-%H%M%S')}-{name}"
    temporary = inbox / f"{stem}.tmp"
    temporary.write_text(text if text.endswith("\n") else text + "\n", encoding="utf-8")
    os.replace(temporary, inbox / f"{stem}.txt")
    return control / "done" / f"{stem}.result.txt"


def wait(result: pathlib.Path, timeout: float) -> int:
    deadline, shown = time.monotonic() + timeout, 0
    while True:
        lines = result.read_text(encoding="utf-8").splitlines() if result.exists() else []
        for line in lines[shown:]:
            print(line)
        shown = len(lines)
        if lines and lines[-1].startswith("finished:"):
            return 0 if lines[-1] == "finished: ok" else 1
        if time.monotonic() > deadline:
            print(f"(no 'finished:' line after {timeout:.0f} s; {result.name} may still complete)")
            return 2
        time.sleep(0.25)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("script", nargs="?", type=pathlib.Path, help="a script file")
    parser.add_argument("-c", "--command", action="append", default=[], help="a script line (repeatable)")
    parser.add_argument("--mod", type=pathlib.Path, default=DEFAULT_MOD, help="the mod's folder")
    parser.add_argument("--timeout", type=float, default=120.0, help="seconds to wait for the result")
    parser.add_argument("--no-wait", action="store_true", help="submit and return at once")
    args = parser.parse_args()

    if bool(args.script) == bool(args.command):
        parser.error("give either a script file or -c commands")
    if args.script:
        text, name = args.script.read_text(encoding="utf-8"), args.script.stem
    else:
        text, name = "\n".join(args.command), "inline"

    result = submit(args.mod / "control", name, text)
    print(f"submitted; result will be {result}")
    return 0 if args.no_wait else wait(result, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
