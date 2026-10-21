# The in-game regression

Host tests cannot reach a redirect's effect on the game. This harness protects the field-tested behaviour: it deploys
a build, launches the game, reaches the save, runs the scripts in `scripts/` in order and checks the log lines each
one produces. It is run on both builds (`build/` with the dev tools, `build-release/` without) and both game versions
(1.005 live, the 1.004 copy) at the end of every cleanup phase; the baseline recorded before the first refactor commit
is `baseline/`.

```
python tools/regression/run.py                       # the dev build on the live game (1.005)
python tools/regression/run.py --version 1.004       # the dev build on the 1.004 copy (its exe started directly)
python tools/regression/run.py --build release       # the release build: its startup lines only (no control channel)
python tools/regression/run.py --in-field 10 20      # the game is already at the save: scripts 10 and 20 only
python tools/regression/run.py --restore             # put the DLL that was installed back when done
```

Each script is a control script (`src/control/script.h`) whose comments carry its checks:

```
# expect: <regex>        at least one line of the script's log slice matches
# expect-dev: <regex>    the same, checked on the dev build only (a line of the dev tools, e.g. the control channel)
# expect-not: <regex>    no line matches
# scope: log             the checks run over the whole log of this launch (the startup script)
# timeout: <seconds>     how long to wait for the script's result (300 by default)
```

Every script also gets three checks: no "raised an exception", no "check failed:", no command channel stop. A script
must leave player 2 out; the harness quits the game through the mod at the end and keeps the log under `ue4ss/logs/`.

Reaching the save: `continue_save.ps1` presses through the title, then `wait-until field` must hold (the field running,
the game ready, player 1's controller on the End classes, and not the title: the title's scene is a field too). The
game sometimes shows popups at start-up (seen when the two versions are launched one after the other), which swallow
the fixed presses; the harness then presses one more Enter per round and waits again, four rounds, before giving up
with "the field is not reached". Running the versions back to back is therefore allowed but slower.

The two HUD passes' timing lines are the perf log's (`perf_log.txt` in the mod folder); no script expects them.

What the loop cannot establish stays the user's play check: feel, audio, and anything needing a human reaction.

The harness talks to the mod through one class, `Control` in `run.py` (today the file inbox via `tools/control.py`),
so the control port's switch to TCP changes one place.
