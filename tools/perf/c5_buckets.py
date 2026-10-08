"""C5 (perf-2026-10-08): RHI-thread (or other) time in exclusive buckets, first match wins in this order:
present, queue ops (D3D12 CCommandQueue ExecuteCommandLists/Signal/Wait, QueryVideoMemoryInfo), other kernel/driver,
command translation (game code + D3D12 command-list calls). Each bucket split by whether the sample is in the capture's
own ETW work (kernel EtwWriteTransfer/stack walk). usage: python c5_buckets.py <dumpdir> <fps> RHI|RT|GT|TG"""
import sys, os, collections
from tidcfg import GT, RT, RHI, PID
os.environ['LSS_DUMP'] = sys.argv[1]
from etwlib import *
fps, thr = float(sys.argv[2]), sys.argv[3]
mods = modules(); ST = stacks(mods); PD = PData()
th = threads(); names = dict(zip(th.tid, th.name))
TH = {'GT': lambda t: t == GT, 'RT': lambda t: t == RT, 'RHI': lambda t: t == RHI,
      'TG': lambda t: (names.get(t, '') or '').startswith('TaskGraphThread')}[thr]
A, B = window(); L = (B - A) / 1e9
sm = samples(); sm = sm[(sm.t_ns >= A) & (sm.t_ns < B)]; sm = sm[[TH(t) for t in sm.tid]]
ETW = ('EtwWriteTransfer', 'EtwpTraceStackWalk', 'EtwpEventWriteFull', 'EtwpLogKernelEvent', 'EtwpStackWalkApc', 'EtwpTraceStackKey', 'EtwTraceContextSwap', 'EtwTraceReadyThread')
PRESENT = 0x140adf4b4
def k(f): return f[0].lower().endswith('.sys') or f[0].lower() in ('ntoskrnl.exe', 'hal.dll')
c = collections.Counter(); cache = {}
for sid in sm['stack']:
    if sid not in cache:
        fr = ST.get(sid, [])
        etw = any(f[2] and any(x in f[2] for x in ETW) for f in fr)
        gf = {IDA_BASE + (PD.func_start(f[1]) or 0) for f in fr if f[0] == 'ff7rebirth_.exe'}
        syms = [f[2] or '' for f in fr if f[0] in ('D3D12Core.dll', 'dxgi.dll')]
        if PRESENT in gf: b = 'present (FD3D12Viewport_SwapChainPresent1 0x140ADF4B4, incl. sl.interposer/sl.dlss_g)'
        elif any(s.startswith('CCommandQueue') or 'QueryVideoMemoryInfo' in s for s in syms): b = 'queue ops (CCommandQueue ExecuteCommandLists/Signal/Wait, QueryVideoMemoryInfo)'
        elif fr and (k(fr[0]) or fr[0][0] in ('nvwgf2umx.dll', 'ntdll.dll', 'win32u.dll', 'KernelBase.dll')): b = 'other driver/kernel/ntdll leaf'
        else: b = 'command translation and other user code (game, D3D12Core command lists)'
        cache[sid] = (b, etw)
    b, e = cache[sid]
    c[(b, e)] += 1
tot = sum(c.values())
print(f'== {thr}: {tot/L/fps:.2f} ms/f')
for b in sorted({x[0] for x in c}, key=lambda b: -(c[(b, True)] + c[(b, False)])):
    a = c[(b, True)] + c[(b, False)]
    print(f'  {a/L/fps:6.2f} ms/f ({a/tot:5.1%})  of which capture ETW {c[(b, True)]/L/fps:5.2f}  | {b}')
print(f'  total capture ETW {sum(v for (b, e), v in c.items() if e)/L/fps:.2f} ms/f')
