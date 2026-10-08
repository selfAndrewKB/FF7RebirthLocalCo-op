"""C2 (perf-2026-10-08): how much sampled time per thread group is the capture's own ETW work (event writes with
kernel stack walks, e.g. DxgKrnl GPU.light events on each submission, and context-switch/ready stack walks), from the
symbolized kernel frames of a TP_SYMDIR dump. Also the NVIDIA UMD user-mode stack capture (ntdll RtlCaptureStackBackTrace
under nvwgf2umx, RE-perf-solo-vs-split-2026-10-03 §5). usage: python c2_etw_overhead.py <dumpdir> <label> <fps>"""
import sys, os, collections
from tidcfg import GT, RT, RHI, PID
os.environ['LSS_DUMP'] = sys.argv[1]
from etwlib import *
label, fps = sys.argv[2], float(sys.argv[3])
mods = modules(); ST = stacks(mods)
th = threads(); names = dict(zip(th.tid, th.name))
A, B = window(); L = (B - A) / 1e9
sm = samples(); sm = sm[(sm.t_ns >= A) & (sm.t_ns < B)]
ETW = ('EtwWriteTransfer', 'EtwpTraceStackWalk', 'EtwpEventWriteFull', 'EtwpLogKernelEvent', 'EtwpStackWalkApc', 'EtwTraceReadyThread',
       'EtwTraceContextSwap', 'EtwpTraceStackKey', 'EtwWrite', 'EtwpWriteUserEvent', 'EtwEventWrite', 'EtwTraceThread')
def grp(t):
    n = names.get(t, '') or ''
    if t == GT: return 'GameThread'
    if t == RT: return 'RenderThread'
    if t == RHI: return 'RHIThread'
    if n.startswith('TaskGraphThread'): return 'TaskGraph'
    return 'other'
cnt = collections.Counter(); sub = collections.Counter(); cache = {}
for sid, tid in zip(sm['stack'], sm.tid):
    if sid not in cache:
        fr = ST.get(sid, [])
        syms = [f[2] for f in fr]
        etw = next((s for s in syms if s and any(k in s for k in ETW)), None)
        umd = any(f[0] == 'ntdll.dll' and f[2] in ('RtlCaptureStackBackTrace', 'RtlWalkFrameChain', 'RtlVirtualUnwind2') for f in fr) and any(f[0] == 'nvwgf2umx.dll' for f in fr)
        # where the ETW write came from: the first non-ETW kernel frame above (root-ward) the ETW frames
        src = ''
        if etw:
            i = max(j for j, s in enumerate(syms) if s and any(k in s for k in ETW))
            nxt = next((fr[j] for j in range(i + 1, len(fr)) if fr[j][2] and not fr[j][2].startswith(('Mc', 'Etw'))), None)
            src = f'{nxt[0].split(".")[0]}!{nxt[2]}' if nxt else '?'
        cache[sid] = (etw is not None, umd, src)
    e, u, src = cache[sid]
    g = grp(tid)
    cnt[(g, 'all')] += 1
    if e: cnt[(g, 'etw')] += 1; sub[(g, src)] += 1
    if u: cnt[(g, 'umd')] += 1
for (g, k), v in list(cnt.items()): cnt[('Process', k)] += v
for (g, s), v in list(sub.items()): sub[('Process', s)] += v
for g in ('RHIThread', 'GameThread', 'RenderThread', 'TaskGraph', 'other', 'Process'):
    a = cnt[(g, 'all')]
    if not a: continue
    print(f'{label} {g}: all {a/L/fps:.2f} ms/f; ETW write+stackwalk {cnt[(g,"etw")]/L/fps:.2f} ms/f ({cnt[(g,"etw")]/a:.1%}); '
          f'NVIDIA UMD stack capture {cnt[(g,"umd")]/L/fps:.2f} ms/f')
    top = sorted(((v, s) for (gg, s), v in sub.items() if gg == g), reverse=True)[:6]
    print('    ETW from: ' + '; '.join(f'{s} {v/L/fps:.2f}' for v, s in top))
