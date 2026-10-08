"""perf-2026-10-08 (cloud PC, AWS g6.2xlarge): v0.62.0 split in Costa del Sol, game 1.005.
captures/split-20261008-060559: pid 13004, GT 5096 (unnamed, AboveNormal, top CPU), RT 12872 "RenderThread 4", RHI 5080."""
import os, re
CFG = {'cloud': dict(PID=13004, GT=5096, RT=12872, RHI=5080)}
CAP = os.environ.get('LSS_CAP', 'cloud')
PID = int(os.environ.get('LSS_PID', CFG[CAP]['PID']))
GT, RT, RHI = CFG[CAP]['GT'], CFG[CAP]['RT'], CFG[CAP]['RHI']
_here = os.path.dirname(os.path.abspath(__file__))
FPS = {}
for _l in CFG:
    FPS[_l] = float('nan')
    for _f in (f'p3_summary_{_l}.txt', f'p3h_summary_{_l}.txt'):
        try:
            FPS[_l] = float(re.search(r'([\d.]+) fps', open(os.path.join(_here, _f)).read()).group(1)); break
        except Exception:
            pass
