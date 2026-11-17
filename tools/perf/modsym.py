"""Symbolize LocalSplitScreen main.dll RVAs with its PDB via llvm-symbolizer (dbghelp via ctypes failed, error 126).
The build output LocalSplitScreen.dll (SHA-256 7BC841AB...2FA0) is byte-identical to the deployed main.dll of both captures."""
import os, shutil, subprocess, functools, pefile
from tidcfg import GT, RT, RHI, PID, FPS  # LSS_CAP picks the capture

BUILD = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'Game__Shipping__Win64')
# LSS_MODDLL points at a saved copy of the matching build DLL (its PDB beside it), because the build folder moves on;
# check its SHA-256 against the capture's modDllHash first.
DLL = os.environ.get('LSS_MODDLL', os.path.join(BUILD, 'LocalSplitScreen.dll'))
# A captured DLL without a PDB has its RVAs mapped to the current build first (modmap.py -> modmap.json).
import json
_MAP = {int(k): v for k, v in json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'modmap.json'))).items()} if os.path.exists(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'modmap.json')) else None
SYMBOLIZER = os.environ.get('LSS_SYMBOLIZER') or shutil.which('llvm-symbolizer')  # LLVM's, on PATH or named
if not SYMBOLIZER:
    raise SystemExit('llvm-symbolizer is not on PATH: set LSS_SYMBOLIZER to it')
IMAGE_BASE = pefile.PE(DLL, fast_load=True).OPTIONAL_HEADER.ImageBase
_cache = {}


def resolve(rvas):
    if _MAP is not None:
        for r in set(rvas):
            if r not in _cache and (_MAP.get(r) or [None])[0] is None:
                _cache[r] = f'?? [unmatched captured rva {r:#x}]'
    todo = [r for r in set(rvas) if r not in _cache]
    if todo:
        inp = '\n'.join(f'{IMAGE_BASE + r:#x}' for r in todo) + '\n'
        out = subprocess.run([SYMBOLIZER, '--obj=' + DLL, '--no-inlines', '--functions=linkage', '--demangle', '--output-style=GNU'],
                             input=inp, capture_output=True, text=True).stdout.splitlines()
        # GNU style: 2 lines per address (function, file:line)
        for i, r in enumerate(todo):
            fn = out[2 * i] if 2 * i < len(out) else '??'
            loc = out[2 * i + 1].split('\\')[-1] if 2 * i + 1 < len(out) else ''
            _cache[r] = f'{fn} [{loc}]'
    return [_cache[r] for r in rvas]


def sym(rva):
    return resolve([rva])[0]


if __name__ == '__main__':
    import sys
    rv = [int(a, 16) for a in sys.argv[1:]]
    for a, s in zip(rv, resolve(rv)):
        print(f'{a:#x}', s)
