"""The mod README's Layout, generated from each module's first comment.

    python tools/layout_doc.py            rewrite the README's Layout block
    python tools/layout_doc.py --check    exit 1 when the README is out of date (a ctest, so the build says so)

Every module under src/ (a header, or a .cpp with no header) is one row: its name and the first sentence of the first
comment block in its header (the paragraph of `//` lines after `#pragma once`, before any code). A module with no such
comment shows "(no header comment)" so the gap is visible. The folders carry a one-line description kept in FOLDERS
below (the only hand-kept text). tests/ is one row with its count. The block between the README's "## Layout" fence
lines is replaced; nothing else in the README is touched.
"""
import pathlib
import re
import sys
import textwrap

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / 'src'
README = ROOT / 'README.md'

FOLDERS = {
    'platform': 'the game image, the fact checks, the redirects, guards, the log, the settings file, the command registry',
    'facts': 'the engine facts two or more modules share, one declaration per address',
    'players': 'the engine\'s own paths: local players, reflection calls, console and settings access; the split layout; '
               'each player\'s member generation, the log descriptions and the field reads every module shares',
    'session': 'players 2 and 3\'s join, party member and leave; the lead gate and the ride-along',
    'scopes': 'one game call made as player 2 or 3',
    'camera': 'each player\'s cameras',
    'input': 'pads, sticks and movement',
    'feedback': 'rumble and sound',
    'battle': 'players 2 and 3\'s battle: menus, targets, the enemies\' coin, synergies, the warps and the rates',
    'traversal': 'climbing, vaulting and the rest of traversal',
    'world': 'the world around each player: crowds, triggers, story boxes, pickups, shops, the catch-up warps',
    'rides': 'chocobos, the wheelie and the shared rides',
    'views': 'the view points, the whole-screen states, the streaming views, the columns and mirror views, the window span',
    'hud': 'the HUD in each view: the passes, the band, the minimap, prompts, markers and pins, the battle HUD',
    'renderer': 'one view family per player, and the renderer\'s performance levers',
    'menu': 'the Co-op page: a row in the pause and title menus opening a page of the game\'s Options screen',
    'control': 'the console: the command language, the runner and the socket (built with the dev tools)',
    'dev': 'the dev tools: the console\'s install, the watches\' install and ticks',
}
ORDER = ['platform', 'facts', 'players', 'session', 'scopes', 'camera', 'input', 'feedback', 'battle', 'traversal',
         'world', 'rides', 'views', 'hud', 'renderer', 'menu', 'control', 'dev']
WIDTH = 120
NAME_COL = 28  # "    name" padded to this column, then the description


def first_comment(path: pathlib.Path) -> str:
    """The first sentence of the first `//` paragraph before any code line."""
    lines = path.read_text(encoding='utf-8').split('\n')
    para = []
    for line in lines[:60]:
        s = line.strip()
        if para:
            if s.startswith('//'):
                body = s[2:].strip()
                if body == '' or body.startswith('- '):
                    break  # the paragraph ends; a list is not the sentence
                para.append(body)
                continue
            break
        if s.startswith('//'):
            body = s[2:].strip()
            if body:
                para.append(body)
            continue
        if s == '' or s.startswith('#pragma') or s.startswith('#include') or s.startswith('#define'):
            continue
        break  # code before any comment
    if not para:
        return '(no header comment)'
    text = ' '.join(para)
    sentence = text
    for m in re.finditer(r'\.\s', text):
        if re.search(r'(e\.g|i\.e|vs|cf)$', text[:m.start()]):
            continue
        sentence = text[:m.start() + 1]
        break
    if sentence.endswith('.') and not sentence.endswith('..'):
        sentence = sentence[:-1]
    if len(sentence) > 160:
        # The references in parentheses (research docs, plans, tests) belong to the header.
        shorter = re.sub(r'\s*\((?:[^()]|\([^()]*\))*(?:RE-|research/|plan/|tests/|§)(?:[^()]|\([^()]*\))*\)', '',
                         sentence)
        if shorter.strip():
            sentence = shorter.strip().rstrip(',;:')
    if len(sentence) > 230:
        cut = sentence[:230].rsplit(' ', 1)[0]
        sentence = cut + ' …'
    return sentence


def modules():
    """The top-level rows and {folder: [(name, description)]}, headers preferred over their .cpp."""
    by_folder = {}
    top = []
    seen = set()
    for path in sorted(SRC.rglob('*')):
        if path.suffix not in ('.h', '.cpp'):
            continue
        rel = path.relative_to(SRC)
        key = rel.with_suffix('')
        if key in seen:
            continue
        header = path.with_suffix('.h')
        src = header if header.exists() else path
        seen.add(key)
        if len(rel.parts) == 1:
            top.append((rel.name, first_comment(src)))
        else:
            name = key.relative_to(rel.parts[0]).as_posix()
            if not path.with_suffix('.cpp').exists() and src.suffix == '.h':
                name += '.h'
            by_folder.setdefault(rel.parts[0], []).append((name, first_comment(src)))
    return top, by_folder


def wrap(name: str, desc: str, indent: int) -> str:
    head = ' ' * indent + name
    first = head + ' ' * max(NAME_COL - len(head), 1)
    body = textwrap.wrap(desc, width=WIDTH - NAME_COL, break_long_words=False, break_on_hyphens=False) or ['']
    if len(head) >= NAME_COL:  # a long name: its description starts on the next line
        return '\n'.join([head] + [' ' * NAME_COL + b for b in body])
    return '\n'.join([first + body[0]] + [' ' * NAME_COL + b for b in body[1:]])


def render():
    top, by_folder = modules()
    lines = ['src/']
    for name, desc in top:
        lines.append(wrap(name, desc, 2))
    folders = [f for f in ORDER if f in by_folder] + sorted(f for f in by_folder if f not in ORDER)
    for folder in folders:
        lines.append(wrap(folder + '/', FOLDERS.get(folder, '(no folder description: add it to tools/layout_doc.py)'), 2))
        for name, desc in by_folder[folder]:
            lines.append(wrap(name, desc, 4))
    tests = sorted((ROOT / 'tests').glob('*_test.cpp'))
    lines.append(wrap('tests/', f'{len(tests)} host-side test programs (ctest), one per pure header or code stub; '
                                'run without the game', 0))
    lines.append(wrap('tools/', 'the control scripts, the regression harness, the version match, this file', 0))
    count = len(top) + sum(len(v) for v in by_folder.values())
    return '\n'.join(lines), count


def main() -> int:
    block, count = render()
    raw = README.read_bytes().decode('utf-8')
    crlf = '\r\n' in raw
    text = raw.replace('\r\n', '\n')
    m = re.search(r'(## Layout\n(?:[^\n]*\n)*?\n?```\n)(.*?)(```)', text, flags=re.S)
    if not m:
        print('README has no "## Layout" fenced block', file=sys.stderr)
        return 2
    new = text[:m.start(2)] + block + '\n' + text[m.end(2):]
    stale = new != text
    if '--check' in sys.argv:
        print(f'{count} modules; README Layout {"OUT OF DATE: run python tools/layout_doc.py" if stale else "current"}')
        return 1 if stale else 0
    README.write_bytes((new.replace('\n', '\r\n') if crlf else new).encode('utf-8'))
    print(f'{count} modules written to {README}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
