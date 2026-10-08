"""P16 (perf-2026-10-03c, generalised in perf-2026-10-04): RHI-thread time by the win32u syscall stub on the stack (first
win32u frame from the leaf), plus by Streamline/RTSS module presence, ms/frame, for one or more captures.
usage: python p16_rhi_syscalls.py <label>=<dump> [<label>=<dump> ...]   (labels from tidcfg: v056, v047/new, old; fps from tidcfg.FPS)"""
import os, sys, collections, importlib

res = {}
for lab, d in (a.split('=', 1) for a in sys.argv[1:]):
    os.environ['LSS_CAP'] = lab; os.environ['LSS_DUMP'] = d
    import tidcfg; importlib.reload(tidcfg)
    import etwlib; importlib.reload(etwlib); E = etwlib
    fps = tidcfg.FPS[lab]
    mods = E.modules(); paths = E.module_paths(mods, tidcfg.PID); ST = E.stacks(mods)
    A, B = E.window(); L = (B - A) / 1e9
    sm = E.samples(); sm = sm[(sm.t_ns >= A) & (sm.t_ns < B) & (sm.tid == tidcfg.RHI)]
    c = collections.Counter(); cache = {}
    for sid in sm['stack']:
        if sid not in cache:
            fr = ST.get(sid, []); keys = []
            w = next((f for f in fr if f[0] == 'win32u.dll'), None)
            if w: keys.append('win32u!' + (w[2] or E.Exports.name(paths['win32u.dll'], w[1]).split('+')[0]))
            ms = {f[0] for f in fr}
            for m in ('sl.interposer.dll', 'sl.dlss_g.dll', 'RTSSHooks64.dll', 'GameOverlayRenderer64.dll', 'nvwgf2umx.dll'):
                if m in ms: keys.append('module ' + m)
            cache[sid] = keys
        for k in cache[sid]: c[k] += 1
    c['all RHI samples'] = len(sm)
    res[lab] = {k: v / L / fps for k, v in c.items()}
labs = list(res)
keys = sorted(set().union(*res.values()), key=lambda k: -sum(r.get(k, 0) for r in res.values()))
print(f'{"RHI ms/f":55s} ' + '  '.join(f'{l:>6s}' for l in labs))
for k in keys[:18]:
    print(f'{k:55s} ' + '  '.join(f'{res[l].get(k, 0):6.3f}' for l in labs))
