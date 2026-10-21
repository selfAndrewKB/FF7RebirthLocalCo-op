"""The in-game regression: deploys a build, launches the game, reaches the save, runs the scripts in order and checks
each one's `# expect:` / `# expect-not:` lines against the slice of UE4SS.log it produced.

    python tools/regression/run.py                       the dev build on the live game (1.005)
    python tools/regression/run.py --version 1.004       the dev build on the 1.004 copy (its exe started directly)
    python tools/regression/run.py --build release       the release build: its startup lines only (no control channel)
    python tools/regression/run.py --in-field 10 20      the game is already at the save: run scripts 10 and 20 only

Every script in tools/regression/scripts/ is a control script (src/control/script.h) with checks in comments:
    # expect: <regex>        at least one line of the script's log slice matches (Python re.search)
    # expect-dev: <regex>    the same, checked on the dev build only (a line of the dev tools)
    # expect-not: <regex>    no line matches
    # scope: log             the checks run over the whole log of this launch (the startup script)
    # timeout: <seconds>     how long to wait for the script's result (300 by default)
plus three checks every script gets: no "raised an exception", no "check failed:", no command channel stop. A script
must leave player 2 out (the next one joins afresh; the last one is followed by `quit`).

The report goes to tools/regression/out/<stamp>-<version>-<build>.md; the baseline kept under version control is
tools/regression/baseline/<version>-<build>.md (copied there by hand after the run that defines it).

The mod is talked to through one function, Control(), so the control port's switch to TCP changes one place.
"""
import argparse
import datetime as dt
import pathlib
import re
import shutil
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent  # LocalSplitScreen/
TOOLS = ROOT / "tools"
SCRIPTS = HERE / "scripts"
OUT = HERE / "out"

GAMES = {
    "1.005": {
        "dir": pathlib.Path(r"G:\_SSD Games\SteamLibrary\steamapps\common\FINAL FANTASY VII REBIRTH\End\Binaries\Win64"),
        "direct": False,
    },
    "1.004": {
        "dir": pathlib.Path(r"G:\_SSD Games\SteamLibrary\steamapps\common\FINAL FANTASY VII REBIRTH - Copy 1.004\End\Binaries\Win64"),
        "direct": True,
    },
}
BUILDS = {
    "dev": ROOT / "build" / "Game__Shipping__Win64" / "LocalSplitScreen.dll",
    "release": ROOT / "build-release" / "Game__Shipping__Win64" / "LocalSplitScreen.dll",
}
ALWAYS_EXPECT_NOT = [r"raised an exception", r"check failed:", r"an exception stopped the command channel"]


def mod_dir(game: pathlib.Path) -> pathlib.Path:
    return game / "ue4ss" / "Mods" / "LocalSplitScreen"


def log_path(game: pathlib.Path) -> pathlib.Path:
    return game / "ue4ss" / "UE4SS.log"


def powershell(script: pathlib.Path, *args: str, timeout: float = 600) -> subprocess.CompletedProcess:
    return subprocess.run(["pwsh", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script), *args],
                          capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)


def game_running() -> bool:
    out = subprocess.run(["pwsh", "-NoProfile", "-Command", "(Get-Process ff7rebirth_ -ErrorAction SilentlyContinue).Count"],
                         capture_output=True, text=True).stdout.strip()
    return out not in ("", "0")


def close_game_window(wait_seconds: float = 180) -> bool:
    """A release build has no control channel: WM_CLOSE to the game window, as a player's close. Never a kill."""
    subprocess.run(["pwsh", "-NoProfile", "-Command",
                    "Get-Process ff7rebirth_ -ErrorAction SilentlyContinue | ForEach-Object { [void]$_.CloseMainWindow() }"],
                   capture_output=True, text=True)
    deadline = time.monotonic() + wait_seconds
    while time.monotonic() < deadline:
        if not game_running():
            return True
        time.sleep(2)
    return False


class Control:
    """The one place the harness talks to the mod: today the file inbox through tools/control.py."""

    def __init__(self, game: pathlib.Path):
        self.mod = mod_dir(game)

    def run_file(self, script: pathlib.Path, timeout: float) -> tuple[int, str]:
        proc = subprocess.run([sys.executable, str(TOOLS / "control.py"), str(script), "--mod", str(self.mod),
                               "--timeout", str(timeout)], capture_output=True, text=True, encoding="utf-8", errors="replace")
        return proc.returncode, proc.stdout + proc.stderr

    def run(self, *commands: str, timeout: float = 120) -> tuple[int, str]:
        args = []
        for command in commands:
            args += ["-c", command]
        proc = subprocess.run([sys.executable, str(TOOLS / "control.py"), *args, "--mod", str(self.mod), "--timeout",
                               str(timeout)], capture_output=True, text=True, encoding="utf-8", errors="replace")
        return proc.returncode, proc.stdout + proc.stderr


