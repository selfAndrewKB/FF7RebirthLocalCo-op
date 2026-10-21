"""The in-game regression: deploys a build, launches the game, reaches the save, runs the scripts in order and checks
each one's `# expect:` / `# expect-not:` lines against the slice of UE4SS.log it produced.

    python tools/regression/run.py                       the dev build on the live game (1.005)
    python tools/regression/run.py --version 1.004       the dev build on the 1.004 copy (its exe started directly)
    python tools/regression/run.py --build release       the release build: the scripts driven by the mod's hotkeys
    python tools/regression/run.py --in-field 10 20      the game is already at the save: run scripts 10 and 20 only

Every script in tools/regression/scripts/ is a control script (src/control/script.h) with checks in comments:
    # expect: <regex>        at least one line of the script's log slice matches (Python re.search)
    # expect-dev: <regex>    the same, checked on the dev build only (a line of the dev tools)
    # expect-not: <regex>    no line matches
    # scope: log             the checks run over the whole log of this launch (the startup script)
    # needs: control         the script runs only through the control channel (the dev build): it is skipped on
                             the key route (the release build, which has no control channel)
    # timeout: <seconds>     how long to wait for the script's result (300 by default)
plus three checks every script gets: no "raised an exception", no "check failed:", no command channel stop. A script
must leave player 2 out (the next one joins afresh; the last one is followed by `quit`).

The report goes to tools/regression/out/<stamp>-<version>-<build>.md; the baseline kept under version control is
tools/regression/baseline/<version>-<build>.md (copied there by hand after the run that defines it).

Two routes drive the mod behind one interface (run_file, wait_until): Control is the file inbox through
tools/control.py (the dev build); Keys presses the mod's hotkeys through tools/keys.ps1 and waits on the mod's own
log lines (the release build). The control port's switch to TCP changes Control only.
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

# A tick of `wait`/`wait-until` is one frame; the key route counts them at the slowest frame rate the game is played at,
# so its waits are never shorter than the control route's.
SECONDS_PER_TICK = 1.0 / 60.0


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


class Control:
    """The control channel: the file inbox through tools/control.py (the dev build)."""

    name = "control"

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

    def wait_until(self, what: str, ticks: int) -> tuple[bool, str]:
        code, out = self.run(f"wait-until {what} {ticks}", timeout=ticks * SECONDS_PER_TICK + 60)
        return code == 0, out


# The mod's switches: the key, and the state the switch starts in at launch (players/join_keys.h). A `layout`, `bars`,
# `own-camera`, `sticks` or `third-view` command presses the key only when the state it asks for is not the one held.
# The chosen layout, the bars and the sticks persist across a leave; player 2's own camera is the start of every join
# (views/player_view_point.h), and the third view leaves with player 2.
KEY_SWITCHES = {
    "layout": ("Ctrl+F5", "top-bottom"),
    "bars": ("Ctrl+F4", "off"),
    "own-camera": ("Ctrl+F6", "on"),
    "sticks": ("Ctrl+F9", "per-pad"),
    "third-view": ("Ctrl+F3", "off"),
}
RESET_AT_JOIN = {"own-camera": "on"}
RESET_AT_LEAVE = {"third-view": "off"}

# The mod's own lines that end a `wait-until` (player_two/session.cpp, player_two/field_exit.cpp).
WAIT_LINES = {
    "playing": re.compile(r"player 2 took .+ after \d+ tick"),
    "out": re.compile(r"leave: player 2 left|player 2 was removed by the game"),
}
FIELD_LEFT    = re.compile(r"field exit: field step 13 running 0")
FIELD_SETTLED = re.compile(r"field exit: field step 13 running 1")


def field_reached(lines: list[str]) -> bool:
    """The save's field is up: a settled field was left (the title's, "step 13 running 0": Continue confirmed) and a
    field settled after it ("step 13 running 1"). The title's own scene is a field, and its start-up logs "0 running 0
    ... 13 running 1" like a load, so a settled line alone (or a "running 0" of a lower step) is not enough: a run of
    2026-10-21 took the title for the field that way. (The mod's "whole screen: location" line is no signal here: the
    release build reads the location only once player 2 is in.)"""
    left = next((i for i, line in enumerate(lines) if FIELD_LEFT.search(line)), None)
    return left is not None and any(FIELD_SETTLED.search(line) for line in lines[left + 1:])


class Keys:
    """The hotkey route: the commands a key expresses are pressed through tools/keys.ps1; the waits watch the log for
    the mod's own lines; the rest (status, stick, split-fov, single-view, note, ...) is skipped and reported."""

    name = "keys"

    def __init__(self, game: pathlib.Path):
        self.log = Log(log_path(game))
        self.state = {name: start for name, (_, start) in KEY_SWITCHES.items()}
        self.skipped: list[str] = []

    def press(self, key: str) -> str:
        proc = powershell(TOOLS / "keys.ps1", "-Keys", key, timeout=60)
        text = (proc.stdout + proc.stderr).strip()
        return f"pressed {key}" if proc.returncode == 0 and f"pressed {key}" in text else f"PRESS FAILED {key}: {text}"

    def wait_lines(self, seconds: float, done) -> tuple[bool, list[str]]:
        """Reads the log until `done(lines so far)` holds or the time is up."""
        lines: list[str] = []
        deadline = time.monotonic() + seconds
        while True:
            lines += self.log.new_lines()
            if done(lines):
                return True, lines
            if time.monotonic() >= deadline:
                return False, lines
            time.sleep(0.5)

    def wait_until(self, what: str, ticks: int) -> tuple[bool, str]:
        seconds = ticks * SECONDS_PER_TICK
        if what == "field":
            ok, _ = self.wait_lines(seconds, field_reached)
        elif what in WAIT_LINES:
            ok, _ = self.wait_lines(seconds, lambda lines: any(WAIT_LINES[what].search(line) for line in lines))
        else:
            return False, f"wait-until {what}: unknown"
        return ok, f"wait-until {what} {ticks} -> {'ok' if ok else 'TIMEOUT'} (watched the log for up to {seconds:.0f} s)"

    def command(self, line: str) -> tuple[bool, str]:
        words = line.split()
        verb = words[0]
        if verb == "join":
            self.state.update(RESET_AT_JOIN)
            return True, f"{line} -> {self.press('Ctrl+F7')}"
        if verb == "leave":
            self.state.update(RESET_AT_LEAVE)
            return True, f"{line} -> {self.press('Ctrl+F8')}"
        if verb in KEY_SWITCHES and len(words) == 2:
            key, _ = KEY_SWITCHES[verb]
            wanted, held = words[1], self.state[verb]
            if wanted == held:
                return True, f"{line} -> no press ({verb} is {held} already)"
            self.state[verb] = wanted
            return True, f"{line} -> {self.press(key)} (was {held})"
        if verb == "wait" and len(words) == 2:
            time.sleep(int(words[1]) * SECONDS_PER_TICK)
            return True, f"{line} -> waited"
        if verb == "wait-until" and len(words) in (2, 3):
            # Over the lines since the last wait ended: the press before this wait has produced its lines already.
            return self.wait_until(words[1], int(words[2]) if len(words) == 3 else 600)
        self.skipped.append(line)
        return True, f"{line} -> skipped (no key for it)"

    def run_file(self, script: pathlib.Path, timeout: float) -> tuple[int, str]:
        """Every command runs even after a failed wait (the script's leave must still happen); the verdict is 1 then."""
        out: list[str] = []
        failed = False
        self.log.skip_to_end()
        for raw in script.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            ok, text = self.command(line)
            out.append(text)
            failed = failed or not ok
        return (1 if failed else 0), "\n".join(out) + "\n"


