"""C3 (perf-2026-10-08): the game's GPU contexts under HAGS: per-context and union busy (upper bound, p3h method), how
the three heavy contexts' submissions line up in time, and the ctx -> node mapping from DxgKrnl Context DC_Start/Start
events if present. usage: python c3_gpu_contexts.py <dumpdir> <pid>"""
import sys, os, gzip, collections
import numpy as np, pandas as pd
D, PID = sys.argv[1], sys.argv[2]
sub, done, pres, ctxinfo = [], [], [], {}
with gzip.open(os.path.join(D, 'dxg.tsv.gz'), 'rt') as f:
    next(f)
    for line in f:
        p = line.rstrip('\n').split('\t', 7)
        i = p[1]
        if i in ('178', '180'):
            kv = dict(x.split('=', 1) for x in p[7].split(';') if '=' in x)
            if i == '178': sub.append((int(p[0]), p[4], int(kv['hContext']), int(kv['SubmitSequence']), kv.get('PacketType', ''), kv.get('bPresent','')))
            else: done.append((int(kv['hContext']), int(kv['SubmitSequence']), int(p[0])))
        elif i == '184' and p[4] == PID: pres.append(int(p[0]))
        elif p[2] in ('Context', 'HwQueue') and p[4] == PID:
            kv = dict(x.split('=', 1) for x in p[7].split(';') if '=' in x)
            k = kv.get('hContext') or kv.get('hHwQueue')
            if k: ctxinfo.setdefault(int(k), (p[2], p[3], {a: b for a, b in kv.items() if a in ('NodeOrdinal', 'EngineAffinity', 'hDevice', 'ContextType', 'Flags', 'hContext', 'hHwQueue', 'ParentContext', 'hParentContext')}))
S = pd.DataFrame(sub, columns=['t', 'pid', 'ctx', 'seq', 'ptype', 'present'])
C = pd.DataFrame(done, columns=['ctx', 'seq', 'td']).drop_duplicates(['ctx', 'seq'], keep='last')
M = S[S.pid == PID].merge(C, on=['ctx', 'seq'])
WA, WB = map(int, open(os.path.join(D, 'window.txt')).read().split())
t = np.sort(np.array(pres)); t = t[(t >= WA) & (t < WB)]; A, B = t[0], t[-1]; nf = len(t) - 1
def ivs(g):
    g = g.sort_values('seq'); prev = np.concatenate([[0], np.maximum.accumulate(g.td.values)[:-1]])
    st = np.maximum(g.t.values, prev); en = g.td.values; ok = en > st
    return list(zip(st[ok], en[ok]))
def union(iv):
    iv = sorted(iv); o = []
    for s, e in iv:
        if o and s <= o[-1][1]: o[-1][1] = max(o[-1][1], e)
        else: o.append([s, e])
    return o
def busy(iv):
    return sum(max(0, min(e, B) - max(s, A)) for s, e in iv)
allv = []
for c, g in M.groupby('ctx'):
    v = union(ivs(g)); allv += v
    gw = g[(g.t >= A) & (g.t < B)]
    print(f'ctx {c}: {len(gw)/nf:.1f}/f busy {busy(v)/(B-A):.1%} {busy(v)/1e6/nf:.2f} ms/f; ptype {gw.ptype.value_counts().head(3).to_dict()}; present {gw.present.value_counts().to_dict()}; info {ctxinfo.get(c)}')
u = union(allv)
print(f'UNION of all game contexts: busy {busy(u)/(B-A):.1%} = {busy(u)/1e6/nf:.2f} ms/f (upper bound); frame {(B-A)/1e6/nf:.2f} ms')
# do the 3 heavy contexts submit at the same times?
heavy = M.groupby('ctx').size().sort_values(ascending=False).index[:3]
ts = {c: np.sort(M[M.ctx == c].t.values) for c in heavy}
c0 = heavy[0]
for c in heavy[1:]:
    d = np.abs(ts[c][:, None] - ts[c0][None, :2000]).min(axis=1)[:2000] if False else None
    idx = np.searchsorted(ts[c0], ts[c]); idx = np.clip(idx, 1, len(ts[c0]) - 1)
    dd = np.minimum(np.abs(ts[c] - ts[c0][idx - 1]), np.abs(ts[c] - ts[c0][idx])) / 1e3
    print(f'ctx {c} submit vs nearest ctx {c0} submit: median {np.median(dd):.1f} us, p90 {np.percentile(dd, 90):.1f} us')
print('ctxinfo entries for game:', len(ctxinfo))