class Log:
    """UE4SS.log, read from where the last read stopped (the game keeps it open; Python opens it shared)."""

    def __init__(self, path: pathlib.Path):
        self.path = path
        self.offset = 0

    def skip_to_end(self) -> None:
        self.offset = self.path.stat().st_size if self.path.exists() else 0

    def new_lines(self) -> list[str]:
        if not self.path.exists():
            return []
        with open(self.path, "rb") as file:
            file.seek(self.offset)
            data = file.read()
            self.offset = file.tell()
        return data.decode("utf-8", errors="replace").splitlines()

    def all_lines(self) -> list[str]:
        if not self.path.exists():
            return []
        return self.path.read_bytes().decode("utf-8", errors="replace").splitlines()


class Script:
    def __init__(self, path: pathlib.Path):
        self.path = path
        self.name = path.stem
        self.expect: list[str] = []
        self.expect_dev: list[str] = []  # checked on the dev build only (lines of the dev tools)
        self.expect_not: list[str] = []
        self.scope_log = False
        self.timeout = 300.0
        self.commands = 0
        for line in path.read_text(encoding="utf-8").splitlines():
            text = line.strip()
            if text.startswith("# expect-not:"):
                self.expect_not.append(text[len("# expect-not:"):].strip())
            elif text.startswith("# expect-dev:"):
                self.expect_dev.append(text[len("# expect-dev:"):].strip())
            elif text.startswith("# expect:"):
                self.expect.append(text[len("# expect:"):].strip())
            elif text.startswith("# scope:"):
                self.scope_log = text[len("# scope:"):].strip() == "log"
            elif text.startswith("# timeout:"):
                self.timeout = float(text[len("# timeout:"):].strip())
            elif text and not text.startswith("#"):
                self.commands += 1


class Result:
    def __init__(self, script: Script):
        self.script = script
        self.ran_ok: bool | None = None  # the control channel's verdict (None: not run)
        self.control_output = ""
        self.missing: list[str] = []            # expect lines nothing matched
        self.present: list[tuple[str, str]] = []  # expect-not lines that matched, with the first line
        self.matched: dict[str, str] = {}       # expect -> the first matching line
        self.lines = 0

    @property
    def passed(self) -> bool:
        return self.ran_ok is not False and not self.missing and not self.present


def check(script: Script, lines: list[str], result: Result, build: str = "dev") -> None:
    result.lines = len(lines)
    for pattern in script.expect + (script.expect_dev if build == "dev" else []):
        rx = re.compile(pattern)
        hit = next((line for line in lines if rx.search(line)), None)
        if hit is None:
            result.missing.append(pattern)
        else:
            result.matched[pattern] = hit
    for pattern in script.expect_not + ALWAYS_EXPECT_NOT:
        rx = re.compile(pattern)
        hit = next((line for line in lines if rx.search(line)), None)
        if hit is not None:
            result.present.append((pattern, hit))


def deploy_and_launch(build: str, version: str, stamp: str, no_deploy: bool) -> tuple[bool, str, str | None]:
    """relaunch.ps1: quit a running game through the mod, back the installed DLL up, copy the build in, launch, wait for
    the ready line. Returns (ready, output, backup name)."""
    game = GAMES[version]
    args = ["-Build", str(BUILDS[build]), "-GameDir", str(game["dir"])]
    if game["direct"]:
        args.append("-Direct")
    label = f"regression-{stamp}"
    if not no_deploy:
        args += ["-Deploy", "-Label", label]
    proc = powershell(TOOLS / "relaunch.ps1", *args, timeout=900)
    output = proc.stdout + proc.stderr
    ready = "ready: every engine check passed" in output or "v" in output and "starting" in output
    ready = ready and "no ready line" not in output
    backup = f"main.dll.bak-{label}" if not no_deploy and "installed" in output else None
    return ready, output, backup


def reach_the_field(control: Control, rounds: int = 4) -> tuple[bool, str]:
    """From the title right after the ready line: press through to Continue and the load, then wait for the field
    (`wait-until field`: the save loaded and playable, not the title). The game sometimes shows popups at start-up
    (seen when the two versions are launched one after the other), which swallow the fixed presses; while the field
    is not reached, one more Enter is pressed per round (it confirms a popup, or Continue, or the resume dialog) and
    the wait repeated. Each wait covers a load (2700 ticks: 27 s at 100 fps, 45 s at 60)."""
    pressed = powershell(TOOLS / "continue_save.ps1", timeout=300)
    text = pressed.stdout + pressed.stderr
    for attempt in range(rounds):
        code, out = control.run("wait-until field 2700", timeout=120)
        text += out
        if code == 0:
            return True, text
        text += f"the field is not reached (round {attempt + 1} of {rounds}): one more Enter\n"
        keys = powershell(TOOLS / "keys.ps1", "-Keys", "Enter", timeout=60)
        text += keys.stdout + keys.stderr
    return False, text


