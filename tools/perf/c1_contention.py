"""C1 (perf-2026-10-08): whole-machine CPU contention from all-process context switches (cswitch_all.tsv.gz, new in this
tp2 build). Per logical CPU busy (non-idle), per process cores, and for GT/RT/RHI the share of their run time during
which the SMT sibling (cpu ^ 1, ASSUMPTION: Windows pairs SMT siblings as 2k/2k+1) ran a non-idle thread, split
into game vs other process. usage: python c1_contention.py <dumpdir> <label>"""
import sys, os
from tidcfg import GT, RT, RHI, PID
import numpy as np, pandas as pd
D, label = sys.argv[1], sys.argv[2]
a, b = map(int, open(os.path.join(D, 'window.txt')).read().split()); L = (b - a) / 1e9
c = pd.read_csv(os.path.join(D, 'cswitch_all.tsv.gz'), sep='\t')
c = c[(c.stop_ns > a) & (c.start_ns < b)].copy()
c['s'] = np.maximum(c.start_ns, a); c['e'] = np.minimum(c.stop_ns, b); c['run'] = c.e - c.s
busy = c[c.pid != 0]
procrun = pd.read_csv(os.path.join(D, 'procrun.tsv'), sep='\t')
names = dict(zip(procrun.pid, procrun.image))
print(f'== {label}: window {L:.2f} s; machine busy {busy.run.sum()/1e9/L:.2f} of {c.cpu.nunique()} logical CPUs')
print('per CPU busy %: ' + ', '.join(f'{int(k)}:{v/1e9/L*100:.0f}' for k, v in busy.groupby('cpu').run.sum().items()))
pc = busy.groupby('pid').run.sum().sort_values(ascending=False) / 1e9 / L
print('cores by process: ' + ', '.join(f'{names.get(p, p)}({p}) {v:.3f}' for p, v in pc.head(14).items()))
iv = {}
for cpu, g in busy.groupby('cpu'):
    g = g.sort_values('s'); iv[cpu] = (g.s.values, g.e.values, g.pid.values)
for tid, nm in ((GT, 'GameThread'), (RT, 'RenderThread'), (RHI, 'RHIThread')):
    me = busy[busy.tid == tid]; tot = me.run.sum(); ovg = 0; ovo = 0
    for cpu, g in me.groupby('cpu'):
        if (cpu ^ 1) not in iv: continue
        ss, se, sp = iv[cpu ^ 1]
        for s0, e0 in g[['s', 'e']].values:
            i = np.searchsorted(se, s0); j = np.searchsorted(ss, e0)
            if j > i:
                o = np.clip(np.minimum(se[i:j], e0) - np.maximum(ss[i:j], s0), 0, None)
                game = sp[i:j] == PID
                ovg += o[game].sum(); ovo += o[~game].sum()
    cpus = (me.groupby('cpu').run.sum() / tot * 100).round(0).to_dict()
    print(f'{nm}: run {tot/1e6/L:.0f} ms/s; SMT sibling busy {(ovg+ovo)/tot:.1%} of its run time (game {ovg/tot:.1%}, other processes {ovo/tot:.1%}); CPU share % {cpus}')
# ready time of game threads when ALL CPUs busy? approximate: machine-wide busy timeline 1ms bins
bins = np.arange(a, b, 1_000_000)
occ = np.zeros(len(bins))
for s0, e0 in busy[['s', 'e']].values:
    i0 = (s0 - a) // 1_000_000; i1 = (e0 - a) // 1_000_000
    if i0 == i1: occ[i0] += (e0 - s0) / 1e6
    else:
        occ[i0] += ((i0 + 1) * 1_000_000 + a - s0) / 1e6; occ[i0 + 1:i1] += 1
        if i1 < len(occ): occ[i1] += (e0 - (i1 * 1_000_000 + a)) / 1e6
q = np.percentile(occ, [50, 90, 99])
print(f'busy logical CPUs per 1-ms bin: mean {occ.mean():.2f} p50 {q[0]:.2f} p90 {q[1]:.2f} p99 {q[2]:.2f}; bins with >=7.5 busy: {(occ>=7.5).mean():.1%}')
