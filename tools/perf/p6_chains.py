"""P6: top call chains for samples matching a predicate (game frames as IDA fn starts, other modules as module+export).
usage: python p6_chains.py <dumpdir> <label> <fps> <thread: GT|RT|RHI|TG|ALL> <mode: inc|first|leaf> <arg> [depth] [k]
  inc  <0xFN>   : samples whose stack contains a frame in game fn 0xFN (fn start)
  first <0xFN>  : samples whose first game frame is in fn 0xFN
  leaf <text>   : samples whose leaf (module!export or fn) contains <text>
Prints chains leaf->root, truncated to depth; ms/frame = samples/s / fps."""
import sys, os, collections
from tidcfg import GT, RT, RHI, PID, FPS  # perf-2026-10-03c: LSS_CAP=old|new
import pandas as pd
os.environ['LSS_DUMP'] = sys.argv[1]
from etwlib import *

label, fps, thr, mode, arg = sys.argv[2], float(sys.argv[3]), sys.argv[4], sys.argv[5], sys.argv[6]
depth = int(sys.argv[7]) if len(sys.argv) > 7 else 14
k = int(sys.argv[8]) if len(sys.argv) > 8 else 8
mods = modules(); paths = module_paths(mods); ST = stacks(mods); PD = PData()
th = threads(); names = dict(zip(th.tid, th.name))
TH = {'GT': lambda t: t == GT, 'RT': lambda t: t == RT, 'RHI': lambda t: t == RHI,
      'TG': lambda t: (names.get(t, '') or '').startswith('TaskGraphThread'), 'ALL': lambda t: True}[thr]
A, B = window(); L = (B - A) / 1e9
sm = samples(); sm = sm[(sm.t_ns >= A) & (sm.t_ns < B)]
sm = sm[[TH(t) for t in sm.tid]]


def is_kernel(m):
    return m.lower().endswith('.sys') or m.lower() in ('ntoskrnl.exe', 'hal.dll') or m == '?'


def fr_name(m, rva, sym=''):
    if sym and m != 'ff7rebirth_.exe':
        return f'{m.rsplit(".", 1)[0]}!{sym}'
    return _fr_name(m, rva)


def _fr_name(m, rva):
    if m == 'ff7rebirth_.exe':
        s = PD.func_start(rva)
        return f'{IDA_BASE + (s if s is not None else rva):#x}' + ('' if s is not None else '?')
    p = paths.get(m, '')
    if isinstance(p, str) and p.upper().startswith('C:\\WINDOWS'):
        return f'{m.split(".")[0]}!{Exports.name(p, rva).split("+")[0]}'
    return f'{m}+{rva:#x}'


want = int(arg, 16) if mode in ('inc', 'first') else None
cnt = collections.Counter(); cup = collections.Counter(); n = 0
cache = {}
for sid in sm['stack']:
    if sid not in cache:
        fr = [f for f in ST.get(sid, []) if not is_kernel(f[0])]
        nm = [fr_name(*f) for f in fr]
        if mode == 'inc':
            ok = any(f[0] == 'ff7rebirth_.exe' and (PD.func_start(f[1]) or -1) + IDA_BASE == want for f in fr)
        elif mode == 'first':
            g = [f for f in fr if f[0] == 'ff7rebirth_.exe']
            ok = bool(g) and (PD.func_start(g[0][1]) or -1) + IDA_BASE == want
        else:
            ok = bool(nm) and arg in nm[0]
        # collapse consecutive duplicates
        ch = []
        for x in nm:
            if not ch or ch[-1] != x:
                ch.append(x)
        up = ()
        if ok and want is not None:
            i = next((j for j, x in enumerate(ch) if x == f'{want:#x}'), None)
            if i is not None:
                up = tuple(ch[i:i + depth])
        cache[sid] = (ok, tuple(ch[:depth]), up)
    ok, ch, up = cache[sid]
    if ok:
        cnt[ch] += 1; n += 1
        if up: cup[up] += 1
print(f'== {label} {thr} {mode} {arg}: {n/L:.1f} samples/s = {n/L/fps:.3f} ms/frame')
print('-- leaf-side chains:')
for ch, v in cnt.most_common(k):
    print(f'{v/L/fps:7.3f} ms/f : ' + ' < '.join(ch))
if cup:
    print('-- callers (target fn upward):')
    for ch, v in cup.most_common(k):
        print(f'{v/L/fps:7.3f} ms/f : ' + ' < '.join(ch))
