"""The README's project map: one line per folder of src/, and the repository's other folders.

    python tools/layout_doc.py            rewrite the map in README.md
    python tools/layout_doc.py --check    exit 1 when the map is out of date (a ctest, so the build says so)

The folders and top-level .h/.cpp files are read from disk; their descriptions are the tables below. A folder or file
with no description shows a marker, so the check fails until one is written. Only the fenced block under "## Project map" in
the README is replaced.
"""
import pathlib
import re
import sys
import textwrap

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / 'src'
README = ROOT / 'README.md'

TOP = {
    'mod.cpp': 'the entry point',
    'modules': 'every module, in install order',
    'engine_tick': 'the mod\'s call once per frame',
}
FOLDERS = {
    'platform': 'game versions and address checks, game-code patches, safe memory access, the log, settings',
    'facts': 'addresses and offsets that several modules share',
    'players': 'shared engine access: local players, calls into the engine, the split layout',
    'session': 'joining and leaving, and the party member each player plays',
    'scopes': 'running one game call as player 2 or 3',
    'camera': 'each player\'s camera',
    'input': 'controllers, sticks and movement',
    'feedback': 'rumble and sound',
    'battle': 'command menus, targeting, enemy attention, synergies, battle options',
    'traversal': 'climbing, vaulting and the rest of traversal',
    'world': 'crowds, triggers, pickups, shops, levers, catch-up warps',
    'rides': 'chocobos and other rides',
    'views': 'screen layouts, full-screen moments, multiple monitors',
    'hud': 'each player\'s HUD',
    'renderer': 'one render pass per player, and frame-rate work',
    'menu': 'the Co-op options page',
    'control': 'the development console',
    'dev': 'development tools and watches',
    'ue4ss_compat': 'header shims for the UE4SS version the mod builds against',
}
ORDER = ['platform', 'facts', 'players', 'session', 'scopes', 'camera', 'input', 'feedback', 'battle', 'traversal',
         'world', 'rides', 'views', 'hud', 'renderer', 'menu', 'control', 'dev', 'ue4ss_compat']
OTHER = [
    ('setup/', 'the Multi Monitor Setup app'),
    ('tests/', 'host tests, run without the game'),
    ('tools/', 'build lock, deploy, console client, regression runner, version matching, releases'),
]
MISSING = '(no description: add one to tools/layout_doc.py)'
WIDTH = 120
NAME_COL = 20


def row(name: str, desc: str, indent: int) -> str:
    head = ' ' * indent + name
    body = textwrap.wrap(desc, width=WIDTH - NAME_COL, break_long_words=False) or ['']
    if len(head) >= NAME_COL:
        return '\n'.join([head] + [' ' * NAME_COL + b for b in body])
    return '\n'.join([head.ljust(NAME_COL) + body[0]] + [' ' * NAME_COL + b for b in body[1:]])


def render() -> str:
    tops = sorted({p.stem if p.stem in TOP else p.name for p in SRC.iterdir() if p.suffix in ('.h', '.cpp')})
    folders = {p.name for p in SRC.iterdir() if p.is_dir()}
    lines = ['src/']
    lines += [row(name, TOP.get(name, MISSING), 2) for name in tops]
    lines += [row(f + '/', FOLDERS.get(f, MISSING), 2)
              for f in [f for f in ORDER if f in folders] + sorted(folders - set(ORDER))]
    lines += [row(name, desc, 0) for name, desc in OTHER]
    return '\n'.join(lines)


def main() -> int:
    raw = README.read_bytes().decode('utf-8')
    crlf = '\r\n' in raw
    text = raw.replace('\r\n', '\n')
    m = re.search(r'(## Project map\n(?:[^\n]*\n)*?\n?```\n)(.*?)(```)', text, flags=re.S)
    if not m:
        print('README has no "## Project map" fenced block', file=sys.stderr)
        return 2
    new = text[:m.start(2)] + render() + '\n' + text[m.end(2):]
    stale = new != text
    if '--check' in sys.argv:
        print(f'README project map {"OUT OF DATE: run python tools/layout_doc.py" if stale else "current"}')
        return 1 if stale else 0
    README.write_bytes((new.replace('\n', '\r\n') if crlf else new).encode('utf-8'))
    print(f'project map written to {README}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
