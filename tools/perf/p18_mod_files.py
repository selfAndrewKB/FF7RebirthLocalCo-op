"""P18 (perf-2026-10-04): LocalSplitScreen time by source file and by function:line, per thread group.
For every sample with a mod frame: each distinct mod source file on the stack gets the sample (inclusive by file; a
sample counts once per file, so files nest and do not add up), and each distinct mod function:line likewise. Also the
root-most mod file (the entry) so the entries do add up.
usage: python p18_mod_files.py <dumpdir> <label> <fps> [filter-substring ...]  -> p18_mod_files_<label>.txt"""
import sys, os, collections, re
from tidcfg import GT, RT, RHI, PID, FPS
os.environ['LSS_DUMP'] = sys.argv[1]
from etwlib import *
import modsym

label, fps = sys.argv[2], float(sys.argv[3])
filters = sys.argv[4:]
mods = modules(); ST = stacks(mods)
th = threads(); names = dict(zip(th.tid, th.name))
A, B = window(); L = (B - A) / 1e9
sm = samples(); sm = sm[(sm.t_ns >= A) & (sm.t_ns < B)]
MOD = 'mod:LocalSplitScreen'


def tgrp(t):
    n = names.get(t, '') or ''
    if t == GT: return 'GT'
    if n.startswith('RenderThread'): return 'RT'
    if n == 'RHIThread': return 'RHI'
    if n.startswith('TaskGraphThread'): return 'TG'
    return 'other'


rv = set()
for sid in set(sm['stack']):
    for f in ST.get(sid, []):
        if f[0] == MOD: rv.add(f[1])
modsym.resolve(list(rv))


def parts(rva):
    s = modsym.sym(rva)
    m = re.match(r'(.*) \[(.*?)(?::(\d+))?\]$', s)
    fn, loc = (m.group(1), m.group(2)) if m else (s, '?')
    fn = re.sub(r"`anonymous namespace'::", '', fn).replace('lss::', '')
    return fn, loc, s.split(' [')[-1].rstrip(']') if ' [' in s else '?'


byfile = collections.Counter(); byfn = collections.Counter(); roots = collections.Counter(); leaf = collections.Counter()
cache = {}
for sid, tid in zip(sm['stack'], sm.tid):
    if sid not in cache:
        fr = [f for f in ST.get(sid, []) if f[0] == MOD]
        if not fr:
            cache[sid] = None
        else:
            ps = [parts(f[1]) for f in fr]
            files = set(p[1] for p in ps)
            fns = set(f'{p[0][:90]} [{p[2]}]' for p in ps)
            cache[sid] = (files, fns, ps[-1][1], f'{ps[0][0][:90]} [{ps[0][2]}]')
    x = cache[sid]
    if x is None: continue
    g = tgrp(tid)
    for f in x[0]: byfile[(g, f)] += 1
    for f in x[1]: byfn[(g, f)] += 1
    roots[(g, x[2])] += 1
    leaf[(g, x[3])] += 1
out = open(f'p18_mod_files_{label}.txt', 'w')
def P(s=''): print(s); out.write(s + '\n')
P(f'== {label}: LocalSplitScreen by source file (inclusive, nested), ms/frame (fps {fps})')
for (g, f), v in byfile.most_common(60):
    P(f'  {v/L/fps:7.3f} {g:5s} {f}')
P('\n-- by root-most mod file (entries; these add up per thread):')
for (g, f), v in roots.most_common(30):
    P(f'  {v/L/fps:7.3f} {g:5s} {f}')
P('\n-- by function:line (inclusive), top 60:')
for (g, f), v in byfn.most_common(60):
    P(f'  {v/L/fps:7.3f} {g:5s} {f}')
if filters:
    P('\n-- every function:line matching ' + ', '.join(filters) + ' (inclusive), any thread:')
    for (g, f), v in sorted(byfn.items(), key=lambda x: -x[1]):
        if any(s in f for s in filters):
            P(f'  {v/L/fps:7.3f} {g:5s} {f}')
    P('  (files) ' + '; '.join(f'{f} {g} {v/L/fps:.3f}' for (g, f), v in sorted(byfile.items(), key=lambda x: -x[1]) if any(s in f for s in filters)))
out.close()
