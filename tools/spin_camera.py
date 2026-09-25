"""Spin the camera in a LocalSplitScreen session and watch for the game's crash dialog.

A crash test for two different views: with player 2 in and player 2's half on its own camera (Ctrl+F6),
spin the camera at a steady rate and record whether, and when, the game crashes.

    Check:  python spin_camera.py --pid PID
    Run:    python spin_camera.py --pid PID --run --output NEW_DIR --seconds 360

It sends relative mouse movement only while the game window has focus (Escape or focus loss
stops it), clicks once inside the game first so Unreal takes the mouse, and never presses a key.
Every stop (duration, Escape, focus loss, which a crash dialog also causes) is followed by a
crash check and --observe-seconds of watching. On a crash it records the time and leaves the dialog open (Aftermath keeps writing while
it is up; never press OK before the dump is complete). Outputs: result.json and one UE4SS.log.gz.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime, timezone
import gzip
import hashlib
import json
import math
import ntpath
from pathlib import Path
import re
import time

CRASH_TITLE = 'The ff7rebirth has crashed and will close'
GAME_TITLE = 'FINAL FANTASY VII REBIRTH'
SCAN_SECONDS = 0.020
MOD = b'[LocalSplitScreen] '


class MOUSEINPUT(C.Structure):
    _fields_ = [('dx', W.LONG), ('dy', W.LONG), ('mouseData', W.DWORD),
                ('dwFlags', W.DWORD), ('time', W.DWORD), ('dwExtraInfo', C.c_size_t)]


class INPUT(C.Structure):
    class _DATA(C.Union):
        _fields_ = [('mi', MOUSEINPUT), ('pad', C.c_byte * 32)]
    _anonymous_ = ('data',)
    _fields_ = [('type', W.DWORD), ('data', _DATA)]


def session_state(log_text):
    """What the mod's log says now: is player 2 in, and does its half show its own camera? (pure; tested below)"""
    events = [(m.start(), kind) for kind, pattern in (
        ('joined', r'\] player 2 joined'), ('left', r'\] player 2 (left|was removed|is no longer)'),
        ('left', r'a map is loading: player 2 left'),
        ('turn_on', r'player 2\'s half shows player 2\'s own camera'), ('turn_off', r'player 2\'s half shows player 1\'s camera'))
        for m in re.finditer(pattern, log_text)]
    player_two = turned = False
    for _, kind in sorted(events):
        if kind == 'joined':
            player_two, turned = True, False
        elif kind == 'left':
            player_two, turned = False, False
        elif kind == 'turn_on' and player_two:
            turned = True
        elif kind == 'turn_off':
            turned = False
    return dict(player_two=player_two, turned=turned,
                ready='ready: every engine check passed' in log_text)


