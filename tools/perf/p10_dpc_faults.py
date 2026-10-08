"""P10: DPC/ISR load (all CPUs) and hard faults in the common window, plus GPU node usage by the game on minor nodes.
usage: python p10_dpc_faults.py <dumpdir> <label>"""
import sys, os
from tidcfg import GT, RT, RHI, PID, FPS  # perf-2026-10-03c: LSS_CAP=old|new
import pandas as pd
D, label = sys.argv[1], sys.argv[2]
A, B = map(int, open(os.path.join(D, 'window.txt')).read().split()); L = (B - A) / 1e9
for kind in ('dpc', 'isr'):
    d = pd.read_csv(os.path.join(D, f'{kind}.tsv.gz'), sep='\t', usecols=['StartTime', 'StopTime', 'Processor', 'HandlerImage'])
    d = d[(d.StartTime >= A) & (d.StartTime < B)]
    d['dur'] = (d.StopTime - d.StartTime) / 1e3  # us
    g = d.groupby('HandlerImage').dur.agg(['size', 'sum', 'max']).sort_values('sum', ascending=False).head(6)
    per_cpu = d.groupby('Processor').dur.sum() / 1e3 / L  # ms/s
    print(f'== {label} {kind.upper()}: {len(d)/L:.0f}/s, total {d.dur.sum()/1e3/L:.1f} ms/s (all CPUs), busiest CPU {per_cpu.idxmax()} '
          f'{per_cpu.max():.1f} ms/s ({per_cpu.max()/10:.2f}%), >100us: {(d.dur > 100).sum()/L:.1f}/s, >1ms: {(d.dur > 1000).sum()}, max {d.dur.max():.0f} us')
    for img, r in g.iterrows():
        print(f'     {str(img):24s} {r["size"]/L:7.0f}/s {r["sum"]/1e3/L:7.2f} ms/s  max {r["max"]:.0f} us')
hf = pd.read_csv(os.path.join(D, 'hardfault.tsv.gz'), sep='\t')
hf = hf[(hf.Timestamp >= A) & (hf.Timestamp < B)]
gp = hf[hf.FaultingProcess == PID]
print(f'== {label} hard faults: all {len(hf)/L:.1f}/s; game {len(gp)/L:.1f}/s; game threads {gp.FaultingThread.value_counts().head(4).to_dict()}; '
      f'top files {gp.FileName.value_counts().head(3).to_dict()}')