class Script:
    def __init__(self, path: pathlib.Path):
        self.path = path
        self.name = path.stem
        self.expect: list[str] = []
        self.expect_dev: list[str] = []  # checked on the dev build only (lines of the dev tools)
        self.expect_not: list[str] = []
        self.scope_log = False
        self.needs_control = False  # skipped on the key route
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
            elif text.startswith("# needs:"):
                self.needs_control = text[len("# needs:"):].strip() == "control"
            elif text.startswith("# timeout:"):
                self.timeout = float(text[len("# timeout:"):].strip())
            elif text and not text.startswith("#"):
                self.commands += 1


class Result:
    def __init__(self, script: Script):
        self.script = script
        self.ran_ok: bool | None = None  # the route's verdict (None: not run)
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


def reach_the_field(route, rounds: int = 4) -> tuple[bool, str]:
    """From the title right after the ready line: press through to Continue and the load, then wait for the field
    (`wait-until field`: the save loaded and playable, not the title; on the key route, the field lines the mod logs
    through the load). The game sometimes shows popups at start-up (seen when the two versions are launched one after
    the other), which swallow the fixed presses; while the field is not reached, one more Enter is pressed per round
    (it confirms a popup, or Continue, or the resume dialog) and the wait repeated. Each wait covers a load (2700
    ticks: 27 s at 100 fps, 45 s at 60)."""
    if isinstance(route, Keys):
        route.log.skip_to_end()  # the load's field lines come after the presses
    pressed = powershell(TOOLS / "continue_save.ps1", timeout=300)
    text = pressed.stdout + pressed.stderr
    for attempt in range(rounds):
        ok, out = route.wait_until("field", 2700)
        text += out + "\n"
        if ok:
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
            lines.append("The route reported a failure:")
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
    parser.add_argument("--route", choices=("control", "keys"), default=None,
                        help="how the mod is driven: control (the dev build's default) or keys (the release build's)")
    parser.add_argument("--no-deploy", action="store_true", help="launch what is installed")
    parser.add_argument("--in-field", action="store_true", help="the game runs at the save already: no launch, no quit")
    parser.add_argument("--keep-running", action="store_true", help="leave the game running at the end")
    parser.add_argument("--restore", action="store_true", help="put the backed-up DLL back after the run (and quit first)")
    parser.add_argument("scripts", nargs="*", help="script numbers or names to run (all by default)")
    args = parser.parse_args()

    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    game = GAMES[args.version]
    route_name = args.route or ("keys" if args.build == "release" else "control")
    route = Keys(game["dir"]) if route_name == "keys" else Control(game["dir"])
    log = Log(log_path(game["dir"]))
    OUT.mkdir(exist_ok=True)
    notes: list[str] = [f"route: {route_name}"]
    deploy_output = ""
    backup = None

    all_scripts = [Script(p) for p in sorted(SCRIPTS.glob("*.txt"))]
    wanted = [s for s in all_scripts if not args.scripts or any(s.name.startswith(w) or s.name == w for w in args.scripts)]
    if route_name == "keys":
        left_out = [s.name for s in wanted if s.needs_control]
        wanted = [s for s in wanted if not s.needs_control]
        if left_out:
            notes.append("skipped on the key route (`# needs: control`): " + ", ".join(left_out))

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
        print("reaching the field...")
        ok, text = reach_the_field(route)
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
            if script.commands > 0:
                result.ran_ok, result.control_output = (lambda c: (c[0] == 0, c[1]))(route.run_file(script.path, script.timeout))
            log.skip_to_end()
        else:
            log.skip_to_end()
            code, output = route.run_file(script.path, script.timeout)
            result.ran_ok, result.control_output = code == 0, output
            time.sleep(1.0)  # the last lines of the script land after its result
            check(script, log.new_lines(), result, args.build)
        print("   pass" if result.passed else f"   FAIL: missing {len(result.missing)}, present {len(result.present)}, ran ok {result.ran_ok}")
    if isinstance(route, Keys) and route.skipped:
        notes.append("commands without a key, skipped: " + "; ".join(sorted(set(route.skipped))))

    if not args.in_field and not args.keep_running:
        if route_name == "keys":
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