class Game:
    """The Win32 boundary: one process, its one game window, its crash dialog."""

    def __init__(self, pid):
        self.pid = pid
        self.u, self.k = C.WinDLL('user32', use_last_error=True), C.WinDLL('kernel32', use_last_error=True)
        self.enum_proc = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
        for dll, name, args, result in [
            (self.u, 'EnumWindows', [self.enum_proc, W.LPARAM], W.BOOL),
            (self.u, 'IsWindowVisible', [W.HWND], W.BOOL),
            (self.u, 'GetWindowThreadProcessId', [W.HWND, C.POINTER(W.DWORD)], W.DWORD),
            (self.u, 'GetWindowTextW', [W.HWND, W.LPWSTR, C.c_int], C.c_int),
            (self.u, 'GetClassNameW', [W.HWND, W.LPWSTR, C.c_int], C.c_int),
            (self.u, 'GetForegroundWindow', [], W.HWND),
            (self.u, 'GetClientRect', [W.HWND, C.POINTER(W.RECT)], W.BOOL),
            (self.u, 'ClientToScreen', [W.HWND, C.POINTER(W.POINT)], W.BOOL),
            (self.u, 'WindowFromPoint', [W.POINT], W.HWND),
            (self.u, 'SetCursorPos', [C.c_int, C.c_int], W.BOOL),
            (self.u, 'GetAsyncKeyState', [C.c_int], W.SHORT),
            (self.u, 'IsHungAppWindow', [W.HWND], W.BOOL),
            (self.u, 'SendInput', [W.UINT, C.POINTER(INPUT), C.c_int], W.UINT),
            (self.k, 'OpenProcess', [W.DWORD, W.BOOL, W.DWORD], W.HANDLE),
            (self.k, 'QueryFullProcessImageNameW', [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)], W.BOOL),
            (self.k, 'GetProcessTimes', [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4, W.BOOL),
            (self.k, 'WaitForSingleObject', [W.HANDLE, W.DWORD], W.DWORD),
            (self.k, 'CloseHandle', [W.HANDLE], W.BOOL),
        ]:
            fn = getattr(dll, name)
            fn.argtypes, fn.restype = args, result
        self.process = self.k.OpenProcess(0x1000 | 0x100000, False, pid)
        if not self.process:
            raise C.WinError(C.get_last_error())
        path, length = C.create_unicode_buffer(32768), W.DWORD(32768)
        if not self.k.QueryFullProcessImageNameW(self.process, 0, path, C.byref(length)):
            raise C.WinError(C.get_last_error())
        if ntpath.basename(path.value).lower() != 'ff7rebirth_.exe':
            raise RuntimeError('PID is not ff7rebirth_.exe')
        self.path = Path(path.value)
        stamps = [W.FILETIME() for _ in range(4)]
        self.k.GetProcessTimes(self.process, *(C.byref(s) for s in stamps))
        self.created = ((stamps[0].dwHighDateTime << 32) | stamps[0].dwLowDateTime) / 1e7 - 11644473600
        windows = [w for w in self.windows() if w['class'] == 'UnrealWindow' and w['title'] == GAME_TITLE]
        if len(windows) != 1:
            raise RuntimeError('Expected exactly one visible game window')
        self.hwnd = windows[0]['hwnd']

    def owner(self, hwnd):
        pid = W.DWORD()
        self.u.GetWindowThreadProcessId(hwnd, C.byref(pid))
        return pid.value

    def windows(self):
        found = []

        @self.enum_proc
        def visit(hwnd, _):
            if self.owner(hwnd) == self.pid and self.u.IsWindowVisible(hwnd):
                title, cls = C.create_unicode_buffer(512), C.create_unicode_buffer(128)
                self.u.GetWindowTextW(hwnd, title, len(title))
                self.u.GetClassNameW(hwnd, cls, len(cls))
                found.append({'hwnd': hwnd, 'title': title.value, 'class': cls.value})
            return True
        self.u.EnumWindows(visit, 0)
        return found

    def crash_dialog(self):
        return any(w['class'] == '#32770' and w['title'] == CRASH_TITLE for w in self.windows())

    def alive(self):
        return self.k.WaitForSingleObject(self.process, 0) == 0x102

    def focused(self):
        return self.alive() and self.u.GetForegroundWindow() == self.hwnd

    def escape(self):
        return bool(self.u.GetAsyncKeyState(0x1B) & 0x8000)

    def hung(self):
        return self.alive() and bool(self.u.IsHungAppWindow(self.hwnd))

    def click_inside(self):
        """One left click in the upper middle of the game client, so Unreal takes the mouse."""
        rect, point = W.RECT(), W.POINT()
        if not self.focused() or not self.u.GetClientRect(self.hwnd, C.byref(rect)):
            raise RuntimeError('Game window not focused; click into the game first')
        point.x, point.y = rect.right // 2, rect.bottom // 4
        self.u.ClientToScreen(self.hwnd, C.byref(point))
        if self.owner(self.u.WindowFromPoint(point)) != self.pid:
            raise RuntimeError('Another window covers the game')
        self.u.SetCursorPos(point.x, point.y)
        clicks = (INPUT * 2)(INPUT(type=0, mi=MOUSEINPUT(dwFlags=0x0002)), INPUT(type=0, mi=MOUSEINPUT(dwFlags=0x0004)))
        if self.u.SendInput(2, clicks, C.sizeof(INPUT)) != 2:
            raise RuntimeError('Click failed')

    def move(self, dx):
        if not self.focused():
            return False
        # MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE, as in the old baseline runs.
        packet = INPUT(type=0, mi=MOUSEINPUT(dx=dx, dwFlags=0x0001 | 0x2000))
        return self.u.SendInput(1, C.byref(packet), C.sizeof(INPUT)) == 1

    def close(self):
        if self.process:
            self.k.CloseHandle(self.process)
            self.process = None


def read_log(game):
    return (game.path.parent / 'ue4ss' / 'UE4SS.log').read_bytes()


def watch(game, start, until):
    """Scans for the crash dialog until `until`. Returns the outcome, or None if nothing happened.

    The dialog is checked before process exit is judged: a crash can end the process soon after
    its dialog appears, and that is still a crash."""
    while True:
        now = time.monotonic()
        if game.crash_dialog():
            return 'CRASH_DIALOG', round(now - start, 3)
        if not game.alive():
            return 'PROCESS_EXITED', round(now - start, 3)  # no dialog seen: unclassified, NOT "no crash"
        if now >= until:
            return None, None
        time.sleep(SCAN_SECONDS)