def report_text(stamp: str, version: str, build: str, deploy_output: str, results: list[Result], notes: list[str]) -> str:
    lines = [f"# Regression {stamp}: {version}, {build} build", ""]
    passed = sum(1 for r in results if r.passed)
    lines.append(f"**{passed} of {len(results)} scripts passed.**")
    lines += ["", "| script | result | log lines | missing | present |", "|---|---|---|---|---|"]
    for r in results:
        verdict = "pass" if r.passed else ("FAIL" if r.ran_ok is not False else "FAIL (the script failed)")
        lines.append(f"| {r.script.name} | {verdict} | {r.lines} | {len(r.missing)} | {len(r.present)} |")
    for r in results:
        lines += ["", f"## {r.script.name}", ""]
        if r.ran_ok is False:
            lines.append("The control channel reported a failure:")
            lines.append("```")
            lines += r.control_output.strip().splitlines()[-12:]
            lines.append("```")
        for pattern in r.missing:
            lines.append(f"- MISSING `{pattern}`")
        for pattern, hit in r.present:
            lines.append(f"- PRESENT `{pattern}`: `{hit.strip()}`")
        for pattern, hit in r.matched.items():
            lines.append(f"- ok `{pattern}`: `{hit.strip()[:160]}`")
    if notes:
        lines += ["", "## Notes", ""] + [f"- {n}" for n in notes]
    lines += ["", "## Launch", "", "```"] + deploy_output.strip().splitlines()[-25:] + ["```", ""]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", choices=BUILDS, default="dev")
    parser.add_argument("--version", choices=GAMES, default="1.005")
    parser.add_argument("--no-deploy", action="store_true", help="launch what is installed")
    parser.add_argument("--in-field", action="store_true", help="the game runs at the save already: no launch, no quit")
    parser.add_argument("--keep-running", action="store_true", help="leave the game running at the end")
    parser.add_argument("--restore", action="store_true", help="put the backed-up DLL back after the run (and quit first)")
    parser.add_argument("scripts", nargs="*", help="script numbers or names to run (all by default)")
    args = parser.parse_args()

    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    game = GAMES[args.version]
    control = Control(game["dir"])
    log = Log(log_path(game["dir"]))
    OUT.mkdir(exist_ok=True)
    notes: list[str] = []
    deploy_output = ""
    backup = None

    all_scripts = [Script(p) for p in sorted(SCRIPTS.glob("*.txt"))]
    wanted = [s for s in all_scripts if not args.scripts or any(s.name.startswith(w) or s.name == w for w in args.scripts)]
    if args.build == "release":
        wanted = [s for s in wanted if s.scope_log]  # no control channel: the startup checks only
        notes.append("release build: the control channel is not built, so only the startup script ran")

    if not args.in_field:
        if game_running():
            print("a game is running; relaunch.ps1 quits it through the mod first (a release build is closed by its window)")
            if args.build == "release" or args.no_deploy:
                close_game_window()
        ready, deploy_output, backup = deploy_and_launch(args.build, args.version, stamp, args.no_deploy)
        print(deploy_output.strip())
        if not ready:
            print("the mod did not log its ready line; stopping")
            (OUT / f"{stamp}-{args.version}-{args.build}.md").write_text(
                report_text(stamp, args.version, args.build, deploy_output, [], ["the ready line never came"]), encoding="utf-8")
            return 2
        if args.build != "release":
            print("reaching the field...")
            ok, text = reach_the_field(control)
            print(text.strip().splitlines()[-1] if text.strip() else "")
            if not ok:
                notes.append("the field did not become ready after Continue: the scripts that need it were skipped")
                wanted = [s for s in wanted if s.scope_log]
    results: list[Result] = []
    for script in wanted:
        result = Result(script)
        results.append(result)
        print(f"-- {script.name}")
        if script.scope_log:
            check(script, log.all_lines(), result, args.build)
            if script.commands > 0 and args.build != "release":
                result.ran_ok, result.control_output = (lambda c: (c[0] == 0, c[1]))(control.run_file(script.path, script.timeout))
            log.skip_to_end()
        else:
            log.skip_to_end()
            code, output = control.run_file(script.path, script.timeout)
            result.ran_ok, result.control_output = code == 0, output
            time.sleep(1.0)  # the last lines of the script land after its result
            check(script, log.new_lines(), result, args.build)
        print("   pass" if result.passed else f"   FAIL: missing {len(result.missing)}, present {len(result.present)}, ran ok {result.ran_ok}")

    if not args.in_field and not args.keep_running:
        if args.build == "release":
            closed = close_game_window()
            notes.append("closed by its window" if closed else "the game did not close by its window; left running")
        else:
            quit_out = powershell(TOOLS / "relaunch.ps1", "-NoLaunch", "-GameDir", str(game["dir"]), timeout=300)
            notes.append("quit through the mod: " + " ".join(quit_out.stdout.strip().splitlines()[-2:]))
        if args.restore and backup and not game_running():
            dll = mod_dir(game["dir"]) / "dlls" / "main.dll"
            shutil.copyfile(dll.with_name(backup), dll)
            notes.append(f"{backup} put back as main.dll")
    report = report_text(stamp, args.version, args.build, deploy_output, results, notes)
    out = OUT / f"{stamp}-{args.version}-{args.build}.md"
    out.write_text(report, encoding="utf-8")
    print(f"report: {out}")
    return 0 if all(r.passed for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
