# The in-game regression

Host tests cannot reach a redirect's effect on the game. This harness protects the field-tested behaviour: it deploys
a build, launches the game, reaches the save, runs the scripts in `scripts/` in order and checks the log lines each
one produces. It is run on both builds (`build/` with the dev tools, `build-release/` without) and both game versions
(1.005 live, the 1.004 copy) at the end of every cleanup phase; the baseline recorded before the first refactor commit
is `baseline/`.

```
python tools/regression/run.py                       # the dev build on the live game (1.005)
python tools/regression/run.py --version 1.004       # the dev build on the 1.004 copy (its exe started directly)
python tools/regression/run.py --build release       # the release build, driven by the mod's hotkeys (the key route)
python tools/regression/run.py --in-field 10 20      # the game is already at the save: scripts 10 and 20 only
python tools/regression/run.py --restore             # put the DLL that was installed back when done
python tools/regression/run.py --route keys          # the dev build through the keys (the key route checked on itself)
```

Each script is a control script (`src/control/script.h`) whose comments carry its checks:

```
# expect: <regex>        at least one line of the script's log slice matches
# expect-dev: <regex>    the same, checked on the dev build only (a line of the dev tools, e.g. the control channel)
# expect-not: <regex>    no line matches
# scope: log             the checks run over the whole log of this launch (the startup script)
# needs: control         runs only through the control channel; skipped on the key route
# timeout: <seconds>     how long to wait for the script's result (300 by default)
```

Every script also gets three checks: no "raised an exception", no "check failed:", no command channel stop. A script
must leave player 2 out; the harness quits the game at the end (through the mod, or by its window on the key route)
and keeps the log under `ue4ss/logs/`.

Two routes drive the mod behind one interface in `run.py`:

- **Control** (the dev build): the file inbox through `tools/control.py`; every command, every `wait-until` answered by
  the session itself. The control port's switch to TCP changes this class only.
- **Keys** (the release build, which has no control channel): `join`, `leave`, `layout`, `bars`, `own-camera`, `sticks`
  and `third-view` are pressed as the mod's hotkeys through `tools/keys.ps1` (a switch is pressed only when the state
  asked for differs from the one held, starting from the mod's start-up states); `wait` sleeps the ticks at 60 fps;
  `wait-until playing|out|field` watches the log for the mod's own lines (the take, the leave, the field settling after
  a load). Commands with no key (`status`, `stick`, `split-fov`, `single-view`, `note`, ...) are skipped and listed in
  the report. Expectations written for the control route's echo lines are `# expect-dev:`; what both routes must show
  is the engine's readbacks (the half aspect, the HUD's pass, the restores).

Reaching the save: `continue_save.ps1` presses through the title, then `wait-until field` must hold (the field running,
the game ready, player 1's controller on the End classes, and not the title: the title's scene is a field too; the key
route reads the mod's "field step" lines: the title's settled field leaving at Continue, then a field settling). The game
sometimes
shows popups at start-up (seen when the two versions are launched one after the other), which swallow the fixed
presses; the harness then presses one more Enter per round and waits again, four rounds, before giving up with "the
field is not reached". Running the versions back to back is therefore allowed but slower.

The two HUD passes' timing lines are the perf log's (`perf_log.txt` in the mod folder); no script expects them.

What the loop cannot establish stays the user's play check: feel, audio, and anything needing a human reaction.