def spin(game, seconds, dx, hz, observe_seconds):
    """Spins while the game has focus; then keeps watching for `observe_seconds`.

    A crash dialog takes focus from the game window, so losing focus is never taken as the end of
    the test: every stop is followed by an immediate dialog check and the observation window."""
    start = time.monotonic()
    result = dict(outcome='NO_CRASH_OBSERVED', sent=0)
    next_scan, stop_reason = start, None
    game.click_inside()
    while stop_reason is None:
        now = time.monotonic()
        if now >= next_scan or not game.focused():
            outcome, at = watch(game, start, now)  # one immediate scan
            if outcome:
                result.update(outcome=outcome, after_seconds=at, stopped_by='crash_or_exit')
                result['spun_seconds'] = round(time.monotonic() - start, 3)
                return result
            next_scan = now + SCAN_SECONDS
        stop_reason = ('escape' if game.escape() else 'focus_lost' if not game.focused()
                       else 'duration' if now - start >= seconds else None)
        if stop_reason is None and game.move(dx):
            result['sent'] += 1
        time.sleep(1 / hz)

    result['stopped_by'] = stop_reason
    result['spun_seconds'] = round(time.monotonic() - start, 3)
    outcome, at = watch(game, start, time.monotonic() + observe_seconds)
    if outcome:
        result.update(outcome=outcome, after_seconds=at)
    result['observed_seconds_after_spin'] = observe_seconds
    if result['outcome'] == 'NO_CRASH_OBSERVED' and game.hung():
        result['outcome'] = 'NOT_RESPONDING'  # a hang shows no dialog; never read it as "no crash"
    if stop_reason != 'duration' and result['outcome'] == 'NO_CRASH_OBSERVED':
        result['outcome'] = 'STOPPED_EARLY_NO_CRASH_OBSERVED'  # not a full-length run
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--pid', type=int, required=True)
    p.add_argument('--run', action='store_true')
    p.add_argument('--output', type=Path)
    p.add_argument('--seconds', type=float, default=360)
    p.add_argument('--dx', type=int, default=50, help='mouse counts per event (the old baseline: 50)')
    p.add_argument('--hz', type=int, default=1000, help='events per second requested (the old baseline: 1000)')
    p.add_argument('--observe-seconds', type=float, default=15, help='keep watching for a crash after the spin stops')
    p.add_argument('--allow-shared-view', action='store_true', help='spin with player 2 on player 1''s camera (the same-camera control)')
    args = p.parse_args()
    if not (math.isfinite(args.seconds) and args.seconds > 0 and math.isfinite(args.observe_seconds) and args.observe_seconds >= 0 and 1 <= abs(args.dx) <= 32767 and 1 <= args.hz <= 2000):
        p.error('seconds > 0; dx within +/-32767; hz 1..2000')
    if args.run and not args.output:
        p.error('--run needs a new --output directory')

    game = Game(args.pid)
    try:
        text = read_log(game).decode('utf-8', errors='replace')
        state = session_state(text)
        startup = re.search(r'\[([^]]+)\] Console created', text)
        state['log_matches_process'] = bool(startup) and abs(
            datetime.fromisoformat(startup[1]).timestamp() - game.created) <= 15
        print(json.dumps(dict(pid=args.pid, **state)))
        if not args.run:
            return 0
        if not state['log_matches_process'] or not state['ready']:
            raise SystemExit('The log is not this process\'s, or the mod did not start: nothing sent')
        if not state['player_two']:
            raise SystemExit('Player 2 is not in (Ctrl+F7): nothing sent')
        if not state['turned'] and not args.allow_shared_view:
            raise SystemExit('Player 2\'s half is not on its own camera (Ctrl+F6), and --allow-shared-view was not given: nothing sent')

        args.output.mkdir(parents=True, exist_ok=False)
        started = datetime.now(timezone.utc).isoformat()
        result = spin(game, args.seconds, args.dx, args.hz, args.observe_seconds)
        result.update(pid=args.pid, started_utc=started, requested_seconds=args.seconds, dx=args.dx, hz=args.hz,
                      turned=state['turned'])
        data = read_log(game)
        with gzip.GzipFile(filename=str(args.output / 'UE4SS.log.gz'), mode='wb', mtime=0) as out:
            out.write(data)
        result['log_sha256'] = hashlib.sha256(data).hexdigest()
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        print(json.dumps(result))
        if result['outcome'] in ('CRASH_DIALOG', 'PROCESS_EXITED', 'NOT_RESPONDING'):
            print('CRASH: leave the dialog open until the Aftermath dump stops growing; do not press OK before that.')
        return 0
    finally:
        game.close()


if __name__ == '__main__':
    raise SystemExit(main())
