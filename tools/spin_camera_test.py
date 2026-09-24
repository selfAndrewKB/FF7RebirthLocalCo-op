"""Tests for spin_camera.session_state, the only logic that decides whether input may be sent."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from spin_camera import session_state  # noqa: E402

READY = '[t] [LocalSplitScreen] ready: every engine check passed. Ctrl+F7 adds player 2\n'
JOIN = "[t] [LocalSplitScreen] player 2 joined (sharing player 1's camera for now)\n"
TURN = "[t] [LocalSplitScreen] TEST: player 2's view turned 30 degrees away from player 1's\n"
BACK = "[t] [LocalSplitScreen] TEST: player 2's view back on player 1's camera (turn off)\n"
LEFT = '[t] [LocalSplitScreen] player 2 left\n'
LOAD = '[t] [LocalSplitScreen] a map is loading: player 2 left (press Ctrl+F7 to rejoin afterwards)\n'


def check(name, text, player_two, turned):
    state = session_state(text)
    ok = state['player_two'] == player_two and state['turned'] == turned and state['ready'] == ('ready:' in text)
    print(('ok     ' if ok else 'FAILED ') + name, state)
    return ok


cases = [
    check('fresh', READY, False, False),
    check('joined', READY + JOIN, True, False),
    check('joined and turned', READY + JOIN + TURN, True, True),
    check('turned back', READY + JOIN + TURN + BACK, True, False),
    check('left while turned', READY + JOIN + TURN + LEFT, False, False),
    check('map load while turned', READY + JOIN + TURN + LOAD, False, False),
    check('rejoin resets the turn', READY + JOIN + TURN + LEFT + JOIN, True, False),
    check('turn without player 2 is ignored', READY + TURN, False, False),
]

# --- spin(): a crash must never be reported as "no crash" -------------------------------------
from spin_camera import spin  # noqa: E402


class FakeGame:
    """Scripted: the crash dialog appears (and takes focus) after `crash_after_moves` moves, or
    `crash_after_seconds` after the spin ends; the process can exit instead."""

    def __init__(self, crash_after_moves=None, crash_late=None, exit_after_moves=None, hang=False):
        self.moves, self.crash_after_moves, self.exit_after_moves = 0, crash_after_moves, exit_after_moves
        self.crash_late, self.stopped_at, self.is_hung = crash_late, None, hang
        self.escape_after_moves = None

    def _dialog_up(self):
        import time as t
        if self.crash_after_moves is not None and self.moves >= self.crash_after_moves:
            return True
        return self.crash_late is not None and self.stopped_at is not None and t.monotonic() >= self.stopped_at + self.crash_late

    def click_inside(self):
        pass

    def crash_dialog(self):
        return self._dialog_up()

    def alive(self):
        return self.exit_after_moves is None or self.moves < self.exit_after_moves

    def focused(self):
        return self.alive() and not self._dialog_up()   # the dialog takes focus

    def escape(self):
        return self.escape_after_moves is not None and self.moves >= self.escape_after_moves

    def hung(self):
        return self.is_hung

    def move(self, dx):
        if not self.focused():
            return False
        self.moves += 1
        return True


import time as _time  # noqa: E402
_real_monotonic = _time.monotonic


def run(game, seconds=0.05, observe=0.3):
    result = spin(game, seconds, 50, 1000, observe)
    if game.stopped_at is None:
        game.stopped_at = _real_monotonic()
    return result


def check_outcome(name, result, expected):
    ok = result['outcome'] == expected
    print(('ok     ' if ok else 'FAILED ') + name, result['outcome'])
    return ok


class LateCrash(FakeGame):
    """The dialog appears 0.1 s after the spin's duration ends."""

    def move(self, dx):
        moved = super().move(dx)
        self.stopped_at = _real_monotonic()  # refreshed each move; the last one is the spin's end
        return moved


cases += [
    check_outcome('crash while spinning (dialog takes focus between scans)', run(FakeGame(crash_after_moves=5)),
                  'CRASH_DIALOG'),
    check_outcome('crash just after the spin ends', run(LateCrash(crash_late=0.1)), 'CRASH_DIALOG'),
    check_outcome('process exits with no dialog', run(FakeGame(exit_after_moves=5)), 'PROCESS_EXITED'),
    check_outcome('hung window at the end', run(FakeGame(hang=True)), 'NOT_RESPONDING'),
    check_outcome('a clean full-length run', run(FakeGame()), 'NO_CRASH_OBSERVED'),
]
escaped = FakeGame()
escaped.escape_after_moves = 3
cases.append(check_outcome('Escape: not a full-length run', run(escaped), 'STOPPED_EARLY_NO_CRASH_OBSERVED'))

print('all tests passed' if all(cases) else f'{cases.count(False)} failed')
sys.exit(0 if all(cases) else 1)
